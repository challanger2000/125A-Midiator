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

bool chordContainsPc(const PadSettings& s, int rootDegree, int pc) {
    const auto& scale = RiffEngine::scaleDefinition(s.scale);
    for (int offset : {0, 2, 4}) {
        const int degree = (rootDegree + offset) % scale.count;
        if (degreePc(s, degree) == wrap12(pc))
            return true;
    }
    return false;
}

int chooseContextRootDegree(const Phrase& guitar,
                            const Phrase& bass,
                            const PadSettings& s,
                            int startStep,
                            int spanSteps,
                            int fallbackDegree) {
    const auto& scale = RiffEngine::scaleDefinition(s.scale);
    int bestDegree = fallbackDegree;
    double bestScore = -1.0;
    bool sawContext = false;

    for (int candidate = 0; candidate < scale.count; ++candidate) {
        double score = candidate == fallbackDegree ? 0.20 : 0.0;

        for (int offset = 0; offset < spanSteps; ++offset) {
            const int step = startStep + offset;
            if (step >= guitar.usedSteps() && step >= bass.usedSteps())
                break;

            const bool strong = (step % 4) == 0;

            if (step < bass.usedSteps() && bass.steps[step].noteCount > 0) {
                sawContext = true;
                const int pc = bass.steps[step].notes[0].pitch;
                if (chordContainsPc(s, candidate, pc))
                    score += strong ? 3.0 : 2.0;
                if (degreePc(s, candidate) == wrap12(pc))
                    score += strong ? 1.5 : 0.5;
            }

            if (step < guitar.usedSteps() && guitar.steps[step].noteCount > 0) {
                sawContext = true;
                const int pc = guitar.steps[step].notes[0].pitch;
                if (chordContainsPc(s, candidate, pc))
                    score += strong ? 2.0 : 1.0;
                if (degreePc(s, candidate) == wrap12(pc))
                    score += strong ? 0.8 : 0.2;
            }
        }

        if (score > bestScore) {
            bestScore = score;
            bestDegree = candidate;
        }
    }

    return sawContext ? bestDegree : fallbackDegree;
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
    s.contextFollow = std::clamp(s.contextFollow, 0.0f, 1.0f);
    s.centerMidi = std::clamp(s.centerMidi, 48, 72);

    PadPhrase out{};
    out.bars = std::clamp(std::max(guitar.bars, bass.bars), 1, kMaxBars);

    Rng rng(seed);
    Rng contextRng(seed ^ 0x434F4E54u);
    const auto& scale = RiffEngine::scaleDefinition(s.scale);

    int chordEverySteps = 16;
    float halfBarProbability = 0.0f;
    if (s.style == StyleId::HeavyIndustrial)
        halfBarProbability = 0.12f + 0.78f * s.movement;
    else if (s.style == StyleId::DarkRockGothic)
        halfBarProbability = 0.04f + 0.62f * s.movement;
    else
        halfBarProbability = 0.06f + 0.70f * s.movement;

    // Movement should react progressively: lower values mostly hold whole-bar
    // harmony, higher values increasingly introduce half-bar changes.
    chordEverySteps = rng.chance(std::clamp(halfBarProbability, 0.0f, 0.96f))
        ? 8 : 16;

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

        // Context Follow makes the pad harmonically serve the actual riff.
        // It only chooses the diatonic chord root; voicing, movement, tension
        // and sustain remain independent pad decisions.
        if (contextRng.chance(s.contextFollow)) {
            rootDegree = chooseContextRootDegree(
                guitar, bass, s, step, chordEverySteps, rootDegree);
        }

        std::array<int, 3> degrees{{
            rootDegree,
            (rootDegree + 2) % scale.count,
            (rootDegree + 4) % scale.count
        }};

        // Pads deliberately stay sparse: normally 2-3 distinct chord tones.
        // A possible fourth voice is added later only as an octave doubling.
        int harmonicVoices = 3;
        const float contextRichness = 1.0f - s.contextFollow;
        if (s.style == StyleId::NDHIndustrial &&
            rng.chance(0.55f * contextRichness))
            harmonicVoices = 2;
        else if (s.style == StyleId::HeavyIndustrial &&
                 rng.chance(0.28f * contextRichness))
            harmonicVoices = 2;

        auto& dst = out.steps[step];
        dst.noteCount = harmonicVoices;

        const int durationBase = chordEverySteps;
        int duration = std::max(2, static_cast<int>(std::lround(
            durationBase * (0.55 + 0.45 * s.sustain))));
        duration = std::min(duration, out.usedSteps() - step);

        for (int v = 0; v < harmonicVoices; ++v) {
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

            if (v >= 2 && pitch + 12 <= 88) {
                const float voiceBias = (v == 2) ? -0.08f : 0.10f;
                const float openProbability =
                    std::clamp(0.10f + 0.78f * s.spread + voiceBias, 0.0f, 0.96f);
                if (rng.chance(openProbability))
                    pitch += 12;
            }

            // Tension remains scale-safe: color voice may move to the adjacent
            // scale degree instead of using chromatic out-of-key notes.
            if (v == harmonicVoices - 1 && rng.chance(0.15f + 0.55f * s.tension)) {
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

        // At most one additional pad voice, and only as a true octave
        // doubling of an existing harmonic tone. This adds size without
        // inventing a fourth independent chord degree.
        float octaveDoubleProbability = 0.04f + 0.18f * s.spread;
        if (s.style == StyleId::DarkRockGothic)
            octaveDoubleProbability += 0.08f;
        else if (s.style == StyleId::HeavyIndustrial)
            octaveDoubleProbability += 0.03f;

        if (harmonicVoices < kMaxPadVoices &&
            rng.chance(std::clamp(octaveDoubleProbability, 0.0f, 0.32f))) {
            const int sourceIndex =
                (harmonicVoices > 2 && rng.chance(0.35f)) ? harmonicVoices - 1 : 0;
            const auto source = dst.notes[sourceIndex];

            int doubledPitch = source.pitch;
            const bool canUp = source.pitch + 12 <= 88;
            const bool canDown = source.pitch - 12 >= 45;

            if (canUp && canDown)
                doubledPitch += rng.chance(0.68f) ? 12 : -12;
            else if (canUp)
                doubledPitch += 12;
            else if (canDown)
                doubledPitch -= 12;

            if (doubledPitch != source.pitch) {
                dst.notes[dst.noteCount++] = {
                    doubledPitch,
                    std::clamp(source.velocity - 4, 1, 126),
                    duration
                };
            }
        }

        sortVoicing(dst);
    }

    return out;
}

} // namespace midiator
