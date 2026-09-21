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

    for (int bars : {1, 2, 4, 8}) {
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

    int lowZone = 0;
    int highZone = 0;

    for (unsigned seed = 1; seed <= 128; ++seed) {
        const auto phrase = midiator::RiffEngine::generate(s, 40000u + seed);

        for (int i = 0; i < phrase.usedSteps(); ++i) {
            const auto& step = phrase.steps[i];
            for (int n = 0; n < step.noteCount; ++n) {
                const int v = step.notes[n].velocity;
                require(v >= 1 && v <= 126, "generated velocity must stay in safe MIDI range");
                require(!(v >= 73 && v <= 87),
                        "V1 should keep a visible gap between mute-like and open-like velocity zones");
                if (v <= 72) ++lowZone;
                if (v >= 88) ++highZone;
            }
        }
    }

    require(lowZone > 0, "generator must produce mute-like velocity notes");
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
    testFastSixteenthBurstsExist();
    testStyleEnginesHaveDistinctRhythmLanguages();
    testFourBarRoleDevelopment();

    std::cout << "Midiator core tests: PASS\n";
    return 0;
}
