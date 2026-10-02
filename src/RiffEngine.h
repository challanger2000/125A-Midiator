#pragma once

#include <array>
#include <cstdint>

namespace midiator {

constexpr int kStepsPerBar = 16;
constexpr int kMaxBars = 16;
constexpr int kMaxSteps = kStepsPerBar * kMaxBars;
constexpr int kMaxNotesPerStep = 2;

enum class StyleId : int {
    NDHIndustrial = 0,
    DarkRockGothic,
    HeavyIndustrial,
    Count
};

enum class SectionType : int {
    Free = 0,
    Intro,
    Verse,
    PreChorus,
    Chorus,
    Breakdown,
    Outro,
    Count
};

enum class ScaleId : int {
    NaturalMinor = 0,
    Phrygian,
    Dorian,
    HarmonicMinor,
    PhrygianDominant,
    MinorPentatonic,
    Blues,
    Count
};

struct ScaleDefinition {
    const char* name = "";
    std::array<int, 8> intervals{};
    int count = 0;
    const char* character = "";
    const char* characteristicInterval = "";
};

struct GeneratorSettings {
    int rootPitchClass = 9;       // A
    ScaleId scale = ScaleId::Phrygian;
    StyleId style = StyleId::NDHIndustrial;
    SectionType section = SectionType::Free;
    int bars = 2;
    float density = 0.56f;
    float complexity = 0.42f;
    float repetition = 0.72f;
    float powerChordChance = 0.25f;
    bool powerChordsEnabled = true;
    float palmMuteChance = 0.70f;
    int lowRootMidi = 33;         // A1
};

struct Note {
    int pitch = 0;
    int velocity = 0;
    int lengthSteps = 1;

    bool operator==(const Note& o) const {
        return pitch == o.pitch && velocity == o.velocity && lengthSteps == o.lengthSteps;
    }
};

struct Step {
    std::array<Note, kMaxNotesPerStep> notes{};
    int noteCount = 0;

    bool operator==(const Step& o) const {
        if (noteCount != o.noteCount)
            return false;
        for (int i = 0; i < noteCount; ++i)
            if (!(notes[i] == o.notes[i]))
                return false;
        return true;
    }
};

struct Phrase {
    int bars = 2;
    std::array<Step, kMaxSteps> steps{};

    int usedSteps() const { return bars * kStepsPerBar; }

    bool operator==(const Phrase& o) const {
        if (bars != o.bars)
            return false;
        for (int i = 0; i < usedSteps(); ++i)
            if (!(steps[i] == o.steps[i]))
                return false;
        return true;
    }
};

// Stable musical-engine facade. The current implementation is the Guitar Brain;
// future Bass/Drum/Pad brains should use the shared Phrase/Step/Note types above
// instead of adding instrument conditionals to the guitar generator.
class RiffEngine {
public:
    static const ScaleDefinition& scaleDefinition(ScaleId id);
    static Phrase generate(const GeneratorSettings& settings, uint32_t seed);
    static Phrase vary(const Phrase& source,
                       const GeneratorSettings& settings,
                       float amount,
                       uint32_t seed);

    static bool isScaleTone(int pitch, int rootPitchClass, ScaleId scale);

private:
    static int nearestRootForPitchClass(int pitchClass, int aroundMidi);
};

} // namespace midiator
