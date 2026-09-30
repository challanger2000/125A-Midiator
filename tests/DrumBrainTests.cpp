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


void testDensityAndComplexityHaveDistinctMaterialEffects() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    auto measure = [&](float density, float complexity) {
        long long hits = 0;
        long long toms = 0;
        long long ghost = 0;
        long long oddHats = 0;

        for (unsigned seed = 1; seed <= 256; ++seed) {
            DrumSettings s{};
            s.density = density;
            s.complexity = complexity;
            const auto d = DrumBrain::generate(guitar, bass, s, 65000u + seed);

            for (int i = 0; i < d.usedSteps(); ++i) {
                const int local = i % kStepsPerBar;
                for (int h = 0; h < d.steps[i].hitCount; ++h) {
                    ++hits;
                    const auto voice = d.steps[i].hits[h].voice;
                    if (voice == DrumVoice::LowTom ||
                        voice == DrumVoice::MidTom ||
                        voice == DrumVoice::HighTom)
                        ++toms;
                    if (voice == DrumVoice::GhostSnare)
                        ++ghost;
                    if ((voice == DrumVoice::ClosedHat || voice == DrumVoice::OpenHat) &&
                        (local % 2) != 0)
                        ++oddHats;
                }
            }
        }
        return std::array<long long,4>{hits,toms,ghost,oddHats};
    };

    const auto densityLow = measure(0.0f, 0.30f);
    const auto densityHigh = measure(1.0f, 0.30f);
    require(densityHigh[0] > densityLow[0] + 2000,
            "Drum Density must materially increase total activity");

    const auto complexityLow = measure(0.48f, 0.0f);
    const auto complexityHigh = measure(0.48f, 1.0f);
    require(complexityHigh[1] > complexityLow[1] + 300,
            "Drum Complexity must materially increase tom movement");
    require(complexityHigh[2] > complexityLow[2] + 200,
            "Drum Complexity must materially increase ghost-note detail");
    require(complexityHigh[3] > complexityLow[3] + 600,
            "Drum Complexity must materially increase 16th-hat motion");
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

void testStylesHaveDistinctDrumLanguages() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    struct Metrics {
        long long kicks = 0;
        long long hats16 = 0;
        long long openHats = 0;
        long long toms = 0;
    };

    auto measure = [&](StyleId style) {
        Metrics m{};
        DrumSettings s{};
        s.style = style;
        s.complexity = 0.65f;
        s.density = 0.60f;
        for (unsigned seed = 1; seed <= 256; ++seed) {
            const auto d = DrumBrain::generate(guitar, bass, s, 90000u + seed);
            for (int i = 0; i < d.usedSteps(); ++i) {
                const int local = i % kStepsPerBar;
                for (int h = 0; h < d.steps[i].hitCount; ++h) {
                    switch (d.steps[i].hits[h].voice) {
                        case DrumVoice::Kick:
                            ++m.kicks;
                            break;
                        case DrumVoice::ClosedHat:
                            if ((local % 2) != 0) ++m.hats16;
                            break;
                        case DrumVoice::OpenHat:
                            ++m.openHats;
                            if ((local % 2) != 0) ++m.hats16;
                            break;
                        case DrumVoice::LowTom:
                        case DrumVoice::MidTom:
                        case DrumVoice::HighTom:
                            ++m.toms;
                            break;
                        default:
                            break;
                    }
                }
            }
        }
        return m;
    };

    const auto ndh = measure(StyleId::NDHIndustrial);
    const auto dark = measure(StyleId::DarkRockGothic);
    const auto heavy = measure(StyleId::HeavyIndustrial);

    require(heavy.kicks > dark.kicks + 400,
            "Heavy Industrial drums must use materially more kick pressure than Dark Rock");
    require(heavy.hats16 > ndh.hats16,
            "Heavy Industrial drums must use more 16th-hat pressure than NDH");
    require(dark.openHats > ndh.openHats,
            "Dark Rock drums must breathe more through open hats than NDH");
    require(dark.toms > ndh.toms,
            "Dark Rock drums must permit more tom movement than NDH");
}

void testVerifiedMapsNeverEmitSamePitchTwicePerStep() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    const DrumMapId maps[] = {
        DrumMapId::GeneralMidi,
        DrumMapId::EZdrummer3,
        DrumMapId::PerfectDrums
    };

    for (auto mapId : maps) {
        const auto map = DrumMidiMap::preset(mapId);
        for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
            for (unsigned seed = 1; seed <= 1024; ++seed) {
                DrumSettings s{};
                s.style = static_cast<StyleId>(style);
                s.complexity = 1.0f;
                s.density = 1.0f;

                const auto d = DrumBrain::generate(
                    guitar, bass, s,
                    120000u + seed + static_cast<unsigned>(style) * 5000u);

                for (int i = 0; i < d.usedSteps(); ++i) {
                    bool usedPitch[128]{};
                    const auto& step = d.steps[i];
                    for (int h = 0; h < step.hitCount; ++h) {
                        const int pitch = map.midiNote(step.hits[h].voice);
                        require(pitch >= 0 && pitch < 128,
                                "verified drum map must stay inside MIDI pitch range");
                        require(!usedPitch[pitch],
                                "verified drum map must never emit the same MIDI pitch twice on one step");
                        usedPitch[pitch] = true;
                    }
                }
            }
        }
    }
}

