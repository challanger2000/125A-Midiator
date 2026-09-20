#include "RiffEngine.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void testBarsAndScaleSafety() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 9; // A
    s.scale = midiator::ScaleId::Phrygian;

    for (int bars : {1, 2, 4, 8}) {
        s.bars = bars;
        const auto phrase = midiator::RiffEngine::generate(s, 12345u + static_cast<unsigned>(bars));
        require(phrase.bars == bars, "bar count must be preserved");
        require(phrase.usedSteps() == bars * 16, "4/4 must use 16 sixteenth steps per bar");

        bool foundNote = false;
        for (int i = 0; i < phrase.usedSteps(); ++i) {
            const auto& step = phrase.steps[i];
            for (int n = 0; n < step.noteCount; ++n) {
                foundNote = true;
                const auto& note = step.notes[n];
                require(note.velocity >= 1 && note.velocity <= 126, "velocity 127 is reserved and must not be generated");
                require(note.pitch >= 0 && note.pitch <= 127, "MIDI pitch must be valid");

                if (n == 0)
                    require(midiator::RiffEngine::isScaleTone(note.pitch, s.rootPitchClass, s.scale),
                            "primary riff notes must remain in the selected scale");
                else
                    require(note.pitch == step.notes[0].pitch + 7,
                            "second note in V1 power chord must be a perfect fifth");
            }
        }
        require(foundNote, "generated phrase must contain notes");
    }
}

void testDeterminism() {
    midiator::GeneratorSettings s{};
    const auto a = midiator::RiffEngine::generate(s, 0x12345678u);
    const auto b = midiator::RiffEngine::generate(s, 0x12345678u);
    require(a == b, "same seed and settings must produce identical riff");
}

void testVariationKeepsTonalFrame() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 9;
    s.scale = midiator::ScaleId::Phrygian;
    s.bars = 4;

    const auto source = midiator::RiffEngine::generate(s, 777u);
    const auto unchanged = midiator::RiffEngine::vary(source, s, 0.0f, 888u);
    require(source == unchanged, "0 percent variation must be bit-identical");

    const auto varied = midiator::RiffEngine::vary(source, s, 0.55f, 999u);
    require(varied.bars == source.bars, "variation must not change phrase length");

    for (int i = 0; i < varied.usedSteps(); ++i) {
        const auto& step = varied.steps[i];
        for (int n = 0; n < step.noteCount; ++n) {
            const auto& note = step.notes[n];
            require(note.velocity <= 126, "variation must never create velocity 127");
            if (n == 0)
                require(midiator::RiffEngine::isScaleTone(note.pitch, s.rootPitchClass, s.scale),
                        "variation must remain in selected root and scale");
            else
                require(note.pitch == step.notes[0].pitch + 7,
                        "variation power chord fifth must remain intact");
        }
    }
}

void testAllScalesGenerateValidTones() {
    midiator::GeneratorSettings s{};
    s.rootPitchClass = 4; // E
    s.bars = 2;

    for (int scale = 0; scale < static_cast<int>(midiator::ScaleId::Count); ++scale) {
        s.scale = static_cast<midiator::ScaleId>(scale);
        const auto phrase = midiator::RiffEngine::generate(s, 1000u + static_cast<unsigned>(scale));
        for (int i = 0; i < phrase.usedSteps(); ++i) {
            if (phrase.steps[i].noteCount > 0)
                require(midiator::RiffEngine::isScaleTone(
                            phrase.steps[i].notes[0].pitch, s.rootPitchClass, s.scale),
                        "all supported scales must constrain primary pitches");
        }
    }
}

} // namespace

int main() {
    testBarsAndScaleSafety();
    testDeterminism();
    testVariationKeepsTonalFrame();
    testAllScalesGenerateValidTones();

    std::cout << "Midiator core tests: PASS\n";
    return 0;
}
