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
        require(a.steps[i].noteCount >= 2 && a.steps[i].noteCount <= 4,
                "Pad chord must use 2-3 harmonic tones plus optional octave doubling");
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

void testAllStylesStayWithinTwoOrThreeHarmonicPitchClasses() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    for (int style = 0; style < static_cast<int>(StyleId::Count); ++style) {
        for (unsigned seed = 1; seed <= 512; ++seed) {
            PadSettings s{};
            s.style = static_cast<StyleId>(style);
            const auto p = PadBrain::generate(guitar, bass, s, 40000u + seed);

            for (int i = 0; i < p.usedSteps(); ++i) {
                const auto& st = p.steps[i];
                if (st.noteCount <= 0)
                    continue;

                bool pcSeen[12]{};
                int unique = 0;
                for (int n = 0; n < st.noteCount; ++n) {
                    const int pc = (st.notes[n].pitch % 12 + 12) % 12;
                    if (!pcSeen[pc]) {
                        pcSeen[pc] = true;
                        ++unique;
                    }
                }

                require(unique >= 2 && unique <= 3,
                        "every Pad style must stay within 2-3 distinct harmonic pitch classes");
            }
        }
    }
}

void testFourthPadVoiceIsOctaveDoubleOnly() {
    Phrase guitar{}, bass{};
    makeContext(guitar, bass);

    int fourVoiceChords = 0;
    for (unsigned seed = 1; seed <= 1024; ++seed) {
        PadSettings s{};
        s.style = StyleId::DarkRockGothic;
        s.spread = 1.0f;
        const auto p = PadBrain::generate(guitar, bass, s, 50000u + seed);

        for (int i = 0; i < p.usedSteps(); ++i) {
            const auto& st = p.steps[i];
            if (st.noteCount <= 0)
                continue;

            bool seen[12]{};
            int unique = 0;
            for (int n = 0; n < st.noteCount; ++n) {
                const int pc = (st.notes[n].pitch % 12 + 12) % 12;
                if (!seen[pc]) {
                    seen[pc] = true;
                    ++unique;
                }
            }
            require(unique >= 2 && unique <= 3,
                    "Pad harmony must contain only 2-3 distinct pitch classes");

            if (st.noteCount == 4) {
                ++fourVoiceChords;
                bool foundExactOctavePair = false;
                for (int a = 0; a < 4; ++a)
                    for (int b = a + 1; b < 4; ++b)
                        if (std::abs(st.notes[a].pitch - st.notes[b].pitch) == 12)
                            foundExactOctavePair = true;
                require(foundExactOctavePair,
                        "a fourth Pad voice must be an exact octave doubling");
            }
        }
    }
    require(fourVoiceChords > 0,
            "Pad octave-doubling fixture must observe occasional fourth voices");
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

    auto changedHarmony = [&](float tension) {
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

                unsigned maskA = 0, maskB = 0;
                for (int n = 0; n < a.steps[i].noteCount; ++n)
                    maskA |= 1u << ((a.steps[i].notes[n].pitch % 12 + 12) % 12);
                for (int n = 0; n < b.steps[i].noteCount; ++n)
                    maskB |= 1u << ((b.steps[i].notes[n].pitch % 12 + 12) % 12);

                ++compared;
                if (maskA != maskB)
                    ++changed;
            }
        }
        return compared > 0 ? static_cast<double>(changed) / compared : 0.0;
    };

    const double low = changedHarmony(0.25f);
    const double mid = changedHarmony(0.50f);
    const double high = changedHarmony(1.0f);

    require(mid > low + 0.05,
            "Pad Tension 50% must alter harmony more than 25%");
    require(high > mid + 0.10,
            "Pad Tension 100% must alter harmony materially more than 50%");
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
    s.spread = 1.0f;
    const auto p = PadBrain::generate(guitar, bass, s, 101u);

    std::array<int, 3> previousUnique{{-1,-1,-1}};
    int previousCount = 0;

    for (int i = 0; i < p.usedSteps(); ++i) {
        const auto& step = p.steps[i];
        if (step.noteCount <= 0)
            continue;

        // Collapse an optional octave-doubled fourth voice to its harmonic
        // pitch class. Voice-leading must be judged by the actual 2-3
        // harmonic tones, not by sorted array indices.
        std::array<int, 3> currentUnique{{-1,-1,-1}};
        int currentCount = 0;
        bool seenPc[12]{};

        for (int n = 0; n < step.noteCount; ++n) {
            const int pitch = step.notes[n].pitch;
            const int pc = (pitch % 12 + 12) % 12;
            if (seenPc[pc])
                continue;
            seenPc[pc] = true;
            require(currentCount < 3,
                    "Pad harmony must have at most three unique pitch classes");
            currentUnique[currentCount++] = pitch;
        }

        if (previousCount > 0) {
            for (int c = 0; c < currentCount; ++c) {
                int bestDistance = 999;
                for (int prev = 0; prev < previousCount; ++prev)
                    bestDistance = std::min(
                        bestDistance,
                        std::abs(currentUnique[c] - previousUnique[prev]));
                require(bestDistance <= 12,
                        "Pad voice-leading must keep every harmonic tone within an octave of prior harmony");
            }
        }

        previousUnique = currentUnique;
        previousCount = currentCount;
    }
}

} // namespace

int main() {
    testDeterministicPolyphonicScaleSafe();
    testAllStylesStayWithinTwoOrThreeHarmonicPitchClasses();
    testFourthPadVoiceIsOctaveDoubleOnly();
    testMovementIncreasesHarmonicActivity();
    testSpreadProgressivelyWidensVoicings();
    testMovementProgressivelyAddsHarmonicEvents();
    testTensionProgressivelyAddsColorVoices();
    testContextFollowAlignsPadsWithRiff();
    testVoiceLeadingAvoidsWildJumps();
    std::cout << "Midiator Pad Brain tests: PASS\n";
    return 0;
}
