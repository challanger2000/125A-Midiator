#include "RiffEngine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace midiator {
namespace {

struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed ? seed : 0x6d2b79f5u) {}

    uint32_t nextU32() {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    float unit() {
        return static_cast<float>(nextU32() & 0x00ffffffu) / static_cast<float>(0x01000000u);
    }

    int range(int lo, int hi) {
        if (hi <= lo)
            return lo;
        return lo + static_cast<int>(nextU32() % static_cast<uint32_t>(hi - lo + 1));
    }

    bool chance(float p) {
        return unit() < std::clamp(p, 0.0f, 1.0f);
    }
};

constexpr std::array<ScaleDefinition, static_cast<int>(ScaleId::Count)> kScales{{
    {"Natural Minor / Aeolian", {0, 2, 3, 5, 7, 8, 10, 0}, 7,
     "dark, stable, melodic", "minor 3rd and minor 6th"},
    {"Phrygian", {0, 1, 3, 5, 7, 8, 10, 0}, 7,
     "dark, tense, aggressive", "minor 2nd above the root"},
    {"Dorian", {0, 2, 3, 5, 7, 9, 10, 0}, 7,
     "minor, open, driving", "major 6th in a minor context"},
    {"Harmonic Minor", {0, 2, 3, 5, 7, 8, 11, 0}, 7,
     "dark, dramatic, exotic", "major 7th leading tone"},
    {"Phrygian Dominant", {0, 1, 4, 5, 7, 8, 10, 0}, 7,
     "aggressive, exotic, dominant", "minor 2nd plus major 3rd"},
    {"Minor Pentatonic", {0, 3, 5, 7, 10, 0, 0, 0}, 5,
     "direct, heavy, familiar", "minor 3rd and perfect 5th"},
    {"Blues", {0, 3, 5, 6, 7, 10, 0, 0}, 6,
     "gritty, tense, blues-heavy", "tritone / blue note"}
}};

int wrap12(int v) {
    v %= 12;
    return v < 0 ? v + 12 : v;
}

int chooseDegree(Rng& rng, const GeneratorSettings& s, int stepInBar) {
    const auto& scale = kScales[static_cast<int>(s.scale)];

    // Heavy/industrial riffs need a strong pedal tone, but not a one-note monoculture.
    float rootProbability = 0.28f + 0.30f * s.repetition;
    if ((stepInBar % 4) == 0)
        rootProbability += 0.16f;

    if (rng.chance(std::clamp(rootProbability, 0.0f, 0.82f)))
        return 0;

    std::array<int, 8> candidates{};
    int n = 0;

    auto push = [&](int idx) {
        if (idx >= 1 && idx < scale.count)
            candidates[n++] = idx;
    };

    // Put characteristic tones first so dark modes keep their identity.
    if (s.scale == ScaleId::Phrygian || s.scale == ScaleId::PhrygianDominant)
        push(1);
    if (s.scale == ScaleId::Blues)
        push(3);

    push(2);
    push(3);
    push(4);
    push(5);
    push(6);

    if (n == 0)
        return 0;

    const int characteristicSlots =
        (s.scale == ScaleId::Phrygian || s.scale == ScaleId::PhrygianDominant ||
         s.scale == ScaleId::Blues) ? 1 : 0;

    if (characteristicSlots > 0 && rng.chance(0.34f + 0.18f * s.complexity))
        return candidates[0];

    return candidates[rng.range(0, n - 1)];
}

int rootBaseForPitchClass(int pitchClass, int lowRootMidi) {
    int p = std::clamp(lowRootMidi, 0, 127);
    while (wrap12(p) != wrap12(pitchClass))
        ++p;
    return std::min(p, 120);
}

bool shouldHit(Rng& rng, const GeneratorSettings& s, int globalStep) {
    if (globalStep == 0)
        return true;

    const int pos = globalStep % 16;
    float weight = 0.34f;
    if ((pos % 4) == 0)
        weight = 0.88f;
    else if ((pos % 2) == 0)
        weight = 0.61f;

    // Characteristic industrial syncopation on late 16ths.
    if (pos == 3 || pos == 7 || pos == 11 || pos == 15)
        weight += 0.10f * s.complexity;

    const float probability = std::clamp((0.20f + 0.95f * s.density) * weight, 0.0f, 0.96f);
    return rng.chance(probability);
}

