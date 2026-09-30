#include "PadBrain.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace midiator {
namespace {

struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed ? seed : 0x7f4a7c15u) {}
    uint32_t nextU32() {
        uint32_t x = state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        state = x;
        return x;
    }
    float unit() {
        return static_cast<float>(nextU32() & 0x00ffffffu) /
               static_cast<float>(0x01000000u);
    }
    bool chance(float p) {
        return unit() < std::clamp(p, 0.0f, 1.0f);
    }
};

int wrap12(int v) {
    v %= 12;
    return v < 0 ? v + 12 : v;
}

int nearestPitchForPc(int pc, int around) {
    int best = around;
    int bestDistance = 999;
    for (int p = std::max(36, around - 18); p <= std::min(96, around + 18); ++p) {
        if (wrap12(p) != wrap12(pc))
            continue;
        const int d = std::abs(p - around);
        if (d < bestDistance) {
            best = p;
            bestDistance = d;
        }
    }
    return best;
}

int degreePc(const PadSettings& s, int degree) {
    const auto& scale = RiffEngine::scaleDefinition(s.scale);
    degree %= scale.count;
    if (degree < 0) degree += scale.count;
    return wrap12(s.rootPitchClass + scale.intervals[degree]);
}

int nearestScaleTone(int pitch, const PadSettings& s) {
    int best = pitch;
    int bestDistance = 999;
    for (int p = std::max(36, pitch - 6); p <= std::min(96, pitch + 6); ++p) {
        if (!RiffEngine::isScaleTone(p, s.rootPitchClass, s.scale))
            continue;
        const int d = std::abs(p - pitch);
        if (d < bestDistance) {
            best = p;
            bestDistance = d;
        }
    }
    return best;
}

void sortVoicing(PadStep& step) {
    std::sort(step.notes.begin(), step.notes.begin() + step.noteCount,
              [](const PadNote& a, const PadNote& b) { return a.pitch < b.pitch; });
}

} // namespace

PadPhrase PadBrain::generate(const Phrase& guitar,
                             const Phrase& bass,
                             const PadSettings& settings,
                             uint32_t seed) {
    PadSettings s = settings;
    s.rootPitchClass = wrap12(s.rootPitchClass);
    s.movement = std::clamp(s.movement, 0.0f, 1.0f);
    s.spread = std::clamp(s.spread, 0.0f, 1.0f);
    s.tension = std::clamp(s.tension, 0.0f, 1.0f);
    s.sustain = std::clamp(s.sustain, 0.0f, 1.0f);
    s.centerMidi = std::clamp(s.centerMidi, 48, 72);

    PadPhrase out{};
    out.bars = std::clamp(std::max(guitar.bars, bass.bars), 1, kMaxBars);

    Rng rng(seed);
    const auto& scale = RiffEngine::scaleDefinition(s.scale);

    int chordEverySteps = 16;
    if (s.style == StyleId::HeavyIndustrial)
        chordEverySteps = s.movement >= 0.35f ? 8 : 16;
    else if (s.style == StyleId::DarkRockGothic)
        chordEverySteps = s.movement >= 0.70f ? 8 : 16;
    else
        chordEverySteps = s.movement >= 0.55f ? 8 : 16;

    const std::array<int, 8> progression{{0, 5, 2, 6, 0, 3, 1, 4}};
    std::array<int, kMaxPadVoices> previous{{-1, -1, -1, -1}};

    for (int step = 0; step < out.usedSteps(); step += chordEverySteps) {
        const int chordIndex = step / chordEverySteps;
        int rootDegree = progression[static_cast<size_t>(chordIndex % progression.size())] %
                         std::max(1, scale.count);

        if (s.style == StyleId::NDHIndustrial && chordIndex > 0 &&
            !rng.chance(0.22f + 0.38f * s.movement)) {
            rootDegree = 0;
        }

        std::array<int, 4> degrees{{
            rootDegree,
            (rootDegree + 2) % scale.count,
            (rootDegree + 4) % scale.count,
            (rootDegree + 6) % scale.count
        }};

        int voices = 3;
        if (s.style == StyleId::DarkRockGothic)
            voices = 4;
        else if (s.style == StyleId::HeavyIndustrial && s.tension > 0.45f)
            voices = 4;

        auto& dst = out.steps[step];
        dst.noteCount = voices;

        const int durationBase = chordEverySteps;
        int duration = std::max(2, static_cast<int>(std::lround(
            durationBase * (0.55 + 0.45 * s.sustain))));
        duration = std::min(duration, out.usedSteps() - step);

        for (int v = 0; v < voices; ++v) {
            int pc = degreePc(s, degrees[v]);
            int target = s.centerMidi + (v - 1) * 5;
            if (v == 0)
                target -= 7;

            int pitch = nearestPitchForPc(pc, target);

            // Voice leading: prefer the closest inversion to the previous voice.
            if (previous[v] >= 0) {
                int best = pitch;
                int bestDistance = std::abs(pitch - previous[v]);
                for (int octave : {-12, 12}) {
                    const int candidate = pitch + octave;
                    if (candidate < 45 || candidate > 88)
                        continue;
                    const int d = std::abs(candidate - previous[v]);
                    if (d < bestDistance) {
                        best = candidate;
                        bestDistance = d;
                    }
                }
                pitch = best;
            }

            if (v >= 2 && s.spread > 0.55f && pitch + 12 <= 88)
                pitch += 12;

            // Tension remains scale-safe: color voice may move to the adjacent
            // scale degree instead of using chromatic out-of-key notes.
            if (v == voices - 1 && rng.chance(0.15f + 0.55f * s.tension)) {
                const int colorDegree = (degrees[v] + 1) % scale.count;
                pitch = nearestPitchForPc(degreePc(s, colorDegree), pitch);
            }

            pitch = nearestScaleTone(std::clamp(pitch, 45, 88), s);
            previous[v] = pitch;

            int velocity = 66 + (v == 0 ? 7 : 0);
            if (s.style == StyleId::HeavyIndustrial)
                velocity += 5;
            else if (s.style == StyleId::DarkRockGothic)
                velocity -= 3;

            dst.notes[v] = {pitch, std::clamp(velocity, 1, 126), duration};
        }

        sortVoicing(dst);
    }

    return out;
}

} // namespace midiator
