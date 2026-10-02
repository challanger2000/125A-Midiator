#include "RiffEngine.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

int structuralDifference(const midiator::Phrase& a, const midiator::Phrase& b) {
    const int used = std::min(a.usedSteps(), b.usedSteps());
    int different = std::abs(a.usedSteps() - b.usedSteps());
    for (int i = 0; i < used; ++i) {
        const auto& x = a.steps[i];
        const auto& y = b.steps[i];
        if (x.noteCount != y.noteCount) {
            ++different;
            continue;
        }
        if (x.noteCount == 0)
            continue;
        if (x.notes[0].pitch != y.notes[0].pitch ||
            x.notes[0].lengthSteps != y.notes[0].lengthSteps ||
            (x.noteCount > 1) != (y.noteCount > 1))
            ++different;
    }
    return different;
}

void testBarsAndScaleSafety() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 9; // A
    s.scale = midiator::ScaleId::Phrygian;

    for (int bars : {1, 2, 4, 8, 16}) {
        s.bars = bars;
        const auto phrase = midiator::RiffEngine::generate(s, 12345u + static_cast<unsigned>(bars));
        require(phrase.bars == bars, "bar count must be preserved");
        require(phrase.usedSteps() == bars * 16, "4/4 must use 16 sixteenth steps per bar");

        bool foundNote = false;
        for (int i = 0; i < phrase.usedSteps(); ++i) {
            const auto& step = phrase.steps[i];
            for (int n = 0; n < step.noteCount; ++n) {
                foundNote = true;
                const auto& note = step.notes[n];
                require(note.velocity >= 1 && note.velocity <= 126, "velocity 127 is reserved and must not be generated");
                require(note.pitch >= 0 && note.pitch <= 127, "MIDI pitch must be valid");

                if (n == 0)
                    require(midiator::RiffEngine::isScaleTone(note.pitch, s.rootPitchClass, s.scale),
                            "primary riff notes must remain in the selected scale");
                else
                    require(note.pitch == step.notes[0].pitch + 7,
                            "second note in V1 power chord must be a perfect fifth");
            }
        }
        require(foundNote, "generated phrase must contain notes");
    }
}

void testDeterminism() {
    midiator::GeneratorSettings s{};
    const auto a = midiator::RiffEngine::generate(s, 0x12345678u);
    const auto b = midiator::RiffEngine::generate(s, 0x12345678u);
    require(a == b, "same seed and settings must produce identical riff");
}

void testVariationKeepsTonalFrame() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 9;
    s.scale = midiator::ScaleId::Phrygian;
    s.bars = 4;

    const auto source = midiator::RiffEngine::generate(s, 777u);
    const auto unchanged = midiator::RiffEngine::vary(source, s, 0.0f, 888u);
    require(source == unchanged, "0 percent variation must be bit-identical");

    const auto varied = midiator::RiffEngine::vary(source, s, 0.55f, 999u);
    require(varied.bars == source.bars, "variation must not change phrase length");

    for (int i = 0; i < varied.usedSteps(); ++i) {
        const auto& step = varied.steps[i];
        for (int n = 0; n < step.noteCount; ++n) {
            const auto& note = step.notes[n];
            require(note.velocity <= 126, "variation must never create velocity 127");
            if (n == 0)
                require(midiator::RiffEngine::isScaleTone(note.pitch, s.rootPitchClass, s.scale),
                        "variation must remain in selected root and scale");
            else
                require(note.pitch == step.notes[0].pitch + 7,
                        "variation power chord fifth must remain intact");
        }
    }
}

