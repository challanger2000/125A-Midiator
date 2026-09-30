#pragma once

#include "RiffEngine.h"
#include "PadBrain.h"

#include <array>
#include <algorithm>
#include <cstdint>

namespace midiator {

struct SynthSettings {
    int rootPitchClass = 9;
    ScaleId scale = ScaleId::Phrygian;
    StyleId style = StyleId::NDHIndustrial;
    float activity = 0.46f;
    float movement = 0.42f;
    float repetition = 0.62f;
    float syncopation = 0.34f;
    float sustain = 0.30f;
    float harmonicFollow = 0.68f;
    int centerMidi = 72;
};

class SynthBrain {
public:
    static Phrase generate(const Phrase& guitar,
                           const Phrase& bass,
                           const PadPhrase& pads,
                           const SynthSettings& settings,
                           uint32_t seed);
};

} // namespace midiator
