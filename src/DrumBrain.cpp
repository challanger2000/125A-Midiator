#include "DrumBrain.h"

#include <algorithm>
#include <cmath>

namespace midiator {
namespace {

class Rng {
public:
    explicit Rng(uint32_t seed) : state_(seed ? seed : 0x125AD00Du) {}
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

bool phraseHit(const Phrase& p, int step) {
    return step >= 0 && step < p.usedSteps() && p.steps[step].noteCount > 0;
}

void addHit(DrumStep& step, DrumVoice voice, int velocity) {
    if (step.hitCount >= kMaxDrumHitsPerStep)
        return;
    for (int i = 0; i < step.hitCount; ++i)
        if (step.hits[i].voice == voice)
            return;
    step.hits[step.hitCount++] = {voice, std::clamp(velocity, 1, 126)};
}

int humanizedVelocity(Rng& rng, int base, float humanize) {
    const int span = static_cast<int>(std::lround(10.0f * std::clamp(humanize, 0.0f, 1.0f)));
    return std::clamp(base + rng.range(-span, span), 1, 126);
}

} // namespace

DrumMidiMap DrumMidiMap::preset(DrumMapId id) {
    DrumMidiMap m{};

    // Verified General MIDI principal-kit mapping. GhostSnare deliberately
    // shares the acoustic snare note: ghost notes are a velocity/articulation
    // treatment of the same drum, not an electric-snare substitution.
    m.note = {
        36, // Kick
        38, // Snare
        42, // Closed Hat
        46, // Open Hat
        49, // Crash 1
        51, // Ride Cymbal 1
        41, // Low Floor Tom
        47, // Low-Mid Tom
        50, // High Tom
        38  // Ghost Snare -> same acoustic snare, lower velocity
    };

    switch (id) {
        case DrumMapId::GeneralMidi:
            break;

        case DrumMapId::EZdrummer3:
            // Toontrack EZdrummer 3 Standard Layout:
            // Kick Hit 36, Snare Center 38, Hi-Hat Closed Tip 42,
            // Hi-Hat Open Edge 2 46, Crash 1 Crash 55, Ride Edge 52,
            // Floortom 2 Center 41, Racktom 2 Center 47,
            // Racktom 1 Center 48.
            m.note = {
                36, 38, 42, 46, 55, 52, 41, 47, 48, 38
            };
            break;

        case DrumMapId::PerfectDrums:
            // Perfect Drums default layout (manual section 7.3):
            // C1 kick=36, D1 snare=38, E3 closed-tip hat=64,
            // B1 open hat=47, D2 Crash 1 edge=50, E2 Ride tip=52,
            // F1/G1/A1 Tom 3/2/1 centers=41/43/45.
            m.note = {
                36, 38, 64, 47, 50, 52, 41, 43, 45, 38
            };
            break;

        case DrumMapId::SuperiorDrummer3:
            // Compatibility fallback only. SD3 exposes its current MIDI
            // Mapping Layout and mappings can vary with library/preset.
            // Do not present this fallback as a verified universal SD3 map.
            break;

        case DrumMapId::SSD55:
            // Compatibility fallback only. SSD5.5's global Map tab is
            // user/preset configurable, so no single factory-independent
            // static mapping is claimed here.
            break;

        case DrumMapId::Custom:
        case DrumMapId::Count:
            break;
    }
    return m;
}

bool DrumMidiMap::presetIsVerified(DrumMapId id) {
    switch (id) {
        case DrumMapId::GeneralMidi:
        case DrumMapId::EZdrummer3:
        case DrumMapId::PerfectDrums:
            return true;
        case DrumMapId::SuperiorDrummer3:
        case DrumMapId::SSD55:
        case DrumMapId::Custom:
        case DrumMapId::Count:
            return false;
    }
    return false;
}

DrumPhrase DrumBrain::generate(const Phrase& guitar,
                               const Phrase& bass,
                               const DrumSettings& in,
                               uint32_t seed) {
    DrumSettings s = in;
    s.follow = std::clamp(s.follow, 0.0f, 1.0f);
    s.density = std::clamp(s.density, 0.0f, 1.0f);
    s.complexity = std::clamp(s.complexity, 0.0f, 1.0f);
    s.humanize = std::clamp(s.humanize, 0.0f, 1.0f);

    float kickContextFactor = 1.0f;
    float independentKickFactor = 1.0f;
    float hatSixteenthFactor = 1.0f;
    float ghostFactor = 1.0f;
    float fillFactor = 1.0f;
    float openHatFactor = 1.0f;

    switch (s.style) {
        case StyleId::NDHIndustrial:
            // Mechanical, riff-locked, restrained fills.
            kickContextFactor = 1.08f;
            independentKickFactor = 0.82f;
            hatSixteenthFactor = 0.90f;
            ghostFactor = 0.75f;
            fillFactor = 0.70f;
            openHatFactor = 0.75f;
            break;
        case StyleId::DarkRockGothic:
            // More breathing room and cymbal movement, fewer machine kicks.
            kickContextFactor = 0.82f;
            independentKickFactor = 0.72f;
            hatSixteenthFactor = 0.72f;
            ghostFactor = 1.18f;
            fillFactor = 1.15f;
            openHatFactor = 1.35f;
            break;
        case StyleId::HeavyIndustrial:
            // Aggressive kick reinforcement and faster upper-kit pressure.
            kickContextFactor = 1.15f;
            independentKickFactor = 1.25f;
            hatSixteenthFactor = 1.30f;
            ghostFactor = 0.90f;
            fillFactor = 1.10f;
            openHatFactor = 0.90f;
            break;
        case StyleId::Count:
            break;
    }

    DrumPhrase out{};
    out.bars = std::clamp(std::max(guitar.bars, bass.bars), 1, kMaxBars);
    Rng rng(seed);
    // Velocity humanization must never perturb structural generation.
    // Keep a separate deterministic stream so Humanize changes dynamics only.
    Rng velocityRng(seed ^ 0x48A91E37u);
    // Section-transition fills use their own stream so enabling/expanding
    // transition language never rewrites the underlying groove topology.
    Rng transitionRng(seed ^ 0x46494C4Cu);

    for (int step = 0; step < out.usedSteps(); ++step) {
        const int local = step % kStepsPerBar;
        const int beat = local / 4;
        const bool downbeat = local == 0;
        const bool quarter = (local % 4) == 0;
        const bool eighth = (local % 2) == 0;
        const bool guitarHit = phraseHit(guitar, step);
        const bool bassHit = phraseHit(bass, step);

        auto& ds = out.steps[step];

        // Hat backbone: stable 8ths at low density, with controlled 16ths as
        // density/complexity rise.
        if (eighth || rng.chance(
                std::clamp((0.10f + 0.55f * s.density * s.complexity) *
                               hatSixteenthFactor,
                           0.0f, 1.0f))) {
            const bool open = (local == 14 || local == 6) &&
                              rng.chance(std::clamp(
                                  (0.10f + 0.30f * s.complexity) * openHatFactor,
                                  0.0f, 1.0f));
            addHit(ds,
                   open ? DrumVoice::OpenHat : DrumVoice::ClosedHat,
                   humanizedVelocity(velocityRng, eighth ? 86 : 70, s.humanize));
        }

        // Backbeat remains musically stable.
        if (beat == 1 && local % 4 == 0)
            addHit(ds, DrumVoice::Snare, humanizedVelocity(velocityRng, static_cast<int>(100.0f + 12.0f * fillIntensity), s.humanize));
        if (beat == 3 && local % 4 == 0)
            addHit(ds, DrumVoice::Snare, humanizedVelocity(velocityRng, 116, s.humanize));

        // Kick language: Follow continuously crossfades between an independent
        // pulse and explicit Guitar/Bass reinforcement. Keep the low end of the
        // control genuinely independent instead of accidentally landing most
        // random kicks on already-dense context steps.
        bool kick = false;
        const float shapedFollow =
            0.22f * s.follow + 0.78f * s.follow * s.follow;

        if (guitarHit || bassHit) {
            const float context = (guitarHit && bassHit) ? 1.0f : 0.78f;
            kick = rng.chance(std::clamp(
                (0.10f + 0.86f * shapedFollow) *
                    context * kickContextFactor,
                0.0f, 1.0f));
        }

        if (!kick && quarter && !guitarHit && !bassHit)
            kick = rng.chance(std::clamp(
                (0.30f + 0.42f * (1.0f - shapedFollow)) *
                    independentKickFactor,
                0.0f, 1.0f));

        if (!kick && !quarter && !guitarHit && !bassHit &&
            rng.chance(std::clamp(
                (0.04f + 0.18f * s.density) *
                    (0.40f + 0.60f * (1.0f - shapedFollow)) *
                    independentKickFactor,
                0.0f, 1.0f)))
            kick = true;

        // Do not stack kick blindly under every backbeat at low density.
        if (kick && !(local == 4 || local == 12) || (kick && s.density > 0.62f))
            addHit(ds, DrumVoice::Kick, humanizedVelocity(velocityRng, quarter ? 112 : 98, s.humanize));

        // Ghost notes before/after backbeats.
        if ((local == 3 || local == 11) &&
            rng.chance(std::clamp(
                (0.05f + 0.32f * s.complexity) * ghostFactor,
                0.0f, 1.0f)))
            addHit(ds, DrumVoice::GhostSnare, humanizedVelocity(velocityRng, 48, s.humanize));

        // Crash is structural punctuation, not a mandatory bar marker.
        // Always mark the phrase opening; later downbeats only punctuate
        // larger 4-bar sections or occasional 2-bar transitions.
        if (s.crashOnDownbeat && downbeat) {
            const int bar = step / kStepsPerBar;
            const bool phraseOpening = bar == 0;
            const bool sectionOpening = (bar % 4) == 0;
            const bool transitionAccent =
                (bar % 2) == 0 && bar > 0 &&
                rng.chance(0.08f + 0.22f * s.complexity);
            if (phraseOpening || sectionOpening || transitionAccent)
                addHit(ds, DrumVoice::Crash,
                       humanizedVelocity(velocityRng, 118, s.humanize));
        }

        const int bar = step / kStepsPerBar;
        const float fillIntensity = std::clamp(s.fillIntensity, 0.0f, 1.0f);
        const int fillStart =
            fillIntensity >= 0.875f ? 12 :
            fillIntensity >= 0.625f ? 13 :
            fillIntensity >= 0.375f ? 14 : 15;

        // Small two-bar fills keep their musical occurrence logic; intensity
        // changes how much of the final beat they are allowed to occupy.
        if (fillIntensity > 0.0001f &&
            (bar % 2) == 1 && local >= fillStart && s.complexity > 0.35f) {
            const float p = std::clamp(
                (0.10f + 0.42f * s.complexity) * fillFactor,
                0.0f, 1.0f);
            if (rng.chance(p)) {
                DrumVoice tom = DrumVoice::LowTom;
                if (local >= 14) tom = DrumVoice::MidTom;
                if (local == 15) tom = DrumVoice::HighTom;
                const int velocity = static_cast<int>(std::lround(
                    86.0f + 18.0f * fillIntensity + (local - 12) * 4.0f));
                addHit(ds, tom,
                       humanizedVelocity(velocityRng, velocity, s.humanize));
            }
        }

        // Major section transition: every eighth bar gets a deliberate
        // drummer-like fill. Fill Intensity controls its span: low settings
        // are a pickup on the final sixteenth, 100% uses the full last beat.
        // Faster 1/32 rolls remain a future ratchet/substep feature.
        const bool eightBarBoundary =
            fillIntensity > 0.0001f &&
            ((bar + 1) % 8) == 0 && local >= fillStart;

        if (eightBarBoundary) {
            const bool fullRoll =
                fillIntensity >= 0.50f &&
                (s.complexity >= 0.20f || transitionRng.chance(0.55f));

            if (s.style == StyleId::NDHIndustrial) {
                // Mechanical snare roll with a final tom punctuation.
                if ((local == 12 || local == 14 || local == 15) ||
                    (local == 13 && fullRoll)) {
                    addHit(ds, DrumVoice::Snare,
                           humanizedVelocity(
                               velocityRng,
                               static_cast<int>(84.0f + 14.0f * fillIntensity) + (local - 12) * 7,
                               s.humanize));
                }
                if (local == 15 && s.complexity >= 0.25f)
                    addHit(ds, DrumVoice::HighTom,
                           humanizedVelocity(velocityRng, static_cast<int>(96.0f + 12.0f * fillIntensity), s.humanize));
            } else if (s.style == StyleId::HeavyIndustrial) {
                // Aggressive machine-like roll: snare each sixteenth, kick
                // reinforcement and an ascending tom at the end.
                addHit(ds, DrumVoice::Snare,
                       humanizedVelocity(
                           velocityRng,
                           static_cast<int>(86.0f + 12.0f * fillIntensity) + (local - 12) * 7,
                           s.humanize));
                if (local == 12 || local == 14)
                    addHit(ds, DrumVoice::Kick,
                           humanizedVelocity(velocityRng, static_cast<int>(96.0f + 12.0f * fillIntensity), s.humanize));
                if (local == 14)
                    addHit(ds, DrumVoice::MidTom,
                           humanizedVelocity(velocityRng, static_cast<int>(94.0f + 11.0f * fillIntensity), s.humanize));
                if (local == 15)
                    addHit(ds, DrumVoice::HighTom,
                           humanizedVelocity(velocityRng, static_cast<int>(100.0f + 14.0f * fillIntensity), s.humanize));
            } else if (s.style == StyleId::DarkRockGothic) {
                // More organic tom run with a restrained snare pickup.
                if (local == 12)
                    addHit(ds, DrumVoice::LowTom,
                           humanizedVelocity(velocityRng, static_cast<int>(90.0f + 8.0f * fillIntensity), s.humanize));
                if (local == 13 && fullRoll)
                    addHit(ds, DrumVoice::Snare,
                           humanizedVelocity(velocityRng, static_cast<int>(84.0f + 8.0f * fillIntensity), s.humanize));
                if (local == 14)
                    addHit(ds, DrumVoice::MidTom,
                           humanizedVelocity(velocityRng, static_cast<int>(94.0f + 10.0f * fillIntensity), s.humanize));
                if (local == 15)
                    addHit(ds, DrumVoice::HighTom,
                           humanizedVelocity(velocityRng, static_cast<int>(100.0f + 12.0f * fillIntensity), s.humanize));
            }
        }
    }

    return out;
}

} // namespace midiator
