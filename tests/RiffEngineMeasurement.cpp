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
                      << (n.velocity <= 72 ? "P" : "O")
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
            if (n.velocity <= 72) ++muteLike;
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
            for (int sidx = 0; sidx < 16; ++sidx) {
                const auto& a = p.steps[(bar - 1) * 16 + sidx];
                const auto& b = p.steps[bar * 16 + sidx];
                if (a == b) {
                    ++sameSteps;
                } else {
                    identical = false;
                }
            }
            ++comparedBarPairs;
            if (sameSteps >= 10) ++repeatedBarPairs;
            if (identical) ++completelyIdenticalBarPairs;
        }
    }

    GeneratorSettings lowVarSettings = s;
    const auto base = RiffEngine::generate(lowVarSettings, 123456u);
    const auto var20 = RiffEngine::vary(base, lowVarSettings, 0.20f, 123457u);
    const auto var50 = RiffEngine::vary(base, lowVarSettings, 0.50f, 123458u);
    const auto var80 = RiffEngine::vary(base, lowVarSettings, 0.80f, 123459u);

    auto pct = [](double x) { return x * 100.0; };

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
    std::cout << "Phrases containing >= quarter-note rest: "
              << pct(static_cast<double>(phrasesWithLongRest) / phrases) << "%\n";
    std::cout << "Phrases containing >= 8 consecutive hit steps: "
              << pct(static_cast<double>(phrasesWithLongHitRun) / phrases) << "%\n";
    std::cout << "Variation changed steps (20%): " << countDiffSteps(base, var20) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (50%): " << countDiffSteps(base, var50) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (80%): " << countDiffSteps(base, var80) << "/" << base.usedSteps() << "\n";

    GeneratorSettings exampleSettings = s;
    exampleSettings.bars = 4;
    printPhraseGrid(RiffEngine::generate(exampleSettings, 101u), "Example riff A - A Phrygian");
    printPhraseGrid(RiffEngine::generate(exampleSettings, 202u), "Example riff B - A Phrygian");
    printPhraseGrid(RiffEngine::generate(exampleSettings, 303u), "Example riff C - A Phrygian");

    return 0;
}
