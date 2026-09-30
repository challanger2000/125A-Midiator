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

    // Start from General MIDI. Vendor presets are intentionally explicit
    // adapters, so the composition engine never depends on note numbers.
    m.note = {
        36, // Kick
        38, // Snare
        42, // Closed Hat
        46, // Open Hat
        49, // Crash
        51, // Ride
        41, // Low Tom
        47, // Mid Tom
        50, // High Tom
        40  // Ghost Snare / secondary snare articulation
    };

    switch (id) {
        case DrumMapId::GeneralMidi:
            break;
        case DrumMapId::EZdrummer3:
        case DrumMapId::SuperiorDrummer3:
            // Toontrack core mapping is GM-compatible for the principal kit
            // pieces used by the first Drum Brain.
            break;
        case DrumMapId::SSD55:
            // SSD core kick/snare/hat/tom layout is also GM-like enough for
            // these principal voices; specialized articulations come later.
            break;
        case DrumMapId::PerfectDrums:
            // Keep principal-kit GM mapping in V1; expose Custom for deviations.
            break;
        case DrumMapId::Custom:
        case DrumMapId::Count:
            break;
    }
    return m;
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

    DrumPhrase out{};
    out.bars = std::clamp(std::max(guitar.bars, bass.bars), 1, kMaxBars);
    Rng rng(seed);

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
        if (eighth || rng.chance(0.10f + 0.55f * s.density * s.complexity)) {
            const bool open = (local == 14 || local == 6) &&
                              rng.chance(0.10f + 0.30f * s.complexity);
            addHit(ds,
                   open ? DrumVoice::OpenHat : DrumVoice::ClosedHat,
                   humanizedVelocity(rng, eighth ? 86 : 70, s.humanize));
        }

        // Backbeat remains musically stable.
        if (beat == 1 && local % 4 == 0)
            addHit(ds, DrumVoice::Snare, humanizedVelocity(rng, 112, s.humanize));
        if (beat == 3 && local % 4 == 0)
            addHit(ds, DrumVoice::Snare, humanizedVelocity(rng, 116, s.humanize));

        // Kick language: shared anchors from guitar/bass plus independent
        // quarter-note support when Follow is low.
        bool kick = false;
        if (guitarHit || bassHit) {
            const float context = (guitarHit && bassHit) ? 1.0f : 0.78f;
            kick = rng.chance((0.24f + 0.68f * s.follow) * context);
        }
        if (!kick && quarter)
            kick = rng.chance(0.22f + 0.34f * (1.0f - s.follow));
        if (!kick && !quarter && rng.chance(0.03f + 0.16f * s.density))
            kick = true;

        // Do not stack kick blindly under every backbeat at low density.
        if (kick && !(local == 4 || local == 12) || (kick && s.density > 0.62f))
            addHit(ds, DrumVoice::Kick, humanizedVelocity(rng, quarter ? 112 : 98, s.humanize));

        // Ghost notes before/after backbeats.
        if ((local == 3 || local == 11) &&
            rng.chance(0.05f + 0.32f * s.complexity))
            addHit(ds, DrumVoice::GhostSnare, humanizedVelocity(rng, 48, s.humanize));

        // Phrase opening and controlled section punctuation.
        if (s.crashOnDownbeat && downbeat)
            addHit(ds, DrumVoice::Crash, humanizedVelocity(rng, 118, s.humanize));

        // Simple tom fill language on the final beat of every second bar.
        const int bar = step / kStepsPerBar;
        if ((bar % 2) == 1 && local >= 12 && s.complexity > 0.35f) {
            const float p = 0.10f + 0.42f * s.complexity;
            if (rng.chance(p)) {
                DrumVoice tom = DrumVoice::LowTom;
                if (local >= 14) tom = DrumVoice::MidTom;
                if (local == 15) tom = DrumVoice::HighTom;
                addHit(ds, tom, humanizedVelocity(rng, 96 + (local - 12) * 4, s.humanize));
            }
        }
    }

    return out;
}

} // namespace midiator
