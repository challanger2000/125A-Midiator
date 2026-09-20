# 125A Midiator

**125A Midiator** is a VST3 MIDI riff generator and mutation tool for heavy guitar workflows.

## Project goal

Midiator is intended to generate and transform convincing guitar-oriented MIDI riffs directly inside the DAW.

The focus is not on random note generation. The generator should create musically coherent riffs by combining rhythm, repetition, variation, scale knowledge, guitar-oriented pitch movement and controlled randomness.

The first version is deliberately narrow: **guitar only, MIDI only, 4/4 only**.

## V1 scope

- VST3 MIDI plugin
- 4/4 time signature
- 1-4 bar phrases
- DAW tempo / transport sync
- selectable root note
- selectable scale / mode
- guitar-oriented riff generation
- direct MIDI output to external VST instruments
- Generate mode
- Mutate mode
- Lockable phrase areas / steps
- deterministic regeneration when desired
- project-state persistence
- no internal audio engine
- no samples
- no amp / cab processing

## Musical design principles

Midiator must not behave like a simple random MIDI generator.

Generation should be hierarchical:

1. rhythm / groove
2. phrase structure
3. repetition and variation
4. pitch movement
5. scale / mode constraints
6. controlled chromatic notes
7. optional humanization

The riff should remain recognizable as a phrase instead of becoming a stream of unrelated notes.

### Guitar-oriented behavior

The engine should be able to favor structures such as:

- pedal-note / chug riffs
- repeated-note motifs
- syncopation
- rests and gaps
- semitone tension
- fourths and fifths
- tritone movement
- octave movement
- call-and-response phrases
- motif repetition with controlled mutation

Playability and usable register must be considered. Guitar-specific articulation / keyswitch support can be added later as an instrument-profile layer.

## Theory / learning layer

The UI should not only expose scale names. It should also show useful musical information, for example:

- notes contained in the selected scale
- characteristic intervals
- short tonal description
- suitable use cases
- quick audition of scales / modes

This makes Midiator useful as both an idea generator and a practical theory aid.

## Initial scale candidates

The exact list is still open, but likely starting points include:

- Natural Minor / Aeolian
- Phrygian
- Dorian
- Harmonic Minor
- Phrygian Dominant
- Minor Pentatonic
- Blues

## Architecture principle

The musical phrase engine and the guitar-specific interpretation should remain separate.

That allows later extensions such as bass, synth or drum-derived MIDI without redesigning the core generator.

For V1, however, only the guitar layer is in scope.

## Non-goals for V1

- no internal guitar sound
- no standalone application
- no complete song arranger
- no automatic bass generation
- no drum generation
- no synth generation
- no exotic time signatures
- no AI / ML dependency required

## Development priority

**Musical quality before feature count.**

A small number of convincing riff engines is preferable to a large number of controls that produce generic or random-sounding results.


## Development status

Current work happens on the `development` branch.

The first prototype contains a pure C++ riff engine, live VST3 MIDI output, NEW/VARIATION controls and automated core tests. GUI work is intentionally deferred until the musical and host-integration core is stable.

Current prototype safety checks also prevent same-pitch retrigger overlap and use the proven VST3 MIDI event-bus layout.
