#include "PadBrain.h"
#include "RiffEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace midiator;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void makeContext(Phrase& guitar, Phrase& bass) {
    GeneratorSettings g{};
    g.bars = 4;
    guitar = RiffEngine::generate(g, 0x50414401u);
    bass = guitar;
    for (int i = 0; i < bass.usedSteps(); ++i) {
        if (bass.steps[i].noteCount <= 0)
            continue;
        bass.steps[i].noteCount = 1;
        while (bass.steps[i].notes[0].pitch > 48)
            bass.steps[i].notes[0].pitch -= 12;
    }
}

void testDeterministicPolyphonicScaleSafe() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);
    PadSettings s{};
    const auto a = PadBrain::generate(guitar, bass, s, 1234u);
    const auto b = PadBrain::generate(guitar, bass, s, 1234u);

    require(a.bars == b.bars, "Pad Brain must be deterministic");
    int chords = 0;
    for (int i = 0; i < a.usedSteps(); ++i) {
        require(a.steps[i].noteCount == b.steps[i].noteCount,
                "Pad deterministic chord topology must match");
        if (a.steps[i].noteCount <= 0)
            continue;
        ++chords;
        require(a.steps[i].noteCount >= 3 && a.steps[i].noteCount <= 4,
                "Pad chord must use 3-4 voices");
        for (int n = 0; n < a.steps[i].noteCount; ++n) {
            const auto& x = a.steps[i].notes[n];
            const auto& y = b.steps[i].notes[n];
            require(x.pitch == y.pitch && x.velocity == y.velocity &&
                    x.lengthSteps == y.lengthSteps,
                    "Pad deterministic notes must match");
            require(x.pitch >= 45 && x.pitch <= 88,
                    "Pad voice must stay in musical pad register");
            require(RiffEngine::isScaleTone(x.pitch, s.rootPitchClass, s.scale),
                    "Pad voices must remain scale-safe");
        }
    }
    require(chords >= 2, "Pad Brain must generate harmonic events");
}

void testDarkRockUsesRicherVoicings() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    PadSettings ndh{};
    ndh.style = StyleId::NDHIndustrial;
    PadSettings dark = ndh;
    dark.style = StyleId::DarkRockGothic;

    const auto a = PadBrain::generate(guitar, bass, ndh, 77u);
    const auto b = PadBrain::generate(guitar, bass, dark, 77u);

    long long voicesA = 0, chordsA = 0, voicesB = 0, chordsB = 0;
    for (int i = 0; i < a.usedSteps(); ++i) {
        if (a.steps[i].noteCount > 0) { voicesA += a.steps[i].noteCount; ++chordsA; }
        if (b.steps[i].noteCount > 0) { voicesB += b.steps[i].noteCount; ++chordsB; }
    }

    require(chordsA > 0 && chordsB > 0, "Pad style test needs chords");
    require(static_cast<double>(voicesB) / chordsB >
            static_cast<double>(voicesA) / chordsA + 0.5,
            "Dark Rock/Gothic pads must use richer voicings than NDH");
}

void testMovementIncreasesHarmonicActivity() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    PadSettings low{};
    low.movement = 0.0f;
    PadSettings high = low;
    high.movement = 1.0f;

    auto countChords = [&](const PadSettings& s) {
        const auto p = PadBrain::generate(guitar, bass, s, 99u);
        int n = 0;
        for (int i = 0; i < p.usedSteps(); ++i)
            if (p.steps[i].noteCount > 0)
                ++n;
        return n;
    };

    require(countChords(high) > countChords(low),
            "Pad Movement must increase harmonic event rate");
}

void testVoiceLeadingAvoidsWildJumps() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);
    PadSettings s{};
    s.style = StyleId::DarkRockGothic;
    s.movement = 1.0f;
    const auto p = PadBrain::generate(guitar, bass, s, 101u);

    std::array<int, kMaxPadVoices> previous{{-1,-1,-1,-1}};
    for (int i = 0; i < p.usedSteps(); ++i) {
        if (p.steps[i].noteCount <= 0)
            continue;
        for (int v = 0; v < p.steps[i].noteCount; ++v) {
            const int pitch = p.steps[i].notes[v].pitch;
            if (previous[v] >= 0)
                require(std::abs(pitch - previous[v]) <= 12,
                        "Pad voice-leading must avoid jumps larger than an octave");
            previous[v] = pitch;
        }
    }
}

} // namespace

int main() {
    testDeterministicPolyphonicScaleSafe();
    testDarkRockUsesRicherVoicings();
    testMovementIncreasesHarmonicActivity();
    testVoiceLeadingAvoidsWildJumps();
    std::cout << "Midiator Pad Brain tests: PASS\n";
    return 0;
}
