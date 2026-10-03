#pragma once

#include <array>
#include <cstdint>

namespace midiator {

constexpr int kStepsPerBar = 16;
constexpr int kMaxBars = 16;
constexpr int kMaxSteps = kStepsPerBar * kMaxBars;
constexpr int kMaxNotesPerStep = 2;

enum class StyleId : int {
    // IDs 0..2 are frozen for V3+ project-state compatibility.
    NDHIndustrial = 0,
    DarkRockGothic,
    HeavyIndustrial,
    ClassicHeavy,
    Thrash,
    Groove,
    Death,
    MelodicDeath,
    Metalcore,
    NuMetal,
    Doom,
    DjentProgressive,
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

struct NewRiffAcceptance {
    int minStructuralDifference = 6;
    double maxOnsetJaccard = 0.48;
};

// Dense metal languages naturally share more 16th-note onset positions than
// sparse/groove languages. NEW RIFF therefore uses a style-aware overlap
// ceiling while still requiring substantial structural change. The original
// three styles deliberately keep their historical acceptance contract.
inline NewRiffAcceptance newRiffAcceptance(StyleId style,
                                           int usedSteps) noexcept {
    auto scaledDifference = [usedSteps](int divisor) noexcept {
        const int scaled = usedSteps / divisor;
        return scaled > 6 ? scaled : 6;
    };

    switch (style) {
        case StyleId::ClassicHeavy:
            return {scaledDifference(4), 0.62};
        case StyleId::Thrash:
            return {scaledDifference(3), 0.72};
        case StyleId::Groove:
            return {scaledDifference(3), 0.52};
        case StyleId::Death:
            return {scaledDifference(3), 0.78};
        case StyleId::MelodicDeath:
            return {scaledDifference(4), 0.62};
        case StyleId::Metalcore:
            return {scaledDifference(3), 0.52};
        case StyleId::NuMetal:
            return {scaledDifference(3), 0.52};
        case StyleId::Doom:
            return {scaledDifference(4), 0.55};
        case StyleId::DjentProgressive:
            return {scaledDifference(3), 0.52};
        case StyleId::NDHIndustrial:
        case StyleId::DarkRockGothic:
        case StyleId::HeavyIndustrial:
        case StyleId::Count:
            return {scaledDifference(3), 0.48};
    }
    return {scaledDifference(3), 0.48};
}

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
    // Guitar-library articulation contract: palm-muted NoteOns are always
    // strictly below this MIDI velocity. New projects default to 30 so a
    // single Guitar Out can drive libraries with <30 as well as <40 zones.
    int palmMuteVelocityThreshold = 30;
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