int velocityFor(Rng& rng, bool palmMute, bool accent) {
    if (palmMute) {
        // Intentionally below the open-note zone. Never use 127.
        return rng.range(accent ? 54 : 42, accent ? 72 : 62);
    }

    return rng.range(accent ? 104 : 88, accent ? 120 : 106);
}

void createStepNote(Step& step,
                    int pitch,
                    bool powerChord,
                    bool palmMute,
                    bool accent,
                    int lengthSteps,
                    Rng& rng) {
    step.noteCount = 1;
    step.notes[0] = {pitch, velocityFor(rng, palmMute, accent), lengthSteps};

    if (powerChord && pitch <= 120) {
        step.noteCount = 2;
        int secondVelocity = step.notes[0].velocity - rng.range(0, 5);

        // Keep both notes of a dyad inside the same visible articulation zone.
        if (step.notes[0].velocity >= 88)
            secondVelocity = std::max(88, secondVelocity);
        else
            secondVelocity = std::clamp(secondVelocity, 1, 72);

        step.notes[1] = {pitch + 7, secondVelocity, lengthSteps};
    }
}

int clampMusicalPitch(int pitch, int base) {
    const int low = std::max(0, base);
    const int high = std::min(120, base + 24);
    while (pitch < low)
        pitch += 12;
    while (pitch > high)
        pitch -= 12;
    return std::clamp(pitch, 0, 120);
}

void sanitizeOverlaps(Phrase& phrase) {
    const int used = phrase.usedSteps();

    for (int stepIndex = 0; stepIndex < used; ++stepIndex) {
        auto& step = phrase.steps[stepIndex];

        for (int noteIndex = 0; noteIndex < step.noteCount; ++noteIndex) {
            auto& note = step.notes[noteIndex];
            const int requestedLength = std::max(1, note.lengthSteps);
            const int endStep = std::min(used, stepIndex + requestedLength);

            for (int futureStep = stepIndex + 1; futureStep < endStep; ++futureStep) {
                const auto& future = phrase.steps[futureStep];
                bool retriggered = false;

                for (int futureNote = 0; futureNote < future.noteCount; ++futureNote) {
                    if (future.notes[futureNote].pitch == note.pitch) {
                        retriggered = true;
                        break;
                    }
                }

                if (retriggered) {
                    note.lengthSteps = std::max(1, futureStep - stepIndex);
                    break;
                }
            }
        }
    }
}

} // namespace

const ScaleDefinition& RiffEngine::scaleDefinition(ScaleId id) {
    int i = std::clamp(static_cast<int>(id), 0, static_cast<int>(ScaleId::Count) - 1);
    return kScales[static_cast<size_t>(i)];
}

bool RiffEngine::isScaleTone(int pitch, int rootPitchClass, ScaleId scaleId) {
    const int rel = wrap12(pitch - rootPitchClass);
    const auto& s = scaleDefinition(scaleId);
    for (int i = 0; i < s.count; ++i)
        if (s.intervals[i] == rel)
            return true;
    return false;
}

int RiffEngine::nearestRootForPitchClass(int pitchClass, int aroundMidi) {
    int best = std::clamp(aroundMidi, 0, 127);
    int bestDistance = 128;
    for (int p = 0; p <= 127; ++p) {
        if (wrap12(p) == wrap12(pitchClass)) {
            int d = std::abs(p - aroundMidi);
            if (d < bestDistance) {
                best = p;
                bestDistance = d;
            }
        }
    }
    return best;
}

