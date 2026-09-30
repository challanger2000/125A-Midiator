# 125A Midiator

**125A Midiator** is a multi-role VST3 MIDI songwriting generator for heavy, industrial and dark-rock workflows. It generates coordinated MIDI parts for external instruments; it contains no internal audio engine.

## Current development scope

Midiator currently generates five independently routable musical roles:

- **Guitar Out** — riff-focused heavy guitar MIDI
- **Bass Out** — monophonic bass accompaniment derived from the riff context
- **Drums Out** — semantic drum patterns with mapping abstraction
- **Pad Out** — polyphonic sustained harmony with voice-leading
- **Synth Out** — monophonic hook / arp-oriented melodic material

Each role uses its own VST3 event output bus. The roles are not multiplexed onto one MIDI output.

The current timing scope is **4/4**, using an internal 16th-note grid and phrase lengths of 1, 2, 4 or 8 bars.

## Shared musical frame

The engine shares root note, scale / mode, style, phrase length, transport position and DAW tempo.

Implemented styles: NDH / Industrial, Dark Rock / Gothic and Heavy Industrial.

Bass is style-aware; Drums derive kick pressure from Guitar/Bass context; Pads use voice-leading plus Guitar/Bass context-follow; Synth keeps its own motif while optionally targeting active Pad harmony.

## Scales / modes

- Natural Minor / Aeolian
- Phrygian
- Dorian
- Harmonic Minor
- Phrygian Dominant
- Minor Pentatonic
- Blues

## GUI controls currently exposed

Root / Root Source, Scale / Mode, Bars, Style, Density, Complexity, Repetition, Power Chords on/off and amount, Palm Mute, Variation Amount, NEW RIFF and VARIATION.

Bass, Drum, Pad and Synth control layers are implemented internally but are not yet fully exposed as dedicated GUI parameter sections. Their current settings are derived deterministically from the shared musical frame and internal defaults.

## Drum mapping

Verified static maps: General MIDI, EZdrummer 3 Standard Layout and Perfect Drums default layout.

Superior Drummer 3 and SSD5.5 are treated as configurable / preset-dependent mappings and are not claimed as universally verified static maps. Custom mapping remains the safe route for library-specific deviations.

Ghost Snare uses the acoustic snare pitch with reduced velocity rather than substituting GM Electric Snare.

## Host and lifecycle behavior

The processor requests VST3 musical timeline, tempo, time signature and transport state. It flushes active notes on stop, timeline jumps and phrase replacement, keeps per-bus active-note state, sorts same-sample NoteOff before NoteOn, supports realtime/offline processing and stages events in fixed realtime-safe memory one musical cycle at a time, so very large offline blocks can span many cycles without whole-block scheduler overflow.

## State

State V5 stores the shared musical settings plus the exact generated Guitar, Bass, Drum, Pad and Synth phrases. This freezes project recall even if generation algorithms change later. Legacy V1-V4 states remain supported; their missing companion roles are regenerated once during migration and are then preserved by the next V5 save.

## Automated verification

The development branch checks deterministic generation, scale safety, phrase integrity, role-specific controls, style behavior, Pad voice-leading/context-follow, Synth motif/harmony follow, Drum Humanize invariance, verified drum maps, five dedicated event output buses, per-bus balance/flush, large offline blocks with cycle-chunked scheduling, state roundtrip plus frozen V1-V4 migration fixtures, GUI/editor lifecycle, Steinberg Validator and statistical measurement reports.

A frozen deterministic **golden five-role arrangement fingerprint** protects a known NDH / A Phrygian four-bar reference so unintended musical changes fail CI immediately.

## Branches

Active development happens on **development**.

The default `main` branch is not the current implementation baseline and should not be used as the source of truth for ongoing engineering work.
