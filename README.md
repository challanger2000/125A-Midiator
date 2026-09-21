# 125A Midiator

**125A Midiator** is a VST3 MIDI riff generator and theory helper for heavy guitar workflows.

## Purpose

Midiator generates musically structured guitar-oriented MIDI riffs directly inside the DAW. It is designed as an idea source and theory aid, not as an automatic finished-performance system: generated MIDI can be recorded, edited and arranged freely in the host.

The V1 scope is deliberately narrow: **guitar-focused, MIDI-only, 4/4**.

## Implemented V1 scope

- VST3 MIDI/event processor with MIDI In and MIDI Out
- 4/4 with an internal 16th-note grid
- phrase lengths: 1, 2, 4 or 8 bars
- DAW tempo, musical timeline and transport synchronization
- selectable root note
- selectable scale/mode
- NEW RIFF for a completely new phrase inside the current tonal frame
- VARIATION for controlled mutation while preserving root, scale and phrase length
- Density, Complexity, Repetition, Power Chords, Palm Mute and Variation Amount controls
- explicit MIDI power chords as root + perfect fifth
- velocity-oriented articulation shaping
- automatic velocity generation limited to 1–126; velocity 127 is intentionally reserved
- deterministic phrase generation
- complete generated phrase stored in project state
- live MIDI output to external VST instruments
- resizable VSTGUI interface
- theory display for scale notes, tonal character and characteristic interval
- no internal audio engine, samples, amp or cab processing

## Initial scales

- Natural Minor / Aeolian
- Phrygian
- Dorian
- Harmonic Minor
- Phrygian Dominant
- Minor Pentatonic
- Blues

## Musical design

Generation is hierarchical rather than purely random:

1. rhythm and groove
2. motif / phrase structure
3. repetition with controlled development
4. scale-constrained pitch movement
5. articulation-oriented velocity shaping

The engine favors pedal/chug patterns, repeated motives, syncopation, rests, characteristic scale tones and controlled bar-to-bar development. Four-bar phrases use an A / A' / answer / turnaround concept while retaining a recognizable identity.

## Guitar-oriented MIDI

Power chords are emitted as two actual MIDI notes: root + perfect fifth. Library-specific one-key chord modes are therefore not required.

Velocity zones are deliberately separated so mute-like and open-like events remain easy to recognize and edit. Exact articulation thresholds are instrument-specific; Midiator does not claim one library's mapping as a universal standard.

## VARIATION contract

VARIATION may alter notes, rhythm and articulation, but it does **not** silently change:

- root
- scale/mode
- phrase length

A separate transpose function may be added in a later version.

## Host and lifecycle safety

The processor explicitly requests the VST3 process context required for musical timeline position, tempo and transport state. Active generated notes are flushed on transport stop, timeline jumps and phrase replacement, and generated note lengths are constrained at the phrase loop boundary.

The processor accepts both VST3 32-bit and 64-bit symbolic audio sample modes even though Midiator itself does not process audio buffers.

## Automated verification

The development build includes automated checks for:

- deterministic generation
- 1/2/4/8-bar integrity
- scale membership
- velocity safety
- explicit power-chord fifths
- same-pitch overlap prevention
- loop-boundary note safety
- variation tonal-frame preservation
- control behavior and statistical musical sanity
- complete processor state roundtrip
- NEW RIFF / VARIATION rising-edge behavior
- same-block button press+release handling
- live MIDI NoteOn/NoteOff behavior
- transport-stop and timeline-jump note flushing
- GUI contract and resizable editor constraints
- Steinberg VST3 validator

Current development work happens on the `development` branch.