void testAllScalesGenerateValidTones() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 4; // E
    s.bars = 2;

    for (int scale = 0; scale < static_cast<int>(midiator::ScaleId::Count); ++scale) {
        s.scale = static_cast<midiator::ScaleId>(scale);
        const auto phrase = midiator::RiffEngine::generate(s, 1000u + static_cast<unsigned>(scale));
        for (int i = 0; i < phrase.usedSteps(); ++i) {
            if (phrase.steps[i].noteCount > 0)
                require(midiator::RiffEngine::isScaleTone(
                            phrase.steps[i].notes[0].pitch, s.rootPitchClass, s.scale),
                        "all supported scales must constrain primary pitches");
        }
    }
}


void testNoSamePitchOverlap() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 9;
    s.scale = midiator::ScaleId::Phrygian;
    s.bars = 8;
    s.density = 0.90f;
    s.repetition = 0.90f;

    const auto phrase = midiator::RiffEngine::generate(s, 424242u);

    for (int stepIndex = 0; stepIndex < phrase.usedSteps(); ++stepIndex) {
        const auto& step = phrase.steps[stepIndex];
        for (int noteIndex = 0; noteIndex < step.noteCount; ++noteIndex) {
            const auto& note = step.notes[noteIndex];
            require(stepIndex + note.lengthSteps <= phrase.usedSteps(),
                    "note must not extend beyond the looping phrase boundary");
            for (int futureStep = stepIndex + 1;
                 futureStep < std::min(phrase.usedSteps(), stepIndex + note.lengthSteps);
                 ++futureStep) {
                const auto& future = phrase.steps[futureStep];
                for (int futureNote = 0; futureNote < future.noteCount; ++futureNote)
                    require(future.notes[futureNote].pitch != note.pitch,
                            "same pitch must not retrigger before the previous note ends");
            }
        }
    }
}

void testStatisticalMusicalSanity() {
    using midiator::GeneratorSettings;
    using midiator::RiffEngine;
    using midiator::ScaleId;

    int totalPhrases = 0;
    int totalPrimaryNotes = 0;
    int totalRootNotes = 0;
    int totalOffbeatHits = 0;
    int totalHits = 0;
    int emptyPhrases = 0;

    for (int root = 0; root < 12; ++root) {
        for (int scale = 0; scale < static_cast<int>(ScaleId::Count); ++scale) {
            for (int seed = 1; seed <= 64; ++seed) {
                GeneratorSettings s{};
                s.rootPitchClass = root;
                s.scale = static_cast<ScaleId>(scale);
                s.bars = 4;

                const auto phrase = RiffEngine::generate(
                    s, static_cast<unsigned>(seed + root * 1000 + scale * 10000));
                ++totalPhrases;

                int phraseNotes = 0;
                for (int stepIndex = 0; stepIndex < phrase.usedSteps(); ++stepIndex) {
                    const auto& step = phrase.steps[stepIndex];
                    if (step.noteCount <= 0)
                        continue;

                    ++totalHits;
                    ++phraseNotes;

                    if ((stepIndex % 4) != 0)
                        ++totalOffbeatHits;

                    const auto& note = step.notes[0];
                    ++totalPrimaryNotes;

                    int pc = note.pitch % 12;
                    if (pc < 0) pc += 12;
                    if (pc == root)
                        ++totalRootNotes;
                }

                if (phraseNotes == 0)
                    ++emptyPhrases;
            }
        }
    }

    require(emptyPhrases == 0, "no generated phrase may be empty");
    require(totalPrimaryNotes > 0, "statistical suite must generate notes");

    const double rootRatio = static_cast<double>(totalRootNotes) /
                             static_cast<double>(totalPrimaryNotes);
    require(rootRatio >= 0.35 && rootRatio <= 0.90,
            "heavy-riff generator should strongly favor, but not exclusively use, the tonal root");

    const double offbeatRatio = static_cast<double>(totalOffbeatHits) /
                                static_cast<double>(totalHits);
    require(offbeatRatio >= 0.20,
            "generated riffs should contain meaningful offbeat/syncopated activity");
}