Phrase RiffEngine::generate(const GeneratorSettings& in, uint32_t seed) {
    GeneratorSettings s = in;
    s.bars = std::clamp(s.bars, 1, kMaxBars);
    s.density = std::clamp(s.density, 0.0f, 1.0f);
    s.complexity = std::clamp(s.complexity, 0.0f, 1.0f);
    s.repetition = std::clamp(s.repetition, 0.0f, 1.0f);
    s.powerChordChance = std::clamp(s.powerChordChance, 0.0f, 1.0f);
    s.palmMuteChance = std::clamp(s.palmMuteChance, 0.0f, 1.0f);

    Phrase result{};
    result.bars = s.bars;

    Rng rng(seed);
    const auto& scale = scaleDefinition(s.scale);
    const int base = rootBaseForPitchClass(s.rootPitchClass, s.lowRootMidi);

    auto makeMusicalStep = [&](int stepInBar, bool preferRoot) {
        Step out{};
        const bool accent = (stepInBar % 4) == 0;

        int degree = preferRoot ? 0 : chooseDegree(rng, s, stepInBar);
        int pitch = base + scale.intervals[degree];

        if (!preferRoot && rng.chance(0.08f + 0.20f * s.complexity))
            pitch += 12;

        pitch = clampMusicalPitch(pitch, base);

        const bool palmMute = rng.chance(s.palmMuteChance * (accent ? 0.68f : 1.0f));

        // Palm-muted power chords are musically valid, so chord generation must
        // not depend on the note being "open".
        const float chordChance = s.powerChordChance * (accent ? 1.0f : 0.30f);
        const bool powerChord = rng.chance(chordChance);

        const int length = palmMute
            ? 1
            : (rng.chance(0.34f + 0.28f * (1.0f - s.density)) ? 2 : 1);

        createStepNote(out, pitch, powerChord, palmMute, accent, length, rng);
        return out;
    };

    // A full 4/4 bar is the primary motif unit. This prevents the default
    // generator from merely repeating an 8-step half-bar twice.
    std::array<Step, kStepsPerBar> baseBar{};

    for (int i = 0; i < kStepsPerBar; ++i) {
        if (!shouldHit(rng, s, i))
            continue;
        baseBar[i] = makeMusicalStep(i, false);
    }

    // Always give the riff a stable downbeat anchor.
    baseBar[0] = makeMusicalStep(0, true);

    // At moderate/high complexity, guarantee at least a little tonal movement
    // in the seed bar when density allows it.
    if (s.complexity >= 0.25f && s.density >= 0.25f) {
        int nonRootCount = 0;
        for (const auto& step : baseBar) {
            if (step.noteCount > 0 &&
                wrap12(step.notes[0].pitch) != wrap12(s.rootPitchClass))
                ++nonRootCount;
        }

        if (nonRootCount < 2) {
            const int candidates[] = {3, 6, 10, 14};
            for (int pos : candidates) {
                if (pos >= kStepsPerBar)
                    continue;
                if (baseBar[pos].noteCount == 0 && !shouldHit(rng, s, pos))
                    continue;

                Step movement = makeMusicalStep(pos, false);
                if (movement.noteCount > 0 &&
                    wrap12(movement.notes[0].pitch) == wrap12(s.rootPitchClass)) {
                    const int degree = (scale.count > 2) ? 2 : 1;
                    const int pitch = clampMusicalPitch(base + scale.intervals[degree], base);
                    const bool palmMute = rng.chance(s.palmMuteChance);
                    createStepNote(movement, pitch, false, palmMute, false, palmMute ? 1 : 2, rng);
                }

                baseBar[pos] = movement;
                if (++nonRootCount >= 2)
                    break;
            }
        }
    }

    auto roleFactorForBar = [&](int bar) {
        if (bar == 0)
            return 0.0f;

        const int role = bar % 4;
        float factor = 1.0f;
        if (role == 1)
            factor = 0.80f;   // A'
        else if (role == 2)
            factor = 1.10f;   // answer / development
        else if (role == 3)
            factor = 1.50f;   // turnaround

        if (bar >= 4)
            factor += 0.15f;  // second four-bar phrase develops further

        return factor;
    };

    for (int bar = 0; bar < s.bars; ++bar) {
        int changedSteps = 0;
        const float roleFactor = roleFactorForBar(bar);
        const float mutationChance =
            (0.04f + (1.0f - s.repetition) * (0.16f + 0.34f * s.complexity)) * roleFactor;

        for (int i = 0; i < kStepsPerBar; ++i) {
            Step current = baseBar[i];

            if (bar > 0 && rng.chance(mutationChance)) {
                ++changedSteps;
                const float action = rng.unit();

                if (current.noteCount == 0) {
                    if (action < 0.55f && shouldHit(rng, s, i))
                        current = makeMusicalStep(i, false);
                } else if (action < 0.18f && i != 0) {
                    current = {};
                } else if (action < 0.76f) {
                    const bool oldPowerChord = current.noteCount == 2;
                    const bool oldPalmMute = current.notes[0].velocity <= 72;
                    const int oldLength = current.notes[0].lengthSteps;

                    int degree = chooseDegree(rng, s, i);
                    int pitch = clampMusicalPitch(base + scale.intervals[degree], base);

                    createStepNote(current, pitch, oldPowerChord, oldPalmMute,
                                   (i % 4) == 0, oldLength, rng);
                } else {
                    current = makeMusicalStep(i, false);
                }
            }

            // Turnaround bars get a little extra activity/change in beat 4,
            // creating a phrase ending rather than a flat repeated loop.
            if (bar > 0 && (bar % 4) == 3 && i >= 12 &&
                rng.chance(0.10f + 0.18f * s.complexity)) {
                ++changedSteps;
                if (i == 15 && rng.chance(0.60f))
                    current = {};
                else
                    current = makeMusicalStep(i, false);
            }

            result.steps[bar * kStepsPerBar + i] = current;
        }

        // Unless Repetition is intentionally almost maxed, avoid accidental
        // byte-identical bars. One small answer note/rest is enough.
        if (bar > 0 && changedSteps == 0 && s.repetition < 0.95f) {
            const int pos = 12 + rng.range(0, 3);
            auto& target = result.steps[bar * kStepsPerBar + pos];
            if (target.noteCount > 0 && pos != 12)
                target = {};
            else
                target = makeMusicalStep(pos, false);
        }
    }

    sanitizeOverlaps(result);
    return result;
}

