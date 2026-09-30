#include "BassBrain.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace midiator {
namespace {

class Rng {
public:
    explicit Rng(uint32_t seed) : state_(seed ? seed : 0x125AB455u) {}

    uint32_t next() {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }

    float unit() { return static_cast<float>(next() & 0x00FFFFFFu) / 16777216.0f; }
    bool chance(float p) { return unit() < std::clamp(p, 0.0f, 1.0f); }
    int range(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1));
    }

private:
    uint32_t state_;
};

int wrap12(int v) {
    v %= 12;
    return v < 0 ? v + 12 : v;
}

int alignedRoot(int pitchClass, int around) {
    around = std::clamp(around, 24, 60);
    int best = around;
    int bestDistance = 128;
    for (int p = 24; p <= 60; ++p) {
        if (wrap12(p) != wrap12(pitchClass))
            continue;
        const int d = std::abs(p - around);
        if (d < bestDistance) {
            best = p;
            bestDistance = d;
        }
    }
    return best;
}

int nearestScalePitch(int wanted, int rootPitchClass, ScaleId scale) {
    for (int distance = 0; distance <= 12; ++distance) {
        const int up = wanted + distance;
        if (up >= 24 && up <= 60 && RiffEngine::isScaleTone(up, rootPitchClass, scale))
            return up;
        const int down = wanted - distance;
        if (down >= 24 && down <= 60 &&
            RiffEngine::isScaleTone(down, rootPitchClass, scale))
            return down;
    }
    return std::clamp(wanted, 24, 60);
}

bool guitarHitAt(const Phrase& guitar, int step) {
    return step >= 0 && step < guitar.usedSteps() && guitar.steps[step].noteCount > 0;
}

int primaryGuitarPitch(const Phrase& guitar, int step, int fallback) {
    if (!guitarHitAt(guitar, step))
        return fallback;
    return guitar.steps[step].notes[0].pitch;
}

int chooseBassPitch(Rng& rng,
                    const Phrase& guitar,
                    int step,
                    const BassSettings& s,
                    int root,
                    bool strongBeat) {
    // Heavy/NDH bass should live primarily on the pedal root, but selectively
    // follow important guitar movement instead of duplicating every guitar note.
    float rootChance = 0.72f - 0.34f * s.movement;
    if (s.style == StyleId::NDHIndustrial)
        rootChance += 0.10f;
    else if (s.style == StyleId::DarkRockGothic)
        rootChance -= 0.12f;
    else if (s.style == StyleId::HeavyIndustrial)
        rootChance += 0.02f;
    if (strongBeat)
        rootChance += 0.16f;

    if (rng.chance(rootChance))
        return root;

    int guitarPitch = primaryGuitarPitch(guitar, step, root);
    int targetPc = wrap12(guitarPitch);
    int target = root;
    while (wrap12(target) != targetPc && target < 60)
        ++target;

    if (!RiffEngine::isScaleTone(target, s.rootPitchClass, s.scale))
        target = nearestScalePitch(target, s.rootPitchClass, s.scale);

    // Prefer the compact low register; high guitar octaves should not drag bass
    // into an implausibly high register.
    while (target > root + 12)
        target -= 12;
    while (target < root)
        target += 12;

    float octaveChance = s.octaveChance;
    if (s.style == StyleId::HeavyIndustrial)
        octaveChance = std::min(1.0f, octaveChance * 1.8f + 0.06f);
    else if (s.style == StyleId::DarkRockGothic)
        octaveChance *= 0.75f;
    if (rng.chance(octaveChance) && target <= 48)
        target += 12;

    // Keep the final register correction pitch-class preserving. A hard clamp
    // to 24/60 can turn an otherwise scale-safe note chromatic at the edge.
    while (target > 60)
        target -= 12;
    while (target < 24)
        target += 12;

    if (!RiffEngine::isScaleTone(target, s.rootPitchClass, s.scale))
        target = nearestScalePitch(target, s.rootPitchClass, s.scale);

    return target;
}