void testControlMonotonicity() {
    midiator::GeneratorSettings sparse{};
    sparse.bars = 8;
    sparse.density = 0.15f;

    midiator::GeneratorSettings dense = sparse;
    dense.density = 0.90f;

    long long sparseHits = 0;
    long long denseHits = 0;

    for (unsigned seed = 1; seed <= 128; ++seed) {
        const auto a = midiator::RiffEngine::generate(sparse, seed);
        const auto b = midiator::RiffEngine::generate(dense, seed);

        for (int i = 0; i < a.usedSteps(); ++i) {
            sparseHits += a.steps[i].noteCount > 0 ? 1 : 0;
            denseHits += b.steps[i].noteCount > 0 ? 1 : 0;
        }
    }

    require(denseHits > sparseHits * 1.35,
            "Density control must measurably increase note-event density");
}

void testVariationDistance() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 9;
    s.scale = midiator::ScaleId::Phrygian;
    s.bars = 8;

    for (unsigned seed = 1; seed <= 64; ++seed) {
        const auto source = midiator::RiffEngine::generate(s, 10000u + seed);
        const auto low = midiator::RiffEngine::vary(source, s, 0.20f, 20000u + seed);
        const auto high = midiator::RiffEngine::vary(source, s, 0.80f, 30000u + seed);

        int lowDiff = 0;
        int highDiff = 0;

        for (int i = 0; i < source.usedSteps(); ++i) {
            if (!(source.steps[i] == low.steps[i]))
                ++lowDiff;
            if (!(source.steps[i] == high.steps[i]))
                ++highDiff;
        }

        require(lowDiff < source.usedSteps() * 0.55,
                "20 percent variation must preserve most of the source phrase");
        require(highDiff >= lowDiff,
                "higher variation amount should not change fewer steps than low variation");
        require(highDiff < source.usedSteps() * 0.95,
                "even strong variation should preserve some phrase identity");
    }
}

void testVelocityZonesRemainSeparated() {
    midiator::GeneratorSettings s{};
    s.bars = 8;
    s.palmMuteChance = 0.75f;

    int muteZone = 0;
    int highZone = 0;

    for (unsigned seed = 1; seed <= 128; ++seed) {
        const auto phrase = midiator::RiffEngine::generate(s, 40000u + seed);

        for (int i = 0; i < phrase.usedSteps(); ++i) {
            const auto& step = phrase.steps[i];
            for (int n = 0; n < step.noteCount; ++n) {
                const int v = step.notes[n].velocity;
                require(v >= 1 && v <= 126, "generated velocity must stay in safe MIDI range");
                require(!(v >= 41 && v <= 87),
                        "V1 should keep a visible gap between palm-mute and open-note velocity zones");
                if (v < 88)
                    require(v >= 30 && v <= 40,
                            "normal palm mutes must stay between velocity 30 and 40");
                if (v <= 40) ++muteZone;
                if (v >= 88) ++highZone;
            }
        }
    }

    require(muteZone > 0, "generator must produce mute-like velocity notes");
    require(highZone > 0, "generator must produce open/sustain-like velocity notes");
}


void testDefaultVariationIsAudiblyStructural() {
    midiator::GeneratorSettings s{};
    s.bars = 2;

    long long totalDiff = 0;
    int weakCases = 0;
    constexpr int samples = 128;

    for (unsigned seed = 1; seed <= samples; ++seed) {
        const auto source = midiator::RiffEngine::generate(s, 70000u + seed);
        const auto varied = midiator::RiffEngine::vary(source, s, 0.35f, 80000u + seed);
        const int diff = structuralDifference(source, varied);
        totalDiff += diff;
        if (diff < 4)
            ++weakCases;
    }

    const double averageDiff = static_cast<double>(totalDiff) / samples;
    require(averageDiff >= 6.0,
            "default 35 percent variation must change an audible number of structural steps");
    require(weakCases <= samples / 8,
            "default variation must rarely collapse into an almost inaudible change");
}

