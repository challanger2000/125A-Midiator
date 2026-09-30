#include "RiffEngine.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

using namespace midiator;

static const char* pitchClassName(int pc) {
    static const char* names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    pc %= 12;
    if (pc < 0) pc += 12;
    return names[pc];
}

static void printPhraseGrid(const Phrase& p, const char* title) {
    std::cout << "\n" << title << "\n";
    std::cout << "Legend: P=palm/mute-like, O=open-like, 5=power chord\n";

    for (int bar = 0; bar < p.bars; ++bar) {
        std::cout << "Bar " << (bar + 1) << ": ";
        for (int i = 0; i < 16; ++i) {
            const auto& step = p.steps[bar * 16 + i];
            if (step.noteCount <= 0) {
                std::cout << "[----]";
                continue;
            }

            const auto& n = step.notes[0];
            const int octave = (n.pitch / 12) - 1;
            std::cout << "[" << pitchClassName(n.pitch) << octave
                      << (n.velocity <= 40 ? "P" : "O")
                      << (step.noteCount == 2 ? "5" : " ")
                      << "]";
        }
        std::cout << "\n";
    }
}

static int countHits(const Phrase& p) {
    int hits = 0;
    for (int i = 0; i < p.usedSteps(); ++i)
        hits += p.steps[i].noteCount > 0 ? 1 : 0;
    return hits;
}

static int countDiffSteps(const Phrase& a, const Phrase& b) {
    int d = 0;
    const int n = std::min(a.usedSteps(), b.usedSteps());
    for (int i = 0; i < n; ++i)
        if (!(a.steps[i] == b.steps[i]))
            ++d;
    return d + std::abs(a.usedSteps() - b.usedSteps());
}

static double onsetJaccard(const Phrase& a, const Phrase& b) {
    const int n = std::min(a.usedSteps(), b.usedSteps());
    int intersection = 0;
    int unionCount = 0;
    for (int i = 0; i < n; ++i) {
        const bool ha = a.steps[i].noteCount > 0;
        const bool hb = b.steps[i].noteCount > 0;
        if (ha || hb) ++unionCount;
        if (ha && hb) ++intersection;
    }
    return unionCount > 0
        ? static_cast<double>(intersection) / static_cast<double>(unionCount)
        : 1.0;
}

static int longestHitRun(const Phrase& p) {
    int longest = 0;
    int current = 0;
    for (int i = 0; i < p.usedSteps(); ++i) {
        if (p.steps[i].noteCount > 0) {
            ++current;
            longest = std::max(longest, current);
        } else {
            current = 0;
        }
    }
    return longest;
}


struct SweepMetrics {
    double hits = 0.0;
    double rootShare = 0.0;
    double muteShare = 0.0;
    double chordShare = 0.0;
    double longNoteShare = 0.0;
    double distinctPitchClasses = 0.0;
    double avgAbsJump = 0.0;
    double adjacentBarJaccard = 0.0;
    double fast4Share = 0.0;
    double longRestShare = 0.0;
};

