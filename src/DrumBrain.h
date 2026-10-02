#pragma once

#include "RiffEngine.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace midiator {

enum class DrumVoice : uint8_t {
    Kick = 0,
    Snare,
    ClosedHat,
    OpenHat,
    Crash,
    Ride,
    LowTom,
    MidTom,
    HighTom,
    GhostSnare,
    Count
};

struct DrumHit {
    DrumVoice voice = DrumVoice::Kick;
    int velocity = 100;
};

constexpr int kMaxDrumHitsPerStep = 4;

struct DrumStep {
    int hitCount = 0;
    std::array<DrumHit, kMaxDrumHitsPerStep> hits{};
};

struct DrumPhrase {
    int bars = 2;
    std::array<DrumStep, kMaxSteps> steps{};

    int usedSteps() const {
        return std::clamp(bars, 1, kMaxBars) * kStepsPerBar;
    }
};

struct DrumSettings {
    StyleId style = StyleId::NDHIndustrial;
    float follow = 0.72f;      // lock kick accents to guitar/bass context
    float density = 0.48f;     // overall activity
    float complexity = 0.30f;  // hat motion, ghost detail, tom vocabulary
    float fillIntensity = 0.50f; // size of structural fills; 0 = off
    float humanize = 0.20f;    // velocity variance only for now
    bool crashOnDownbeat = true;
};

enum class DrumMapId : uint8_t {
    GeneralMidi = 0,
    EZdrummer3,
    SuperiorDrummer3,
    SSD55,
    PerfectDrums,
    Custom,
    Count
};

struct DrumMidiMap {
    std::array<int, static_cast<size_t>(DrumVoice::Count)> note{};

    int midiNote(DrumVoice voice) const {
        return std::clamp(note[static_cast<size_t>(voice)], 0, 127);
    }

    static DrumMidiMap preset(DrumMapId id);
    static bool presetIsVerified(DrumMapId id);
};

class DrumBrain {
public:
    static DrumPhrase generate(const Phrase& guitar,
                               const Phrase& bass,
                               const DrumSettings& settings,
                               uint32_t seed);
};

} // namespace midiator
