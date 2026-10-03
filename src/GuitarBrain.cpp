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

float guitarRootBias(StyleId style) {
    switch (style) {
        case StyleId::NDHIndustrial: return 0.10f;
        case StyleId::DarkRockGothic: return -0.09f;
        case StyleId::HeavyIndustrial: return 0.04f;
        case StyleId::ClassicHeavy: return 0.02f;
        case StyleId::Thrash: return 0.08f;
        case StyleId::Groove: return 0.06f;
        case StyleId::Death: return 0.04f;
        case StyleId::MelodicDeath: return -0.06f;
        case StyleId::Metalcore: return 0.08f;
        case StyleId::NuMetal: return 0.10f;
        case StyleId::Doom: return -0.04f;
        case StyleId::DjentProgressive: return 0.02f;
        case StyleId::Count: break;
    }
    return 0.0f;
}

float guitarOctaveFactor(StyleId style) {
    switch (style) {
        case StyleId::NDHIndustrial: return 0.55f;
        case StyleId::ClassicHeavy: return 0.82f;
        case StyleId::Thrash: return 0.55f;
        case StyleId::Groove: return 0.65f;
        case StyleId::Death: return 0.50f;
        case StyleId::Metalcore: return 0.55f;
        case StyleId::NuMetal: return 0.45f;
        case StyleId::Doom: return 0.40f;
        case StyleId::DjentProgressive: return 0.65f;
        default: return 1.0f;
    }
}

float guitarOctaveAdd(StyleId style) {
    if (style == StyleId::DarkRockGothic) return 0.12f;
    if (style == StyleId::MelodicDeath) return 0.14f;
    return 0.0f;
}

float guitarPalmFactor(StyleId style) {
    switch (style) {
        case StyleId::DarkRockGothic: return 0.48f;
        case StyleId::HeavyIndustrial: return 1.18f;
        case StyleId::ClassicHeavy: return 0.85f;
        case StyleId::Thrash: return 1.30f;
        case StyleId::Groove: return 1.15f;
        case StyleId::Death: return 1.20f;
        case StyleId::MelodicDeath: return 0.85f;
        case StyleId::Metalcore: return 1.25f;
        case StyleId::NuMetal: return 1.10f;
        case StyleId::Doom: return 0.35f;
        case StyleId::DjentProgressive: return 1.35f;
        default: return 1.0f;
    }
}

float guitarChordFactor(StyleId style) {
    switch (style) {
        case StyleId::DarkRockGothic: return 1.15f;
        case StyleId::HeavyIndustrial: return 0.90f;
        case StyleId::ClassicHeavy: return 1.25f;
        case StyleId::Thrash: return 0.85f;
        case StyleId::Groove: return 1.05f;
        case StyleId::Death: return 0.75f;
        case StyleId::MelodicDeath: return 1.10f;
        case StyleId::Metalcore: return 1.15f;
        case StyleId::NuMetal: return 1.20f;
        case StyleId::Doom: return 1.35f;
        case StyleId::DjentProgressive: return 0.78f;
        default: return 1.0f;
    }
}