void testPowerChordAmountIsReliable() {
    using midiator::GeneratorSettings;
    using midiator::RiffEngine;
    using midiator::StyleId;

    auto countChords = [](const midiator::Phrase& phrase) {
        int count = 0;
        for (int i = 0; i < phrase.usedSteps(); ++i)
            count += phrase.steps[i].noteCount > 1 ? 1 : 0;
        return count;
    };

    for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
        GeneratorSettings off{};
        off.bars = 2;
        off.style = static_cast<StyleId>(style);
        off.powerChordChance = 1.0f;
        off.powerChordsEnabled = false;

        GeneratorSettings normal = off;
        normal.powerChordsEnabled = true;
        normal.powerChordChance = 0.25f;

        GeneratorSettings high = off;
        high.powerChordsEnabled = true;
        high.powerChordChance = 0.80f;

        for (unsigned seed = 1; seed <= 64; ++seed) {
            const auto none = RiffEngine::generate(off, 150000u + seed);
            const auto some = RiffEngine::generate(normal, 150000u + seed);
            const auto many = RiffEngine::generate(high, 150000u + seed);

            require(countChords(none) == 0,
                    "0 percent Power Chords must generate no dyads");
            require(countChords(some) >= 1,
                    "default 25 percent Power Chords must produce at least one dyad");
            require(countChords(many) >= countChords(some),
                    "higher Power Chords amount must not produce fewer dyads");
        }
    }
}

void testVariationRespectsPowerChordsOff() {
    midiator::GeneratorSettings s{};
    s.bars = 4;
    s.powerChordsEnabled = false;
    s.powerChordChance = 1.0f;

    for (unsigned seed = 1; seed <= 128; ++seed) {
        const auto source = midiator::RiffEngine::generate(s, 210000u + seed);
        const auto varied = midiator::RiffEngine::vary(source, s, 0.80f, 220000u + seed);

        for (int i = 0; i < varied.usedSteps(); ++i)
            require(varied.steps[i].noteCount <= 1,
                    "VARIATION must never create power-chord dyads while Power Chords is OFF");
    }
}

void testFastSixteenthBurstsExist() {
    using midiator::GeneratorSettings;
    using midiator::RiffEngine;
    using midiator::StyleId;

    auto hasRun = [](const midiator::Phrase& phrase, int minimum) {
        int run = 0;
        for (int i = 0; i < phrase.usedSteps(); ++i) {
            if (phrase.steps[i].noteCount > 0) {
                ++run;
                if (run >= minimum)
                    return true;
            } else {
                run = 0;
            }
        }
        return false;
    };

    int ndhFast = 0;
    int darkFast = 0;
    int heavyFast = 0;
    constexpr int samples = 256;

    for (int seed = 1; seed <= samples; ++seed) {
        GeneratorSettings s{};
        s.bars = 2;
        s.density = 0.56f;
        s.complexity = 0.42f;

        s.style = StyleId::NDHIndustrial;
        ndhFast += hasRun(RiffEngine::generate(s, 120000u + seed), 4) ? 1 : 0;

        s.style = StyleId::DarkRockGothic;
        darkFast += hasRun(RiffEngine::generate(s, 120000u + seed), 4) ? 1 : 0;

        s.style = StyleId::HeavyIndustrial;
        heavyFast += hasRun(RiffEngine::generate(s, 120000u + seed), 4) ? 1 : 0;
    }

    require(ndhFast >= samples / 8,
            "NDH/Industrial must sometimes generate audible four-sixteenth runs");
    require(heavyFast >= samples / 4,
            "Heavy Industrial must frequently generate audible four-sixteenth runs");
    require(heavyFast > darkFast,
            "Heavy Industrial must create fast sixteenth runs more often than Dark Rock/Gothic");
}

