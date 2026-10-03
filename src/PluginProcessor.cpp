#include "PluginProcessor.h"
#include "SectionProfiles.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "vstgui/lib/controls/ctextlabel.h"
#include "vstgui/lib/controls/ccontrol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace Steinberg::Vst {
namespace {

constexpr double kStepQuarterNotes = 0.25;
constexpr int kMaxScheduledEvents = 8192;
constexpr int kMaxTriggerInputEvents = 2048;
constexpr uint32_t kStateMagic = 0x4D445231u; // "MDR1"
constexpr uint32_t kStateVersion = 14u;
constexpr int kLegacyStateSteps = 128; // V1-V7 fixed phrase payload width
constexpr const char* kMsgNewRiff = "125A.Midiator.NewRiff";
constexpr const char* kMsgVariation = "125A.Midiator.Variation";

template <typename T>
bool writeValue(IBStream* stream, const T& value) {
    if (!stream)
        return false;
    int32 written = 0;
    return stream->write((void*)&value, static_cast<int32>(sizeof(T)), &written) == kResultOk &&
           written == static_cast<int32>(sizeof(T));
}

template <typename T>
bool readValue(IBStream* stream, T& value) {
    if (!stream)
        return false;
    int32 read = 0;
    return stream->read(&value, static_cast<int32>(sizeof(T)), &read) == kResultOk &&
           read == static_cast<int32>(sizeof(T));
}


bool writePhraseState(IBStream* state, const midiator::Phrase& phrase) {
    const int32 phraseBars = phrase.bars;
    if (!writeValue(state, phraseBars))
        return false;

    for (int i = 0; i < midiator::kMaxSteps; ++i) {
        const auto& step = phrase.steps[i];
        const int32 noteCount = std::clamp(step.noteCount, 0, midiator::kMaxNotesPerStep);
        if (!writeValue(state, noteCount))
            return false;

        for (int n = 0; n < midiator::kMaxNotesPerStep; ++n) {
            const int32 pitch = step.notes[n].pitch;
            const int32 velocity = step.notes[n].velocity;
            const int32 lengthSteps = step.notes[n].lengthSteps;
            if (!writeValue(state, pitch) || !writeValue(state, velocity) ||
                !writeValue(state, lengthSteps))
                return false;
        }
    }
    return true;
}

bool readPhraseState(IBStream* state, midiator::Phrase& phrase, int fallbackBars,
                     int serializedSteps, bool allow16Bars,
                     bool strict = false) {
    int32 phraseBars = 0;
    if (!readValue(state, phraseBars))
        return false;

    const bool validLegacyBars =
        phraseBars == 1 || phraseBars == 2 || phraseBars == 4 || phraseBars == 8;
    const bool validBars = validLegacyBars || (allow16Bars && phraseBars == 16);
    if (strict && !validBars)
        return false;
    if (serializedSteps < 1 || serializedSteps > midiator::kMaxSteps)
        return false;

    phrase.bars = validBars ? phraseBars : fallbackBars;
    for (auto& step : phrase.steps)
        step = {};

    for (int i = 0; i < serializedSteps; ++i) {
        int32 noteCount = 0;
        if (!readValue(state, noteCount))
            return false;
        if (strict && (noteCount < 0 || noteCount > midiator::kMaxNotesPerStep))
            return false;
        phrase.steps[i].noteCount =
            std::clamp<int32>(noteCount, 0, midiator::kMaxNotesPerStep);

        for (int n = 0; n < midiator::kMaxNotesPerStep; ++n) {
            int32 pitch = 0, velocity = 0, lengthSteps = 1;
            if (!readValue(state, pitch) || !readValue(state, velocity) ||
                !readValue(state, lengthSteps))
                return false;
            if (strict && (pitch < 0 || pitch > 127 ||
                           velocity < 0 || velocity > 126 ||
                           lengthSteps < 1 || lengthSteps > 16))
                return false;
            phrase.steps[i].notes[n].pitch = std::clamp<int32>(pitch, 0, 127);
            phrase.steps[i].notes[n].velocity = std::clamp<int32>(velocity, 0, 126);
            phrase.steps[i].notes[n].lengthSteps = std::clamp<int32>(lengthSteps, 1, 16);
        }
    }
    return true;
}

bool writeDrumPhraseState(IBStream* state, const midiator::DrumPhrase& phrase) {
    const int32 bars = phrase.bars;
    if (!writeValue(state, bars))
        return false;

    for (int i = 0; i < midiator::kMaxSteps; ++i) {
        const auto& step = phrase.steps[i];
        const int32 hitCount = std::clamp(step.hitCount, 0, midiator::kMaxDrumHitsPerStep);
        if (!writeValue(state, hitCount))
            return false;
        for (int n = 0; n < midiator::kMaxDrumHitsPerStep; ++n) {
            const int32 voice = static_cast<int32>(step.hits[n].voice);
            const int32 velocity = step.hits[n].velocity;
            if (!writeValue(state, voice) || !writeValue(state, velocity))
                return false;
        }
    }
    return true;
}

bool readDrumPhraseState(IBStream* state, midiator::DrumPhrase& phrase, int fallbackBars,
                         int serializedSteps, bool allow16Bars,
                         bool strict = false) {
    int32 bars = 0;
    if (!readValue(state, bars))
        return false;

    const bool validLegacyBars = bars == 1 || bars == 2 || bars == 4 || bars == 8;
    const bool validBars = validLegacyBars || (allow16Bars && bars == 16);
    if (strict && !validBars)
        return false;
    if (serializedSteps < 1 || serializedSteps > midiator::kMaxSteps)
        return false;

    phrase.bars = validBars ? bars : fallbackBars;
    for (auto& step : phrase.steps)
        step = {};

    for (int i = 0; i < serializedSteps; ++i) {
        int32 hitCount = 0;
        if (!readValue(state, hitCount))
            return false;
        if (strict && (hitCount < 0 || hitCount > midiator::kMaxDrumHitsPerStep))
            return false;
        phrase.steps[i].hitCount =
            std::clamp<int32>(hitCount, 0, midiator::kMaxDrumHitsPerStep);
        for (int n = 0; n < midiator::kMaxDrumHitsPerStep; ++n) {
            int32 voice = 0, velocity = 100;
            if (!readValue(state, voice) || !readValue(state, velocity))
                return false;
            const int32 maxVoice =
                static_cast<int32>(midiator::DrumVoice::Count) - 1;
            if (strict && (voice < 0 || voice > maxVoice ||
                           velocity < 1 || velocity > 126))
                return false;
            phrase.steps[i].hits[n].voice = static_cast<midiator::DrumVoice>(
                std::clamp<int32>(voice, 0, maxVoice));
            phrase.steps[i].hits[n].velocity = std::clamp<int32>(velocity, 1, 126);
        }
    }
    return true;
}

bool writePadPhraseState(IBStream* state, const midiator::PadPhrase& phrase) {
    const int32 bars = phrase.bars;
    if (!writeValue(state, bars))
        return false;

    for (int i = 0; i < midiator::kMaxSteps; ++i) {
        const auto& step = phrase.steps[i];
        const int32 noteCount = std::clamp(step.noteCount, 0, midiator::kMaxPadVoices);
        if (!writeValue(state, noteCount))
            return false;
        for (int n = 0; n < midiator::kMaxPadVoices; ++n) {
            const int32 pitch = step.notes[n].pitch;
            const int32 velocity = step.notes[n].velocity;
            const int32 lengthSteps = step.notes[n].lengthSteps;
            if (!writeValue(state, pitch) || !writeValue(state, velocity) ||
                !writeValue(state, lengthSteps))
                return false;
        }
    }
    return true;
}

bool readPadPhraseState(IBStream* state, midiator::PadPhrase& phrase, int fallbackBars,
                        int serializedSteps, bool allow16Bars,
                        bool strict = false) {
    int32 bars = 0;
    if (!readValue(state, bars))
        return false;

    const bool validLegacyBars = bars == 1 || bars == 2 || bars == 4 || bars == 8;
    const bool validBars = validLegacyBars || (allow16Bars && bars == 16);
    if (strict && !validBars)
        return false;
    if (serializedSteps < 1 || serializedSteps > midiator::kMaxSteps)
        return false;

    phrase.bars = validBars ? bars : fallbackBars;
    for (auto& step : phrase.steps)
        step = {};

    for (int i = 0; i < serializedSteps; ++i) {
        int32 noteCount = 0;
        if (!readValue(state, noteCount))
            return false;
        if (strict && (noteCount < 0 || noteCount > midiator::kMaxPadVoices))
            return false;
        phrase.steps[i].noteCount =
            std::clamp<int32>(noteCount, 0, midiator::kMaxPadVoices);
        for (int n = 0; n < midiator::kMaxPadVoices; ++n) {
            int32 pitch = 60, velocity = 76, lengthSteps = 1;
            if (!readValue(state, pitch) || !readValue(state, velocity) ||
                !readValue(state, lengthSteps))
                return false;
            if (strict && (pitch < 0 || pitch > 127 ||
                           velocity < 0 || velocity > 126 ||
                           lengthSteps < 1 || lengthSteps > serializedSteps))
                return false;
            phrase.steps[i].notes[n].pitch = std::clamp<int32>(pitch, 0, 127);
            phrase.steps[i].notes[n].velocity = std::clamp<int32>(velocity, 0, 126);
            phrase.steps[i].notes[n].lengthSteps =
                std::clamp<int32>(lengthSteps, 1, serializedSteps);
        }
    }
    return true;
}

bool readStateHeader(IBStream* state,
                     uint32_t& stateVersion,
                     midiator::GeneratorSettings& settings,
                     float& variationAmount,
                     uint32_t& seed,
                     int& manualRootPitchClass,
                     bool& midiRootSource,
                     bool& powerChordsEnabled,
                     midiator::DrumMapId& drumMapId,
                     midiator::BassSettings& bassSettings,
                     midiator::DrumSettings& drumSettings,
                     midiator::PadSettings& padSettings,
                     midiator::SynthSettings& synthSettings) {
    uint32_t magic = 0;
    uint32_t version = 0;
    int32 root = 0;
    int32 scale = 0;
    int32 bars = 0;

    if (!readValue(state, magic) || !readValue(state, version) ||
        magic != kStateMagic || (version < 1u || version > kStateVersion) ||
        !readValue(state, root) || !readValue(state, scale) || !readValue(state, bars) ||
        !readValue(state, settings.density) || !readValue(state, settings.complexity) ||
        !readValue(state, settings.repetition) || !readValue(state, settings.powerChordChance) ||
        !readValue(state, settings.palmMuteChance) || !readValue(state, variationAmount) ||
        !readValue(state, seed)) {
        return false;
    }

    const bool strictCurrent = version >= 5u;
    const bool validLegacyBars =
        bars == 1 || bars == 2 || bars == 4 || bars == 8;
    const bool validBars =
        validLegacyBars || (version >= 8u && bars == 16);
    const bool validScale =
        scale >= 0 && scale < static_cast<int32>(midiator::ScaleId::Count);
    const bool validUnitFloats =
        std::isfinite(settings.density) && settings.density >= 0.0f && settings.density <= 1.0f &&
        std::isfinite(settings.complexity) && settings.complexity >= 0.0f && settings.complexity <= 1.0f &&
        std::isfinite(settings.repetition) && settings.repetition >= 0.0f && settings.repetition <= 1.0f &&
        std::isfinite(settings.powerChordChance) && settings.powerChordChance >= 0.0f && settings.powerChordChance <= 1.0f &&
        std::isfinite(settings.palmMuteChance) && settings.palmMuteChance >= 0.0f && settings.palmMuteChance <= 1.0f &&
        std::isfinite(variationAmount) && variationAmount >= 0.0f && variationAmount <= 1.0f;

    if (strictCurrent &&
        (root < 0 || root > 11 || !validScale || !validBars || !validUnitFloats))
        return false;

    settings.rootPitchClass = std::clamp<int32>(root, 0, 11);

    if (version >= 2u) {
        int32 storedManualRoot = settings.rootPitchClass;
        int32 storedRootSource = 1;
        if (!readValue(state, storedManualRoot) || !readValue(state, storedRootSource))
            return false;
        if (strictCurrent &&
            (storedManualRoot < 0 || storedManualRoot > 11 ||
             (storedRootSource != 0 && storedRootSource != 1)))
            return false;
        manualRootPitchClass = std::clamp<int32>(storedManualRoot, 0, 11);
        midiRootSource = storedRootSource != 0;

        if (version >= 3u) {
            int32 storedStyle = 0;
            if (!readValue(state, storedStyle))
                return false;
            // V3-V10 only knew the original three style IDs. The unreleased
            // V11 development line and V12 know the expanded enum. Historical
            // states must not reinterpret formerly invalid values as new genres.
            const int32 maxStyle =
                version >= 11u
                    ? static_cast<int32>(midiator::StyleId::Count) - 1
                    : static_cast<int32>(midiator::StyleId::HeavyIndustrial);
            if (strictCurrent && (storedStyle < 0 || storedStyle > maxStyle))
                return false;
            settings.style = static_cast<midiator::StyleId>(
                std::clamp<int32>(storedStyle, 0, maxStyle));
        } else {
            settings.style = midiator::StyleId::NDHIndustrial;
        }

        if (version >= 4u) {
            int32 storedPowerChordsEnabled = 1;
            if (!readValue(state, storedPowerChordsEnabled))
                return false;
            if (strictCurrent &&
                (storedPowerChordsEnabled != 0 && storedPowerChordsEnabled != 1))
                return false;
            powerChordsEnabled = storedPowerChordsEnabled != 0;
            settings.powerChordsEnabled = powerChordsEnabled;
        } else {
            powerChordsEnabled = true;
            settings.powerChordsEnabled = true;
        }

        if (version >= 6u) {
            int32 storedDrumMap = static_cast<int32>(midiator::DrumMapId::GeneralMidi);
            if (!readValue(state, storedDrumMap))
                return false;
            const bool verifiedMap =
                storedDrumMap == static_cast<int32>(midiator::DrumMapId::GeneralMidi) ||
                storedDrumMap == static_cast<int32>(midiator::DrumMapId::EZdrummer3) ||
                storedDrumMap == static_cast<int32>(midiator::DrumMapId::PerfectDrums);
            if (!verifiedMap)
                return false;
            drumMapId = static_cast<midiator::DrumMapId>(storedDrumMap);
        } else {
            drumMapId = midiator::DrumMapId::GeneralMidi;
        }

        if (version >= 7u) {
            if (!readValue(state, bassSettings.follow) ||
                !readValue(state, bassSettings.movement) ||
                !readValue(state, drumSettings.density) ||
                !readValue(state, drumSettings.complexity) ||
                !readValue(state, padSettings.spread) ||
                !readValue(state, padSettings.tension) ||
                !readValue(state, synthSettings.activity) ||
                !readValue(state, synthSettings.movement))
                return false;

            const float values[] = {
                bassSettings.follow, bassSettings.movement,
                drumSettings.density, drumSettings.complexity,
                padSettings.spread, padSettings.tension,
                synthSettings.activity, synthSettings.movement
            };
            for (float value : values)
                if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                    return false;

            if (version >= 9u) {
                if (!readValue(state, drumSettings.fillIntensity) ||
                    !std::isfinite(drumSettings.fillIntensity) ||
                    drumSettings.fillIntensity < 0.0f ||
                    drumSettings.fillIntensity > 1.0f)
                    return false;
            } else {
                drumSettings.fillIntensity = 0.50f;
            }

            if (version >= 10u) {
                int32 storedSection = 0;
                if (!readValue(state, storedSection))
                    return false;

                // The abandoned development-only Song Mode also used V11 and
                // packed AUTO into bit 0x100 of the Section word. V12 has no
                // Song Mode. Accept that V11 header, discard the retired flag,
                // and keep the selected manual Section. Extra cached song data
                // follows the normal five-role payload and is intentionally
                // ignored by the current state reader.
                if (version == 11u) {
                    constexpr int32 kRetiredSongModeFlag = 0x100;
                    constexpr int32 kAllowedV11SectionBits =
                        kRetiredSongModeFlag | 0xFF;
                    if (storedSection < 0 ||
                        (storedSection & ~kAllowedV11SectionBits) != 0)
                        return false;
                    storedSection &= 0xFF;
                }

                const int32 maxSection =
                    static_cast<int32>(midiator::SectionType::Count) - 1;
                if (storedSection < 0 || storedSection > maxSection)
                    return false;
                settings.section = static_cast<midiator::SectionType>(storedSection);
            } else {
                settings.section = midiator::SectionType::Free;
            }
        } else {
            bassSettings.follow = 0.72f;
            bassSettings.movement = 0.34f;
            drumSettings.density = 0.48f;
            drumSettings.complexity = 0.30f;
            drumSettings.fillIntensity = 0.50f;
            padSettings.spread = 0.42f;
            padSettings.tension = 0.18f;
            synthSettings.activity = 0.46f;
            synthSettings.movement = 0.42f;
            settings.section = midiator::SectionType::Free;
        }
    } else {
        // V1 had only one fixed root and therefore maps naturally to Manual.
        manualRootPitchClass = settings.rootPitchClass;
        midiRootSource = false;
        settings.style = midiator::StyleId::NDHIndustrial;
        powerChordsEnabled = true;
        settings.powerChordsEnabled = true;
        drumMapId = midiator::DrumMapId::GeneralMidi;
        bassSettings.follow = 0.72f;
        bassSettings.movement = 0.34f;
        drumSettings.density = 0.48f;
        drumSettings.complexity = 0.30f;
        drumSettings.fillIntensity = 0.50f;
        padSettings.spread = 0.42f;
        padSettings.tension = 0.18f;
        synthSettings.activity = 0.46f;
        synthSettings.movement = 0.42f;
        settings.section = midiator::SectionType::Free;
    }

    settings.scale = static_cast<midiator::ScaleId>(
        std::clamp<int32>(scale, 0, static_cast<int32>(midiator::ScaleId::Count) - 1));
    settings.bars = validBars ? bars : 2;
    settings.density = std::clamp(settings.density, 0.0f, 1.0f);
    settings.complexity = std::clamp(settings.complexity, 0.0f, 1.0f);
    settings.repetition = std::clamp(settings.repetition, 0.0f, 1.0f);
    settings.powerChordChance = std::clamp(settings.powerChordChance, 0.0f, 1.0f);
    settings.palmMuteChance = std::clamp(settings.palmMuteChance, 0.0f, 1.0f);
    variationAmount = std::clamp(variationAmount, 0.0f, 1.0f);
    stateVersion = version;
    return true;
}

bool readExtendedStateTail(IBStream* state,
                           uint32_t version,
                           int& threshold,
                           bool& midiNoteTrigger) {
    // V12 and older encoded the historical fixed PM zone ending at velocity
    // 40. Use an exclusive threshold of 41 to reproduce that behavior exactly.
    threshold = version >= 13u ? 30 : 41;
    midiNoteTrigger = false;

    if (version >= 13u) {
        int32 storedThreshold = 30;
        if (!readValue(state, storedThreshold) ||
            storedThreshold < 3 || storedThreshold > 87)
            return false;
        threshold = storedThreshold;
    }

    if (version >= 14u) {
        int32 storedTrigger = 0;
        if (!readValue(state, storedTrigger) ||
            (storedTrigger != 0 && storedTrigger != 1))
            return false;
        midiNoteTrigger = storedTrigger != 0;
    }
    return true;
}

uint32_t nextSeed(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x ? x : 0x125A2026u;
}

int normalizedIndex(ParamValue v, int count) {
    if (count <= 1)
        return 0;
    return std::clamp(static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * (count - 1))), 0, count - 1);
}


} // namespace