static SweepMetrics measureSettings(const GeneratorSettings& settings,
                                    unsigned seedBase,
                                    int samples = 256) {
    SweepMetrics m{};
    long long totalHits = 0;
    long long rootNotes = 0;
    long long muteNotes = 0;
    long long chordHits = 0;
    long long longNotes = 0;
    long long jumpCount = 0;
    long long jumpSum = 0;
    long long distinctPcTotal = 0;
    long long barPairs = 0;
    double barJaccardSum = 0.0;
    int fast4Phrases = 0;
    int longRestPhrases = 0;

    for (int sidx = 0; sidx < samples; ++sidx) {
        const auto p = RiffEngine::generate(settings, seedBase + static_cast<unsigned>(sidx));
        bool pcs[12] = {};
        int previousPitch = -1;
        int run = 0;
        int longestRun = 0;
        int rest = 0;
        int longestRest = 0;

        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0) {
                run = 0;
                ++rest;
                longestRest = std::max(longestRest, rest);
                continue;
            }

            rest = 0;
            ++run;
            longestRun = std::max(longestRun, run);
            ++totalHits;
            if (st.noteCount > 1) ++chordHits;

            const auto& n = st.notes[0];
            const int pc = (n.pitch % 12 + 12) % 12;
            pcs[pc] = true;
            if (pc == settings.rootPitchClass) ++rootNotes;
            if (n.velocity <= 40) ++muteNotes;
            if (n.lengthSteps > 1) ++longNotes;

            if (previousPitch >= 0) {
                jumpSum += std::abs(n.pitch - previousPitch);
                ++jumpCount;
            }
            previousPitch = n.pitch;
        }

        if (longestRun >= 4) ++fast4Phrases;
        if (longestRest >= 4) ++longRestPhrases;

        int distinct = 0;
        for (bool used : pcs) distinct += used ? 1 : 0;
        distinctPcTotal += distinct;

        for (int bar = 1; bar < p.bars; ++bar) {
            int intersection = 0;
            int unionCount = 0;
            for (int i = 0; i < kStepsPerBar; ++i) {
                const bool a = p.steps[(bar - 1) * kStepsPerBar + i].noteCount > 0;
                const bool b = p.steps[bar * kStepsPerBar + i].noteCount > 0;
                if (a || b) ++unionCount;
                if (a && b) ++intersection;
            }
            barJaccardSum += unionCount > 0
                ? static_cast<double>(intersection) / unionCount : 1.0;
            ++barPairs;
        }
    }

    const double phraseCount = static_cast<double>(samples);
    const double hitCount = std::max(1.0, static_cast<double>(totalHits));
    m.hits = totalHits / phraseCount;
    m.rootShare = rootNotes / hitCount;
    m.muteShare = muteNotes / hitCount;
    m.chordShare = chordHits / hitCount;
    m.longNoteShare = longNotes / hitCount;
    m.distinctPitchClasses = distinctPcTotal / phraseCount;
    m.avgAbsJump = jumpCount > 0 ? static_cast<double>(jumpSum) / jumpCount : 0.0;
    m.adjacentBarJaccard = barPairs > 0 ? barJaccardSum / barPairs : 1.0;
    m.fast4Share = static_cast<double>(fast4Phrases) / phraseCount;
    m.longRestShare = static_cast<double>(longRestPhrases) / phraseCount;
    return m;
}

static void printSweepLine(const char* label, double value, const SweepMetrics& m) {
    std::cout << label << " " << std::setw(5) << value * 100.0 << "%:"
              << " hits=" << m.hits
              << " root=" << m.rootShare * 100.0 << "%"
              << " mute=" << m.muteShare * 100.0 << "%"
              << " chords=" << m.chordShare * 100.0 << "%"
              << " longNotes=" << m.longNoteShare * 100.0 << "%"
              << " pitchClasses=" << m.distinctPitchClasses
              << " avgJump=" << m.avgAbsJump
              << " barJaccard=" << m.adjacentBarJaccard * 100.0 << "%"
              << " fast4=" << m.fast4Share * 100.0 << "%"
              << " longRest=" << m.longRestShare * 100.0 << "%"
              << "\n";
}

