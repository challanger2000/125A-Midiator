#include "RiffEngine.h"
#include "BassBrain.h"
#include "DrumBrain.h"
#include "PadBrain.h"
#include "SynthBrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace midiator;

namespace {

void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

float sweep(unsigned seed, unsigned salt) {
    return static_cast<float>((seed * (17u + salt * 11u) + salt * 23u) % 101u) / 100.0f;
}

bool isExactOctavePair(const PadStep& step) {
    for (int a = 0; a < step.noteCount; ++a)
        for (int b = a + 1; b < step.noteCount; ++b)
            if (std::abs(step.notes[a].pitch - step.notes[b].pitch) == 12)
                return true;
    return false;
}

} // namespace

int main() {
    static constexpr std::array<int,4> barsList{{1,2,4,8}};
    long long arrangements = 0;

    for (int scaleIndex = 0;
         scaleIndex < static_cast<int>(ScaleId::Count);
         ++scaleIndex) {
        const auto scale = static_cast<ScaleId>(scaleIndex);

        for (int styleIndex = 0;
             styleIndex < static_cast<int>(StyleId::Count);
             ++styleIndex) {
            const auto style = static_cast<StyleId>(styleIndex);

            for (int bars : barsList) {
                for (unsigned seed = 1; seed <= 64; ++seed) {
                    GeneratorSettings gs{};
                    gs.rootPitchClass = static_cast<int>((seed + scaleIndex * 3u +
                                                         styleIndex * 5u) % 12u);
                    gs.scale = scale;
                    gs.style = style;
                    gs.bars = bars;
                    gs.density = sweep(seed, 1);
                    gs.complexity = sweep(seed, 2);
                    gs.repetition = sweep(seed, 3);
                    gs.powerChordChance = sweep(seed, 4);
                    gs.palmMuteChance = sweep(seed, 5);
                    gs.powerChordsEnabled = (seed % 5u) != 0u;

                    const uint32_t baseSeed =
                        0xA1100000u ^
                        static_cast<uint32_t>(scaleIndex * 0x10000) ^
                        static_cast<uint32_t>(styleIndex * 0x1000) ^
                        static_cast<uint32_t>(bars * 0x100) ^
                        seed;

                    const auto guitar = RiffEngine::generate(gs, baseSeed);
                    require(guitar.bars == bars, "Guitar bars must match requested arrangement length");
                    require(guitar.usedSteps() == bars * kStepsPerBar,
                            "Guitar usedSteps must match bars");

                    for (int i = 0; i < guitar.usedSteps(); ++i) {
                        const auto& step = guitar.steps[i];
                        require(step.noteCount >= 0 && step.noteCount <= kMaxNotesPerStep,
                                "Guitar note count must remain bounded");
                        if (step.noteCount <= 0)
                            continue;

                        const auto& primary = step.notes[0];
                        require(primary.pitch >= 0 && primary.pitch <= 127,
                                "Guitar primary pitch must stay MIDI-safe");
                        require(primary.velocity >= 1 && primary.velocity <= 126,
                                "Guitar primary velocity must stay MIDI-safe");
                        require(primary.lengthSteps >= 1 && primary.lengthSteps <= 16,
                                "Guitar note length must stay bounded");
                        require(RiffEngine::isScaleTone(primary.pitch, gs.rootPitchClass, gs.scale),
                                "Guitar primary note must stay scale-safe");

                        if (step.noteCount == 2) {
                            const auto& fifth = step.notes[1];
                            require(fifth.pitch - primary.pitch == 7,
                                    "Guitar second note must remain an exact power-chord fifth");
                            require(fifth.pitch >= 0 && fifth.pitch <= 127,
                                    "Guitar fifth must stay MIDI-safe");
                        }
                    }

                    BassSettings bs{};
                    bs.rootPitchClass = gs.rootPitchClass;
                    bs.scale = gs.scale;
                    bs.style = gs.style;
                    bs.follow = sweep(seed, 6);
                    bs.movement = sweep(seed, 7);
                    bs.passing = sweep(seed, 8);
                    bs.octaveChance = sweep(seed, 9);
                    bs.sustain = sweep(seed, 10);
                    const auto bass = BassBrain::generate(
                        guitar, bs, baseSeed ^ 0xB4552026u);

                    require(bass.bars == bars, "Bass bars must match Guitar");
                    require(bass.usedSteps() == guitar.usedSteps(),
                            "Bass phrase length must match Guitar");
                    for (int i = 0; i < bass.usedSteps(); ++i) {
                        const auto& step = bass.steps[i];
                        require(step.noteCount >= 0 && step.noteCount <= 1,
                                "Bass must remain monophonic");
                        if (step.noteCount <= 0)
                            continue;
                        const auto& n = step.notes[0];
                        require(n.pitch >= 24 && n.pitch <= 60,
                                "Bass pitch must stay inside supported register");
                        require(n.velocity >= 1 && n.velocity <= 126,
                                "Bass velocity must stay MIDI-safe");
                        require(n.lengthSteps >= 1 && n.lengthSteps <= 16,
                                "Bass length must stay bounded");
                        require(RiffEngine::isScaleTone(n.pitch, bs.rootPitchClass, bs.scale),
                                "Bass note must stay scale-safe");
                    }

                    DrumSettings ds{};
                    ds.style = gs.style;
                    ds.follow = sweep(seed, 11);
                    ds.density = sweep(seed, 12);
                    ds.complexity = sweep(seed, 13);
                    ds.humanize = sweep(seed, 14);
                    const auto drums = DrumBrain::generate(
                        guitar, bass, ds, baseSeed ^ 0xD12A2026u);

                    require(drums.bars == bars, "Drum bars must match arrangement");
                    require(drums.usedSteps() == guitar.usedSteps(),
                            "Drum phrase length must match Guitar");
                    for (int i = 0; i < drums.usedSteps(); ++i) {
                        const auto& step = drums.steps[i];
                        require(step.hitCount >= 0 && step.hitCount <= kMaxDrumHitsPerStep,
                                "Drum hit count must stay bounded");
                        for (int h = 0; h < step.hitCount; ++h) {
                            require(static_cast<int>(step.hits[h].voice) >= 0 &&
                                    static_cast<int>(step.hits[h].voice) <
                                        static_cast<int>(DrumVoice::Count),
                                    "Drum voice enum must remain valid");
                            require(step.hits[h].velocity >= 1 &&
                                    step.hits[h].velocity <= 126,
                                    "Drum velocity must stay MIDI-safe");
                        }
                    }

                    PadSettings ps{};
                    ps.rootPitchClass = gs.rootPitchClass;
                    ps.scale = gs.scale;
                    ps.style = gs.style;
                    ps.movement = sweep(seed, 15);
                    ps.spread = sweep(seed, 16);
                    ps.tension = sweep(seed, 17);
                    ps.sustain = sweep(seed, 18);
                    ps.contextFollow = sweep(seed, 19);
                    const auto pads = PadBrain::generate(
                        guitar, bass, ps, baseSeed ^ 0x50414426u);

                    require(pads.bars == bars, "Pad bars must match arrangement");
                    require(pads.usedSteps() == guitar.usedSteps(),
                            "Pad phrase length must match Guitar");
                    for (int i = 0; i < pads.usedSteps(); ++i) {
                        const auto& step = pads.steps[i];
                        require(step.noteCount >= 0 && step.noteCount <= kMaxPadVoices,
                                "Pad voice count must stay bounded");
                        if (step.noteCount <= 0)
                            continue;

                        bool pcs[12]{};
                        int uniquePcs = 0;
                        for (int n = 0; n < step.noteCount; ++n) {
                            const auto& note = step.notes[n];
                            require(note.pitch >= 45 && note.pitch <= 88,
                                    "Pad pitch must stay inside supported register");
                            require(note.velocity >= 1 && note.velocity <= 126,
                                    "Pad velocity must stay MIDI-safe");
                            require(note.lengthSteps >= 1 && note.lengthSteps <= kMaxSteps,
                                    "Pad length must stay bounded");
                            require(RiffEngine::isScaleTone(
                                        note.pitch, ps.rootPitchClass, ps.scale),
                                    "Pad note must stay scale-safe");
                            const int pc = (note.pitch % 12 + 12) % 12;
                            if (!pcs[pc]) {
                                pcs[pc] = true;
                                ++uniquePcs;
                            }
                        }
                        require(uniquePcs >= 2 && uniquePcs <= 3,
                                "Pad harmony must use exactly 2-3 distinct pitch classes");
                        if (step.noteCount == 4)
                            require(isExactOctavePair(step),
                                    "Pad fourth voice must be an exact octave doubling");
                    }

                    SynthSettings ss{};
                    ss.rootPitchClass = gs.rootPitchClass;
                    ss.scale = gs.scale;
                    ss.style = gs.style;
                    ss.activity = sweep(seed, 20);
                    ss.movement = sweep(seed, 21);
                    ss.repetition = sweep(seed, 22);
                    ss.syncopation = sweep(seed, 23);
                    ss.sustain = sweep(seed, 24);
                    ss.harmonicFollow = sweep(seed, 25);
                    const auto synth = SynthBrain::generate(
                        guitar, bass, pads, ss, baseSeed ^ 0x53594E26u);

                    require(synth.bars == bars, "Synth bars must match arrangement");
                    require(synth.usedSteps() == guitar.usedSteps(),
                            "Synth phrase length must match Guitar");
                    for (int i = 0; i < synth.usedSteps(); ++i) {
                        const auto& step = synth.steps[i];
                        require(step.noteCount >= 0 && step.noteCount <= 2,
                                "Synth must stay within compact one/two-note gestures");
                        if (step.noteCount <= 0)
                            continue;

                        for (int n = 0; n < step.noteCount; ++n) {
                            const auto& note = step.notes[n];
                            require(note.pitch >= 48 && note.pitch <= 96,
                                    "Synth pitch must stay inside supported register");
                            require(note.velocity >= 1 && note.velocity <= 126,
                                    "Synth velocity must stay MIDI-safe");
                            require(note.lengthSteps >= 1 && note.lengthSteps <= 2,
                                    "Synth gestures must stay short");
                            require(RiffEngine::isScaleTone(
                                        note.pitch, ss.rootPitchClass, ss.scale),
                                    "Synth note must stay scale-safe");
                        }
                        if (step.noteCount == 2)
                            require(step.notes[0].pitch != step.notes[1].pitch,
                                    "Synth chord-stab notes must remain distinct");
                    }

                    ++arrangements;
                }
            }
        }
    }

    require(arrangements == 7ll * 3ll * 4ll * 64ll,
            "property matrix must execute the complete arrangement grid");
    std::cout << "Midiator arrangement property matrix: PASS ("
              << arrangements << " arrangements)\n";
    return 0;
}