MidiatorProcessor::MidiatorProcessor() {
    setControllerClass(ControllerUID);

    // The sequencer depends on musical timeline position, tempo and
    // transport play/stop state for sample-accurate event scheduling.
    processContextRequirements.needProjectTimeMusic()
                              .needBarPositionMusic()
                              .needTempo()
                              .needTimeSignature()
                              .needTransportState();

    // generateNew() already regenerates Bass -> Drums -> Pad -> Synth.
    generateNew();
}

tresult PLUGIN_API MidiatorProcessor::initialize(FUnknown* context) {
    auto r = AudioEffect::initialize(context);
    if (r != kResultOk)
        return r;

    addEventInput(STR16("MIDI In"), 16, kMain, BusInfo::kDefaultActive);

    // Each musical role owns a real VST3 event output bus. This is deliberate:
    // hosts can route every generated part to a separate instrument without
    // relying on MIDI-channel multiplexing inside one shared output.
    addEventOutput(STR16("Guitar Out"), 16, kMain, BusInfo::kDefaultActive);
    addEventOutput(STR16("Bass Out"),   16, kAux,  BusInfo::kDefaultActive);
    addEventOutput(STR16("Drums Out"),  16, kAux,  BusInfo::kDefaultActive);
    addEventOutput(STR16("Pad Out"),    16, kAux,  BusInfo::kDefaultActive);
    addEventOutput(STR16("Synth Out"),  16, kAux,  BusInfo::kDefaultActive);
    return kResultOk;
}

tresult PLUGIN_API MidiatorProcessor::setActive(TBool state) {
    if (!state) {
        // No event list is available from setActive(false), so never discard
        // active-note knowledge here. Force a defensive all-note flush on the
        // next process call after reactivation.
        lifecyclePanicPending_ = true;
    } else if (!lifecyclePanicPending_) {
        // Initial activation has no preceding notes to preserve.
        for (auto& bus : activePitchesByBus_)
            bus.fill(false);
    }

    wasPlaying_ = false;
    haveExpectedProjectTime_ = false;
    expectedProjectTimeQn_ = 0.0;
    haveTransportAnchor_ = false;
    transportAnchorQn_ = 0.0;
    triggerHeldPitches_.fill(false);
    triggerHeldCount_ = 0;
    return AudioEffect::setActive(state);
}

tresult PLUGIN_API MidiatorProcessor::setProcessing(TBool state) {
    // Hosts may call setProcessing(false) before they deliver a final stopped
    // process block. There is no IEventList here, so clearing active pitches
    // would make a later NoteOff recovery impossible. Preserve the state and
    // request an all-note panic on the next process call instead.
    if (!state) {
        lifecyclePanicPending_ = true;
    } else if (!lifecyclePanicPending_) {
        for (auto& bus : activePitchesByBus_)
            bus.fill(false);
    }

    wasPlaying_ = false;
    haveExpectedProjectTime_ = false;
    expectedProjectTimeQn_ = 0.0;
    haveTransportAnchor_ = false;
    transportAnchorQn_ = 0.0;
    triggerHeldPitches_.fill(false);
    triggerHeldCount_ = 0;
    return kResultOk;
}

tresult PLUGIN_API MidiatorProcessor::canProcessSampleSize(int32 symbolicSampleSize) {
    // Midiator processes only VST3 event data and never dereferences audio
    // buffers, so both standard host sample formats are equally valid.
    return (symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64)
        ? kResultTrue
        : kResultFalse;
}

tresult PLUGIN_API MidiatorProcessor::getState(IBStream* state) {
    if (!state)
        return kInvalidArgument;

    const int32 root = settings_.rootPitchClass;
    const int32 scale = static_cast<int32>(settings_.scale);
    const int32 bars = settings_.bars;

    if (!writeValue(state, kStateMagic) || !writeValue(state, kStateVersion) ||
        !writeValue(state, root) || !writeValue(state, scale) || !writeValue(state, bars) ||
        !writeValue(state, settings_.density) || !writeValue(state, settings_.complexity) ||
        !writeValue(state, settings_.repetition) || !writeValue(state, settings_.powerChordChance) ||
        !writeValue(state, settings_.palmMuteChance) || !writeValue(state, variationAmount_) ||
        !writeValue(state, seed_)) {
        return kResultFalse;
    }

    const int32 manualRoot = manualRootPitchClass_;
    const int32 rootSource = midiRootSource_ ? 1 : 0;
    const int32 style = static_cast<int32>(settings_.style);
    const int32 powerChordsEnabled = settings_.powerChordsEnabled ? 1 : 0;
    const int32 drumMap = static_cast<int32>(drumMapId_);
    if (!writeValue(state, manualRoot) || !writeValue(state, rootSource) ||
        !writeValue(state, style) || !writeValue(state, powerChordsEnabled) ||
        !writeValue(state, drumMap) ||
        !writeValue(state, bassSettings_.follow) ||
        !writeValue(state, bassSettings_.movement) ||
        !writeValue(state, drumSettings_.density) ||
        !writeValue(state, drumSettings_.complexity) ||
        !writeValue(state, padSettings_.spread) ||
        !writeValue(state, padSettings_.tension) ||
        !writeValue(state, synthSettings_.activity) ||
        !writeValue(state, synthSettings_.movement) ||
        !writeValue(state, drumSettings_.fillIntensity) ||
        !writeValue(state, static_cast<int32>(settings_.section)))
        return kResultFalse;

    if (!writePhraseState(state, phrase_) ||
        !writePhraseState(state, bassPhrase_) ||
        !writeDrumPhraseState(state, drumPhrase_) ||
        !writePadPhraseState(state, padPhrase_) ||
        !writePhraseState(state, synthPhrase_)) {
        return kResultFalse;
    }

    const int32 palmMuteVelocityThreshold =
        std::clamp(settings_.palmMuteVelocityThreshold, 3, 87);
    if (!writeValue(state, palmMuteVelocityThreshold))
        return kResultFalse;
    const int32 triggerMode = midiNoteTrigger_ ? 1 : 0;
    if (!writeValue(state, triggerMode))
        return kResultFalse;

    return kResultOk;
}