void testStyleEnginesHaveDistinctRhythmLanguages() {
    using midiator::GeneratorSettings;
    using midiator::RiffEngine;
    using midiator::StyleId;

    long long ndhHits = 0, darkHits = 0, heavyHits = 0;
    long long ndhOff16 = 0, darkOff16 = 0, heavyOff16 = 0;
    constexpr unsigned samples = 192;

    for (unsigned seed = 1; seed <= samples; ++seed) {
        GeneratorSettings ndh{};
        ndh.bars = 2;
        ndh.style = StyleId::NDHIndustrial;
        GeneratorSettings dark = ndh;
        dark.style = StyleId::DarkRockGothic;
        GeneratorSettings heavy = ndh;
        heavy.style = StyleId::HeavyIndustrial;

        const auto a = RiffEngine::generate(ndh, 90000u + seed);
        const auto b = RiffEngine::generate(dark, 90000u + seed);
        const auto c = RiffEngine::generate(heavy, 90000u + seed);

        for (int i = 0; i < a.usedSteps(); ++i) {
            const int pos = i % 16;
            if (a.steps[i].noteCount > 0) {
                ++ndhHits;
                if ((pos % 2) == 1) ++ndhOff16;
            }
            if (b.steps[i].noteCount > 0) {
                ++darkHits;
                if ((pos % 2) == 1) ++darkOff16;
            }
            if (c.steps[i].noteCount > 0) {
                ++heavyHits;
                if ((pos % 2) == 1) ++heavyOff16;
            }
        }
    }

    require(darkHits < ndhHits,
            "Dark Rock/Gothic must leave more rhythmic space than NDH/Industrial");
    require(heavyOff16 > ndhOff16 && heavyOff16 > darkOff16,
            "Heavy Industrial must create the strongest displaced sixteenth-note activity");
    require(heavyHits > darkHits,
            "Heavy Industrial must be materially denser than Dark Rock/Gothic at equal controls");
}

void testFourBarRoleDevelopment() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 9;
    s.scale = midiator::ScaleId::Phrygian;
    s.bars = 4;
    s.repetition = 0.72f;

    int identicalAdjacentBars = 0;
    int phraseCount = 0;

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto p = midiator::RiffEngine::generate(s, 50000u + seed);
        ++phraseCount;

        for (int bar = 1; bar < 4; ++bar) {
            bool identical = true;
            for (int step = 0; step < 16; ++step) {
                if (!(p.steps[(bar - 1) * 16 + step] == p.steps[bar * 16 + step])) {
                    identical = false;
                    break;
                }
            }
            identicalAdjacentBars += identical ? 1 : 0;
        }
    }

    require(identicalAdjacentBars == 0,
            "default four-bar generation should not create byte-identical adjacent bars");
    require(phraseCount == 256, "four-bar role test must execute all samples");
}


void testEightBarMacroDevelopmentStaysMusical() {
    midiator::GeneratorSettings s{};
    s.bars = 8;
    s.rootPitchClass = 9;
    s.scale = midiator::ScaleId::Phrygian;
    s.style = midiator::StyleId::NDHIndustrial;
    s.density = 0.56f;
    s.complexity = 0.42f;
    s.repetition = 0.72f;

    auto jaccard = [](const midiator::Phrase& p, int a, int b) {
        int intersection = 0;
        int unionCount = 0;
        for (int step = 0; step < midiator::kStepsPerBar; ++step) {
            const bool ah =
                p.steps[a * midiator::kStepsPerBar + step].noteCount > 0;
            const bool bh =
                p.steps[b * midiator::kStepsPerBar + step].noteCount > 0;
            if (ah || bh) ++unionCount;
            if (ah && bh) ++intersection;
        }
        return unionCount > 0
            ? static_cast<double>(intersection) / unionCount : 1.0;
    };

    double adjacent = 0.0;
    double sectionMirror = 0.0;
    int adjacentPairs = 0;
    int sectionPairs = 0;

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto p = midiator::RiffEngine::generate(s, 970000u + seed);
        for (int bar = 1; bar < 8; ++bar) {
            adjacent += jaccard(p, bar - 1, bar);
            ++adjacentPairs;
        }
        for (int bar = 0; bar < 4; ++bar) {
            sectionMirror += jaccard(p, bar, bar + 4);
            ++sectionPairs;
        }
    }

    const double adjacentMean = adjacent / adjacentPairs;
    const double sectionMean = sectionMirror / sectionPairs;

    require(adjacentMean >= 0.68 && adjacentMean <= 0.82,
            "default 8-bar phrase must stay recognizable without becoming static");
    require(sectionMean >= 0.48 && sectionMean <= 0.72,
            "bars 5-8 must develop the first four bars without becoming unrelated");
}

