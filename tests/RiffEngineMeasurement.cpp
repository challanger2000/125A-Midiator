#include "RiffEngine.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

using namespace midiator;

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

    for (unsigned seed = 1; seed <= 1000; ++seed) {
        const auto p = RiffEngine::generate(s, seed);
        ++phrases;
        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0) continue;
            ++hits;
            if ((i % 4) != 0) ++offbeats;
            if (st.noteCount == 2) ++powerChords;

            const auto& n = st.notes[0];
            ++primaryNotes;
            if ((n.pitch % 12 + 12) % 12 == s.rootPitchClass) ++rootNotes;
            if (n.velocity <= 72) ++muteLike;
            if (n.velocity >= 88) ++openLike;
            if (n.velocity == 127) ++vel127;
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
    std::cout << "Variation changed steps (20%): " << countDiffSteps(base, var20) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (50%): " << countDiffSteps(base, var50) << "/" << base.usedSteps() << "\n";
    std::cout << "Variation changed steps (80%): " << countDiffSteps(base, var80) << "/" << base.usedSteps() << "\n";

    return 0;
}