void testMappingLayerIndependentOfComposition() {
    const auto gm = DrumMidiMap::preset(DrumMapId::GeneralMidi);
    require(gm.midiNote(DrumVoice::Kick) == 36, "GM kick mapping must be 36");
    require(gm.midiNote(DrumVoice::Snare) == 38, "GM snare mapping must be 38");
    require(gm.midiNote(DrumVoice::ClosedHat) == 42, "GM closed hat must be 42");
    require(gm.midiNote(DrumVoice::OpenHat) == 46, "GM open hat must be 46");
    require(gm.midiNote(DrumVoice::Crash) == 49, "GM crash must be 49");
    require(gm.midiNote(DrumVoice::Ride) == 51, "GM ride must be 51");
    require(gm.midiNote(DrumVoice::GhostSnare) == 38,
            "GM ghost snare must use acoustic snare pitch with lower velocity");
    require(DrumMidiMap::presetIsVerified(DrumMapId::GeneralMidi),
            "General MIDI preset must be marked verified");

    const auto ez = DrumMidiMap::preset(DrumMapId::EZdrummer3);
    require(ez.midiNote(DrumVoice::Kick) == 36, "EZD3 standard kick must be 36");
    require(ez.midiNote(DrumVoice::Snare) == 38, "EZD3 standard snare center must be 38");
    require(ez.midiNote(DrumVoice::ClosedHat) == 42, "EZD3 closed-tip hat must be 42");
    require(ez.midiNote(DrumVoice::OpenHat) == 46, "EZD3 open-edge hat must be 46");
    require(ez.midiNote(DrumVoice::Crash) == 55, "EZD3 Crash 1 must be 55");
    require(ez.midiNote(DrumVoice::Ride) == 52, "EZD3 Ride Edge must be 52");
    require(ez.midiNote(DrumVoice::LowTom) == 41, "EZD3 floor tom must be 41");
    require(ez.midiNote(DrumVoice::MidTom) == 47, "EZD3 rack tom 2 must be 47");
    require(ez.midiNote(DrumVoice::HighTom) == 48, "EZD3 rack tom 1 must be 48");
    require(ez.midiNote(DrumVoice::GhostSnare) == 38,
            "EZD3 ghost snare must retain Snare Center and use velocity");
    require(DrumMidiMap::presetIsVerified(DrumMapId::EZdrummer3),
            "EZD3 standard preset must be marked verified");

    const auto pd = DrumMidiMap::preset(DrumMapId::PerfectDrums);
    require(pd.midiNote(DrumVoice::Kick) == 36, "Perfect Drums kick center must be 36");
    require(pd.midiNote(DrumVoice::Snare) == 38, "Perfect Drums snare center must be 38");
    require(pd.midiNote(DrumVoice::ClosedHat) == 64,
            "Perfect Drums closed-tip hi-hat must be E3/64");
    require(pd.midiNote(DrumVoice::OpenHat) == 47,
            "Perfect Drums open hi-hat must be B1/47");
    require(pd.midiNote(DrumVoice::Crash) == 50,
            "Perfect Drums Crash 1 edge must be D2/50");
    require(pd.midiNote(DrumVoice::Ride) == 52,
            "Perfect Drums Ride tip must be E2/52");
    require(pd.midiNote(DrumVoice::LowTom) == 41, "Perfect Drums Tom 3 must be F1/41");
    require(pd.midiNote(DrumVoice::MidTom) == 43, "Perfect Drums Tom 2 must be G1/43");
    require(pd.midiNote(DrumVoice::HighTom) == 45, "Perfect Drums Tom 1 must be A1/45");
    require(pd.midiNote(DrumVoice::GhostSnare) == 38,
            "Perfect Drums ghost snare must use Snare Center with lower velocity");
    require(DrumMidiMap::presetIsVerified(DrumMapId::PerfectDrums),
            "Perfect Drums default preset must be marked verified");

    require(!DrumMidiMap::presetIsVerified(DrumMapId::SuperiorDrummer3),
            "SD3 must not be claimed as a universal verified static map");
    require(!DrumMidiMap::presetIsVerified(DrumMapId::SSD55),
            "SSD5.5 must not be claimed as a universal verified static map");
}

} // namespace

int main() {
    testDeterministicAndBounded();
    testBackbeatAndStructuralCrash();
    testFollowControlsKickLock();
    testDensityAndComplexityHaveDistinctMaterialEffects();
    testHumanizeChangesVelocityNotPattern();
    testStylesHaveDistinctDrumLanguages();
    testVerifiedMapsNeverEmitSamePitchTwicePerStep();
    testMappingLayerIndependentOfComposition();

    std::cout << "Midiator Drum Brain tests: PASS\n";
    return 0;
}