int chooseDegree(Rng& rng, const GeneratorSettings& s, int stepInBar) {
    const auto& scale = kScales[static_cast<int>(s.scale)];

    // Each style has a different relationship to the pedal root.
    float rootProbability =
        0.28f + 0.30f * s.repetition + guitarRootBias(s.style);

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

bool shouldHit(Rng& rng, const GeneratorSettings& s, int globalStep, int archetype = 0) {
    if (globalStep == 0)
        return true;

    const int pos = globalStep % 16;
    archetype = std::clamp(archetype, 0, 5);

    // Six one-bar rhythm families per style. Bit n marks a preferred 16th.
    static constexpr uint16_t masks[static_cast<int>(StyleId::Count)][6] = {
        {0x5555u,0x0D0Du,0x00F1u,0x4515u,0xD145u,0x03F1u},
        {0x1111u,0x2449u,0x5151u,0x1485u,0x4129u,0x1053u},
        {0xF00Fu,0x0F07u,0x4B19u,0xF871u,0x69C3u,0x0FF1u},
        {0x5555u,0x7777u,0xD555u,0x5755u,0x555Du,0x1555u},
        {0xFFFFu,0x7777u,0xEEEEu,0xF7F7u,0xDDF7u,0x7FFFu},
        {0x5155u,0x4511u,0x5119u,0xA145u,0x214Du,0x4945u},
        {0xFFFFu,0xF7FFu,0xEFEFu,0xFF3Fu,0xDFFFu,0x7F7Fu},
        {0x5755u,0xD555u,0x7557u,0x555Du,0x3575u,0x5D55u},
        {0x0F11u,0xF011u,0x451Fu,0x11F1u,0xD10Fu,0x33C3u},
        {0x1115u,0x4109u,0x1149u,0x5011u,0x2141u,0x090Du},
        {0x1111u,0x1001u,0x0101u,0x0011u,0x1010u,0x4001u},
        {0x4B19u,0xA263u,0x31C5u,0x6925u,0xC319u,0x9661u}
    };

    const int styleIndex = std::clamp(static_cast<int>(s.style), 0,
                                      static_cast<int>(StyleId::Count) - 1);
    const bool preferred = (masks[styleIndex][archetype] & (uint16_t{1} << pos)) != 0;

    // Some riff families deliberately contain contiguous sixteenth-note
    // machine-gun bursts. At normal density these are strongly favored; at
    // high density the burst core becomes deterministic so the generator can
    // actually produce fast 16ths instead of only isolated syncopation.
    bool burstCore = false;
    switch (s.style) {
        case StyleId::NDHIndustrial:
            burstCore = (archetype == 2 && pos >= 4 && pos <= 7) ||
                        (archetype == 5 && pos >= 4 && pos <= 9); break;
        case StyleId::HeavyIndustrial:
            burstCore = (archetype == 0 && (pos <= 3 || pos >= 12)) ||
                        (archetype == 1 && ((pos <= 2) || (pos >= 8 && pos <= 11))) ||
                        (archetype == 3 && ((pos >= 4 && pos <= 6) || pos >= 11)) ||
                        (archetype == 5 && pos >= 4 && pos <= 11); break;
        case StyleId::Thrash:
            burstCore = archetype == 0 ||
                        (archetype == 1 && (pos % 4) != 3) ||
                        (archetype == 5 && pos >= 4); break;
        case StyleId::Death:
            burstCore = archetype == 0 || archetype == 2 || archetype == 4 ||
                        (archetype == 3 && pos >= 8); break;
        case StyleId::Metalcore:
            burstCore = (archetype == 0 && pos <= 3) ||
                        (archetype == 1 && pos >= 8 && pos <= 11); break;
        default: break;
    }

    if (burstCore && s.density >= 0.42f) {
        if (s.density >= 0.74f)
            return true;
        const float burstProbability =
            std::clamp(0.80f + 0.16f * s.complexity, 0.0f, 0.96f);
        return rng.chance(burstProbability);
    }

    float weight = preferred ? 0.92f : 0.16f;
    switch (s.style) {
        case StyleId::NDHIndustrial:
            if ((pos%4)==0) weight+=0.12f; if ((pos%2)==0) weight+=0.06f; break;
        case StyleId::DarkRockGothic:
            weight*=0.78f; if ((pos%4)==0) weight+=0.10f; break;
        case StyleId::HeavyIndustrial:
            if (pos==3||pos==7||pos==11||pos==15) weight+=0.20f+0.12f*s.complexity; break;
        case StyleId::ClassicHeavy:
            if ((pos%4)==0) weight+=0.10f; if ((pos%2)==0) weight+=0.08f; break;
        case StyleId::Thrash:
            weight*=1.05f; if ((pos%2)==0) weight+=0.08f;
            if (pos==3||pos==7||pos==11||pos==15) weight+=0.10f; break;
        case StyleId::Groove:
            if ((pos%4)==0) weight+=0.08f;
            if (pos==2||pos==6||pos==10||pos==14) weight+=0.18f;
            if (pos==3||pos==7||pos==11||pos==15) weight+=0.08f; break;
        case StyleId::Death:
            weight*=1.08f; if ((pos&1)!=0) weight+=0.08f; break;
        case StyleId::MelodicDeath:
            if ((pos%2)==0) weight+=0.10f; if ((pos%4)==0) weight+=0.08f; break;
        case StyleId::Metalcore:
            if (pos==0||pos==3||pos==6||pos==8||pos==11||pos==14) weight+=0.14f; break;
        case StyleId::NuMetal:
            weight*=0.72f; if (pos==2||pos==6||pos==10||pos==14) weight+=0.22f;
            if ((pos%4)==0) weight+=0.06f; break;
        case StyleId::Doom:
            weight*=0.55f; if ((pos%4)==0) weight+=0.24f;
            else if ((pos%2)==0) weight+=0.06f; break;
        case StyleId::DjentProgressive:
            if (pos==3||pos==5||pos==7||pos==10||pos==11||pos==14||pos==15)
                weight+=0.20f+0.08f*s.complexity; break;
        case StyleId::Count: break;
    }

    const float probability = std::clamp((0.18f + 1.02f * s.density) * weight, 0.0f, 0.97f);
    return rng.chance(probability);
}

int velocityFor(Rng& rng, bool palmMute, bool accent,
                int palmMuteVelocityThreshold) {
    if (palmMute) {
        // The threshold is exclusive: PM < VEL = 30 means every generated
        // palm mute is <=29. Keep velocity 0/1 free for dead/chuck/noise
        // articulations and leave a large safety gap below open notes (>=88).
        const int threshold = std::clamp(palmMuteVelocityThreshold, 2, 87);
        if (threshold == 41) {
            // Exact compatibility profile for pre-V13 projects, whose palm
            // mutes historically lived at 30..36 / 34..40.
            return rng.range(accent ? 34 : 30, accent ? 40 : 36);
        }
        const int maximum = threshold - 1;
        const int minimum = std::max(2, threshold - 8);
        const int normalHigh = std::max(minimum, threshold - 4);
        const int accentLow = std::max(minimum, threshold - 5);
        return rng.range(accent ? accentLow : minimum,
                         accent ? maximum : normalHigh);
    }

    return rng.range(accent ? 104 : 88, accent ? 120 : 106);
}

void createStepNote(Step& step,
                    int pitch,
                    bool powerChord,
                    bool palmMute,
                    bool accent,
                    int lengthSteps,
                    Rng& rng,
                    int palmMuteVelocityThreshold) {
    step.noteCount = 1;
    step.notes[0] = {pitch, velocityFor(rng, palmMute, accent,
                                        palmMuteVelocityThreshold), lengthSteps};

    if (powerChord && pitch <= 120) {
        step.noteCount = 2;
        int secondVelocity = step.notes[0].velocity - rng.range(0, 5);

        // Keep both notes of a dyad inside the same visible articulation zone.
        if (step.notes[0].velocity >= 88)
            secondVelocity = std::max(88, secondVelocity);
        else {
            const int threshold =
                std::clamp(palmMuteVelocityThreshold, 2, 87);
            if (threshold == 41) {
                // Exact pre-V13 compatibility: old dyad voices were clamped
                // to the fixed 30..40 palm-mute zone.
                secondVelocity = std::clamp(secondVelocity, 30, 40);
            } else {
                const int pmMax = threshold - 1;
                const int pmMin = std::max(2, pmMax - 7);
                secondVelocity = std::clamp(secondVelocity, pmMin, pmMax);
            }
        }

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


struct GuitarPlayabilityProfile {
    int adjacentMaxJump = 7;
    int eighthMaxJump = 12;
    bool holdRepeatedArticulation = true;
    bool clipAdjacentTails = false;
};

GuitarPlayabilityProfile guitarPlayabilityProfile(StyleId style) {
    switch (style) {
        case StyleId::ClassicHeavy:      return {7, 12, true,  false};
        case StyleId::Thrash:            return {5,  9, true,  true};
        case StyleId::Groove:            return {7, 11, true,  false};
        case StyleId::Death:             return {5,  9, true,  true};
        case StyleId::MelodicDeath:      return {12,16, true,  false};
        case StyleId::Metalcore:         return {5, 10, true,  true};
        case StyleId::NuMetal:           return {7, 10, true,  false};
        case StyleId::Doom:              return {12,14, false, false};
        case StyleId::DjentProgressive:  return {5,  9, true,  true};
        case StyleId::NDHIndustrial:
        case StyleId::DarkRockGothic:
        case StyleId::HeavyIndustrial:
        case StyleId::Count:
            return {24, 24, false, false};
    }
    return {24, 24, false, false};
}

int nearestPlayableOctave(int pitch,
                          int previousPitch,
                          int base,
                          int maxPitch) {
    int best = pitch;
    int bestDistance = std::abs(pitch - previousPitch);

    for (int octaveShift : {-12, 12}) {
        const int candidate = pitch + octaveShift;
        if (candidate < base || candidate > std::min(maxPitch, base + 24))
            continue;

        const int distance = std::abs(candidate - previousPitch);
        if (distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

void forceArticulationZone(Step& step, bool palmMute, bool accent,
                           int palmMuteVelocityThreshold) {
    if (step.noteCount <= 0)
        return;

    const int pmMax = std::clamp(palmMuteVelocityThreshold - 1, 2, 86);
    const int pmNormal = std::max(2, pmMax - (accent ? 1 : 3));
    const int primaryVelocity =
        palmMute ? pmNormal : (accent ? 108 : 98);
    step.notes[0].velocity = primaryVelocity;

    for (int noteIndex = 1; noteIndex < step.noteCount; ++noteIndex) {
        step.notes[noteIndex].velocity = palmMute
            ? std::max(2, primaryVelocity - 3)
            : std::max(88, primaryVelocity - 3);
    }
}

// Convert generated pitch classes into motions a guitarist can execute more
// naturally. Rhythm and harmonic pitch classes stay untouched; only octave
// placement, repeated-note articulation continuity and fast note tails are
// shaped. The original three styles intentionally keep their historical path.
void applyGuitarPlayability(Phrase& phrase,
                            StyleId style,
                            int base,
                            int palmMuteVelocityThreshold) {
    if (static_cast<int>(style) < static_cast<int>(StyleId::ClassicHeavy))
        return;

    const auto profile = guitarPlayabilityProfile(style);
    int previousStep = -1;
    int previousPitch = -1;
    bool previousPalmMute = false;

    for (int i = 0; i < phrase.usedSteps(); ++i) {
        auto& step = phrase.steps[i];
        if (step.noteCount <= 0)
            continue;

        if (previousStep >= 0) {
            const int gap = i - previousStep;
            const int jump = std::abs(step.notes[0].pitch - previousPitch);
            const int allowedJump =
                gap == 1 ? profile.adjacentMaxJump :
                gap == 2 ? profile.eighthMaxJump : 24;

            if (jump > allowedJump) {
                const int maxPrimaryPitch = step.noteCount > 1 ? 120 : 127;
                const int shapedPitch = nearestPlayableOctave(
                    step.notes[0].pitch, previousPitch, base, maxPrimaryPitch);
                const int delta = shapedPitch - step.notes[0].pitch;

                if (delta != 0) {
                    for (int noteIndex = 0; noteIndex < step.noteCount; ++noteIndex)
                        step.notes[noteIndex].pitch += delta;
                }
            }

            // A repeated pedal tone normally stays under the same picking /
            // muting gesture through a fast run. Random mute/open flips on
            // adjacent repeated 16ths sound more like MIDI than guitar.
            const bool currentPalmMute = step.notes[0].velocity < 88;
            const bool repeatedPitch =
                step.notes[0].pitch == previousPitch;
            const bool accent = (i % 4) == 0;
            if (profile.holdRepeatedArticulation &&
                gap == 1 && repeatedPitch && !accent &&
                currentPalmMute != previousPalmMute) {
                forceArticulationZone(step, previousPalmMute, false,
                                      palmMuteVelocityThreshold);
            }

            // High-speed chug/tremolo styles should not leave a previous note
            // ringing through the immediately following 16th onset.
            if (profile.clipAdjacentTails && gap == 1) {
                auto& previous = phrase.steps[previousStep];
                for (int noteIndex = 0; noteIndex < previous.noteCount; ++noteIndex)
                    previous.notes[noteIndex].lengthSteps =
                        std::min(previous.notes[noteIndex].lengthSteps, 1);
            }
        }

        previousStep = i;
        previousPitch = step.notes[0].pitch;
        previousPalmMute = step.notes[0].velocity < 88;
    }
}

void sanitizeOverlaps(Phrase& phrase) {
    const int used = phrase.usedSteps();

    for (int stepIndex = 0; stepIndex < used; ++stepIndex) {
        auto& step = phrase.steps[stepIndex];

        for (int noteIndex = 0; noteIndex < step.noteCount; ++noteIndex) {
            auto& note = step.notes[noteIndex];
            const int requestedLength = std::max(1, note.lengthSteps);

            // Do not let a note extend beyond the phrase boundary. The phrase
            // loops, so a tail crossing the end could overlap the next cycle's
            // downbeat (especially the repeated root/power-chord tones).
            note.lengthSteps = std::min(requestedLength, std::max(1, used - stepIndex));
            const int endStep = stepIndex + note.lengthSteps;

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
    // Complexity controls riff movement/busyness (register shifts, characteristic
    // tones, burst activity and bar development). It intentionally does not mean
    // "more different pitch classes", which would fight the pedal-riff identity.
    s.complexity = std::clamp(s.complexity, 0.0f, 1.0f);
    s.repetition = std::clamp(s.repetition, 0.0f, 1.0f);
    s.powerChordChance = std::clamp(s.powerChordChance, 0.0f, 1.0f);
    s.palmMuteChance = std::clamp(s.palmMuteChance, 0.0f, 1.0f);
    s.palmMuteVelocityThreshold = std::clamp(s.palmMuteVelocityThreshold, 2, 87);

    Phrase result{};
    result.bars = s.bars;

    Rng rng(seed);
    const int rhythmArchetype = rng.range(0, 5);
    const auto& scale = scaleDefinition(s.scale);
    const int base = rootBaseForPitchClass(s.rootPitchClass, s.lowRootMidi);

    auto makeMusicalStep = [&](int stepInBar, bool preferRoot) {
        Step out{};
        const bool accent = (stepInBar % 4) == 0;

        int degree = preferRoot ? 0 : chooseDegree(rng, s, stepInBar);
        int pitch = base + scale.intervals[degree];

        float octaveChance =
            (0.08f + 0.20f * s.complexity) * guitarOctaveFactor(s.style) +
            guitarOctaveAdd(s.style);
        if (!preferRoot && rng.chance(octaveChance))
            pitch += 12;

        pitch = clampMusicalPitch(pitch, base);

        float palmFactor = (accent ? 0.68f : 1.0f) * guitarPalmFactor(s.style);
        const bool palmMute = rng.chance(std::clamp(s.palmMuteChance * palmFactor, 0.0f, 1.0f));

        // Palm-muted power chords are musically valid, so chord generation must
        // not depend on the note being "open".
        float chordChance =
            s.powerChordChance * (accent ? 1.0f : 0.62f) * guitarChordFactor(s.style);
        // New styles use the final deterministic chord-count pass below.
        // Keeping chord lottery out of the structural RNG means changing the
        // Power Chords amount cannot rewrite their rhythm/pitch topology.
        const bool deterministicChordPass =
            static_cast<int>(s.style) >= static_cast<int>(StyleId::ClassicHeavy);
        const bool powerChord = !deterministicChordPass &&
            s.powerChordsEnabled &&
            rng.chance(std::clamp(chordChance, 0.0f, 1.0f));

        int openLength=2;
        float longChance=0.34f+0.28f*(1.0f-s.density);
        if (s.style==StyleId::Doom) { openLength=4; longChance+=0.34f; }
        else if (s.style==StyleId::MelodicDeath) longChance+=0.08f;
        const int length=palmMute?1:(rng.chance(longChance)?openLength:1);

        createStepNote(out, pitch, powerChord, palmMute, accent, length, rng, s.palmMuteVelocityThreshold);
        return out;
    };

    // A full 4/4 bar is the primary motif unit. This prevents the default
    // generator from merely repeating an 8-step half-bar twice.
    std::array<Step, kStepsPerBar> baseBar{};

    for (int i = 0; i < kStepsPerBar; ++i) {
        if (!shouldHit(rng, s, i, rhythmArchetype))
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
                    createStepNote(movement, pitch, false, palmMute, false, palmMute ? 1 : 2, rng, s.palmMuteVelocityThreshold);
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
        const float roleFactor = roleFactorForBar(bar);
        const float development = 1.0f - s.repetition;
        const float mutationChance =
            development <= 0.0001f
                ? 0.0f
                : (0.015f + development * (0.12f + 0.30f * s.complexity)) *
                    roleFactor;

        for (int i = 0; i < kStepsPerBar; ++i) {
            Step current = baseBar[i];

            if (bar > 0 && rng.chance(mutationChance)) {
                const float action = rng.unit();

                if (current.noteCount == 0) {
                    if (action < 0.55f && shouldHit(rng, s, i, rhythmArchetype))
                        current = makeMusicalStep(i, false);
                } else if (action < 0.18f && i != 0) {
                    current = {};
                } else if (action < 0.76f) {
                    const bool oldPowerChord = current.noteCount == 2;
                    const bool oldPalmMute = current.notes[0].velocity < 84;
                    const int oldLength = current.notes[0].lengthSteps;

                    int degree = chooseDegree(rng, s, i);
                    int pitch = clampMusicalPitch(base + scale.intervals[degree], base);

                    createStepNote(current, pitch, oldPowerChord, oldPalmMute,
                                   (i % 4) == 0, oldLength, rng, s.palmMuteVelocityThreshold);
                } else {
                    current = makeMusicalStep(i, false);
                }
            }

            // Turnaround bars get a little extra activity/change in beat 4,
            // creating a phrase ending rather than a flat repeated loop.
            if (bar > 0 && (bar % 4) == 3 && i >= 12 &&
                rng.chance(development * (0.10f + 0.18f * s.complexity))) {
                if (i == 15 && rng.chance(0.60f))
                    current = {};
                else
                    current = makeMusicalStep(i, false);
            }

            result.steps[bar * kStepsPerBar + i] = current;
        }

        // Repetition must control audible bar-to-bar development across the
        // whole 0..100% range. Earlier versions mostly changed pitch while
        // retaining the same onset skeleton, so even low/mid Repetition still
        // sounded like one bar copied eight times.
        if (bar > 0 && development > 0.0001f) {
            const int role = bar % 4;
            const float rhythmWeight =
                role == 1 ? 3.0f :   // A': recognizable but clearly alive
                role == 2 ? 5.0f :   // answer/development
                role == 3 ? 7.0f :   // turnaround
                            4.0f;     // second four-bar phrase restart

            // Repetition decides how far a bar may depart from the motif;
            // Complexity decides how busy that development becomes. Keeping
            // these independent prevents low-Complexity riffs from receiving
            // the same forced rhythmic churn as high-Complexity riffs.
            const float developmentComplexity =
                0.35f + 0.65f * s.complexity;

            int minRhythmChanges =
                static_cast<int>(std::ceil(
                    development * rhythmWeight * developmentComplexity));
            if (bar >= 4)
                minRhythmChanges += static_cast<int>(std::ceil(
                    development * (0.5f + 1.5f * s.complexity)));
            minRhythmChanges = std::clamp(minRhythmChanges, 0, 12);

            const int minStructuralChanges = std::clamp(
                minRhythmChanges +
                    static_cast<int>(std::ceil(
                        development * (0.75f + 1.25f * s.complexity))),
                minRhythmChanges, 14);

            static constexpr int developmentPositions[] = {
                15, 14, 11, 10, 7, 6, 13, 3, 9, 5, 12, 8, 4, 2, 1
            };

            auto differsFromBase = [&](int pos) {
                return !(result.steps[bar * kStepsPerBar + pos] == baseBar[pos]);
            };
            auto rhythmDiffersFromBase = [&](int pos) {
                const bool now =
                    result.steps[bar * kStepsPerBar + pos].noteCount > 0;
                const bool seed = baseBar[pos].noteCount > 0;
                return now != seed;
            };

            int actualChanges = 0;
            int actualRhythmChanges = 0;
            for (int pos = 0; pos < kStepsPerBar; ++pos) {
                actualChanges += differsFromBase(pos) ? 1 : 0;
                actualRhythmChanges += rhythmDiffersFromBase(pos) ? 1 : 0;
            }

            // Rotate the preferred development locations per bar. Using the
            // same first N positions on every bar made different bars share the
            // same onset skeleton even when each one technically contained
            // mutations. A stride of four across the 15 non-downbeat candidates
            // gives each bar a different rhythmic answer while preserving the
            // stable step-0 anchor.
            constexpr int developmentCount =
                static_cast<int>(std::size(developmentPositions));
            // Move development in two-bar phrases rather than every bar:
            // A/A' retain a common rhythmic vocabulary, the answer/turnaround
            // pair moves elsewhere, and bars 5-8 develop again. This gives
            // audible macro motion without destroying motif recognition.
            const int developmentOffset =
                ((bar / 2) * 2) % developmentCount;

            for (int orderIndex = 0; orderIndex < developmentCount; ++orderIndex) {
                if (actualChanges >= minStructuralChanges &&
                    actualRhythmChanges >= minRhythmChanges)
                    break;

                const int candidate =
                    developmentPositions[(orderIndex + developmentOffset) %
                                         developmentCount];
                auto& target = result.steps[bar * kStepsPerBar + candidate];
                const auto& seedStep = baseBar[candidate];

                const bool needRhythm =
                    actualRhythmChanges < minRhythmChanges &&
                    !rhythmDiffersFromBase(candidate);

                if (needRhythm) {
                    // Toggle the seed occupancy. This makes Repetition audibly
                    // rhythmic instead of merely changing pitches on the same grid.
                    if (seedStep.noteCount == 0)
                        target = makeMusicalStep(candidate, false);
                    else
                        target = {};
                } else if (!differsFromBase(candidate)) {
                    if (seedStep.noteCount == 0) {
                        target = makeMusicalStep(candidate, false);
                    } else {
                        const bool oldPowerChord = seedStep.noteCount == 2;
                        const bool oldPalmMute = seedStep.notes[0].velocity < 84;
                        const int oldLength = seedStep.notes[0].lengthSteps;

                        int degree = chooseDegree(rng, s, candidate);
                        int pitch = clampMusicalPitch(base + scale.intervals[degree], base);
                        if (pitch == seedStep.notes[0].pitch) {
                            const int fallbackDegree =
                                (scale.count > 2) ? ((degree + 2) % scale.count) : 1;
                            pitch = clampMusicalPitch(
                                base + scale.intervals[fallbackDegree], base);
                        }

                        createStepNote(target, pitch, oldPowerChord, oldPalmMute,
                                       (candidate % 4) == 0, oldLength, rng, s.palmMuteVelocityThreshold);
                        if (target == seedStep)
                            target = {};
                    }
                }

                actualChanges = 0;
                actualRhythmChanges = 0;
                for (int pos = 0; pos < kStepsPerBar; ++pos) {
                    actualChanges += differsFromBase(pos) ? 1 : 0;
                    actualRhythmChanges += rhythmDiffersFromBase(pos) ? 1 : 0;
                }
            }

            // Two independently developed bars can still land on the same
            // result by chance. At any non-100% Repetition setting, guarantee
            // at least one audible rhythmic distinction from the immediately
            // preceding bar.
            bool identicalToPrevious = true;
            for (int pos = 0; pos < kStepsPerBar; ++pos) {
                if (!(result.steps[bar * kStepsPerBar + pos] ==
                      result.steps[(bar - 1) * kStepsPerBar + pos])) {
                    identicalToPrevious = false;
                    break;
                }
            }
            if (identicalToPrevious) {
                static constexpr int antiClonePositions[] = {
                    15, 11, 7, 14, 10, 6, 13, 9, 5, 3
                };
                const int pos =
                    antiClonePositions[static_cast<size_t>(bar) %
                                       std::size(antiClonePositions)];
                auto& target = result.steps[bar * kStepsPerBar + pos];
                if (target.noteCount > 0)
                    target = {};
                else
                    target = makeMusicalStep(pos, false);
            }
        }
    }

    applyGuitarPlayability(result, s.style, base,
                               s.palmMuteVelocityThreshold);

    // POWER CHORDS is a user-facing musical amount, not merely a tiny
    // per-event lottery. After phrase development, make sure the final phrase
    // contains a representative number of dyads. This also prevents sparse
    // riffs from accidentally containing no power chords at useful settings.
    if (s.powerChordsEnabled && s.powerChordChance > 0.0f) {
        int eligibleHits = 0;
        int existingChords = 0;
        for (int i = 0; i < result.usedSteps(); ++i) {
            const auto& step = result.steps[i];
            if (step.noteCount <= 0 || step.notes[0].pitch > 120)
                continue;
            ++eligibleHits;
            if (step.noteCount > 1)
                ++existingChords;
        }

        float styleFactor = 1.0f;
        switch (s.style) {
            case StyleId::DarkRockGothic: styleFactor=1.10f; break;
            case StyleId::HeavyIndustrial: styleFactor=0.92f; break;
            case StyleId::ClassicHeavy: styleFactor=1.20f; break;
            case StyleId::Thrash: styleFactor=0.86f; break;
            case StyleId::Groove: styleFactor=1.05f; break;
            case StyleId::Death: styleFactor=0.78f; break;
            case StyleId::MelodicDeath: styleFactor=1.08f; break;
            case StyleId::Metalcore: styleFactor=1.12f; break;
            case StyleId::NuMetal: styleFactor=1.18f; break;
            case StyleId::Doom: styleFactor=1.28f; break;
            case StyleId::DjentProgressive: styleFactor=0.82f; break;
            default: break;
        }

        int targetChords = static_cast<int>(std::lround(
            static_cast<float>(eligibleHits) * s.powerChordChance * styleFactor));
        targetChords = std::clamp(targetChords, 0, eligibleHits);
        if (s.powerChordChance >= 0.10f && eligibleHits > 0)
            targetChords = std::max(1, targetChords);

        // Prefer musically strong locations first, then fill other hits.
        static constexpr int preferredPositions[] = {
            0, 8, 4, 12, 6, 14, 2, 10, 3, 11, 7, 15, 5, 13, 1, 9
        };

        for (int bar = 0; bar < result.bars && existingChords < targetChords; ++bar) {
            for (int local : preferredPositions) {
                if (existingChords >= targetChords)
                    break;
                auto& step = result.steps[bar * kStepsPerBar + local];
                if (step.noteCount != 1 || step.notes[0].pitch > 120)
                    continue;

                step.noteCount = 2;
                step.notes[1] = step.notes[0];
                step.notes[1].pitch = step.notes[0].pitch + 7;
                const int pmMin = std::max(
                    2, std::clamp(s.palmMuteVelocityThreshold, 2, 87) - 8);
                step.notes[1].velocity = std::max(
                    step.notes[0].velocity >= 88 ? 88 : pmMin,
                    step.notes[0].velocity - 3);
                ++existingChords;
            }
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
        // Variation must be musically audible at useful mid settings. The old
        // curve selected too few steps and often changed only articulation.
        if (!rng.chance(0.10f + 0.82f * a))
            continue;

        auto& dst = result.steps[step];
        const bool accent = (step % 4) == 0;

        const float action = rng.unit();

        if (dst.noteCount == 0) {
            if (action < (0.18f + 0.52f * a) && shouldHit(rng, s, step)) {
                const int degree = chooseDegree(rng, s, step % 16);
                int pitch = clampMusicalPitch(base + scale.intervals[degree], base);
                const bool palmMute = rng.chance(s.palmMuteChance);
                createStepNote(dst, pitch, false, palmMute, accent, palmMute ? 1 : 2, rng, s.palmMuteVelocityThreshold);
            }
            continue;
        }

        if (action < (0.08f + 0.26f * a) && step != 0) {
            dst = {};
            continue;
        }

        if (action < 0.72f) {
            // Pitch mutation remains inside the selected root + scale.
            const int degree = chooseDegree(rng, s, step % 16);
            int pitch = clampMusicalPitch(base + scale.intervals[degree], base);

            // Keep strong identity by often retaining the pedal root.
            if (rng.chance(0.16f + 0.28f * s.repetition))
                pitch = base;

            const int oldPitch = dst.notes[0].pitch;
            if (pitch == oldPitch && scale.count > 1) {
                int fallbackDegree = rng.range(1, scale.count - 1);
                pitch = clampMusicalPitch(base + scale.intervals[fallbackDegree], base);
                if (pitch == oldPitch)
                    pitch = clampMusicalPitch(base + scale.intervals[(fallbackDegree + 1) % scale.count], base);
            }

            const bool palmMute = dst.notes[0].velocity < 84;
            const bool powerChord = s.powerChordsEnabled && dst.noteCount == 2;
            const int length = dst.notes[0].lengthSteps;
            createStepNote(dst, pitch, powerChord, palmMute, accent, length, rng, s.palmMuteVelocityThreshold);
        } else {
            // Articulation/rhythm variation without changing tonal center.
            const bool palmMute = rng.chance(s.palmMuteChance);
            const bool powerChord = s.powerChordsEnabled &&
                rng.chance(std::clamp(s.powerChordChance * (accent ? 1.0f : 0.62f), 0.0f, 1.0f));
            const int length = palmMute ? 1 : rng.range(1, 2);
            const int pitch = dst.notes[0].pitch;
            createStepNote(dst, pitch, powerChord, palmMute, accent, length, rng, s.palmMuteVelocityThreshold);
        }
    }

    // Preserve a reliable downbeat anchor.
    if (result.steps[0].noteCount == 0)
        createStepNote(result.steps[0], base, false, true, true, 1, rng, s.palmMuteVelocityThreshold);

    auto structurallyDifferent = [](const Step& x, const Step& y) {
        if (x.noteCount != y.noteCount)
            return true;
        if (x.noteCount == 0)
            return false;
        return x.notes[0].pitch != y.notes[0].pitch ||
               x.notes[0].lengthSteps != y.notes[0].lengthSteps ||
               (x.noteCount > 1) != (y.noteCount > 1);
    };

    int structuralChanges = 0;
    for (int i = 0; i < used; ++i)
        structuralChanges += structurallyDifferent(result.steps[i], source.steps[i]) ? 1 : 0;

    // Mid-range variation must already be clearly audible. Do not merely hope
    // that random choices produce enough change: enforce a bounded minimum.
    // 35% on the default 2-bar phrase => 6 structurally changed steps.
    const int minimumStructuralChanges = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(used) * (0.08 + 0.30 * a))),
        1, std::max(1, used - 1));

    for (int pass = 0; structuralChanges < minimumStructuralChanges && pass < used * 2; ++pass) {
        const int step = (3 + pass * 7) % used;
        if (step == 0 || structurallyDifferent(result.steps[step], source.steps[step]))
            continue;

        auto& dst = result.steps[step];
        const auto& src = source.steps[step];
        const bool accent = (step % 4) == 0;

        if (src.noteCount == 0) {
            int degree = 1 + (rng.range(0, std::max(0, scale.count - 2)));
            degree = std::clamp(degree, 1, std::max(1, scale.count - 1));
            int pitch = clampMusicalPitch(base + scale.intervals[degree], base);
            const bool palmMute = rng.chance(s.palmMuteChance);
            createStepNote(dst, pitch, false, palmMute, accent, palmMute ? 1 : 2, rng, s.palmMuteVelocityThreshold);
        } else {
            const int oldPitch = src.notes[0].pitch;
            int pitch = oldPitch;
            for (int attempt = 0; attempt < scale.count && pitch == oldPitch; ++attempt) {
                const int degree = (attempt + 1 + (step % std::max(1, scale.count - 1))) % scale.count;
                pitch = clampMusicalPitch(base + scale.intervals[degree], base);
            }

            if (pitch != oldPitch) {
                const bool palmMute = src.notes[0].velocity < 84;
                const bool powerChord = s.powerChordsEnabled && src.noteCount == 2;
                createStepNote(dst, pitch, powerChord, palmMute, accent,
                               src.notes[0].lengthSteps, rng, s.palmMuteVelocityThreshold);
            } else {
                // Extremely defensive fallback: changing onset is still an
                // audible structural change and cannot accidentally equal src.
                dst = {};
            }
        }

        if (structurallyDifferent(dst, src))
            ++structuralChanges;
    }

    applyGuitarPlayability(result, s.style, base,
                               s.palmMuteVelocityThreshold);
    sanitizeOverlaps(result);
    return result;
}

} // namespace midiator