int main() {
    std::cout << "125A Midiator measurement report\n";
    std::cout << "================================\n";

    GeneratorSettings s{};
    s.rootPitchClass = 9;
    s.scale = ScaleId::Phrygian;
    s.bars = 4;

    long long phrases = 0;
    long long hits = 0;
    long long primaryNotes = 0;
    long long rootNotes = 0;
    long long offbeats = 0;
    long long powerChords = 0;
    long long muteLike = 0;
    long long openLike = 0;
    long long vel127 = 0;
    long long repeatedBarPairs = 0;
    long long comparedBarPairs = 0;
    long long completelyIdenticalBarPairs = 0;
    double onsetJaccardSum = 0.0;
    long long sharedOnsets = 0;
    long long sharedPitchMatches = 0;
    long long phrasesWithLongRest = 0;
    long long phrasesWithLongHitRun = 0;
    long long distinctPrimaryPitchClassesTotal = 0;

    for (unsigned seed = 1; seed <= 1000; ++seed) {
        const auto p = RiffEngine::generate(s, seed);
        ++phrases;
        int longestRest = 0;
        int currentRest = 0;
        int longestHitRun = 0;
        int currentHitRun = 0;
        bool pcs[12] = {};

        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0) {
                ++currentRest;
                longestRest = std::max(longestRest, currentRest);
                currentHitRun = 0;
                continue;
            }

            currentRest = 0;
            ++currentHitRun;
            longestHitRun = std::max(longestHitRun, currentHitRun);
            ++hits;
            if ((i % 4) != 0) ++offbeats;
            if (st.noteCount == 2) ++powerChords;

            const auto& n = st.notes[0];
            ++primaryNotes;
            pcs[(n.pitch % 12 + 12) % 12] = true;
            if ((n.pitch % 12 + 12) % 12 == s.rootPitchClass) ++rootNotes;
            if (n.velocity <= 40) ++muteLike;
            if (n.velocity >= 88) ++openLike;
            if (n.velocity == 127) ++vel127;
        }

        if (longestRest >= 4) ++phrasesWithLongRest;
        if (longestHitRun >= 8) ++phrasesWithLongHitRun;

        int distinctPcs = 0;
        for (bool usedPc : pcs) distinctPcs += usedPc ? 1 : 0;
        distinctPrimaryPitchClassesTotal += distinctPcs;

        for (int bar = 1; bar < p.bars; ++bar) {
            int sameSteps = 0;
            bool identical = true;
            int onsetIntersection = 0;
            int onsetUnion = 0;

            for (int sidx = 0; sidx < 16; ++sidx) {
                const auto& a = p.steps[(bar - 1) * 16 + sidx];
                const auto& b = p.steps[bar * 16 + sidx];

                if (a == b) {
                    ++sameSteps;
                } else {
                    identical = false;
                }

                const bool hitA = a.noteCount > 0;
                const bool hitB = b.noteCount > 0;
                if (hitA || hitB)
                    ++onsetUnion;
                if (hitA && hitB) {
                    ++onsetIntersection;
                    ++sharedOnsets;
                    if (a.notes[0].pitch == b.notes[0].pitch)
                        ++sharedPitchMatches;
                }
            }

            ++comparedBarPairs;
            if (sameSteps >= 10) ++repeatedBarPairs;
            if (identical) ++completelyIdenticalBarPairs;

            onsetJaccardSum += onsetUnion > 0
                ? static_cast<double>(onsetIntersection) / static_cast<double>(onsetUnion)
                : 1.0;
        }
    }


    auto pct = [](double x) { return x * 100.0; };

    std::cout << "\nStyle / NEW-RIFF diversity diagnostics\n";
    std::cout << "--------------------------------------\n";
    const char* styleNames[] = {"NDH / Industrial", "Dark Rock / Gothic", "Heavy Industrial"};
    for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
        GeneratorSettings ds = s;
        ds.bars = 2;
        ds.style = static_cast<StyleId>(style);

        double rawJaccard = 0.0;
        double rawDiff = 0.0;
        int fast4 = 0;
        int fast6 = 0;
        int chordless = 0;
        constexpr int pairs = 512;

        for (int i = 0; i < pairs; ++i) {
            const auto a = RiffEngine::generate(ds, 200000u + static_cast<unsigned>(i * 2));
            const auto b = RiffEngine::generate(ds, 200001u + static_cast<unsigned>(i * 2));
            rawJaccard += onsetJaccard(a, b);
            rawDiff += countDiffSteps(a, b);

            const int run = longestHitRun(a);
            if (run >= 4) ++fast4;
            if (run >= 6) ++fast6;

            bool hasChord = false;
            for (int step = 0; step < a.usedSteps(); ++step)
                hasChord = hasChord || a.steps[step].noteCount > 1;
            if (!hasChord) ++chordless;
        }

        std::cout << styleNames[style] << ": avg independent-riff onset Jaccard "
                  << pct(rawJaccard / pairs)
                  << "%, avg changed steps "
                  << (rawDiff / pairs) << "/32"
                  << ", >=4x16 run " << pct(static_cast<double>(fast4) / pairs)
                  << "%, >=6x16 run " << pct(static_cast<double>(fast6) / pairs)
                  << "%, chordless " << pct(static_cast<double>(chordless) / pairs)
                  << "%\n";
    }

    GeneratorSettings lowVarSettings = s;
    const auto base = RiffEngine::generate(lowVarSettings, 123456u);
    const auto var20 = RiffEngine::vary(base, lowVarSettings, 0.20f, 123457u);
    const auto var50 = RiffEngine::vary(base, lowVarSettings, 0.50f, 123458u);
    const auto var80 = RiffEngine::vary(base, lowVarSettings, 0.80f, 123459u);

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "Phrases sampled: " << phrases << "\n";
    std::cout << "Average hits / 4 bars: " << static_cast<double>(hits) / phrases << "\n";
    std::cout << "Root-note share: " << pct(static_cast<double>(rootNotes) / primaryNotes) << "%\n";
    std::cout << "Offbeat-hit share: " << pct(static_cast<double>(offbeats) / hits) << "%\n";
    std::cout << "Power-chord hit share: " << pct(static_cast<double>(powerChords) / hits) << "%\n";
    std::cout << "Mute-like primary-note share: " << pct(static_cast<double>(muteLike) / primaryNotes) << "%\n";
    std::cout << "Open-like primary-note share: " << pct(static_cast<double>(openLike) / primaryNotes) << "%\n";
    std::cout << "Velocity-127 count: " << vel127 << "\n";
    std::cout << "Average distinct primary pitch classes / phrase: "
              << static_cast<double>(distinctPrimaryPitchClassesTotal) / phrases << "\n";
    std::cout << "Adjacent-bar recognizable similarity (>=10/16 same steps): "
              << pct(static_cast<double>(repeatedBarPairs) / comparedBarPairs) << "%\n";
    std::cout << "Completely identical adjacent bars: "
              << pct(static_cast<double>(completelyIdenticalBarPairs) / comparedBarPairs) << "%\n";
    std::cout << "Adjacent-bar onset Jaccard similarity: "
              << pct(onsetJaccardSum / comparedBarPairs) << "%\n";
    std::cout << "Pitch identity on shared onsets: "
              << (sharedOnsets > 0
                    ? pct(static_cast<double>(sharedPitchMatches) / sharedOnsets)
                    : 0.0)
              << "%\n";
    std::cout << "Phrases containing >= quarter-note rest: "
              << pct(static_cast<double>(phrasesWithLongRest) / phrases) << "%\n";
    std::cout << "Phrases containing >= 8 consecutive hit steps: "
              << pct(static_cast<double>(phrasesWithLongHitRun) / phrases) << "%\n";
    std::cout << "Variation changed steps (20%): " << countDiffSteps(base, var20) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (50%): " << countDiffSteps(base, var50) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (80%): " << countDiffSteps(base, var80) << "/" << base.usedSteps() << "\n";


    std::cout << "\nVariation diagnostics over 512 phrases\n";
    std::cout << "--------------------------------------\n";
    for (float amount : {0.20f, 0.35f, 0.50f, 0.80f}) {
        double diffSum = 0.0;
        double jacSum = 0.0;
        constexpr int samples = 512;
        for (int i = 0; i < samples; ++i) {
            const auto src = RiffEngine::generate(s, 300000u + static_cast<unsigned>(i));
            const auto dst = RiffEngine::vary(src, s, amount, 400000u + static_cast<unsigned>(i));
            diffSum += countDiffSteps(src, dst);
            jacSum += onsetJaccard(src, dst);
        }
        std::cout << "Variation " << pct(amount) << "%: avg changed steps "
                  << diffSum / samples << "/" << s.bars * 16
                  << ", onset Jaccard " << pct(jacSum / samples) << "%\n";
    }


    std::cout << "\nControl sweep diagnostics (256 phrases per point)\n";
    std::cout << "------------------------------------------------\n";
    const float sweepValues[] = {0.0f, 0.25f, 0.50f, 0.75f, 1.0f};

    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.density = v;
        printSweepLine("Density   ", v, measureSettings(x, 500000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.complexity = v;
        printSweepLine("Complexity", v, measureSettings(x, 510000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.repetition = v;
        printSweepLine("Repetition", v, measureSettings(x, 520000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.palmMuteChance = v;
        printSweepLine("Palm Mute ", v, measureSettings(x, 530000u + static_cast<unsigned>(v * 1000.0f)));
    }
    std::cout << "\n";
    for (float v : sweepValues) {
        GeneratorSettings x = s;
        x.powerChordChance = v;
        x.powerChordsEnabled = true;
        printSweepLine("PowerChord", v, measureSettings(x, 540000u + static_cast<unsigned>(v * 1000.0f)));
    }

    std::cout << "\nStyle detail diagnostics (512 phrases each)\n";
    std::cout << "-------------------------------------------\n";
    for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
        GeneratorSettings x = s;
        x.style = static_cast<StyleId>(style);
        const auto m = measureSettings(x, 550000u + static_cast<unsigned>(style * 10000), 512);
        printSweepLine(styleNames[style], 0.0, m);
    }

    GeneratorSettings exampleSettings = s;
    exampleSettings.bars = 4;
    printPhraseGrid(RiffEngine::generate(exampleSettings, 101u), "Example riff A - A Phrygian");
    printPhraseGrid(RiffEngine::generate(exampleSettings, 202u), "Example riff B - A Phrygian");
    printPhraseGrid(RiffEngine::generate(exampleSettings, 303u), "Example riff C - A Phrygian");

    return 0;
}
