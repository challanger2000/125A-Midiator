#include "../src/DrumBrain.h"
#include "../src/BassBrain.h"

#include <cstdlib>
#include <iostream>
#include <cmath>
#include <algorithm>

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

void testBackbeatAndStructuralCrash() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);
    DrumSettings s{};
    s.crashOnDownbeat = true;
    const auto d = DrumBrain::generate(guitar, bass, s, 99u);

    bool openingCrash = false;
    for (int h = 0; h < d.steps[0].hitCount; ++h)
        openingCrash |= d.steps[0].hits[h].voice == DrumVoice::Crash;
    require(openingCrash, "Drum phrase must punctuate phrase opening with crash when enabled");

    int crashCount = 0;
    for (int bar = 0; bar < d.bars; ++bar) {
        const auto& down = d.steps[bar * kStepsPerBar];
        for (int h = 0; h < down.hitCount; ++h)
            crashCount += down.hits[h].voice == DrumVoice::Crash ? 1 : 0;

        for (int local : {4, 12}) {
            bool snare = false;
            const auto& st = d.steps[bar * kStepsPerBar + local];
            for (int h = 0; h < st.hitCount; ++h)
                snare |= st.hits[h].voice == DrumVoice::Snare;
            require(snare, "Drum Brain must maintain stable 2/4 backbeat");
        }
    }

    require(crashCount < d.bars,
            "Crash must remain structural punctuation rather than hit every bar");
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

void testHumanizeChangesVelocityNotPattern() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    DrumSettings dry{};
    dry.humanize = 0.0f;
    DrumSettings human = dry;
    human.humanize = 1.0f;

    long long comparedHits = 0;
    long long changedVelocities = 0;
    long long absoluteVelocityDelta = 0;

    for (unsigned seed = 1; seed <= 128; ++seed) {
        const auto a = DrumBrain::generate(guitar, bass, dry, 80000u + seed);
        const auto b = DrumBrain::generate(guitar, bass, human, 80000u + seed);

        require(a.bars == b.bars, "Humanize must not change drum phrase length");
        for (int i = 0; i < a.usedSteps(); ++i) {
            require(a.steps[i].hitCount == b.steps[i].hitCount,
                    "Humanize must not change drum hit topology");
            for (int h = 0; h < a.steps[i].hitCount; ++h) {
                require(a.steps[i].hits[h].voice == b.steps[i].hits[h].voice,
                        "Humanize must not change drum voices");
                ++comparedHits;
                const int delta = std::abs(
                    a.steps[i].hits[h].velocity - b.steps[i].hits[h].velocity);
                absoluteVelocityDelta += delta;
                if (delta > 0)
                    ++changedVelocities;
            }
        }
    }

    require(comparedHits > 0, "Humanize test must compare generated drum hits");
    require(changedVelocities > comparedHits / 2,
            "High Humanize must alter velocities on most drum hits");
    require(static_cast<double>(absoluteVelocityDelta) / comparedHits > 2.0,
            "High Humanize must create a musically material velocity deviation");
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
    testBackbeatAndStructuralCrash();
    testFollowControlsKickLock();
    testHumanizeChangesVelocityNotPattern();
    testMappingLayerIndependentOfComposition();

    std::cout << "Midiator Drum Brain tests: PASS\n";
    return 0;
}
