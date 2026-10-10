# 125A Midiator

**125A Midiator** is a multi-role VST3 MIDI generator for metal, industrial and dark-rock workflows. It generates coordinated MIDI parts for external instruments; it contains no internal audio engine.

## Current development scope

Midiator currently generates five independently routable musical roles:

- **Guitar Out** — riff-focused heavy guitar MIDI
- **Bass Out** — monophonic bass accompaniment derived from the riff context
- **Drums Out** — semantic drum patterns with mapping abstraction
- **Pad Out** — polyphonic sustained harmony with voice-leading
- **Synth Out** — compact arps, short phrases and occasional two-note chord stabs; Synth Activity 0% is silent, 10% generates rare accents, while 46% and above retain the established rhythm behavior

Each role uses its own VST3 event output bus. Each output exposes one MIDI channel, so hosts do not show fifteen unused channels between Guitar, Bass, Drums, Pad and Synth. The MIDI input remains 16-channel for flexible controller, Root Source and trigger input.

The current timing scope is **4/4**, using an internal 16th-note grid and phrase lengths of 1, 2, 4, 8 or 16 bars.

## Shared musical frame

The engine shares root note, scale / mode, style, phrase length, transport position and DAW tempo.

Implemented styles: NDH / Industrial, Dark Rock / Gothic, Heavy Industrial, Classic Heavy Metal, Thrash Metal, Groove Metal, Death Metal, Melodic Death Metal, Metalcore, Nu Metal, Doom Metal and Djent / Progressive.

Bass is style-aware; Drums derive kick pressure from Guitar/Bass context; Pads use 2-3 harmonic tones with occasional octave doubling, voice-leading and Guitar/Bass context-follow; Synth generates compact arp/ostinato notes, short phrase fragments and occasional two-note chord stabs while optionally targeting active Pad harmony.

## Scales / modes

- Natural Minor / Aeolian
- Phrygian
- Dorian
- Harmonic Minor
- Phrygian Dominant
- Minor Pentatonic
- Blues

## GUI and workflow

The redesigned editor uses a 1120 × 744 base layout, with 80%, 100%, 120% and 150% zoom steps. New instances start at 100% zoom; 80%, 120% and 150% remain selectable. Saved per-project zoom overrides the default and is stored separately in VST3 controller state. Controls are arranged in clear, lightly differentiated panels with readable typography and persistent OPEN/LOCK or OFF/ON text buttons instead of binary dropdown menus.


The interface is split by behavior instead of presenting every parameter as if it did the same thing.

**Tonal / Playback**
- Root and Scale / Mode
- Section Length: 1 / 2 / 4 / 8 / 16 bars
- Root Source: Manual keeps the established guitar register. MIDI follows the absolute input note and octave for Guitar (D1 = MIDI 26, D2 = MIDI 38), while Bass/Pads/Synth retain their own appropriate registers. MIDI root notes 0..120 are supported. The live theory header follows the effective pitch class through host feedback.
- Trigger: TRANSPORT or MIDI NOTE. TRANSPORT follows DAW Play; MIDI NOTE stays silent until at least one trigger note is held and stops when the last trigger note is released.

**Riff Generation**
- Section: FREE / Intro / Verse / Pre / Chorus / Breakdown / Outro
- Metal Style with an on-GUI recommended BPM range
- Density, Complexity, Repetition, Power Chord amount and Palm Mute amount shape the next NEW RIFF / VARIATION rather than silently replacing the current idea
- Power Chords ON/OFF and PM < VEL are direct live edits
- Variation Amount controls VARIATION depth
- NEW RIFF creates a new idea; VARIATION develops the current one

**Role Locks**
- Guitar, Bass, Drums, Pads and Synth each have OPEN / LOCK
- LOCK protects that exact role from NEW RIFF and VARIATION
- Unlocked roles regenerate against the final current arrangement, so a locked Guitar can keep its riff while Bass/Drums/Pads/Synth find new compatible parts
- If all five roles are locked, NEW RIFF and VARIATION are intentional no-ops

**Role Shaping — LIVE**
- Bass: Follow, Movement
- Drums: Density, Complexity, Fill Intensity
- Pads / Choir: Spread, Tension
- Synth: Activity, Movement
- Humanize: global performance timing/velocity variation, default 0%

Role Shaping applies immediately and does not require NEW RIFF. Humanize is deterministic, style-bounded and overlap-safe: chord tones move together, Guitar/Bass retain safe note gaps, repeated drum pitches cannot overlap, and palm-muted Guitar velocities remain below PM < VEL.

## Drum mapping

Verified static maps exposed by the current selector:

- General MIDI
- EZdrummer 3 Standard Layout
- Perfect Drums default layout
- Addictive Drums 2 Standard keymap

The historical three-choice Drum Map automation parameter remains frozen and hidden; the expanded verified selector uses a new parameter ID so old host automation is never reinterpreted.

Superior Drummer 3 and SSD5.5 remain configurable / preset-dependent and are not claimed as universally verified static maps. Libraries such as Krimh that can load a General MIDI map can use Midiator's General MIDI output directly.

Ghost Snare uses the acoustic snare pitch with reduced velocity rather than substituting GM Electric Snare.

## Host and lifecycle behavior

The processor requests VST3 musical timeline, tempo, time signature and transport state. It flushes active notes on stop, timeline jumps and phrase replacement, keeps per-bus active-note state, sorts same-sample NoteOff before NoteOn, supports realtime/offline processing and stages events in fixed realtime-safe memory one musical cycle at a time, so very large offline blocks can span many cycles without whole-block scheduler overflow.

## State

State V16 stores the complete five-role arrangement plus all current musical settings, including the 12-style selector, verified Drum Map, Section, role-shaping controls, PM velocity threshold, Trigger mode, all five role locks and Humanize amount.

Compatibility is explicit:
- V1-V10 historical project states remain readable.
- Unreleased V11 expanded-style and retired Song Mode development states are migrated safely.
- V12 remains the frozen expanded-style state.
- V13 introduced PM < VEL.
- V14 introduced TRANSPORT / MIDI NOTE trigger state.
- V15 adds role locks and Humanize and permits the newly appended verified Addictive Drums 2 map ID.
- V16 appends the absolute MIDI guitar-root note, preserving the selected octave after project reload while remaining backward compatible.
- Older states default all role locks to OPEN and Humanize to 0%.

The exact generated Guitar, Bass, Drum, Pad and Synth phrase payloads are recalled instead of being regenerated on project load.

## Automated verification

The development branch checks deterministic generation, scale safety, phrase integrity, 8- and 16-bar macro-development, role-specific controls, style behavior, Fill Intensity progression and section transitions, Pad voice-leading/context-follow, Synth motif/harmony follow, verified drum maps, role-lock isolation, all-lock no-op behavior, overlap-safe Humanize, PM articulation boundaries, five dedicated one-channel event output buses, per-bus balance/flush, large offline blocks with cycle-chunked scheduling, state roundtrip plus frozen legacy/development migration fixtures, GUI/editor lifecycle, Steinberg Validator and statistical measurement reports.

A frozen deterministic **golden five-role arrangement fingerprint** protects a known NDH / A Phrygian four-bar reference so unintended musical changes fail CI immediately.

## Branches

Active development happens on **development**.

The default `main` branch is not the current implementation baseline and should not be used as the source of truth for ongoing engineering work.