tresult PLUGIN_API MidiatorProcessor::setState(IBStream* state) {
    if (!state)
        return kInvalidArgument;

    midiator::GeneratorSettings restored = settings_;
    float restoredVariation = variationAmount_;
    uint32_t restoredSeed = seed_;
    int restoredManualRoot = manualRootPitchClass_;
    bool restoredMidiRootSource = midiRootSource_;
    bool restoredPowerChordsEnabled = settings_.powerChordsEnabled;
    midiator::DrumMapId restoredDrumMapId = drumMapId_;
    midiator::BassSettings restoredBassSettings = bassSettings_;
    midiator::DrumSettings restoredDrumSettings = drumSettings_;
    midiator::PadSettings restoredPadSettings = padSettings_;
    midiator::SynthSettings restoredSynthSettings = synthSettings_;
    bool restoredMidiNoteTrigger = false;

    uint32_t restoredStateVersion = 0;
    if (!readStateHeader(state, restoredStateVersion, restored, restoredVariation, restoredSeed,
                         restoredManualRoot, restoredMidiRootSource,
                         restoredPowerChordsEnabled, restoredDrumMapId,
                         restoredBassSettings, restoredDrumSettings,
                         restoredPadSettings, restoredSynthSettings))
        return kResultFalse;

    midiator::Phrase restoredPhrase{};
    const bool strictV5 = restoredStateVersion >= 5u;
    const int serializedSteps =
        restoredStateVersion >= 8u ? midiator::kMaxSteps : kLegacyStateSteps;
    const bool allow16Bars = restoredStateVersion >= 8u;
    if (!readPhraseState(state, restoredPhrase, restored.bars,
                         serializedSteps, allow16Bars, strictV5))
        return kResultFalse;

    midiator::Phrase restoredBassPhrase{};
    midiator::DrumPhrase restoredDrumPhrase{};
    midiator::PadPhrase restoredPadPhrase{};
    midiator::Phrase restoredSynthPhrase{};

    if (restoredStateVersion >= 5u) {
        if (!readPhraseState(state, restoredBassPhrase, restored.bars,
                             serializedSteps, allow16Bars, true) ||
            !readDrumPhraseState(state, restoredDrumPhrase, restored.bars,
                                serializedSteps, allow16Bars, true) ||
            !readPadPhraseState(state, restoredPadPhrase, restored.bars,
                               serializedSteps, allow16Bars, true) ||
            !readPhraseState(state, restoredSynthPhrase, restored.bars,
                             serializedSteps, allow16Bars, true)) {
            return kResultFalse;
        }
    }

    if (!readExtendedStateTail(
            state, restoredStateVersion, restored.palmMuteVelocityThreshold,
            restoredMidiNoteTrigger))
        return kResultFalse;

    settings_ = restored;
    variationAmount_ = restoredVariation;
    seed_ = restoredSeed ? restoredSeed : 0x125A2026u;
    manualRootPitchClass_ = restoredManualRoot;
    midiRootSource_ = restoredMidiRootSource;
    settings_.powerChordsEnabled = restoredPowerChordsEnabled;
    drumMapId_ = restoredDrumMapId;
    drumMap_ = midiator::DrumMidiMap::preset(drumMapId_);
    bassSettings_ = restoredBassSettings;
    drumSettings_ = restoredDrumSettings;
    padSettings_ = restoredPadSettings;
    synthSettings_ = restoredSynthSettings;
    midiNoteTrigger_ = restoredMidiNoteTrigger;
    triggerHeldPitches_.fill(false);
    triggerHeldCount_ = 0;
    phrase_ = restoredPhrase;

    if (restoredStateVersion >= 5u) {
        bassPhrase_ = restoredBassPhrase;
        drumPhrase_ = restoredDrumPhrase;
        padPhrase_ = restoredPadPhrase;
        synthPhrase_ = restoredSynthPhrase;
    } else {
        // V1-V4 stored only Guitar. Recreate companion roles once during
        // migration; subsequent V7 saves freeze the exact generated result.
        regenerateBass();
    }

    // If state is restored while processing, preserve knowledge of currently
    // active notes so the next process call can emit proper NoteOff events.
    phraseChangedNeedsFlush_ = true;
    wasPlaying_ = false;
    haveExpectedProjectTime_ = false;
    expectedProjectTimeQn_ = 0.0;
    haveTransportAnchor_ = false;
    transportAnchorQn_ = 0.0;
    return kResultOk;
}

