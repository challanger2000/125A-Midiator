# 125A Midiator

**125A Midiator** is a multi-role VST3 MIDI songwriting generator for heavy, industrial and dark-rock workflows. It generates coordinated MIDI parts for external instruments; it contains no internal audio engine.

## Current development scope

Midiator currently generates five independently routable musical roles:

- **Guitar Out** — riff-focused heavy guitar MIDI
- **Bass Out** — monophonic bass accompaniment derived from the riff context
- **Drums Out** — semantic drum patterns with mapping abstraction
- **Pad Out** — polyphonic sustained harmony with voice-leading
- **Synth Out** — compact arps, short phrases and occasional two-note chord stabs

Each role uses its own VST3 event output bus. The roles are not multiplexed onto one MIDI output.

The current timing scope is **4/4**, using an internal 16th-note grid and phrase lengths of 1, 2, 4, 8 or 16 bars.

## Shared musical frame

The engine shares root note, scale / mode, style, phrase length, transport position and DAW tempo.

Implemented styles: NDH / Industrial, Dark Rock / Gothic and Heavy Industrial.

Bass is style-aware; Drums derive kick pressure from Guitar/Bass context; Pads use 2-3 harmonic tones with occasional octave doubling, voice-leading and Guitar/Bass context-follow; Synth generates compact arp/ostinato notes, short phrase fragments and occasional two-note chord stabs while optionally targeting active Pad harmony.

## Scales / modes

- Natural Minor / Aeolian
- Phrygian
- Dorian
- Harmonic Minor
- Phrygian Dominant
- Minor Pentatonic
- Blues

## GUI controls currently exposed

Root / Root Source, Scale / Mode, Section Length (1/2/4/8/16), Style, Density, Complexity, Repetition, Power Chords on/off and amount, Palm Mute, Variation Amount, NEW RIFF and VARIATION.

The GUI exposes a deliberately small role-shaping layer instead of every internal generator parameter:

- Bass: Follow, Movement
- Drums: Density, Complexity, Fill Intensity
- Pads: Spread, Tension
- Synth: Activity, Movement

Less essential controls (for example Bass Sustain, Drum Humanize, Pad Context Follow and Synth Harmonic Follow) remain internal defaults for V1 so the workflow stays compact and musically predictable.

## Drum mapping

Verified static maps: General MIDI, EZdrummer 3 Standard Layout and Perfect Drums default layout.

Superior Drummer 3 and SSD5.5 are treated as configurable / preset-dependent mappings and are not claimed as universally verified static maps. Custom mapping remains the safe route for library-specific deviations.

Ghost Snare uses the acoustic snare pitch with reduced velocity rather than substituting GM Electric Snare.

## Host and lifecycle behavior

The processor requests VST3 musical timeline, tempo, time signature and transport state. It flushes active notes on stop, timeline jumps and phrase replacement, keeps per-bus active-note state, sorts same-sample NoteOff before NoteOn, supports realtime/offline processing and stages events in fixed realtime-safe memory one musical cycle at a time, so very large offline blocks can span many cycles without whole-block scheduler overflow.

## State

State V9 stores the shared musical settings, verified Drum Map selection, the exposed role-shaping controls including Fill Intensity, and the exact generated Guitar, Bass, Drum, Pad and Synth phrases. V8 expanded the exact phrase payload from 128 to 256 steps so 16-bar sections can be recalled without regeneration; V9 adds persisted Fill Intensity while retaining the V8 phrase layout. Legacy V1-V4 states remain supported by regenerating missing companion roles once during migration; V5 preserves its exact five-role payload, V6 additionally preserves its verified Drum Map, V7 preserves the original eight role-shaping controls with the 128-step payload, and V8 preserves the 256-step payload. All migrated states are then saved in V9 format.

## Automated verification

The development branch checks deterministic generation, scale safety, phrase integrity, 8- and 16-bar macro-development, role-specific controls, style behavior, Fill Intensity progression and section transitions, Pad voice-leading/context-follow, Synth motif/harmony follow, Drum Humanize invariance, verified drum maps, five dedicated event output buses, per-bus balance/flush, large offline blocks with cycle-chunked scheduling, state roundtrip plus frozen V1-V8 migration fixtures, GUI/editor lifecycle, Steinberg Validator and statistical measurement reports.

A frozen deterministic **golden five-role arrangement fingerprint** protects a known NDH / A Phrygian four-bar reference so unintended musical changes fail CI immediately.

## Branches

Active development happens on **development**.

The default `main` branch is not the current implementation baseline and should not be used as the source of truth for ongoing engineering work.