void sanitizeMonophonic(Phrase& p) {
    const int used = p.usedSteps();
    for (int i = 0; i < used; ++i) {
        auto& st = p.steps[i];
        if (st.noteCount <= 0)
            continue;
        st.noteCount = 1;
        auto& n = st.notes[0];
        n.pitch = std::clamp(n.pitch, 24, 60);
        n.velocity = std::clamp(n.velocity, 58, 118);
        n.lengthSteps = std::clamp(n.lengthSteps, 1, std::max(1, used - i));

        for (int j = i + 1; j < std::min(used, i + n.lengthSteps); ++j) {
            if (p.steps[j].noteCount > 0) {
                n.lengthSteps = std::max(1, j - i);
                break;
            }
        }
    }
}

} // namespace

Phrase BassBrain::generate(const Phrase& guitar,
                           const BassSettings& in,
                           uint32_t seed) {
    BassSettings s = in;
    s.follow = std::clamp(s.follow, 0.0f, 1.0f);
    s.movement = std::clamp(s.movement, 0.0f, 1.0f);
    s.passing = std::clamp(s.passing, 0.0f, 1.0f);
    s.octaveChance = std::clamp(s.octaveChance, 0.0f, 1.0f);
    s.sustain = std::clamp(s.sustain, 0.0f, 1.0f);

    Phrase result{};
    result.bars = std::clamp(guitar.bars, 1, kMaxBars);

    Rng rng(seed);
    const int root = alignedRoot(s.rootPitchClass, s.lowRootMidi);

    for (int step = 0; step < result.usedSteps(); ++step) {
        const int local = step % kStepsPerBar;
        const bool strongBeat = (local % 4) == 0;
        const bool guitarHit = guitarHitAt(guitar, step);

        // Follow establishes the shared groove. Even at low follow values,
        // strong beats retain anchors so the bass does not become random filler.
        bool hit = false;
        if (guitarHit) {
            float styleLock = 0.0f;
            if (s.style == StyleId::NDHIndustrial) styleLock = 0.08f;
            else if (s.style == StyleId::HeavyIndustrial) styleLock = 0.20f;
            else if (s.style == StyleId::DarkRockGothic) styleLock = -0.12f;

            if (strongBeat)
                hit = rng.chance(0.55f + 0.45f * s.follow + styleLock);
            else
                hit = rng.chance(0.16f + 0.80f * s.follow + styleLock);
        } else if (strongBeat) {
            // Low FOLLOW gets its own quarter-note pulse so the bass can act
            // like a separate player rather than a transposed guitar copy.
            hit = rng.chance(0.10f + 0.40f * (1.0f - s.follow));
        } else {
            float independentPulse =
                0.02f + 0.16f * (1.0f - s.follow) + 0.18f * s.passing;
            if (s.style == StyleId::DarkRockGothic)
                independentPulse += 0.10f;
            else if (s.style == StyleId::HeavyIndustrial)
                independentPulse = std::max(0.0f, independentPulse - 0.02f);
            if ((local & 1) && rng.chance(independentPulse))
                hit = true;
        }

        if (!hit)
            continue;

        int pitch = chooseBassPitch(rng, guitar, step, s, root, strongBeat);

        // Passing notes are only inserted in weak positions and remain in-scale.
        if (!guitarHit && !strongBeat && s.passing > 0.0f) {
            const int direction = rng.chance(0.5f) ? 2 : -2;
            pitch = nearestScalePitch(pitch + direction, s.rootPitchClass, s.scale);
        }

        const int velocity = strongBeat
            ? rng.range(98, 114)
            : (guitarHit ? rng.range(82, 104) : rng.range(68, 92));

        int length = 1;
        float sustainBias = 0.0f;
        if (s.style == StyleId::DarkRockGothic) sustainBias = 0.14f;
        else if (s.style == StyleId::HeavyIndustrial) sustainBias = -0.06f;
        if (!strongBeat && rng.chance(0.18f + 0.58f * s.sustain + sustainBias))
            length = 2;
        if (strongBeat && !guitarHitAt(guitar, step + 1) &&
            rng.chance(0.10f + 0.38f * s.sustain + sustainBias))
            length = 2;

        result.steps[step].noteCount = 1;
        result.steps[step].notes[0] = {pitch, velocity, length};
    }

    // Reliable downbeat anchor for every phrase.
    if (result.steps[0].noteCount == 0) {
        result.steps[0].noteCount = 1;
        result.steps[0].notes[0] = {root, 110, 1};
    }

    sanitizeMonophonic(result);
    return result;
}

} // namespace midiator
