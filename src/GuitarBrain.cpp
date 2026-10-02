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

    // Each style has a different relationship to the pedal root.
    float rootProbability = 0.28f + 0.30f * s.repetition;
    if (s.style == StyleId::NDHIndustrial)
        rootProbability += 0.10f;
    else if (s.style == StyleId::DarkRockGothic)
        rootProbability -= 0.09f;
    else if (s.style == StyleId::HeavyIndustrial)
        rootProbability += 0.04f;

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

    // Three genuine rhythm languages, each with six deliberately different
    // riff families. Bit n marks a preferred sixteenth-note onset in one bar.
    // NEW RIFF can therefore move between genuinely different groove skeletons
    // without changing the selected style.
    static constexpr uint16_t masks[static_cast<int>(StyleId::Count)][6] = {
        {
            0x5555u, // straight eighth-note machine
            0x0D0Du, // stop/start blocks
            0x00F1u, // four-sixteenth machine burst
            0x4515u, // sparse verse-like pedal pattern
            0xD145u, // back-half push / answer
            0x03F1u  // six-sixteenth drive after the anchor
        },
        {
            0x1111u, // broad quarter-note pulse
            0x2449u, // open melodic gaps
            0x5151u, // wide eighth-note frame
            0x1485u, // delayed dark-rock answer
            0x4129u, // asymmetrical melodic pulse
            0x1053u  // long spaces with clustered response
        },
        {
            0xF00Fu, // two four-sixteenth attack bursts
            0x0F07u, // three-note pickup into four-note stutter
            0x4B19u, // broken accents
            0xF871u, // split burst / late machine-gun ending
            0x69C3u, // split-beat machine pattern
            0x0FF1u  // sustained eight-sixteenth pressure run
        }
    };

    const int styleIndex = std::clamp(static_cast<int>(s.style), 0,
                                      static_cast<int>(StyleId::Count) - 1);
    const bool preferred = (masks[styleIndex][archetype] & (uint16_t{1} << pos)) != 0;

    // Some riff families deliberately contain contiguous sixteenth-note
    // machine-gun bursts. At normal density these are strongly favored; at
    // high density the burst core becomes deterministic so the generator can
    // actually produce fast 16ths instead of only isolated syncopation.
    bool burstCore = false;
    if (s.style == StyleId::NDHIndustrial) {
        burstCore = (archetype == 2 && pos >= 4 && pos <= 7) ||
                    (archetype == 5 && pos >= 4 && pos <= 9);
    } else if (s.style == StyleId::HeavyIndustrial) {
        burstCore = (archetype == 0 && (pos <= 3 || pos >= 12)) ||
                    (archetype == 1 && ((pos <= 2) || (pos >= 8 && pos <= 11))) ||
                    (archetype == 3 && ((pos >= 4 && pos <= 6) || pos >= 11)) ||
                    (archetype == 5 && pos >= 4 && pos <= 11);
    }

    if (burstCore && s.density >= 0.42f) {
        if (s.density >= 0.74f)
            return true;
        const float burstProbability =
            std::clamp(0.80f + 0.16f * s.complexity, 0.0f, 0.96f);
        return rng.chance(burstProbability);
    }

    float weight = preferred ? 0.92f : 0.16f;
    if (s.style == StyleId::NDHIndustrial) {
        if ((pos % 4) == 0) weight += 0.12f;
        if ((pos % 2) == 0) weight += 0.06f;
    } else if (s.style == StyleId::DarkRockGothic) {
        weight *= 0.78f; // more air and longer spaces
        if ((pos % 4) == 0) weight += 0.10f;
    } else {
        // Heavy Industrial deliberately favors displaced late sixteenths.
        if (pos == 3 || pos == 7 || pos == 11 || pos == 15)
            weight += 0.20f + 0.12f * s.complexity;
    }

    const float probability = std::clamp((0.18f + 1.02f * s.density) * weight, 0.0f, 0.97f);
    return rng.chance(probability);
}

int velocityFor(Rng& rng, bool palmMute, bool accent) {
    if (palmMute) {
        // Keep normal palm mutes in a conservative low-velocity guitar zone.
        // Velocity 0/1 stays free for dead/chuck/noise articulations.
        return rng.range(accent ? 34 : 30, accent ? 40 : 36);
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
            secondVelocity = std::clamp(secondVelocity, 30, 40);

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

        float octaveChance = 0.08f + 0.20f * s.complexity;
        if (s.style == StyleId::DarkRockGothic)
            octaveChance += 0.12f;
        else if (s.style == StyleId::NDHIndustrial)
            octaveChance *= 0.55f;
        if (!preferRoot && rng.chance(octaveChance))
            pitch += 12;

        pitch = clampMusicalPitch(pitch, base);

        float palmFactor = accent ? 0.68f : 1.0f;
        if (s.style == StyleId::DarkRockGothic)
            palmFactor *= 0.48f;
        else if (s.style == StyleId::HeavyIndustrial)
            palmFactor *= 1.18f;
        const bool palmMute = rng.chance(std::clamp(s.palmMuteChance * palmFactor, 0.0f, 1.0f));

        // Palm-muted power chords are musically valid, so chord generation must
        // not depend on the note being "open".
        float chordChance = s.powerChordChance * (accent ? 1.0f : 0.62f);
        if (s.style == StyleId::DarkRockGothic)
            chordChance *= 1.15f;
        else if (s.style == StyleId::HeavyIndustrial)
            chordChance *= 0.90f;
        const bool powerChord = s.powerChordsEnabled &&
            rng.chance(std::clamp(chordChance, 0.0f, 1.0f));

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
                                   (i % 4) == 0, oldLength, rng);
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
                                       (candidate % 4) == 0, oldLength, rng);
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
        if (s.style == StyleId::DarkRockGothic)
            styleFactor = 1.10f;
        else if (s.style == StyleId::HeavyIndustrial)
            styleFactor = 0.92f;

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
                step.notes[1].velocity = std::max(
                    step.notes[0].velocity >= 88 ? 88 : 30,
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
                createStepNote(dst, pitch, false, palmMute, accent, palmMute ? 1 : 2, rng);
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
            createStepNote(dst, pitch, powerChord, palmMute, accent, length, rng);
        } else {
            // Articulation/rhythm variation without changing tonal center.
            const bool palmMute = rng.chance(s.palmMuteChance);
            const bool powerChord = s.powerChordsEnabled &&
                rng.chance(std::clamp(s.powerChordChance * (accent ? 1.0f : 0.62f), 0.0f, 1.0f));
            const int length = palmMute ? 1 : rng.range(1, 2);
            const int pitch = dst.notes[0].pitch;
            createStepNote(dst, pitch, powerChord, palmMute, accent, length, rng);
        }
    }

    // Preserve a reliable downbeat anchor.
    if (result.steps[0].noteCount == 0)
        createStepNote(result.steps[0], base, false, true, true, 1, rng);

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
            createStepNote(dst, pitch, false, palmMute, accent, palmMute ? 1 : 2, rng);
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
                               src.notes[0].lengthSteps, rng);
            } else {
                // Extremely defensive fallback: changing onset is still an
                // audible structural change and cannot accidentally equal src.
                dst = {};
            }
        }

        if (structurallyDifferent(dst, src))
            ++structuralChanges;
    }

    sanitizeOverlaps(result);
    return result;
}

} // namespace midiator
