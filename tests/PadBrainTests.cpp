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

void testSpreadProgressivelyWidensVoicings() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    auto averageSpan = [&](float spread) {
        double span = 0.0;
        long long chords = 0;
        for (unsigned seed = 1; seed <= 256; ++seed) {
            PadSettings s{};
            s.spread = spread;
            const auto p = PadBrain::generate(guitar, bass, s, 60000u + seed);
            for (int i = 0; i < p.usedSteps(); ++i) {
                const auto& st = p.steps[i];
                if (st.noteCount <= 0) continue;
                int lo = 127, hi = 0;
                for (int n = 0; n < st.noteCount; ++n) {
                    lo = std::min(lo, st.notes[n].pitch);
                    hi = std::max(hi, st.notes[n].pitch);
                }
                span += hi - lo;
                ++chords;
            }
        }
        return span / std::max<long long>(1, chords);
    };

    const auto low = averageSpan(0.0f);
    const auto mid = averageSpan(0.5f);
    const auto high = averageSpan(1.0f);
    require(mid > low + 2.0,
            "Pad Spread 50% must be audibly wider than 0%");
    require(high > mid + 2.0,
            "Pad Spread 100% must be audibly wider than 50%");
}

void testMovementProgressivelyAddsHarmonicEvents() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    auto averageChords = [&](float movement) {
        double chords = 0.0;
        for (unsigned seed = 1; seed <= 256; ++seed) {
            PadSettings s{};
            s.movement = movement;
            const auto p = PadBrain::generate(guitar, bass, s, 70000u + seed);
            for (int i = 0; i < p.usedSteps(); ++i)
                if (p.steps[i].noteCount > 0)
                    chords += 1.0;
        }
        return chords / 256.0;
    };

    const auto low = averageChords(0.0f);
    const auto mid = averageChords(0.5f);
    const auto high = averageChords(1.0f);
    require(mid > low + 0.5,
            "Pad Movement 50% must add harmonic activity over 0%");
    require(high > mid + 0.5,
            "Pad Movement 100% must add harmonic activity over 50%");
}

void testTensionProgressivelyAddsColorVoices() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    auto changedTopVoices = [&](float tension) {
        long long changed = 0;
        long long compared = 0;
        for (unsigned seed = 1; seed <= 256; ++seed) {
            PadSettings base{};
            base.tension = 0.0f;
            PadSettings test = base;
            test.tension = tension;

            const auto a = PadBrain::generate(guitar, bass, base, 91000u + seed);
            const auto b = PadBrain::generate(guitar, bass, test, 91000u + seed);

            for (int i = 0; i < a.usedSteps(); ++i) {
                if (a.steps[i].noteCount <= 0 || b.steps[i].noteCount <= 0)
                    continue;
                const auto& an = a.steps[i].notes[a.steps[i].noteCount - 1];
                const auto& bn = b.steps[i].notes[b.steps[i].noteCount - 1];
                ++compared;
                if ((an.pitch % 12 + 12) % 12 != (bn.pitch % 12 + 12) % 12)
                    ++changed;
            }
        }
        return compared > 0 ? static_cast<double>(changed) / compared : 0.0;
    };

    const double low = changedTopVoices(0.25f);
    const double mid = changedTopVoices(0.50f);
    const double high = changedTopVoices(1.0f);

    require(mid > low + 0.05,
            "Pad Tension 50% must add more color-voice changes than 25%");
    require(high > mid + 0.10,
            "Pad Tension 100% must add materially more color-voice changes than 50%");
}

void testContextFollowAlignsPadsWithRiff() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    auto contextToneShare = [&](float follow) {
        long long compared = 0;
        long long matching = 0;

        for (unsigned seed = 1; seed <= 256; ++seed) {
            PadSettings s{};
            s.contextFollow = follow;
            const auto pads = PadBrain::generate(guitar, bass, s, 120000u + seed);

            int activeChord = -1;
            for (int i = 0; i < pads.usedSteps(); ++i) {
                if (pads.steps[i].noteCount > 0)
                    activeChord = i;
                if (activeChord < 0)
                    continue;

                auto matchesChord = [&](int pitch) {
                    const int pc = (pitch % 12 + 12) % 12;
                    const auto& chord = pads.steps[activeChord];
                    for (int n = 0; n < chord.noteCount; ++n)
                        if (((chord.notes[n].pitch % 12 + 12) % 12) == pc)
                            return true;
                    return false;
                };

                if (i < guitar.usedSteps() && guitar.steps[i].noteCount > 0) {
                    ++compared;
                    if (matchesChord(guitar.steps[i].notes[0].pitch))
                        ++matching;
                }
                if (i < bass.usedSteps() && bass.steps[i].noteCount > 0) {
                    ++compared;
                    if (matchesChord(bass.steps[i].notes[0].pitch))
                        ++matching;
                }
            }
        }

        return compared > 0 ? static_cast<double>(matching) / compared : 0.0;
    };

    const double independent = contextToneShare(0.0f);
    const double guided = contextToneShare(1.0f);
    require(guided > independent + 0.10,
            "Pad Context Follow must materially improve Guitar/Bass chord-tone alignment");
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
    testSpreadProgressivelyWidensVoicings();
    testMovementProgressivelyAddsHarmonicEvents();
    testTensionProgressivelyAddsColorVoices();
    testContextFollowAlignsPadsWithRiff();
    testVoiceLeadingAvoidsWildJumps();
    std::cout << "Midiator Pad Brain tests: PASS\n";
    return 0;
}
