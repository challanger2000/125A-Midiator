#pragma once

#include "RiffEngine.h"

#include <array>
#include <algorithm>
#include <cstdint>

namespace midiator {

constexpr int kMaxPadVoices = 4;

struct PadNote {
    int pitch = 60;
    int velocity = 76;
    int lengthSteps = 16;
};

struct PadStep {
    int noteCount = 0;
    std::array<PadNote, kMaxPadVoices> notes{};
};

struct PadPhrase {
    int bars = 2;
    std::array<PadStep, kMaxSteps> steps{};

    int usedSteps() const {
        return std::clamp(bars, 1, kMaxBars) * kStepsPerBar;
    }
};

struct PadSettings {
    int rootPitchClass = 9;
    ScaleId scale = ScaleId::Phrygian;
    StyleId style = StyleId::NDHIndustrial;
    float movement = 0.28f;
    float spread = 0.42f;
    float tension = 0.18f;
    float sustain = 0.82f;
    float contextFollow = 0.68f;
    int centerMidi = 60;
};

class PadBrain {
public:
    static PadPhrase generate(const Phrase& guitar,
                              const Phrase& bass,
                              const PadSettings& settings,
                              uint32_t seed);
};

} // namespace midiator
