#include "../src/BassBrain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace midiator;

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

Phrase makeGuitarFixture() {
    GeneratorSettings s{};
    s.bars = 4;
    s.rootPitchClass = 9;
    s.scale = ScaleId::Phrygian;
    s.style = StyleId::NDHIndustrial;
    s.density = 0.56f;
    s.complexity = 0.42f;
    s.repetition = 0.72f;
    return RiffEngine::generate(s, 0xB455F17u);
}

void testDeterministicAndMonophonic() {
    const auto guitar = makeGuitarFixture();
    BassSettings s{};
    const auto a = BassBrain::generate(guitar, s, 12345u);
    const auto b = BassBrain::generate(guitar, s, 12345u);
    require(a == b, "Bass Brain must be deterministic for fixed settings and seed");

    for (int i = 0; i < a.usedSteps(); ++i) {
        const auto& st = a.steps[i];
        require(st.noteCount <= 1, "Bass Brain must remain monophonic");
        if (st.noteCount == 0) continue;
        require(st.notes[0].pitch >= 24 && st.notes[0].pitch <= 60,
                "Bass notes must stay in the defined bass register");
        require(RiffEngine::isScaleTone(st.notes[0].pitch, s.rootPitchClass, s.scale),
                "Bass primary notes must stay in the selected scale");
    }
}

void testFollowControlsGuitarLock() {
    const auto guitar = makeGuitarFixture();
    BassSettings low{};
    low.follow = 0.0f;
    BassSettings high = low;
    high.follow = 1.0f;

    long long lowCoincidence = 0, highCoincidence = 0;
    long long lowHits = 0, highHits = 0;

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto a = BassBrain::generate(guitar, low, 10000u + seed);
        const auto b = BassBrain::generate(guitar, high, 10000u + seed);
        for (int i = 0; i < guitar.usedSteps(); ++i) {
            const bool gh = guitar.steps[i].noteCount > 0;
            if (a.steps[i].noteCount > 0) {
                ++lowHits;
                if (gh) ++lowCoincidence;
            }
            if (b.steps[i].noteCount > 0) {
                ++highHits;
                if (gh) ++highCoincidence;
            }
        }
    }

    const double lowShare = static_cast<double>(lowCoincidence) / std::max<long long>(1, lowHits);
    const double highShare = static_cast<double>(highCoincidence) / std::max<long long>(1, highHits);
    require(highShare > lowShare + 0.20,
            "Bass Follow must materially increase lock to guitar onsets");
}

void testMovementReducesRootDominance() {
    const auto guitar = makeGuitarFixture();
    BassSettings low{};
    low.movement = 0.0f;
    BassSettings high = low;
    high.movement = 1.0f;

    long long lowRoot = 0, highRoot = 0;
    long long lowNotes = 0, highNotes = 0;

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto a = BassBrain::generate(guitar, low, 20000u + seed);
        const auto b = BassBrain::generate(guitar, high, 20000u + seed);
        for (int i = 0; i < guitar.usedSteps(); ++i) {
            if (a.steps[i].noteCount > 0) {
                ++lowNotes;
                if ((a.steps[i].notes[0].pitch % 12) == low.rootPitchClass) ++lowRoot;
            }
            if (b.steps[i].noteCount > 0) {
                ++highNotes;
                if ((b.steps[i].notes[0].pitch % 12) == high.rootPitchClass) ++highRoot;
            }
        }
    }

    const double lowShare = static_cast<double>(lowRoot) / std::max<long long>(1, lowNotes);
    const double highShare = static_cast<double>(highRoot) / std::max<long long>(1, highNotes);
    require(lowShare > highShare + 0.10,
            "Bass Movement must reduce pedal-root dominance");
}

void testNoOverlapAndDownbeatAnchor() {
    const auto guitar = makeGuitarFixture();
    BassSettings s{};
    s.sustain = 1.0f;
    const auto bass = BassBrain::generate(guitar, s, 777u);

    require(bass.steps[0].noteCount == 1,
            "Bass phrase must always contain a downbeat anchor");

    for (int i = 0; i < bass.usedSteps(); ++i) {
        if (bass.steps[i].noteCount == 0) continue;
        const auto& n = bass.steps[i].notes[0];
        for (int j = i + 1; j < std::min(bass.usedSteps(), i + n.lengthSteps); ++j)
            require(bass.steps[j].noteCount == 0,
                    "Bass notes must not overlap later bass onsets");
    }
}

} // namespace

int main() {
    testDeterministicAndMonophonic();
    testFollowControlsGuitarLock();
    testMovementReducesRootDominance();
    testNoOverlapAndDownbeatAnchor();

    std::cout << "Midiator Bass Brain tests: PASS\n";
    return 0;
}
