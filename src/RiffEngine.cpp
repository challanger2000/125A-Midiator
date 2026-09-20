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

    // Industrial/heavy bias: pedal root dominates, then characteristic low degrees.
    if (rng.chance(0.48f + 0.40f * s.repetition))
        return 0;

    std::array<int, 8> candidates{};
    int n = 0;

    auto push = [&](int idx) {
        if (idx >= 0 && idx < scale.count)
            candidates[n++] = idx;
    };

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

    // Strong beats stay simpler; offbeats may move farther.
    if ((stepInBar % 4) == 0 && rng.chance(0.60f))
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
        step.notes[1] = {pitch + 7, std::max(1, step.notes[0].velocity - rng.range(0, 5)), lengthSteps};
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
    const int used = result.usedSteps();

    // Build a motif first, then reuse it with controlled deviations.
    const int motifSteps = (s.complexity > 0.67f) ? 16 : 8;
    std::array<Step, 16> motif{};

    for (int i = 0; i < motifSteps; ++i) {
        if (!shouldHit(rng, s, i))
            continue;

        const bool accent = (i % 4) == 0;
        const int degree = chooseDegree(rng, s, i);
        int pitch = base + scale.intervals[degree];

        if (rng.chance(0.10f + 0.22f * s.complexity))
            pitch += 12;

        pitch = clampMusicalPitch(pitch, base);

        const bool palmMute = rng.chance(s.palmMuteChance * (accent ? 0.68f : 1.0f));
        const bool powerChord = accent && !palmMute && rng.chance(s.powerChordChance);
        int length = palmMute ? 1 : (rng.chance(0.35f + 0.25f * (1.0f - s.density)) ? 2 : 1);

        createStepNote(motif[i], pitch, powerChord, palmMute, accent, length, rng);
    }

    if (motif[0].noteCount == 0)
        createStepNote(motif[0], base, false, true, true, 1, rng);

    for (int step = 0; step < used; ++step) {
        const int motifIndex = step % motifSteps;
        Step current = motif[motifIndex];

        const int phraseBlock = step / motifSteps;
        const float mutationChance = (1.0f - s.repetition) * (0.10f + 0.38f * s.complexity);

        if (phraseBlock > 0 && rng.chance(mutationChance)) {
            if (current.noteCount == 0) {
                if (rng.chance(0.50f) && shouldHit(rng, s, step)) {
                    const int degree = chooseDegree(rng, s, step % 16);
                    int pitch = clampMusicalPitch(base + scale.intervals[degree], base);
                    createStepNote(current, pitch, false, true, false, 1, rng);
                }
            } else if (rng.chance(0.65f)) {
                const int degree = chooseDegree(rng, s, step % 16);
                int pitch = clampMusicalPitch(base + scale.intervals[degree], base);
                const bool accent = (step % 4) == 0;
                const bool palmMute = rng.chance(s.palmMuteChance);
                const bool powerChord = accent && !palmMute && rng.chance(s.powerChordChance);
                createStepNote(current, pitch, powerChord, palmMute, accent, palmMute ? 1 : 2, rng);
            } else {
                current = {};
            }
        }

        // Bar endings get an occasional answer note or short stop.
        if ((step % 16) == 15 && step + 1 < used && rng.chance(0.35f + 0.20f * s.complexity)) {
            current = {};
        }

        result.steps[step] = current;
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
