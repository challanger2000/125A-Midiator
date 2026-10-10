#pragma once

#include <algorithm>

// Reversible root transposition for generated companion MIDI roles.
// This MUST use the immutable musical reference, not the previous folded
// output. Repeated A -> G# -> A returns all MIDI pitches bit-identically,
// even when a note crosses its role's lower or upper register limit.
namespace midiator {

inline int shortestPitchClassDelta(int root, int referenceRoot) noexcept {
    int delta = root - referenceRoot;
    if (delta > 6) delta -= 12;
    else if (delta < -6) delta += 12;
    return delta;
}

template <typename PhraseT>
void retunePhraseFromReference(PhraseT& dst, const PhraseT& src,
                              int newRootClass, int referenceRootClass,
                              int low, int high) noexcept {
    dst = src;
    const int delta = shortestPitchClassDelta(newRootClass, referenceRootClass);
    for (int step = 0; step < dst.usedSteps(); ++step) {
        auto& notes = dst.steps[step];
        if (notes.noteCount <= 0)
            continue;
        // Retune a polyphonic chord as a UNIT. Folding each voice separately
        // can collapse an octave pair into a unison at a register boundary.
        int minRaw = 128;
        int maxRaw = -128;
        for (int n = 0; n < notes.noteCount; ++n) {
            const int raw = src.steps[step].notes[n].pitch + delta;
            minRaw = std::min(minRaw, raw);
            maxRaw = std::max(maxRaw, raw);
        }
        int commonOctaveShift = 0;
        bool fits = false;
        for (int shift : {0, -12, 12, -24, 24, -36, 36, -48, 48}) {
            if (minRaw + shift >= low && maxRaw + shift <= high) {
                commonOctaveShift = shift;
                fits = true;
                break;
            }
        }
        for (int n = 0; n < notes.noteCount; ++n) {
            int pitch = src.steps[step].notes[n].pitch + delta;
            if (fits) {
                pitch += commonOctaveShift;
            } else {
                // Malformed/legacy voicings wider than the supported range
                // have no common octave; keep all individual notes in MIDI.
                while (pitch < low) pitch += 12;
                while (pitch > high) pitch -= 12;
            }
            notes.notes[n].pitch = std::clamp(pitch, low, high);
        }
    }
}

} // namespace midiator