Phrase RiffEngine::vary(const Phrase& source,
                        const GeneratorSettings& in,
                        float amount,
                        uint32_t seed) {
    const float a = std::clamp(amount, 0.0f, 1.0f);
    if (a <= 0.0001f)
        return source;

    GeneratorSettings s = in;
    s.bars = source.bars;

    Phrase result = source;
    Rng rng(seed);
    const auto& scale = scaleDefinition(s.scale);
    const int base = rootBaseForPitchClass(s.rootPitchClass, s.lowRootMidi);
    const int used = result.usedSteps();

    for (int step = 0; step < used; ++step) {
        if (!rng.chance(0.08f + 0.62f * a))
            continue;

        auto& dst = result.steps[step];
        const bool accent = (step % 4) == 0;

        const float action = rng.unit();

        if (dst.noteCount == 0) {
            if (action < 0.35f * a && shouldHit(rng, s, step)) {
                const int degree = chooseDegree(rng, s, step % 16);
                int pitch = clampMusicalPitch(base + scale.intervals[degree], base);
                const bool palmMute = rng.chance(s.palmMuteChance);
                createStepNote(dst, pitch, false, palmMute, accent, palmMute ? 1 : 2, rng);
            }
            continue;
        }

        if (action < 0.18f * a && step != 0) {
            dst = {};
            continue;
        }

        if (action < 0.72f) {
            // Pitch mutation remains inside the selected root + scale.
            const int degree = chooseDegree(rng, s, step % 16);
            int pitch = clampMusicalPitch(base + scale.intervals[degree], base);

            // Keep strong identity by often retaining the pedal root.
            if (rng.chance(0.38f + 0.40f * s.repetition))
                pitch = base;

            const bool palmMute = dst.notes[0].velocity < 84;
            const bool powerChord = dst.noteCount == 2;
            const int length = dst.notes[0].lengthSteps;
            createStepNote(dst, pitch, powerChord, palmMute, accent, length, rng);
        } else {
            // Articulation/rhythm variation without changing tonal center.
            const bool palmMute = rng.chance(s.palmMuteChance);
            const bool powerChord = accent && !palmMute && rng.chance(s.powerChordChance);
            const int length = palmMute ? 1 : rng.range(1, 2);
            const int pitch = dst.notes[0].pitch;
            createStepNote(dst, pitch, powerChord, palmMute, accent, length, rng);
        }
    }

    // Preserve a reliable downbeat anchor.
    if (result.steps[0].noteCount == 0)
        createStepNote(result.steps[0], base, false, true, true, 1, rng);

    sanitizeOverlaps(result);
    return result;
}

} // namespace midiator