void testRepetitionControlBehavior() {
    midiator::GeneratorSettings low{};
    low.bars = 4;
    low.repetition = 0.0f;

    midiator::GeneratorSettings high = low;
    high.repetition = 1.0f;

    long long lowRoot = 0, highRoot = 0;
    long long lowNotes = 0, highNotes = 0;
    long long lowDistinctTotal = 0, highDistinctTotal = 0;
    double lowJaccard = 0.0, highJaccard = 0.0;
    int lowPairs = 0, highPairs = 0;

    auto accumulate = [](const midiator::Phrase& p, int root,
                         long long& roots, long long& notes,
                         long long& distinctTotal, double& jaccard, int& pairs) {
        bool pcs[12] = {};
        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0) continue;
            ++notes;
            const int pc = (st.notes[0].pitch % 12 + 12) % 12;
            pcs[pc] = true;
            if (pc == root) ++roots;
        }
        int distinct = 0;
        for (bool used : pcs) distinct += used ? 1 : 0;
        distinctTotal += distinct;

        for (int bar = 1; bar < p.bars; ++bar) {
            int intersection = 0, unionCount = 0;
            for (int i = 0; i < midiator::kStepsPerBar; ++i) {
                const bool a = p.steps[(bar - 1) * midiator::kStepsPerBar + i].noteCount > 0;
                const bool b = p.steps[bar * midiator::kStepsPerBar + i].noteCount > 0;
                if (a || b) ++unionCount;
                if (a && b) ++intersection;
            }
            jaccard += unionCount > 0 ? static_cast<double>(intersection) / unionCount : 1.0;
            ++pairs;
        }
    };

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto a = midiator::RiffEngine::generate(low, 300000u + seed);
        const auto b = midiator::RiffEngine::generate(high, 300000u + seed);
        accumulate(a, low.rootPitchClass, lowRoot, lowNotes, lowDistinctTotal, lowJaccard, lowPairs);
        accumulate(b, high.rootPitchClass, highRoot, highNotes, highDistinctTotal, highJaccard, highPairs);
    }

    const double lowRootShare = static_cast<double>(lowRoot) / std::max<long long>(1, lowNotes);
    const double highRootShare = static_cast<double>(highRoot) / std::max<long long>(1, highNotes);
    require(highRootShare > lowRootShare + 0.12,
            "Repetition must materially increase pedal/root-note focus");
    require(highDistinctTotal < lowDistinctTotal,
            "Repetition must reduce average pitch-class variety");
    require((highJaccard / highPairs) > (lowJaccard / lowPairs) + 0.10,
            "Repetition must materially increase adjacent-bar groove similarity");

    const std::array<float, 5> values{{0.0f, 0.25f, 0.50f, 0.75f, 1.0f}};
    std::array<double, 5> jaccard{};
    for (size_t vi = 0; vi < values.size(); ++vi) {
        midiator::GeneratorSettings x = low;
        x.repetition = values[vi];
        double sum = 0.0;
        int pairs = 0;
        for (unsigned seed = 1; seed <= 256; ++seed) {
            const auto p = midiator::RiffEngine::generate(
                x, 360000u + seed + static_cast<unsigned>(vi) * 10000u);
            for (int bar = 1; bar < p.bars; ++bar) {
                int intersection = 0, unionCount = 0;
                for (int i = 0; i < midiator::kStepsPerBar; ++i) {
                    const bool a =
                        p.steps[(bar - 1) * midiator::kStepsPerBar + i].noteCount > 0;
                    const bool b =
                        p.steps[bar * midiator::kStepsPerBar + i].noteCount > 0;
                    if (a || b) ++unionCount;
                    if (a && b) ++intersection;
                }
                sum += unionCount > 0
                    ? static_cast<double>(intersection) / unionCount : 1.0;
                ++pairs;
            }
        }
        jaccard[vi] = sum / std::max(1, pairs);
    }

    std::cerr << "Repetition Jaccard 0/25/50/75/100 = "
              << jaccard[0] << ", " << jaccard[1] << ", "
              << jaccard[2] << ", " << jaccard[3] << ", "
              << jaccard[4] << "\n";

    require(jaccard[0] < 0.72,
            "Repetition 0% must create materially different adjacent bars");
    require(jaccard[3] < 0.88,
            "Repetition 75% must still retain audible bar development");
    require(jaccard[4] > 0.95,
            "Repetition 100% must behave as a near-exact repeated motif");
    require(jaccard[4] > jaccard[0] + 0.25,
            "Repetition control must span a broad audible similarity range");
}