void MidiatorProcessor::generateNew() {
    const auto previous = phrase_;

    auto structuralDifference = [](const midiator::Phrase& a, const midiator::Phrase& b) {
        const int used = std::min(a.usedSteps(), b.usedSteps());
        int different = std::abs(a.usedSteps() - b.usedSteps());

        for (int i = 0; i < used; ++i) {
            const auto& x = a.steps[i];
            const auto& y = b.steps[i];

            if (x.noteCount != y.noteCount) {
                ++different;
                continue;
            }
            if (x.noteCount == 0)
                continue;

            if (x.notes[0].pitch != y.notes[0].pitch ||
                x.notes[0].lengthSteps != y.notes[0].lengthSteps ||
                (x.noteCount > 1) != (y.noteCount > 1)) {
                ++different;
            }
        }
        return different;
    };

    auto onsetJaccard = [](const midiator::Phrase& a, const midiator::Phrase& b) {
        const int used = std::min(a.usedSteps(), b.usedSteps());
        int intersection = 0;
        int unionCount = 0;

        for (int i = 0; i < used; ++i) {
            const bool hitA = a.steps[i].noteCount > 0;
            const bool hitB = b.steps[i].noteCount > 0;
            if (hitA || hitB)
                ++unionCount;
            if (hitA && hitB)
                ++intersection;
        }

        if (unionCount == 0)
            return 1.0;
        return static_cast<double>(intersection) / static_cast<double>(unionCount);
    };

    midiator::Phrase candidate{};
    const bool havePrevious = previous.usedSteps() > 0;
    const auto acceptance =
        midiator::newRiffAcceptance(settings_.style, previous.usedSteps());
    const int requiredDifference =
        havePrevious ? acceptance.minStructuralDifference : 0;

    // NEW RIFF must sound like a genuinely new groove, not merely a pitch
    // variation. Keep the structurally valid candidate with the lowest onset
    // overlap and accept immediately once both distance requirements are met.
    midiator::Phrase bestCandidate{};
    double bestOnsetJaccard = 2.0;
    int bestStructuralDifference = -1;
    uint32_t bestSeed = seed_;

    for (int attempt = 0; attempt < 32; ++attempt) {
        seed_ = nextSeed(seed_);
        const auto effectiveSettings = sectionGeneratorSettings(settings_);
        candidate = midiator::RiffEngine::generate(effectiveSettings, seed_);
        midiator::applySectionPhraseShape(candidate, settings_.section, seed_);

        if (!havePrevious) {
            bestCandidate = candidate;
            bestStructuralDifference = candidate.usedSteps();
            bestOnsetJaccard = 0.0;
            bestSeed = seed_;
            break;
        }

        const int difference = structuralDifference(previous, candidate);
        const double jaccard = onsetJaccard(previous, candidate);

        if (jaccard < bestOnsetJaccard ||
            (std::abs(jaccard - bestOnsetJaccard) < 1e-9 &&
             difference > bestStructuralDifference)) {
            bestCandidate = candidate;
            bestOnsetJaccard = jaccard;
            bestStructuralDifference = difference;
            bestSeed = seed_;
        }

        if (difference >= requiredDifference &&
            jaccard <= acceptance.maxOnsetJaccard)
            break;
    }

    seed_ = bestSeed;
    phrase_ = bestCandidate;
    regenerateBass();
    phraseChangedNeedsFlush_ = true;

    // Replacing the musical content must never move the sequencer phase.
    // Keep the existing transport anchor so NEW RIFF, style changes and other
    // regenerations remain locked to the host's running 16th-note grid.
}
void MidiatorProcessor::generateVariation() {
    seed_ = nextSeed(seed_);
    const auto effectiveSettings = sectionGeneratorSettings(settings_);
    phrase_ = midiator::RiffEngine::vary(
        phrase_, effectiveSettings, variationAmount_, seed_);
    midiator::applySectionPhraseShape(phrase_, settings_.section, seed_);
    regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::regenerateBass() {
    bassSettings_.rootPitchClass = settings_.rootPitchClass;
    bassSettings_.scale = settings_.scale;
    bassSettings_.style = settings_.style;
    const auto effectiveBass =
        sectionBassSettings(bassSettings_, settings_.section);
    bassPhrase_ = midiator::BassBrain::generate(
        phrase_, effectiveBass, seed_ ^ 0xB4552026u);
    regenerateDrums();
}

void MidiatorProcessor::regenerateDrums() {
    drumSettings_.style = settings_.style;
    const auto effectiveDrums =
        sectionDrumSettings(drumSettings_, settings_.section);
    drumPhrase_ = midiator::DrumBrain::generate(
        phrase_, bassPhrase_, effectiveDrums, seed_ ^ 0xD12A2026u);
    regeneratePads();
}

void MidiatorProcessor::regeneratePads() {
    padSettings_.rootPitchClass = settings_.rootPitchClass;
    padSettings_.scale = settings_.scale;
    padSettings_.style = settings_.style;
    const auto effectivePads =
        sectionPadSettings(padSettings_, settings_.section);
    padPhrase_ = midiator::PadBrain::generate(
        phrase_, bassPhrase_, effectivePads, seed_ ^ 0x50414426u);
    regenerateSynth();
}

void MidiatorProcessor::regenerateSynth() {
    synthSettings_.rootPitchClass = settings_.rootPitchClass;
    synthSettings_.scale = settings_.scale;
    synthSettings_.style = settings_.style;
    const auto effectiveSynth =
        sectionSynthSettings(synthSettings_, settings_.section);
    synthPhrase_ = midiator::SynthBrain::generate(
        phrase_, bassPhrase_, padPhrase_, effectiveSynth, seed_ ^ 0x53594E26u);
}

void MidiatorProcessor::regenerateSectionFromCurrentSeed() {
    // A section change is an arrangement decision, not a new-composition
    // command. Reuse the current seed so Verse/Chorus/etc. remain recognizably
    // related and switching back to FREE restores the same core riff.
    const auto effectiveSettings = sectionGeneratorSettings(settings_);
    phrase_ = midiator::RiffEngine::generate(effectiveSettings, seed_);
    midiator::applySectionPhraseShape(phrase_, settings_.section, seed_);
    regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::resizePhraseBars(int newBars, bool regenerateCompanions) {
    if (newBars != 1 && newBars != 2 && newBars != 4 &&
        newBars != 8 && newBars != 16)
        return;

    const int oldBars = std::clamp(phrase_.bars, 1, midiator::kMaxBars);
    if (newBars == oldBars) {
        settings_.bars = newBars;
        return;
    }

    midiator::Phrase resized{};
    resized.bars = newBars;

    const int oldSteps = oldBars * midiator::kStepsPerBar;
    const int newSteps = newBars * midiator::kStepsPerBar;

    // Changing phrase length is not a composition command. Preserve the exact
    // existing riff and tile it when extending; truncate it when shortening.
    // NEW RIFF remains the only control that deliberately replaces the idea.
    for (int i = 0; i < newSteps; ++i)
        resized.steps[i] = phrase_.steps[i % oldSteps];

    phrase_ = resized;
    settings_.bars = newBars;
    if (regenerateCompanions)
        regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::applyPowerChordMode(bool enabled, bool regenerateCompanions) {
    settings_.powerChordsEnabled = enabled;

    if (!enabled) {
        for (int i = 0; i < phrase_.usedSteps(); ++i)
            if (phrase_.steps[i].noteCount > 1)
                phrase_.steps[i].noteCount = 1;
        if (regenerateCompanions)
            regenerateBass();
        phraseChangedNeedsFlush_ = true;
        return;
    }

    int eligibleHits = 0;
    int existingChords = 0;
    for (int i = 0; i < phrase_.usedSteps(); ++i) {
        const auto& step = phrase_.steps[i];
        if (step.noteCount <= 0 || step.notes[0].pitch > 120)
            continue;
        ++eligibleHits;
        if (step.noteCount > 1)
            ++existingChords;
    }

    float styleFactor = 1.0f;
    switch (settings_.style) {
        case midiator::StyleId::DarkRockGothic: styleFactor=1.10f; break;
        case midiator::StyleId::HeavyIndustrial: styleFactor=0.92f; break;
        case midiator::StyleId::ClassicHeavy: styleFactor=1.20f; break;
        case midiator::StyleId::Thrash: styleFactor=0.86f; break;
        case midiator::StyleId::Groove: styleFactor=1.05f; break;
        case midiator::StyleId::Death: styleFactor=0.78f; break;
        case midiator::StyleId::MelodicDeath: styleFactor=1.08f; break;
        case midiator::StyleId::Metalcore: styleFactor=1.12f; break;
        case midiator::StyleId::NuMetal: styleFactor=1.18f; break;
        case midiator::StyleId::Doom: styleFactor=1.28f; break;
        case midiator::StyleId::DjentProgressive: styleFactor=0.82f; break;
        default: break;
    }

    int targetChords = static_cast<int>(std::lround(
        static_cast<float>(eligibleHits) * settings_.powerChordChance * styleFactor));
    targetChords = std::clamp(targetChords, 0, eligibleHits);
    if (settings_.powerChordChance >= 0.10f && eligibleHits > 0)
        targetChords = std::max(1, targetChords);

    static constexpr int preferredPositions[] = {
        0, 8, 4, 12, 6, 14, 2, 10, 3, 11, 7, 15, 5, 13, 1, 9
    };

    for (int bar = 0; bar < phrase_.bars && existingChords < targetChords; ++bar) {
        for (int local : preferredPositions) {
            if (existingChords >= targetChords)
                break;
            auto& step = phrase_.steps[bar * midiator::kStepsPerBar + local];
            if (step.noteCount != 1 || step.notes[0].pitch > 120)
                continue;

            step.noteCount = 2;
            step.notes[1] = step.notes[0];
            step.notes[1].pitch = step.notes[0].pitch + 7;
            const int threshold =
                std::clamp(settings_.palmMuteVelocityThreshold, 3, 87);
            const int pmMin =
                threshold == 41 ? 30 : std::max(2, threshold - 8);
            step.notes[1].velocity = std::max(
                step.notes[0].velocity >= 88 ? 88 : pmMin,
                step.notes[0].velocity - 3);
            ++existingChords;
        }
    }

    if (regenerateCompanions)
        regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::applyPalmMuteVelocityThreshold(int threshold) {
    const int oldThreshold = std::clamp(settings_.palmMuteVelocityThreshold, 3, 87);
    const int newThreshold = std::clamp(threshold, 3, 87);
    if (oldThreshold == newThreshold)
        return;

    const int oldMax = std::max(2, oldThreshold - 1);
    const int newMax = std::max(2, newThreshold - 1);
    for (int i = 0; i < phrase_.usedSteps(); ++i) {
        auto& step = phrase_.steps[i];
        for (int n = 0; n < step.noteCount; ++n) {
            auto& note = step.notes[n];
            if (note.velocity >= 88)
                continue;
            const double ratio = static_cast<double>(note.velocity) /
                                 static_cast<double>(oldMax);
            note.velocity = std::clamp(
                static_cast<int>(std::lround(ratio * newMax)), 2, newMax);
        }
    }
    settings_.palmMuteVelocityThreshold = newThreshold;
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::transposePhraseToRoot(int newRootPitchClass, bool regenerateCompanions) {
    newRootPitchClass = std::clamp(newRootPitchClass, 0, 11);
    const int oldRoot = settings_.rootPitchClass;
    if (newRootPitchClass == oldRoot)
        return;

    int delta = newRootPitchClass - oldRoot;
    if (delta > 6)
        delta -= 12;
    else if (delta < -6)
        delta += 12;

    for (int stepIndex = 0; stepIndex < phrase_.usedSteps(); ++stepIndex) {
        auto& step = phrase_.steps[stepIndex];
        for (int noteIndex = 0; noteIndex < step.noteCount; ++noteIndex)
            step.notes[noteIndex].pitch = std::clamp(step.notes[noteIndex].pitch + delta, 0, 127);
    }

    settings_.rootPitchClass = newRootPitchClass;
    if (regenerateCompanions)
        regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::applyParameterChanges(ProcessData& data,
                                             bool guiNewRiff,
                                             bool guiVariation) {

    struct Pending {
        bool hasRoot = false;
        int root = 0;
        bool hasScale = false;
        midiator::ScaleId scale = midiator::ScaleId::Phrygian;
        bool hasBars = false;
        int bars = 2;
        bool hasSectionLength = false;
        int sectionBars = 2;
        bool hasSectionType = false;
        midiator::SectionType sectionType = midiator::SectionType::Free;
        bool hasDensity = false;
        float density = 0.0f;
        bool hasComplexity = false;
        float complexity = 0.0f;
        bool hasRepetition = false;
        float repetition = 0.0f;
        bool hasPowerChordAmount = false;
        float powerChordAmount = 0.0f;
        bool hasPowerChordsEnabled = false;
        bool powerChordsEnabled = true;
        bool hasPalmMute = false;
        float palmMute = 0.0f;
        bool hasPalmMuteVelocity = false;
        int palmMuteVelocity = 30;
        bool hasTriggerMode = false;
        bool midiNoteTrigger = false;
        bool hasVariationAmount = false;
        float variationAmount = 0.0f;
        bool hasLegacyStyle = false;
        midiator::StyleId legacyStyle = midiator::StyleId::NDHIndustrial;
        bool hasMetalStyle = false;
        midiator::StyleId metalStyle = midiator::StyleId::NDHIndustrial;
        bool hasRootSource = false;
        bool midiRootSource = true;
        bool hasDrumMap = false;
        midiator::DrumMapId drumMap = midiator::DrumMapId::GeneralMidi;
        bool hasBassFollow = false;
        float bassFollow = 0.0f;
        bool hasBassMovement = false;
        float bassMovement = 0.0f;
        bool hasDrumDensity = false;
        float drumDensity = 0.0f;
        bool hasDrumComplexity = false;
        float drumComplexity = 0.0f;
        bool hasFillIntensity = false;
        float fillIntensity = 0.0f;
        bool hasPadSpread = false;
        float padSpread = 0.0f;
        bool hasPadTension = false;
        float padTension = 0.0f;
        bool hasSynthActivity = false;
        float synthActivity = 0.0f;
        bool hasSynthMovement = false;
        float synthMovement = 0.0f;
        bool newRiff = false;
        bool variation = false;
    } pending;
    pending.newRiff = guiNewRiff;
    pending.variation = guiVariation;

    // Phase 1: collect only the final point from every queue. VST3 does not
    // guarantee a semantically meaningful queue order, so no musical state is
    // mutated while queues are being enumerated.
    if (data.inputParameterChanges) {
    for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
        auto* queue = data.inputParameterChanges->getParameterData(i);
        if (!queue || queue->getPointCount() <= 0)
            continue;

        const auto id = queue->getParameterId();
        if (id == kNewRiffId || id == kVariationId) {
            if (id == kNewRiffId)
                pending.newRiff = true;
            else
                pending.variation = true;
            continue;
        }

        int32 sampleOffset = 0;
        ParamValue v = 0.0;
        if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, v) != kResultOk)
            continue;
        v = std::clamp(v, 0.0, 1.0);

        switch (id) {
            case kRootId:
                pending.hasRoot = true;
                pending.root = normalizedIndex(v, 12);
                break;
            case kScaleId:
                pending.hasScale = true;
                pending.scale = static_cast<midiator::ScaleId>(
                    normalizedIndex(v, static_cast<int>(midiator::ScaleId::Count)));
                break;
            case kBarsId: {
                // Frozen legacy mapping. Do not extend: old automation at
                // 0, 1/3, 2/3, 1 must remain 1/2/4/8 bars.
                static constexpr int bars[] = {1, 2, 4, 8};
                pending.hasBars = true;
                pending.bars = bars[normalizedIndex(v, 4)];
                break;
            }
            case kSectionLengthId: {
                static constexpr int bars[] = {1, 2, 4, 8, 16};
                pending.hasSectionLength = true;
                pending.sectionBars = bars[normalizedIndex(v, 5)];
                break;
            }
            case kSectionTypeId:
                pending.hasSectionType = true;
                pending.sectionType = static_cast<midiator::SectionType>(
                    normalizedIndex(v, static_cast<int>(midiator::SectionType::Count)));
                break;
            case kDensityId:
                pending.hasDensity = true;
                pending.density = static_cast<float>(v);
                break;
            case kComplexityId:
                pending.hasComplexity = true;
                pending.complexity = static_cast<float>(v);
                break;
            case kRepetitionId:
                pending.hasRepetition = true;
                pending.repetition = static_cast<float>(v);
                break;
            case kPowerChordId:
                pending.hasPowerChordAmount = true;
                pending.powerChordAmount = static_cast<float>(v);
                break;
            case kPowerChordsEnabledId:
                pending.hasPowerChordsEnabled = true;
                pending.powerChordsEnabled = v > 0.5;
                break;
            case kPalmMuteId:
                pending.hasPalmMute = true;
                pending.palmMute = static_cast<float>(v);
                break;
            case kPalmMuteVelocityId:
                pending.hasPalmMuteVelocity = true;
                pending.palmMuteVelocity =
                    3 + normalizedIndex(v, 85);
                break;
            case kTriggerModeId:
                pending.hasTriggerMode = true;
                pending.midiNoteTrigger = v > 0.5;
                break;
            case kVariationAmountId:
                pending.hasVariationAmount = true;
                pending.variationAmount = static_cast<float>(v);
                break;
            case kStyleId:
                // Frozen legacy mapping: 0 / 0.5 / 1.0 -> old styles 0 / 1 / 2.
                pending.hasLegacyStyle = true;
                pending.legacyStyle = static_cast<midiator::StyleId>(
                    normalizedIndex(v, 3));
                break;
            case kMetalStyleId:
                pending.hasMetalStyle = true;
                pending.metalStyle = static_cast<midiator::StyleId>(
                    normalizedIndex(v, static_cast<int>(midiator::StyleId::Count)));
                break;
            case kRootSourceId:
                pending.hasRootSource = true;
                pending.midiRootSource = v > 0.5;
                break;
            case kDrumMapId: {
                pending.hasDrumMap = true;
                const int index = normalizedIndex(v, 3);
                pending.drumMap = index == 0
                    ? midiator::DrumMapId::GeneralMidi
                    : (index == 1 ? midiator::DrumMapId::EZdrummer3
                                  : midiator::DrumMapId::PerfectDrums);
                break;
            }
            case kBassFollowId:
                pending.hasBassFollow = true;
                pending.bassFollow = static_cast<float>(v);
                break;
            case kBassMovementId:
                pending.hasBassMovement = true;
                pending.bassMovement = static_cast<float>(v);
                break;
            case kDrumDensityId:
                pending.hasDrumDensity = true;
                pending.drumDensity = static_cast<float>(v);
                break;
            case kDrumComplexityId:
                pending.hasDrumComplexity = true;
                pending.drumComplexity = static_cast<float>(v);
                break;
            case kFillIntensityId:
                pending.hasFillIntensity = true;
                pending.fillIntensity = static_cast<float>(v);
                break;
            case kPadSpreadId:
                pending.hasPadSpread = true;
                pending.padSpread = static_cast<float>(v);
                break;
            case kPadTensionId:
                pending.hasPadTension = true;
                pending.padTension = static_cast<float>(v);
                break;
            case kSynthActivityId:
                pending.hasSynthActivity = true;
                pending.synthActivity = static_cast<float>(v);
                break;
            case kSynthMovementId:
                pending.hasSynthMovement = true;
                pending.synthMovement = static_cast<float>(v);
                break;
            default:
                break;
        }
    }

    }
 
    // Phase 2: establish the final block settings before any phrase operation.
    // This makes Style + Power-Chord Amount + Toggle deterministic regardless
    // of the host's queue ordering.
    if (pending.hasDensity)
        settings_.density = pending.density;
    if (pending.hasComplexity)
        settings_.complexity = pending.complexity;
    if (pending.hasRepetition)
        settings_.repetition = pending.repetition;
    if (pending.hasPowerChordAmount)
        settings_.powerChordChance = pending.powerChordAmount;
    if (pending.hasPalmMute)
        settings_.palmMuteChance = pending.palmMute;
    if (pending.hasPalmMuteVelocity)
        applyPalmMuteVelocityThreshold(pending.palmMuteVelocity);
    if (pending.hasTriggerMode &&
        pending.midiNoteTrigger != midiNoteTrigger_) {
        midiNoteTrigger_ = pending.midiNoteTrigger;
        phraseChangedNeedsFlush_ = true;
    }
    if (pending.hasVariationAmount)
        variationAmount_ = pending.variationAmount;
    if (pending.hasDrumMap && pending.drumMap != drumMapId_) {
        drumMapId_ = pending.drumMap;
        drumMap_ = midiator::DrumMidiMap::preset(drumMapId_);
        phraseChangedNeedsFlush_ = true;
    }

    bool bassRoleChanged = false;
    bool drumRoleChanged = false;
    bool padRoleChanged = false;
    bool synthRoleChanged = false;

    if (pending.hasBassFollow && pending.bassFollow != bassSettings_.follow) {
        bassSettings_.follow = pending.bassFollow;
        bassRoleChanged = true;
    }
    if (pending.hasBassMovement && pending.bassMovement != bassSettings_.movement) {
        bassSettings_.movement = pending.bassMovement;
        bassRoleChanged = true;
    }
    if (pending.hasDrumDensity && pending.drumDensity != drumSettings_.density) {
        drumSettings_.density = pending.drumDensity;
        drumRoleChanged = true;
    }
    if (pending.hasDrumComplexity && pending.drumComplexity != drumSettings_.complexity) {
        drumSettings_.complexity = pending.drumComplexity;
        drumRoleChanged = true;
    }
    if (pending.hasFillIntensity && pending.fillIntensity != drumSettings_.fillIntensity) {
        drumSettings_.fillIntensity = pending.fillIntensity;
        drumRoleChanged = true;
    }
    if (pending.hasPadSpread && pending.padSpread != padSettings_.spread) {
        padSettings_.spread = pending.padSpread;
        padRoleChanged = true;
    }
    if (pending.hasPadTension && pending.padTension != padSettings_.tension) {
        padSettings_.tension = pending.padTension;
        padRoleChanged = true;
    }
    if (pending.hasSynthActivity && pending.synthActivity != synthSettings_.activity) {
        synthSettings_.activity = pending.synthActivity;
        synthRoleChanged = true;
    }
    if (pending.hasSynthMovement && pending.synthMovement != synthSettings_.movement) {
        synthSettings_.movement = pending.synthMovement;
        synthRoleChanged = true;
    }

    const auto oldScale = settings_.scale;
    const auto oldStyle = settings_.style;
    const auto oldSection = settings_.section;
    if (pending.hasScale)
        settings_.scale = pending.scale;
    // Resolve old/new automation without allowing the newly-added default
    // value to overwrite a meaningful historical automation value (or vice
    // versa). A non-default new style wins over legacy default NDH; a
    // meaningful legacy style wins over a new default NDH.
    if (pending.hasMetalStyle &&
        (pending.metalStyle != midiator::StyleId::NDHIndustrial ||
         !pending.hasLegacyStyle ||
         pending.legacyStyle == midiator::StyleId::NDHIndustrial)) {
        settings_.style = pending.metalStyle;
    } else if (pending.hasLegacyStyle) {
        settings_.style = pending.legacyStyle;
    } else if (pending.hasMetalStyle) {
        settings_.style = pending.metalStyle;
    }
    if (pending.hasSectionType)
        settings_.section = pending.sectionType;

    if (pending.hasRoot)
        manualRootPitchClass_ = pending.root;

    const bool finalMidiRootSource =
        pending.hasRootSource ? pending.midiRootSource : midiRootSource_;

    // MIDI root is also a block-level command. Only the final NoteOn matters,
    // and whether it matters at all is decided from the final Root Source
    // parameter for this same block.
    int finalMidiPitchClass = -1;
    int32 finalMidiSampleOffset = -1;
    if (finalMidiRootSource && data.inputEvents) {
        for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
            Event event{};
            if (data.inputEvents->getEvent(i, event) != kResultOk)
                continue;
            if (event.type != Event::kNoteOnEvent || event.noteOn.velocity <= 0.0f)
                continue;

            int pitchClass = static_cast<int>(event.noteOn.pitch) % 12;
            if (pitchClass < 0)
                pitchClass += 12;
            if (event.sampleOffset >= finalMidiSampleOffset) {
                finalMidiSampleOffset = event.sampleOffset;
                finalMidiPitchClass = pitchClass;
            }
        }
    }

    const bool tonalFrameChanged =
        settings_.scale != oldScale || settings_.style != oldStyle;
    const bool sectionChanged = settings_.section != oldSection;
    const bool freshGeneration = tonalFrameChanged || pending.newRiff;

    bool guitarEdited = false;

    midiRootSource_ = finalMidiRootSource;
    int finalRootPitchClass = settings_.rootPitchClass;
    if (!midiRootSource_)
        finalRootPitchClass = manualRootPitchClass_;
    else if (finalMidiPitchClass >= 0)
        finalRootPitchClass = finalMidiPitchClass;

    if (finalRootPitchClass != settings_.rootPitchClass) {
        if (freshGeneration || sectionChanged) {
            settings_.rootPitchClass = finalRootPitchClass;
        } else {
            transposePhraseToRoot(finalRootPitchClass, false);
            guitarEdited = true;
        }
    }

    // New Section Length wins if both it and frozen legacy Bars arrive in one
    // host block. Legacy Bars remains fully functional for existing projects.
    const bool hasRequestedBars = pending.hasSectionLength || pending.hasBars;
    const int requestedBars =
        pending.hasSectionLength ? pending.sectionBars : pending.bars;

    // Phrase length is a structural edit only when no fresh composition is
    // already required. Otherwise the generator creates the requested length
    // directly, avoiding an intermediate companion-role cascade.
    if (hasRequestedBars && requestedBars != settings_.bars) {
        if (freshGeneration || sectionChanged) {
            settings_.bars = requestedBars;
        } else {
            resizePhraseBars(requestedBars, false);
            guitarEdited = true;
        }
    }

    // Apply the chord toggle after final Style/Amount/Bars/Root are known.
    if (pending.hasPowerChordsEnabled &&
        pending.powerChordsEnabled != settings_.powerChordsEnabled) {
        if (freshGeneration || sectionChanged) {
            settings_.powerChordsEnabled = pending.powerChordsEnabled;
        } else {
            applyPowerChordMode(pending.powerChordsEnabled, false);
            guitarEdited = true;
        }
    }

    if (freshGeneration) {
        generateNew();
    } else if (sectionChanged) {
        regenerateSectionFromCurrentSeed();
    } else if (guitarEdited && !pending.variation) {
        // Root/Bars/Power-Chord edits may all occur in one host block. Refresh
        // Bass -> Drums -> Pad -> Synth once from the final Guitar state.
        regenerateBass();
    }

    // Variation is always last so Style/Scale/New-Riff and structural edits
    // have deterministic semantics independent of VST3 queue ordering.
    if (pending.variation)
        generateVariation();

    const bool arrangementAlreadyRegenerated =
        freshGeneration || sectionChanged || guitarEdited || pending.variation;

    if (!arrangementAlreadyRegenerated) {
        if (bassRoleChanged)
            regenerateBass();
        else if (drumRoleChanged)
            regenerateDrums();
        else if (padRoleChanged)
            regeneratePads();
        else if (synthRoleChanged)
            regenerateSynth();

        if (bassRoleChanged || drumRoleChanged || padRoleChanged || synthRoleChanged)
            phraseChangedNeedsFlush_ = true;
    }
}

bool MidiatorProcessor::flushActiveNotes(
    IEventList* output, double ppqPosition, bool forceAllNotes,
    int32 sampleOffset) {
    if (!output)
        return false;

    bool allDelivered = true;
    for (int bus = 0; bus < kEventOutputBusCount; ++bus) {
        for (int pitch = 0; pitch < 128; ++pitch) {
            const bool trackedActive =
                activePitchesByBus_[static_cast<size_t>(bus)][pitch];
            if (!forceAllNotes && !trackedActive)
                continue;

            Event e{};
            e.busIndex = bus;
            e.sampleOffset = std::max<int32>(0, sampleOffset);
            e.ppqPosition = ppqPosition;
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = 0;
            e.noteOff.pitch = static_cast<int16>(pitch);
            e.noteOff.velocity = 0.0f;
            e.noteOff.noteId = -1;

            // The host owns the event list and may reject an event. Only clear
            // our active-note bookkeeping after the NoteOff was actually
            // accepted; otherwise retry the flush on the next process call.
            if (output->addEvent(e) == kResultOk) {
                activePitchesByBus_[static_cast<size_t>(bus)][pitch] = false;
            } else {
                allDelivered = false;
            }
        }
    }
    return allDelivered;
}

tresult PLUGIN_API MidiatorProcessor::notify(IMessage* message) {
    if (!message || !message->getMessageID())
        return kInvalidArgument;

    if (std::strcmp(message->getMessageID(), kMsgNewRiff) == 0) {
        pendingNewRiffCommands_.fetch_add(1, std::memory_order_release);
        return kResultOk;
    }
    if (std::strcmp(message->getMessageID(), kMsgVariation) == 0) {
        pendingVariationCommands_.fetch_add(1, std::memory_order_release);
        return kResultOk;
    }

    return AudioEffect::notify(message);
}

tresult PLUGIN_API MidiatorProcessor::process(ProcessData& data) {
    if (data.processContext && data.processContext->sampleRate > 0.0)
        sampleRate_ = data.processContext->sampleRate;

    // GUI IConnectionPoint messages, parameter queues and MIDI-root NoteOns all
    // describe the final musical state for this audio block. Consume the GUI
    // atomics first, then apply the complete block exactly once so mixed command
    // paths cannot trigger duplicate five-role compositions.
    const uint32_t newCount =
        pendingNewRiffCommands_.exchange(0, std::memory_order_acq_rel);
    const uint32_t variationCount =
        pendingVariationCommands_.exchange(0, std::memory_order_acq_rel);

    applyParameterChanges(data, newCount > 0, variationCount > 0);

    struct TriggerInputEvent {
        int32 sampleOffset = 0;
        int pitch = 0;
        bool noteOn = false;
    };
    struct TriggerTransition {
        int32 sampleOffset = 0;
        bool openAfter = false;
    };

    std::array<TriggerInputEvent, kMaxTriggerInputEvents> triggerInput{};
    std::array<TriggerTransition, kMaxTriggerInputEvents> triggerTransitions{};
    int triggerInputCount = 0;
    int triggerTransitionCount = 0;
    bool triggerInputOverflow = false;
    if (!midiNoteTrigger_) {
        // Root Source=MIDI may still receive notes in TRANSPORT mode, but those
        // notes must never become latent trigger state if the user later
        // switches to MIDI NOTE.
        triggerHeldPitches_.fill(false);
        triggerHeldCount_ = 0;
    }
    const bool triggerOpenAtBlockStart =
        midiNoteTrigger_ && triggerHeldCount_ > 0;

    if (midiNoteTrigger_ && data.inputEvents) {
        for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
            Event event{};
            if (data.inputEvents->getEvent(i, event) != kResultOk)
                continue;

            bool relevant = false;
            bool noteOn = false;
            int pitch = -1;
            if (event.type == Event::kNoteOnEvent) {
                pitch = static_cast<int>(event.noteOn.pitch);
                noteOn = event.noteOn.velocity > 0.0f;
                relevant = true;
            } else if (event.type == Event::kNoteOffEvent) {
                pitch = static_cast<int>(event.noteOff.pitch);
                noteOn = false;
                relevant = true;
            }
            if (!relevant || pitch < 0 || pitch > 127)
                continue;

            if (triggerInputCount >= kMaxTriggerInputEvents) {
                triggerInputOverflow = true;
                continue;
            }

            triggerInput[triggerInputCount++] = {
                std::clamp<int32>(
                    event.sampleOffset, 0,
                    std::max<int32>(0, data.numSamples - 1)),
                pitch,
                noteOn
            };
        }
    }

    std::sort(triggerInput.begin(), triggerInput.begin() + triggerInputCount,
              [](const TriggerInputEvent& a, const TriggerInputEvent& b) {
                  if (a.sampleOffset != b.sampleOffset)
                      return a.sampleOffset < b.sampleOffset;
                  if (a.noteOn != b.noteOn)
                      return !a.noteOn; // NoteOff before NoteOn at one sample.
                  return a.pitch < b.pitch;
              });

    for (int i = 0; i < triggerInputCount; ++i) {
        const auto& input = triggerInput[i];
        const bool wasOpen = triggerHeldCount_ > 0;

        if (input.noteOn) {
            if (!triggerHeldPitches_[static_cast<size_t>(input.pitch)]) {
                triggerHeldPitches_[static_cast<size_t>(input.pitch)] = true;
                ++triggerHeldCount_;
            }
        } else if (triggerHeldPitches_[static_cast<size_t>(input.pitch)]) {
            triggerHeldPitches_[static_cast<size_t>(input.pitch)] = false;
            triggerHeldCount_ = std::max(0, triggerHeldCount_ - 1);
        }

        const bool isOpen = triggerHeldCount_ > 0;
        if (wasOpen != isOpen &&
            triggerTransitionCount < kMaxTriggerInputEvents) {
            triggerTransitions[triggerTransitionCount++] = {
                input.sampleOffset, isOpen
            };
        }
    }

    if (triggerInputOverflow) {
        triggerHeldPitches_.fill(false);
        triggerHeldCount_ = 0;
        if (midiNoteTrigger_)
            return kResultFalse;
    }

    if (!data.outputEvents)
        return kResultOk;

    const auto* context = data.processContext;
    const bool hasTempo = context && (context->state & ProcessContext::kTempoValid) && context->tempo > 0.0;
    const bool hasProjectTime = context && (context->state & ProcessContext::kProjectTimeMusicValid);
    const bool hasTimeSignature =
        context && (context->state & ProcessContext::kTimeSigValid);
    const bool unsupportedTimeSignature =
        hasTimeSignature &&
        (context->timeSigNumerator != 4 || context->timeSigDenominator != 4);
    const bool playing = context && (context->state & ProcessContext::kPlaying);
    const double currentPpq = hasProjectTime ? context->projectTimeMusic : 0.0;

    if (lifecyclePanicPending_) {
        if (!flushActiveNotes(data.outputEvents, currentPpq, true))
            return kResultFalse;
        lifecyclePanicPending_ = false;
    }

    if (phraseChangedNeedsFlush_) {
        if (!flushActiveNotes(data.outputEvents, currentPpq))
            return kResultFalse;
        phraseChangedNeedsFlush_ = false;
    }

    // V1 is deliberately 4/4-only. If a host supplies a valid non-4/4
    // signature, do not silently run a four-quarter pattern against a
    // different bar grid. Flush held notes and stay silent until 4/4 returns.
    if (unsupportedTimeSignature) {
        bool flushed = true;
        if (wasPlaying_)
            flushed = flushActiveNotes(data.outputEvents, currentPpq, true);
        if (!flushed)
            phraseChangedNeedsFlush_ = true;

        wasPlaying_ = false;
        haveExpectedProjectTime_ = false;
        haveTransportAnchor_ = false;
        transportAnchorQn_ = 0.0;
        return flushed ? kResultOk : kResultFalse;
    }

    if (!playing || !hasTempo || !hasProjectTime || data.numSamples <= 0) {
        bool flushed = true;
        if (wasPlaying_)
            flushed = flushActiveNotes(data.outputEvents, currentPpq, true);
        if (!flushed)
            phraseChangedNeedsFlush_ = true;

        wasPlaying_ = playing;
        haveExpectedProjectTime_ = false;
        haveTransportAnchor_ = false;
        transportAnchorQn_ = 0.0;
        if (!playing) {
            triggerHeldPitches_.fill(false);
            triggerHeldCount_ = 0;
        }
        return flushed ? kResultOk : kResultFalse;
    }

    const double tempo = context->tempo;
    const double qnPerSample = tempo / (60.0 * sampleRate_);
    if (qnPerSample <= 0.0)
        return kResultOk;

    const double blockStartQn = context->projectTimeMusic;
    const double blockEndQn = blockStartQn + static_cast<double>(data.numSamples) * qnPerSample;

    bool timelineJump = false;
    if (wasPlaying_ && haveExpectedProjectTime_) {
        const double tolerance = std::max(1e-6, qnPerSample * 4.0);
        timelineJump = std::abs(blockStartQn - expectedProjectTimeQn_) > tolerance;
        if (timelineJump &&
            !flushActiveNotes(data.outputEvents, blockStartQn, true)) {
            phraseChangedNeedsFlush_ = true;
            return kResultFalse;
        }
    }

    // Lock phrase phase to the host's musical bar grid, not to whichever
    // audio buffer happens to be the first one we process. This preserves
    // sequencer sync when playback starts mid-bar and after timeline jumps.
    if (!wasPlaying_ || !haveTransportAnchor_ || timelineJump) {
        const bool hasBarPosition =
            context && (context->state & ProcessContext::kBarPositionValid);
        transportAnchorQn_ = hasBarPosition ? context->barPositionMusic : blockStartQn;
        haveTransportAnchor_ = true;
    }

    wasPlaying_ = true;
    haveExpectedProjectTime_ = true;
    expectedProjectTimeQn_ = blockEndQn;
    const double patternLengthQn = static_cast<double>(phrase_.bars) * 4.0;

    if (patternLengthQn <= 0.0)
        return kResultOk;

    std::array<ScheduledEvent, kMaxScheduledEvents> scheduled{};
    int scheduledCount = 0;
    bool schedulerOverflow = false;

    auto addScheduled = [&](double eventQn, bool noteOn, int pitch, int velocity,
                            int32 busIndex, double noteOffQn = -1.0) {
        constexpr double eps = 1e-9;
        if (eventQn + eps < blockStartQn || eventQn >= blockEndQn - eps)
            return;
        if (scheduledCount >= kMaxScheduledEvents) {
            schedulerOverflow = true;
            return;
        }

        const double relSamples = (eventQn - blockStartQn) / qnPerSample;
        ScheduledEvent e{};
        e.sampleOffset = std::clamp<int32>(
            static_cast<int32>(std::floor(relSamples + 1e-9)),
            0,
            std::max<int32>(0, data.numSamples - 1));
        e.ppqPosition = eventQn;
        e.noteOn = noteOn;
        e.pitch = std::clamp(pitch, 0, 127);
        e.velocity = std::clamp(velocity, 0, 126);
        e.busIndex = std::clamp<int32>(busIndex, 0, kEventOutputBusCount - 1);

        if (noteOn && noteOffQn > eventQn) {
            const double lengthSamples =
                std::max(1.0, std::round((noteOffQn - eventQn) / qnPerSample));
            e.noteLengthSamples = static_cast<int32>(std::min(
                lengthSamples,
                static_cast<double>(std::numeric_limits<int32>::max())));
        }

        scheduled[scheduledCount++] = e;
    };

    const double localBlockStartQn = blockStartQn - transportAnchorQn_;
    const double localBlockEndQn = blockEndQn - transportAnchorQn_;
    const long long firstCycle =
        static_cast<long long>(std::floor(localBlockStartQn / patternLengthQn)) - 1;
    const long long lastCycle =
        static_cast<long long>(std::floor(localBlockEndQn / patternLengthQn)) + 1;

    auto schedulePhraseCycle = [&](const midiator::Phrase& phrase,
                                   int32 busIndex,
                                   double cycleStartQn) {
        const int usedSteps = phrase.usedSteps();
        constexpr double kSustainGapQn = 1.0 / 16.0;

        for (int stepIndex = 0; stepIndex < usedSteps; ++stepIndex) {
            const auto& step = phrase.steps[stepIndex];
            if (step.noteCount <= 0)
                continue;

            int stepsToNextHit = usedSteps;
            for (int delta = 1; delta <= usedSteps; ++delta) {
                const int nextIndex = (stepIndex + delta) % usedSteps;
                if (phrase.steps[nextIndex].noteCount > 0) {
                    stepsToNextHit = delta;
                    break;
                }
            }

            const double onQn =
                cycleStartQn + static_cast<double>(stepIndex) * kStepQuarterNotes;
            const double nextOnQn =
                onQn + static_cast<double>(stepsToNextHit) * kStepQuarterNotes;
            const double latestOffQn =
                std::max(onQn, nextOnQn - kSustainGapQn);

            for (int n = 0; n < step.noteCount; ++n) {
                const auto& note = step.notes[n];
                // Guitar and Bass are sustained performance lanes. Keep each
                // note alive until immediately before the next onset; the
                // deliberate 1/64-note gap prevents overlap while avoiding
                // audible holes. Articulation is encoded by the generated
                // velocity/chord content, not by prematurely shortening gates.
                const double offQn = latestOffQn;
                addScheduled(onQn, true, note.pitch, note.velocity, busIndex, offQn);
                addScheduled(offQn, false, note.pitch, 0, busIndex);
            }
        }
    };

    auto scheduleDrumCycle = [&](double cycleStartQn) {
        constexpr double kDrumGateQn = kStepQuarterNotes; // exact 1/16-note gate
        for (int stepIndex = 0; stepIndex < drumPhrase_.usedSteps(); ++stepIndex) {
            const auto& step = drumPhrase_.steps[stepIndex];
            if (step.hitCount <= 0)
                continue;
            const double onQn =
                cycleStartQn + static_cast<double>(stepIndex) * kStepQuarterNotes;
            const double offQn = onQn + kDrumGateQn;
            for (int n = 0; n < step.hitCount; ++n) {
                const auto& hit = step.hits[n];
                const int pitch = drumMap_.midiNote(hit.voice);
                addScheduled(onQn, true, pitch, hit.velocity, kDrumsOutBus, offQn);
                addScheduled(offQn, false, pitch, 0, kDrumsOutBus);
            }
        }
    };

    auto schedulePadCycle = [&](double cycleStartQn) {
        constexpr double kPadReleaseGapQn = 1.0 / 16.0; // 1/64-note gap
        const int usedSteps = padPhrase_.usedSteps();

        for (int stepIndex = 0; stepIndex < usedSteps; ++stepIndex) {
            const auto& step = padPhrase_.steps[stepIndex];
            if (step.noteCount <= 0)
                continue;

            int stepsToNextChord = usedSteps;
            for (int delta = 1; delta <= usedSteps; ++delta) {
                const int nextIndex = (stepIndex + delta) % usedSteps;
                if (padPhrase_.steps[nextIndex].noteCount > 0) {
                    stepsToNextChord = delta;
                    break;
                }
            }

            const double onQn =
                cycleStartQn + static_cast<double>(stepIndex) * kStepQuarterNotes;
            const double nextChordQn =
                onQn + static_cast<double>(stepsToNextChord) * kStepQuarterNotes;
            const double latestOffQn =
                std::max(onQn, nextChordQn - kPadReleaseGapQn);

            for (int n = 0; n < step.noteCount; ++n) {
                const auto& note = step.notes[n];
                const double requestedOffQn =
                    onQn + static_cast<double>(std::max(1, note.lengthSteps)) *
                               kStepQuarterNotes - kPadReleaseGapQn;
                const double offQn =
                    std::max(onQn, std::min(latestOffQn, requestedOffQn));
                addScheduled(onQn, true, note.pitch, note.velocity, kPadOutBus, offQn);
                addScheduled(offQn, false, note.pitch, 0, kPadOutBus);
            }
        }
    };

    auto scheduleSynthCycle = [&](double cycleStartQn) {
        const int usedSteps = synthPhrase_.usedSteps();

        for (int stepIndex = 0; stepIndex < usedSteps; ++stepIndex) {
            const auto& step = synthPhrase_.steps[stepIndex];
            if (step.noteCount <= 0)
                continue;

            int stepsToNextHit = usedSteps;
            for (int delta = 1; delta <= usedSteps; ++delta) {
                const int nextIndex = (stepIndex + delta) % usedSteps;
                if (synthPhrase_.steps[nextIndex].noteCount > 0) {
                    stepsToNextHit = delta;
                    break;
                }
            }

            const double onQn =
                cycleStartQn + static_cast<double>(stepIndex) * kStepQuarterNotes;
            const double nextOnQn =
                onQn + static_cast<double>(stepsToNextHit) * kStepQuarterNotes;

            // Synth articulation is independent of Guitar/Bass/Pad gating.
            // Honor the generated note length and allow a legato boundary at
            // the next monophonic onset. Same-sample NoteOff is sorted first.
            for (int n = 0; n < step.noteCount; ++n) {
                const auto& note = step.notes[n];
                const double requestedOffQn =
                    onQn + static_cast<double>(std::max(1, note.lengthSteps)) *
                               kStepQuarterNotes;
                const double offQn = std::max(onQn, std::min(nextOnQn, requestedOffQn));
                addScheduled(onQn, true, note.pitch, note.velocity, kSynthOutBus, offQn);
                addScheduled(offQn, false, note.pitch, 0, kSynthOutBus);
            }
        }
    };

    int triggerTransitionIndex = 0;
    bool triggerGateOpen =
        !midiNoteTrigger_ || triggerOpenAtBlockStart;

    auto applyTriggerTransitionsThrough = [&](int32 sampleOffset) {
        if (!midiNoteTrigger_)
            return true;

        while (triggerTransitionIndex < triggerTransitionCount &&
               triggerTransitions[triggerTransitionIndex].sampleOffset <= sampleOffset) {
            const auto transition =
                triggerTransitions[triggerTransitionIndex++];
            if (!transition.openAfter) {
                const double transitionQn =
                    blockStartQn +
                    static_cast<double>(transition.sampleOffset) * qnPerSample;
                if (!flushActiveNotes(
                        data.outputEvents, transitionQn, false,
                        transition.sampleOffset))
                    return false;
            }
            triggerGateOpen = transition.openAfter;
        }
        return true;
    };

    auto emitScheduled = [&]() {
        std::sort(scheduled.begin(), scheduled.begin() + scheduledCount,
                  [](const ScheduledEvent& a, const ScheduledEvent& b) {
                      if (a.sampleOffset != b.sampleOffset)
                          return a.sampleOffset < b.sampleOffset;
                      if (a.noteOn != b.noteOn)
                          return !a.noteOn;
                      if (a.busIndex != b.busIndex)
                          return a.busIndex < b.busIndex;
                      return a.pitch < b.pitch;
                  });

        bool allDelivered = true;
        for (int i = 0; i < scheduledCount; ++i) {
            const auto& s = scheduled[i];

            if (!applyTriggerTransitionsThrough(s.sampleOffset))
                return false;

            if (midiNoteTrigger_) {
                const bool active =
                    activePitchesByBus_[static_cast<size_t>(s.busIndex)]
                                       [static_cast<size_t>(s.pitch)];
                if (s.noteOn && !triggerGateOpen)
                    continue;
                if (!s.noteOn && !triggerGateOpen && !active)
                    continue;
            }

            Event e{};
            e.busIndex = s.busIndex;
            e.sampleOffset = s.sampleOffset;
            e.ppqPosition = s.ppqPosition;

            if (s.noteOn) {
                e.type = Event::kNoteOnEvent;
                e.noteOn.channel = 0;
                e.noteOn.pitch = static_cast<int16>(s.pitch);
                e.noteOn.velocity = static_cast<float>(s.velocity) / 127.0f;
                e.noteOn.length = s.noteLengthSamples;
                e.noteOn.tuning = 0.0f;
                e.noteOn.noteId = -1;

                if (data.outputEvents->addEvent(e) == kResultOk) {
                    activePitchesByBus_[static_cast<size_t>(s.busIndex)][s.pitch] = true;
                } else {
                    allDelivered = false;
                }
            } else {
                e.type = Event::kNoteOffEvent;
                e.noteOff.channel = 0;
                e.noteOff.pitch = static_cast<int16>(s.pitch);
                e.noteOff.velocity = 0.0f;
                e.noteOff.noteId = -1;

                if (data.outputEvents->addEvent(e) == kResultOk) {
                    activePitchesByBus_[static_cast<size_t>(s.busIndex)][s.pitch] = false;
                } else {
                    allDelivered = false;
                }
            }
        }
        return allDelivered;
    };

    // Stage and emit one musical cycle at a time. The fixed array therefore
    // bounds realtime memory by per-cycle musical complexity rather than by
    // the host's offline block length. Extremely large offline blocks can span
    // arbitrarily many cycles without accumulating all events in one buffer.
    for (long long cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        scheduledCount = 0;
        schedulerOverflow = false;

        const double cycleStartQn =
            transportAnchorQn_ + static_cast<double>(cycle) * patternLengthQn;

        schedulePhraseCycle(phrase_, kGuitarOutBus, cycleStartQn);
        schedulePhraseCycle(bassPhrase_, kBassOutBus, cycleStartQn);
        scheduleDrumCycle(cycleStartQn);
        schedulePadCycle(cycleStartQn);
        scheduleSynthCycle(cycleStartQn);

        if (schedulerOverflow) {
            // This now represents excessive complexity inside one single
            // musical cycle, not merely a large host block.
            phraseChangedNeedsFlush_ =
                !flushActiveNotes(data.outputEvents, blockStartQn);
            return kResultFalse;
        }

        if (!emitScheduled()) {
            // Keep only successfully delivered NoteOns in the active set, and
            // force a cleanup pass before any further musical events.
            phraseChangedNeedsFlush_ = true;
            return kResultFalse;
        }
    }

    if (!applyTriggerTransitionsThrough(std::max<int32>(0, data.numSamples - 1))) {
        phraseChangedNeedsFlush_ = true;
        return kResultFalse;
    }

    return kResultOk;
}

tresult PLUGIN_API MidiatorController::initialize(FUnknown* context) {
    auto r = EditControllerEx1::initialize(context);
    if (r != kResultOk)
        return r;

    auto* root = new StringListParameter(STR16("Root"), kRootId);
    const char16_t* roots[] = {
        STR16("C"), STR16("C#"), STR16("D"), STR16("D#"), STR16("E"), STR16("F"),
        STR16("F#"), STR16("G"), STR16("G#"), STR16("A"), STR16("A#"), STR16("B")
    };
    for (auto* s : roots)
        root->appendString(s);
    root->getInfo().defaultNormalizedValue = 9.0 / 11.0;
    root->setNormalized(root->getInfo().defaultNormalizedValue);
    parameters.addParameter(root);

    auto* rootSource = new StringListParameter(STR16("Root Source"), kRootSourceId);
    rootSource->appendString(STR16("Manual"));
    rootSource->appendString(STR16("MIDI"));
    rootSource->getInfo().defaultNormalizedValue = 1.0;
    rootSource->setNormalized(1.0);
    parameters.addParameter(rootSource);

    auto* legacyStyle = new StringListParameter(STR16("Riff Style (Legacy)"), kStyleId);
    legacyStyle->appendString(STR16("NDH / Industrial"));
    legacyStyle->appendString(STR16("Dark Rock / Gothic"));
    legacyStyle->appendString(STR16("Heavy Industrial"));
    legacyStyle->getInfo().defaultNormalizedValue = 0.0;
    legacyStyle->setNormalized(0.0);
    legacyStyle->getInfo().flags |= ParameterInfo::kIsHidden;
    parameters.addParameter(legacyStyle);

    auto* style = new StringListParameter(STR16("Metal Style"), kMetalStyleId);
    style->appendString(STR16("NDH / Industrial"));
    style->appendString(STR16("Dark Rock / Gothic"));
    style->appendString(STR16("Heavy Industrial"));
    style->appendString(STR16("Classic Heavy Metal"));
    style->appendString(STR16("Thrash Metal"));
    style->appendString(STR16("Groove Metal"));
    style->appendString(STR16("Death Metal"));
    style->appendString(STR16("Melodic Death Metal"));
    style->appendString(STR16("Metalcore"));
    style->appendString(STR16("Nu Metal"));
    style->appendString(STR16("Doom Metal"));
    style->appendString(STR16("Djent / Progressive"));
    style->getInfo().defaultNormalizedValue = 0.0;
    style->setNormalized(0.0);
    parameters.addParameter(style);

    auto* scale = new StringListParameter(STR16("Scale / Mode"), kScaleId);
    scale->appendString(STR16("Natural Minor"));
    scale->appendString(STR16("Phrygian"));
    scale->appendString(STR16("Dorian"));
    scale->appendString(STR16("Harmonic Minor"));
    scale->appendString(STR16("Phrygian Dominant"));
    scale->appendString(STR16("Minor Pentatonic"));
    scale->appendString(STR16("Blues"));
    scale->getInfo().defaultNormalizedValue = 1.0 / 6.0;
    scale->setNormalized(scale->getInfo().defaultNormalizedValue);
    parameters.addParameter(scale);

    auto* bars = new StringListParameter(STR16("Bars (Legacy)"), kBarsId);
    bars->appendString(STR16("1"));
    bars->appendString(STR16("2"));
    bars->appendString(STR16("4"));
    bars->appendString(STR16("8"));
    bars->getInfo().defaultNormalizedValue = 1.0 / 3.0;
    bars->setNormalized(bars->getInfo().defaultNormalizedValue);
    parameters.addParameter(bars);

    auto* sectionLength =
        new StringListParameter(STR16("Section Length"), kSectionLengthId);
    sectionLength->appendString(STR16("1"));
    sectionLength->appendString(STR16("2"));
    sectionLength->appendString(STR16("4"));
    sectionLength->appendString(STR16("8"));
    sectionLength->appendString(STR16("16"));
    sectionLength->getInfo().defaultNormalizedValue = 0.25;
    sectionLength->setNormalized(0.25);
    parameters.addParameter(sectionLength);

    auto* sectionType =
        new StringListParameter(STR16("Section"), kSectionTypeId);
    sectionType->appendString(STR16("FREE"));
    sectionType->appendString(STR16("INTRO"));
    sectionType->appendString(STR16("VERSE"));
    sectionType->appendString(STR16("PRE"));
    sectionType->appendString(STR16("CHORUS"));
    sectionType->appendString(STR16("BREAKDOWN"));
    sectionType->appendString(STR16("OUTRO"));
    sectionType->getInfo().defaultNormalizedValue = 0.0;
    sectionType->setNormalized(0.0);
    parameters.addParameter(sectionType);

    auto addPercent = [&](const char16_t* name, ParamID id, double defaultValue) {
        auto* p = new RangeParameter(name, id, STR16("%"), 0.0, 100.0, defaultValue, 0,
                                     ParameterInfo::kCanAutomate);
        p->setPrecision(0);
        parameters.addParameter(p);
    };

    addPercent(STR16("Density"), kDensityId, 56.0);
    addPercent(STR16("Complexity"), kComplexityId, 42.0);
    addPercent(STR16("Repetition"), kRepetitionId, 72.0);
    auto* drumMap = new StringListParameter(STR16("Drum Map"), kDrumMapId);
    drumMap->appendString(STR16("General MIDI"));
    drumMap->appendString(STR16("EZdrummer 3"));
    drumMap->appendString(STR16("Perfect Drums"));
    drumMap->getInfo().defaultNormalizedValue = 0.0;
    drumMap->setNormalized(0.0);
    parameters.addParameter(drumMap);

    auto* powerChordsEnabled = new StringListParameter(STR16("Power Chords"), kPowerChordsEnabledId);
    powerChordsEnabled->appendString(STR16("OFF"));
    powerChordsEnabled->appendString(STR16("ON"));
    powerChordsEnabled->getInfo().defaultNormalizedValue = 1.0;
    powerChordsEnabled->setNormalized(1.0);
    parameters.addParameter(powerChordsEnabled);

    addPercent(STR16("Power Chords Amount"), kPowerChordId, 25.0);
    addPercent(STR16("Palm Mute"), kPalmMuteId, 70.0);
    auto* pmVelocity = new RangeParameter(
        STR16("PM < Velocity"), kPalmMuteVelocityId, STR16(""),
        3.0, 87.0, 30.0, 84, ParameterInfo::kCanAutomate);
    pmVelocity->setPrecision(0);
    parameters.addParameter(pmVelocity);
    addPercent(STR16("Variation Amount"), kVariationAmountId, 35.0);
    addPercent(STR16("Bass Follow"), kBassFollowId, 72.0);
    addPercent(STR16("Bass Movement"), kBassMovementId, 34.0);
    addPercent(STR16("Drum Density"), kDrumDensityId, 48.0);
    addPercent(STR16("Drum Complexity"), kDrumComplexityId, 30.0);
    addPercent(STR16("Fill Intensity"), kFillIntensityId, 50.0);
    addPercent(STR16("Pad Spread"), kPadSpreadId, 42.0);
    addPercent(STR16("Pad Tension"), kPadTensionId, 18.0);
    addPercent(STR16("Synth Activity"), kSynthActivityId, 46.0);
    addPercent(STR16("Synth Movement"), kSynthMovementId, 42.0);

    auto* triggerMode = new StringListParameter(STR16("Trigger"), kTriggerModeId);
    triggerMode->appendString(STR16("TRANSPORT"));
    triggerMode->appendString(STR16("MIDI NOTE"));
    triggerMode->getInfo().defaultNormalizedValue = 0.0;
    triggerMode->setNormalized(0.0);
    parameters.addParameter(triggerMode);

    auto* newRiff = new RangeParameter(STR16("NEW Riff"), kNewRiffId, STR16(""),
                                       0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate);
    parameters.addParameter(newRiff);

    auto* variation = new RangeParameter(STR16("VARIATION"), kVariationId, STR16(""),
                                         0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate);
    parameters.addParameter(variation);

    return kResultOk;
}

void MidiatorController::valueChanged(VSTGUI::CControl* control) {
    if (!control || control->getValueNormalized() <= 0.5f)
        return;

    const auto tag = static_cast<ParamID>(control->getTag());
    const char* messageId = nullptr;
    double* fallbackState = nullptr;

    if (tag == kNewRiffId) {
        messageId = kMsgNewRiff;
        fallbackState = &fallbackNewRiffState_;
    } else if (tag == kVariationId) {
        messageId = kMsgVariation;
        fallbackState = &fallbackVariationState_;
    } else {
        return;
    }

    // Primary path: explicit controller -> processor command message.
    if (sendMessageID(messageId) == kResultOk)
        return;

    // Fallback for a host that does not connect the two VST3 connection
    // points: alternate the action parameter so the processor still receives
    // a real changed final value.
    *fallbackState = (*fallbackState > 0.5) ? 0.0 : 1.0;
    beginEdit(tag);
    performEdit(tag, *fallbackState);
    endEdit(tag);
}

tresult PLUGIN_API MidiatorController::setComponentState(IBStream* state) {
    if (!state)
        return kInvalidArgument;

    midiator::GeneratorSettings restored{};
    float variationAmount = 0.35f;
    uint32_t seed = 0;
    int manualRoot = restored.rootPitchClass;
    bool midiRootSource = true;
    bool powerChordsEnabled = true;
    bool midiNoteTrigger = false;
    midiator::DrumMapId drumMapId = midiator::DrumMapId::GeneralMidi;
    midiator::BassSettings bassSettings{};
    midiator::DrumSettings drumSettings{};
    midiator::PadSettings padSettings{};
    midiator::SynthSettings synthSettings{};
    uint32_t restoredStateVersion = 0;
    if (!readStateHeader(state, restoredStateVersion, restored, variationAmount, seed,
                         manualRoot, midiRootSource, powerChordsEnabled, drumMapId,
                         bassSettings, drumSettings, padSettings, synthSettings))
        return kResultFalse;

    if (restoredStateVersion >= 13u) {
        midiator::Phrase tempGuitar{}, tempBass{}, tempSynth{};
        midiator::DrumPhrase tempDrums{};
        midiator::PadPhrase tempPads{};
        const int serializedSteps =
            restoredStateVersion >= 8u ? midiator::kMaxSteps : kLegacyStateSteps;
        const bool allow16Bars = restoredStateVersion >= 8u;
        if (!readPhraseState(state, tempGuitar, restored.bars,
                             serializedSteps, allow16Bars, true) ||
            !readPhraseState(state, tempBass, restored.bars,
                             serializedSteps, allow16Bars, true) ||
            !readDrumPhraseState(state, tempDrums, restored.bars,
                                 serializedSteps, allow16Bars, true) ||
            !readPadPhraseState(state, tempPads, restored.bars,
                                serializedSteps, allow16Bars, true) ||
            !readPhraseState(state, tempSynth, restored.bars,
                             serializedSteps, allow16Bars, true) ||
            !readExtendedStateTail(
                state, restoredStateVersion, restored.palmMuteVelocityThreshold,
                midiNoteTrigger))
            return kResultFalse;
    } else {
        restored.palmMuteVelocityThreshold = 41;
        midiNoteTrigger = false;
    }

    auto legacyBarsIndex = [](int bars) -> double {
        switch (bars) {
            case 1: return 0.0;
            case 2: return 1.0 / 3.0;
            case 4: return 2.0 / 3.0;
            case 8:
            case 16: return 1.0;
            default: return 1.0 / 3.0;
        }
    };
    auto sectionBarsIndex = [](int bars) -> double {
        switch (bars) {
            case 1: return 0.0;
            case 2: return 0.25;
            case 4: return 0.50;
            case 8: return 0.75;
            case 16: return 1.0;
            default: return 0.25;
        }
    };

    setParamNormalized(kRootId, static_cast<double>(manualRoot) / 11.0);
    setParamNormalized(kRootSourceId, midiRootSource ? 1.0 : 0.0);
    setParamNormalized(kPowerChordsEnabledId, powerChordsEnabled ? 1.0 : 0.0);
    const double drumMapNormalized =
        drumMapId == midiator::DrumMapId::GeneralMidi ? 0.0 :
        (drumMapId == midiator::DrumMapId::EZdrummer3 ? 0.5 : 1.0);
    setParamNormalized(kDrumMapId, drumMapNormalized);
    const int restoredStyleIndex = static_cast<int>(restored.style);
    const int legacyStyleIndex =
        restoredStyleIndex >= 0 && restoredStyleIndex < 3 ? restoredStyleIndex : 0;
    setParamNormalized(kStyleId, static_cast<double>(legacyStyleIndex) / 2.0);
    setParamNormalized(
        kMetalStyleId,
        static_cast<double>(restoredStyleIndex) /
        static_cast<double>(static_cast<int>(midiator::StyleId::Count) - 1));
    setParamNormalized(kScaleId, static_cast<double>(static_cast<int>(restored.scale)) /
                                  static_cast<double>(static_cast<int>(midiator::ScaleId::Count) - 1));
    setParamNormalized(kBarsId, legacyBarsIndex(restored.bars));
    setParamNormalized(kSectionLengthId, sectionBarsIndex(restored.bars));
    setParamNormalized(
        kSectionTypeId,
        static_cast<double>(static_cast<int>(restored.section)) /
        static_cast<double>(static_cast<int>(midiator::SectionType::Count) - 1));
    setParamNormalized(kDensityId, restored.density);
    setParamNormalized(kComplexityId, restored.complexity);
    setParamNormalized(kRepetitionId, restored.repetition);
    setParamNormalized(kPowerChordId, restored.powerChordChance);
    setParamNormalized(kPalmMuteId, restored.palmMuteChance);
    setParamNormalized(kPalmMuteVelocityId,
                       static_cast<double>(restored.palmMuteVelocityThreshold - 3) / 84.0);
    setParamNormalized(kTriggerModeId, midiNoteTrigger ? 1.0 : 0.0);
    setParamNormalized(kVariationAmountId, variationAmount);
    setParamNormalized(kBassFollowId, bassSettings.follow);
    setParamNormalized(kBassMovementId, bassSettings.movement);
    setParamNormalized(kDrumDensityId, drumSettings.density);
    setParamNormalized(kDrumComplexityId, drumSettings.complexity);
    setParamNormalized(kFillIntensityId, drumSettings.fillIntensity);
    setParamNormalized(kPadSpreadId, padSettings.spread);
    setParamNormalized(kPadTensionId, padSettings.tension);
    setParamNormalized(kSynthActivityId, synthSettings.activity);
    setParamNormalized(kSynthMovementId, synthSettings.movement);

    return kResultOk;
}


tresult PLUGIN_API MidiatorController::setParamNormalized(ParamID tag, ParamValue value) {
    const auto r = EditControllerEx1::setParamNormalized(tag, value);
    if (tag == kRootId || tag == kScaleId)
        refreshTheory();
    if (tag == kStyleId || tag == kMetalStyleId)
        refreshStyleHint();
    return r;
}

IPlugView* PLUGIN_API MidiatorController::createView(FIDString name) {
    if (name && std::strcmp(name, ViewType::kEditor) == 0) {
        auto* editor = new VSTGUI::AspectRatioVST3Editor(
            this, "MidiatorView", "midiator.uidesc");
        editor->setDelegate(this);
        editor->setMinZoomFactor(0.75);
        editor->setAllowedZoomFactors({0.75, 1.0, 1.25, 1.5, 2.0});
        return editor;
    }
    return nullptr;
}

VSTGUI::CView* MidiatorController::verifyView(VSTGUI::CView* view,
                                              const VSTGUI::UIAttributes& attributes,
                                              const VSTGUI::IUIDescription*,
                                              VSTGUI::VST3Editor*) {
    if (!view)
        return nullptr;

    if (auto* control = dynamic_cast<VSTGUI::CControl*>(view)) {
        if (control->getTag() == static_cast<int32_t>(kNewRiffId)) {
            control->setListener(this);
            newRiffButton_ = control;
        } else if (control->getTag() == static_cast<int32_t>(kVariationId)) {
            control->setListener(this);
            variationButton_ = control;
        }
    }

    const auto* id = attributes.getAttributeValue("midiator-id");
    if (id) {
        if (auto* label = dynamic_cast<VSTGUI::CTextLabel*>(view)) {
            if (*id == "theoryKey") theoryKey_ = label;
            else if (*id == "theoryNotes") theoryNotes_ = label;
            else if (*id == "theoryCharacter") theoryCharacter_ = label;
            else if (*id == "theoryInterval") theoryInterval_ = label;
            else if (*id == "styleBpm") styleBpm_ = label;
        }
    }

    refreshTheory();
    refreshStyleHint();
    return view;
}

void MidiatorController::willClose(VSTGUI::VST3Editor*) {
    // verifyView stores raw pointers into the editor's view hierarchy.
    // They become invalid as soon as the editor closes. Clearing them here
    // prevents a second editor instance from touching freed views.
    theoryKey_ = nullptr;
    theoryNotes_ = nullptr;
    theoryCharacter_ = nullptr;
    theoryInterval_ = nullptr;
    styleBpm_ = nullptr;
    newRiffButton_ = nullptr;
    variationButton_ = nullptr;
}

void MidiatorController::refreshTheory() noexcept {
    static constexpr const char* kNoteNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    const int root = normalizedIndex(getParamNormalized(kRootId), 12);
    const auto scaleId = static_cast<midiator::ScaleId>(
        normalizedIndex(getParamNormalized(kScaleId), static_cast<int>(midiator::ScaleId::Count)));
    const auto& def = midiator::RiffEngine::scaleDefinition(scaleId);

    auto set = [](VSTGUI::CTextLabel* label, const std::string& value) {
        if (!label) return;
        label->setText(value.c_str());
        label->invalid();
    };

    set(theoryKey_, std::string(kNoteNames[root]) + "  " + def.name);

    std::string notes = "Notes: ";
    for (int i = 0; i < def.count; ++i) {
        if (i) notes += "  ";
        notes += kNoteNames[(root + def.intervals[i]) % 12];
    }
    set(theoryNotes_, notes);
    set(theoryCharacter_, std::string("Character: ") + def.character);
    set(theoryInterval_, std::string("Signature: ") + def.characteristicInterval);
}

void MidiatorController::refreshStyleHint() noexcept {
    if (!styleBpm_)
        return;

    int style = normalizedIndex(
        getParamNormalized(kMetalStyleId),
        static_cast<int>(midiator::StyleId::Count));
    const int legacy = normalizedIndex(getParamNormalized(kStyleId), 3);
    if (style == 0 && legacy != 0)
        style = legacy;

    static constexpr const char* kBpmHints[] = {
        "REC. BPM 100-135",
        "REC. BPM 80-120",
        "REC. BPM 105-145",
        "REC. BPM 110-160",
        "REC. BPM 160-220",
        "REC. BPM 85-130",
        "REC. BPM 160-240",
        "REC. BPM 140-200",
        "REC. BPM 120-180",
        "REC. BPM 80-120",
        "REC. BPM 50-90",
        "REC. BPM 90-160"
    };

    style = std::clamp(style, 0,
        static_cast<int>(std::size(kBpmHints)) - 1);
    styleBpm_->setText(kBpmHints[style]);
    styleBpm_->invalid();
}

} // namespace Steinberg::Vst

using namespace Steinberg;
using namespace Steinberg::Vst;

#ifndef MIDIATOR_NO_FACTORY
BEGIN_FACTORY_DEF("125A", "https://github.com/challanger2000/125A-Midiator", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(ProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "125A Midiator", Vst::kDistributable, Vst::PlugType::kInstrumentSynth,
           "0.1.0", kVstVersionString, MidiatorProcessor::createInstance)
DEF_CLASS2(INLINE_UID_FROM_FUID(ControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "125A Midiator Controller", 0, "", "0.1.0", kVstVersionString,
           MidiatorController::createInstance)
END_FACTORY
#endif
