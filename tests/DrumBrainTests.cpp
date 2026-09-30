#include "../src/DrumBrain.h"
#include "../src/BassBrain.h"

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

void makeContext(Phrase& guitar, Phrase& bass) {
    GeneratorSettings gs{};
    gs.bars = 4;
    gs.style = StyleId::NDHIndustrial;
    guitar = RiffEngine::generate(gs, 0xD12A0001u);
    BassSettings bs{};
    bass = BassBrain::generate(guitar, bs, 0xD12A0002u);
}

void testDeterministicAndBounded() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    DrumSettings s{};
    const auto a = DrumBrain::generate(guitar, bass, s, 1234u);
    const auto b = DrumBrain::generate(guitar, bass, s, 1234u);

    require(a.bars == b.bars, "Drum Brain bars must be deterministic");
    for (int i = 0; i < a.usedSteps(); ++i) {
        require(a.steps[i].hitCount == b.steps[i].hitCount,
                "Drum Brain hit counts must be deterministic");
        require(a.steps[i].hitCount <= kMaxDrumHitsPerStep,
                "Drum step must stay within fixed realtime hit capacity");
        for (int h = 0; h < a.steps[i].hitCount; ++h) {
            require(a.steps[i].hits[h].voice == b.steps[i].hits[h].voice,
                    "Drum voices must be deterministic");
            require(a.steps[i].hits[h].velocity == b.steps[i].hits[h].velocity,
                    "Drum velocities must be deterministic");
            require(a.steps[i].hits[h].velocity >= 1 && a.steps[i].hits[h].velocity <= 126,
                    "Drum velocity must stay in MIDI-safe range");
        }
    }
}

void testBackbeatAndDownbeatCrash() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);
    DrumSettings s{};
    s.crashOnDownbeat = true;
    const auto d = DrumBrain::generate(guitar, bass, s, 99u);

    for (int bar = 0; bar < d.bars; ++bar) {
        bool crash = false;
        for (int h = 0; h < d.steps[bar * kStepsPerBar].hitCount; ++h)
            crash |= d.steps[bar * kStepsPerBar].hits[h].voice == DrumVoice::Crash;
        require(crash, "Drum phrase must punctuate each bar downbeat with crash when enabled");

        for (int local : {4, 12}) {
            bool snare = false;
            const auto& st = d.steps[bar * kStepsPerBar + local];
            for (int h = 0; h < st.hitCount; ++h)
                snare |= st.hits[h].voice == DrumVoice::Snare;
            require(snare, "Drum Brain must maintain stable 2/4 backbeat");
        }
    }
}

void testFollowControlsKickLock() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    DrumSettings low{};
    low.follow = 0.0f;
    DrumSettings high = low;
    high.follow = 1.0f;

    long long lowKick = 0, highKick = 0;
    long long lowLocked = 0, highLocked = 0;

    auto contextHit = [](const Phrase& p, int step) {
        return step >= 0 && step < p.usedSteps() && p.steps[step].noteCount > 0;
    };

    auto measure = [&](const DrumPhrase& d, long long& kicks, long long& locked) {
        for (int i = 0; i < d.usedSteps(); ++i) {
            bool kick = false;
            for (int h = 0; h < d.steps[i].hitCount; ++h)
                kick |= d.steps[i].hits[h].voice == DrumVoice::Kick;
            if (!kick) continue;
            ++kicks;
            if (contextHit(guitar, i) || contextHit(bass, i))
                ++locked;
        }
    };

    for (unsigned seed = 1; seed <= 256; ++seed) {
        const auto a = DrumBrain::generate(guitar, bass, low, 50000u + seed);
        const auto b = DrumBrain::generate(guitar, bass, high, 50000u + seed);
        measure(a, lowKick, lowLocked);
        measure(b, highKick, highLocked);
    }

    const double lowShare = static_cast<double>(lowLocked) / std::max<long long>(1, lowKick);
    const double highShare = static_cast<double>(highLocked) / std::max<long long>(1, highKick);
    require(highShare > lowShare + 0.15,
            "Drum Follow must materially increase kick lock to guitar/bass context");
}

void testMappingLayerIndependentOfComposition() {
    const auto gm = DrumMidiMap::preset(DrumMapId::GeneralMidi);
    const auto ez = DrumMidiMap::preset(DrumMapId::EZdrummer3);
    require(gm.midiNote(DrumVoice::Kick) == 36, "GM kick mapping must be C1/36");
    require(gm.midiNote(DrumVoice::Snare) == 38, "GM snare mapping must be D1/38");
    require(ez.midiNote(DrumVoice::Kick) == gm.midiNote(DrumVoice::Kick),
            "initial Toontrack core mapping must preserve principal kick note");
}

} // namespace

int main() {
    testDeterministicAndBounded();
    testBackbeatAndDownbeatCrash();
    testFollowControlsKickLock();
    testMappingLayerIndependentOfComposition();

    std::cout << "Midiator Drum Brain tests: PASS\n";
    return 0;
}
