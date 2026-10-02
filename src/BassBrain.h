#pragma once

#include "RiffEngine.h"

#include <cstdint>

namespace midiator {

struct BassSettings {
    int rootPitchClass = 9;          // A
    ScaleId scale = ScaleId::Phrygian;
    StyleId style = StyleId::NDHIndustrial;
    int lowRootMidi = 28;            // E1 reference region; root is aligned upward
    float follow = 0.72f;            // rhythmic lock to the guitar phrase
    float activity = 1.0f;            // internal section-level note thinning
    float movement = 0.34f;          // chord/scale movement away from the pedal root
    float passing = 0.16f;           // controlled connecting notes
    float octaveChance = 0.10f;      // occasional octave reinforcement
    float sustain = 0.34f;           // longer notes in available gaps
};

class BassBrain {
public:
    // Compose a monophonic bass phrase from the shared musical frame and an
    // optional guitar phrase. The guitar is treated as rhythmic/motif context,
    // not copied note-for-note.
    static Phrase generate(const Phrase& guitar,
                           const BassSettings& settings,
                           uint32_t seed);
};

} // namespace midiator
