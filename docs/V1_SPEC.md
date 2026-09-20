# 125A Midiator — V1 working specification

## Purpose

Midiator is a guitar-riff idea generator and theory aid with live MIDI output.

It is not intended to finish a performance automatically. The user remains free to edit notes, velocities and articulations in the DAW.

## Fixed V1 constraints

- VST3
- MIDI only
- guitar-focused
- 4/4 only
- 16th-note internal grid
- phrase lengths: 1, 2, 4 or 8 bars
- library-neutral MIDI output
- no internal sound engine
- no AI/ML dependency

## Core workflow

1. Choose root.
2. Choose scale/mode.
3. Set a small number of musical controls.
4. NEW creates a fresh riff.
5. VARIATION changes the current riff without changing root, scale or bar length.
6. MIDI is emitted live while the DAW transport runs.
7. The user may record/capture and edit the MIDI in the DAW.

## Musical controls

- Density
- Complexity
- Repetition
- Power Chords
- Palm Mute tendency
- Variation Amount

## Tonal safety

VARIATION must never silently change key or scale.

A separate transpose function may be added later, but transposition is not part of variation.

## Velocity policy

Midiator deliberately avoids MIDI velocity 127 in automatic generation because some guitar libraries use 127 for a special articulation such as pinch harmonic/squeal.

V1 uses visibly separated lower and higher velocity zones so that palm-muted/chug-like notes and open/sustain-like notes are easy to identify and edit.

Exact articulation thresholds remain library-specific and are intentionally not hard-coded as universal truth.

## Power chords

V1 emits real MIDI dyads (root + perfect fifth) when a power chord is generated.

Library-specific one-key chord modes are not required.

## Initial scales

- Natural Minor / Aeolian
- Phrygian
- Dorian
- Harmonic Minor
- Phrygian Dominant
- Minor Pentatonic
- Blues

## Engine principle

Generation order:

1. rhythm
2. motif
3. repetition
4. controlled variation
5. pitch movement constrained by selected scale
6. articulation-oriented velocity shaping

The engine should prefer memorable repeated motives and deliberate gaps over high note count.