void testComplexityControlBehavior() {
    midiator::GeneratorSettings low{};
    low.bars = 4;
    low.complexity = 0.0f;

    midiator::GeneratorSettings high = low;
    high.complexity = 1.0f;

    long long lowJump = 0, highJump = 0;
    long long lowJumpCount = 0, highJumpCount = 0;
    int lowFast = 0, highFast = 0;

    auto measure = [](const midiator::Phrase& p,
                      long long& jumpSum, long long& jumpCount, int& fastCount) {
        int previous = -1;
        int run = 0;
        bool fast = false;
        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0) {
                run = 0;
                continue;
            }
            ++run;
            if (run >= 4) fast = true;
            const int pitch = st.notes[0].pitch;
            if (previous >= 0) {
                jumpSum += std::abs(pitch - previous);
                ++jumpCount;
            }
            previous = pitch;
        }
        if (fast) ++fastCount;
    };

    for (unsigned seed = 1; seed <= 256; ++seed) {
        measure(midiator::RiffEngine::generate(low, 310000u + seed),
                lowJump, lowJumpCount, lowFast);
        measure(midiator::RiffEngine::generate(high, 310000u + seed),
                highJump, highJumpCount, highFast);
    }

    const double lowAvgJump = static_cast<double>(lowJump) / std::max<long long>(1, lowJumpCount);
    const double highAvgJump = static_cast<double>(highJump) / std::max<long long>(1, highJumpCount);
    require(highAvgJump > lowAvgJump + 0.70,
            "Complexity must materially increase registral/pitch movement");
    require(highFast > lowFast,
            "Complexity must increase fast/busy riff activity");
}

} // namespace

int main() {
    testBarsAndScaleSafety();
    testDeterminism();
    testVariationKeepsTonalFrame();
    testAllScalesGenerateValidTones();
    testNoSamePitchOverlap();
    testStatisticalMusicalSanity();
    testControlMonotonicity();
    testVariationDistance();
    testVelocityZonesRemainSeparated();
    testDefaultVariationIsAudiblyStructural();
    testPowerChordAmountIsReliable();
    testVariationRespectsPowerChordsOff();
    testFastSixteenthBurstsExist();
    testStyleEnginesHaveDistinctRhythmLanguages();
    testFourBarRoleDevelopment();
    testEightBarMacroDevelopmentStaysMusical();
    testRepetitionControlBehavior();
    testComplexityControlBehavior();

    std::cout << "Midiator core tests: PASS\n";
    return 0;
}
