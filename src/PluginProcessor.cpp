#include "PluginProcessor.h"

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

namespace Steinberg::Vst {
namespace {

constexpr double kStepQuarterNotes = 0.25;
constexpr int kMaxScheduledEvents = 8192;
constexpr uint32_t kStateMagic = 0x4D445231u; // "MDR1"
constexpr uint32_t kStateVersion = 4u;
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

bool readStateHeader(IBStream* state,
                     midiator::GeneratorSettings& settings,
                     float& variationAmount,
                     uint32_t& seed,
                     int& manualRootPitchClass,
                     bool& midiRootSource,
                     bool& powerChordsEnabled) {
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

    settings.rootPitchClass = std::clamp<int32>(root, 0, 11);

    if (version >= 2u) {
        int32 storedManualRoot = settings.rootPitchClass;
        int32 storedRootSource = 1;
        if (!readValue(state, storedManualRoot) || !readValue(state, storedRootSource))
            return false;
        manualRootPitchClass = std::clamp<int32>(storedManualRoot, 0, 11);
        midiRootSource = storedRootSource != 0;

        if (version >= 3u) {
            int32 storedStyle = 0;
            if (!readValue(state, storedStyle))
                return false;
            settings.style = static_cast<midiator::StyleId>(
                std::clamp<int32>(storedStyle, 0, static_cast<int32>(midiator::StyleId::Count) - 1));
        } else {
            settings.style = midiator::StyleId::NDHIndustrial;
        }

        if (version >= 4u) {
            int32 storedPowerChordsEnabled = 1;
            if (!readValue(state, storedPowerChordsEnabled))
                return false;
            powerChordsEnabled = storedPowerChordsEnabled != 0;
            settings.powerChordsEnabled = powerChordsEnabled;
        } else {
            powerChordsEnabled = true;
            settings.powerChordsEnabled = true;
        }
    } else {
        // V1 had only one fixed root and therefore maps naturally to Manual.
        manualRootPitchClass = settings.rootPitchClass;
        midiRootSource = false;
        settings.style = midiator::StyleId::NDHIndustrial;
        powerChordsEnabled = true;
        settings.powerChordsEnabled = true;
    }

    settings.scale = static_cast<midiator::ScaleId>(
        std::clamp<int32>(scale, 0, static_cast<int32>(midiator::ScaleId::Count) - 1));
    settings.bars = (bars == 1 || bars == 2 || bars == 4 || bars == 8) ? bars : 2;
    settings.density = std::clamp(settings.density, 0.0f, 1.0f);
    settings.complexity = std::clamp(settings.complexity, 0.0f, 1.0f);
    settings.repetition = std::clamp(settings.repetition, 0.0f, 1.0f);
    settings.powerChordChance = std::clamp(settings.powerChordChance, 0.0f, 1.0f);
    settings.palmMuteChance = std::clamp(settings.palmMuteChance, 0.0f, 1.0f);
    variationAmount = std::clamp(variationAmount, 0.0f, 1.0f);
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
    if (state) {
        for (auto& bus : activePitchesByBus_)
            bus.fill(false);
        wasPlaying_ = false;
        haveExpectedProjectTime_ = false;
        expectedProjectTimeQn_ = 0.0;
        haveTransportAnchor_ = false;
        transportAnchorQn_ = 0.0;
        phraseChangedNeedsFlush_ = false;
    }
    return AudioEffect::setActive(state);
}

tresult PLUGIN_API MidiatorProcessor::setProcessing(TBool state) {
    // Midiator has no audio DSP resources to start/stop, but hosts and
    // stress-testers legitimately expect the VST3 processing transition
    // to be implemented instead of inheriting kNotImplemented.
    for (auto& bus : activePitchesByBus_)
            bus.fill(false);
    wasPlaying_ = false;
    haveExpectedProjectTime_ = false;
    expectedProjectTimeQn_ = 0.0;
    haveTransportAnchor_ = false;
    transportAnchorQn_ = 0.0;
    phraseChangedNeedsFlush_ = false;
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
    if (!writeValue(state, manualRoot) || !writeValue(state, rootSource) ||
        !writeValue(state, style) || !writeValue(state, powerChordsEnabled))
        return kResultFalse;

    const int32 phraseBars = phrase_.bars;
    if (!writeValue(state, phraseBars))
        return kResultFalse;

    for (int i = 0; i < midiator::kMaxSteps; ++i) {
        const auto& step = phrase_.steps[i];
        const int32 noteCount = std::clamp(step.noteCount, 0, midiator::kMaxNotesPerStep);
        if (!writeValue(state, noteCount))
            return kResultFalse;

        for (int n = 0; n < midiator::kMaxNotesPerStep; ++n) {
            const int32 pitch = step.notes[n].pitch;
            const int32 velocity = step.notes[n].velocity;
            const int32 lengthSteps = step.notes[n].lengthSteps;
            if (!writeValue(state, pitch) || !writeValue(state, velocity) || !writeValue(state, lengthSteps))
                return kResultFalse;
        }
    }

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

    if (!readStateHeader(state, restored, restoredVariation, restoredSeed,
                         restoredManualRoot, restoredMidiRootSource,
                         restoredPowerChordsEnabled))
        return kResultFalse;

    int32 phraseBars = 0;
    if (!readValue(state, phraseBars))
        return kResultFalse;

    midiator::Phrase restoredPhrase{};
    restoredPhrase.bars = (phraseBars == 1 || phraseBars == 2 || phraseBars == 4 || phraseBars == 8)
        ? phraseBars : restored.bars;

    for (int i = 0; i < midiator::kMaxSteps; ++i) {
        int32 noteCount = 0;
        if (!readValue(state, noteCount))
            return kResultFalse;
        restoredPhrase.steps[i].noteCount = std::clamp<int32>(noteCount, 0, midiator::kMaxNotesPerStep);

        for (int n = 0; n < midiator::kMaxNotesPerStep; ++n) {
            int32 pitch = 0;
            int32 velocity = 0;
            int32 lengthSteps = 1;
            if (!readValue(state, pitch) || !readValue(state, velocity) || !readValue(state, lengthSteps))
                return kResultFalse;

            restoredPhrase.steps[i].notes[n].pitch = std::clamp<int32>(pitch, 0, 127);
            restoredPhrase.steps[i].notes[n].velocity = std::clamp<int32>(velocity, 0, 126);
            restoredPhrase.steps[i].notes[n].lengthSteps = std::clamp<int32>(lengthSteps, 1, 16);
        }
    }

    settings_ = restored;
    variationAmount_ = restoredVariation;
    seed_ = restoredSeed ? restoredSeed : 0x125A2026u;
    manualRootPitchClass_ = restoredManualRoot;
    midiRootSource_ = restoredMidiRootSource;
    settings_.powerChordsEnabled = restoredPowerChordsEnabled;
    phrase_ = restoredPhrase;
    regenerateBass();
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
    const int requiredDifference = havePrevious
        ? std::max(6, previous.usedSteps() / 3)
        : 0;

    // NEW RIFF must sound like a genuinely new groove, not merely a pitch
    // variation. Keep the structurally valid candidate with the lowest onset
    // overlap and accept immediately once both distance requirements are met.
    midiator::Phrase bestCandidate{};
    double bestOnsetJaccard = 2.0;
    int bestStructuralDifference = -1;

    for (int attempt = 0; attempt < 32; ++attempt) {
        seed_ = nextSeed(seed_);
        candidate = midiator::RiffEngine::generate(settings_, seed_);

        if (!havePrevious) {
            bestCandidate = candidate;
            bestStructuralDifference = candidate.usedSteps();
            bestOnsetJaccard = 0.0;
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
        }

        if (difference >= requiredDifference && jaccard <= 0.48)
            break;
    }

    phrase_ = bestCandidate;
    regenerateBass();
    phraseChangedNeedsFlush_ = true;

    // Replacing the musical content must never move the sequencer phase.
    // Keep the existing transport anchor so NEW RIFF, style changes and other
    // regenerations remain locked to the host's running 16th-note grid.
}
void MidiatorProcessor::generateVariation() {
    seed_ = nextSeed(seed_);
    phrase_ = midiator::RiffEngine::vary(phrase_, settings_, variationAmount_, seed_);
    regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::regenerateBass() {
    bassSettings_.rootPitchClass = settings_.rootPitchClass;
    bassSettings_.scale = settings_.scale;
    bassSettings_.style = settings_.style;
    bassPhrase_ = midiator::BassBrain::generate(
        phrase_, bassSettings_, seed_ ^ 0xB4552026u);
    regenerateDrums();
}

void MidiatorProcessor::regenerateDrums() {
    drumSettings_.style = settings_.style;
    drumPhrase_ = midiator::DrumBrain::generate(
        phrase_, bassPhrase_, drumSettings_, seed_ ^ 0xD12A2026u);
    regeneratePads();
}

void MidiatorProcessor::regeneratePads() {
    padSettings_.rootPitchClass = settings_.rootPitchClass;
    padSettings_.scale = settings_.scale;
    padSettings_.style = settings_.style;
    padPhrase_ = midiator::PadBrain::generate(
        phrase_, bassPhrase_, padSettings_, seed_ ^ 0x50414426u);
    regenerateSynth();
}

void MidiatorProcessor::regenerateSynth() {
    synthSettings_.rootPitchClass = settings_.rootPitchClass;
    synthSettings_.scale = settings_.scale;
    synthSettings_.style = settings_.style;
    synthPhrase_ = midiator::SynthBrain::generate(
        phrase_, bassPhrase_, padPhrase_, synthSettings_, seed_ ^ 0x53594E26u);
}

void MidiatorProcessor::resizePhraseBars(int newBars) {
    if (newBars != 1 && newBars != 2 && newBars != 4 && newBars != 8)
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
    regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::applyPowerChordMode(bool enabled) {
    settings_.powerChordsEnabled = enabled;

    if (!enabled) {
        for (int i = 0; i < phrase_.usedSteps(); ++i)
            if (phrase_.steps[i].noteCount > 1)
                phrase_.steps[i].noteCount = 1;
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
    if (settings_.style == midiator::StyleId::DarkRockGothic)
        styleFactor = 1.10f;
    else if (settings_.style == midiator::StyleId::HeavyIndustrial)
        styleFactor = 0.92f;

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
            step.notes[1].velocity = std::max(
                step.notes[0].velocity >= 88 ? 88 : 30,
                step.notes[0].velocity - 3);
            ++existingChords;
        }
    }

    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::transposePhraseToRoot(int newRootPitchClass) {
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
    regenerateBass();
    phraseChangedNeedsFlush_ = true;
}

void MidiatorProcessor::applyMidiRootInput(ProcessData& data) {
    if (!midiRootSource_ || !data.inputEvents)
        return;

    for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
        Event event{};
        if (data.inputEvents->getEvent(i, event) != kResultOk)
            continue;

        if (event.type != Event::kNoteOnEvent || event.noteOn.velocity <= 0.0f)
            continue;

        int pitchClass = static_cast<int>(event.noteOn.pitch) % 12;
        if (pitchClass < 0)
            pitchClass += 12;

        // MIDI input is a tonal controller in MIDI-root mode. The source note
        // itself is intentionally not copied to outputEvents.
        transposePhraseToRoot(pitchClass);
    }
}

void MidiatorProcessor::applyParameterChanges(ProcessData& data) {
    if (!data.inputParameterChanges)
        return;

    bool tonalFrameChanged = false;

    for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
        auto* queue = data.inputParameterChanges->getParameterData(i);
        if (!queue || queue->getPointCount() <= 0)
            continue;

        const auto id = queue->getParameterId();

        // Fallback action-parameter path. The normal GUI path uses explicit
        // controller->processor messages; if a host cannot connect the two
        // peers, each delivered fallback parameter change is one command.
        if (id == kNewRiffId || id == kVariationId) {
            if (id == kNewRiffId)
                generateNew();
            else
                generateVariation();
            continue;
        }

        int32 sampleOffset = 0;
        ParamValue v = 0.0;
        if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, v) != kResultOk)
            continue;

        v = std::clamp(v, 0.0, 1.0);

        switch (id) {
            case kRootId: {
                const int next = normalizedIndex(v, 12);
                manualRootPitchClass_ = next;
                if (!midiRootSource_ && next != settings_.rootPitchClass) {
                    transposePhraseToRoot(next);
                }
                break;
            }
            case kScaleId: {
                const auto next = static_cast<midiator::ScaleId>(
                    normalizedIndex(v, static_cast<int>(midiator::ScaleId::Count)));
                if (next != settings_.scale) {
                    settings_.scale = next;
                    tonalFrameChanged = true;
                }
                break;
            }
            case kBarsId: {
                static constexpr int bars[] = {1, 2, 4, 8};
                const int next = bars[normalizedIndex(v, 4)];
                if (next != settings_.bars)
                    resizePhraseBars(next);
                break;
            }
            case kDensityId:
                settings_.density = static_cast<float>(v);
                break;
            case kComplexityId:
                settings_.complexity = static_cast<float>(v);
                break;
            case kRepetitionId:
                settings_.repetition = static_cast<float>(v);
                break;
            case kPowerChordId:
                settings_.powerChordChance = static_cast<float>(v);
                break;
            case kPowerChordsEnabledId: {
                const bool next = v > 0.5;
                if (next != settings_.powerChordsEnabled)
                    applyPowerChordMode(next);
                break;
            }
            case kPalmMuteId:
                settings_.palmMuteChance = static_cast<float>(v);
                break;
            case kVariationAmountId:
                variationAmount_ = static_cast<float>(v);
                break;
            case kStyleId: {
                const auto next = static_cast<midiator::StyleId>(
                    normalizedIndex(v, static_cast<int>(midiator::StyleId::Count)));
                if (next != settings_.style) {
                    settings_.style = next;
                    tonalFrameChanged = true;
                }
                break;
            }
            case kRootSourceId: {
                const bool nextMidi = v > 0.5;
                if (nextMidi != midiRootSource_) {
                    midiRootSource_ = nextMidi;
                    if (!midiRootSource_)
                        transposePhraseToRoot(manualRootPitchClass_);
                }
                break;
            }
            case kNewRiffId:
            case kVariationId:
                // Edge-triggered action parameters are handled point-by-point
                // before this switch.
                break;
            default:
                break;
        }
    }

    if (tonalFrameChanged)
        generateNew();
}

void MidiatorProcessor::flushActiveNotes(IEventList* output, double ppqPosition) {
    if (!output)
        return;

    for (int bus = 0; bus < kEventOutputBusCount; ++bus) {
        for (int pitch = 0; pitch < 128; ++pitch) {
            if (!activePitchesByBus_[static_cast<size_t>(bus)][pitch])
                continue;

            Event e{};
            e.busIndex = bus;
            e.sampleOffset = 0;
            e.ppqPosition = ppqPosition;
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = 0;
            e.noteOff.pitch = static_cast<int16>(pitch);
            e.noteOff.velocity = 0.0f;
            e.noteOff.noteId = -1;
            output->addEvent(e);
            activePitchesByBus_[static_cast<size_t>(bus)][pitch] = false;
        }
    }
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

    // First consume musical parameter/root changes for this block. A subsequent
    // NEW RIFF command must use the newest Style/Scale/Density/etc., not the
    // settings from the previous block.
    applyParameterChanges(data);
    applyMidiRootInput(data);

    // GUI commands arrive through IConnectionPoint. notify() only increments
    // atomics; phrase state is mutated here on the processing thread.
    const uint32_t newCount = pendingNewRiffCommands_.exchange(0, std::memory_order_acq_rel);
    const uint32_t variationCount = pendingVariationCommands_.exchange(0, std::memory_order_acq_rel);

    // Coalesce same-block GUI bursts. Intermediate compositions cannot be
    // observed by the host before this process call returns, so calculating
    // many complete five-role arrangements in one audio block only wastes
    // realtime budget. Separate clicks delivered in separate blocks remain
    // separate commands.
    if (newCount > 0)
        generateNew();
    if (variationCount > 0)
        generateVariation();

    if (!data.outputEvents)
        return kResultOk;

    const auto* context = data.processContext;
    const bool hasTempo = context && (context->state & ProcessContext::kTempoValid) && context->tempo > 0.0;
    const bool hasProjectTime = context && (context->state & ProcessContext::kProjectTimeMusicValid);
    const bool playing = context && (context->state & ProcessContext::kPlaying);
    const double currentPpq = hasProjectTime ? context->projectTimeMusic : 0.0;

    if (phraseChangedNeedsFlush_) {
        flushActiveNotes(data.outputEvents, currentPpq);
        phraseChangedNeedsFlush_ = false;
    }

    if (!playing || !hasTempo || !hasProjectTime || data.numSamples <= 0) {
        if (wasPlaying_)
            flushActiveNotes(data.outputEvents, currentPpq);
        wasPlaying_ = playing;
        haveExpectedProjectTime_ = false;
        haveTransportAnchor_ = false;
        transportAnchorQn_ = 0.0;
        return kResultOk;
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
        if (timelineJump)
            flushActiveNotes(data.outputEvents, blockStartQn);
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

    auto addScheduled = [&](double eventQn, bool noteOn, int pitch, int velocity, int32 busIndex) {
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
        scheduled[scheduledCount++] = e;
    };

    const double localBlockStartQn = blockStartQn - transportAnchorQn_;
    const double localBlockEndQn = blockEndQn - transportAnchorQn_;
    const long long firstCycle = static_cast<long long>(std::floor(localBlockStartQn / patternLengthQn)) - 1;
    const long long lastCycle = static_cast<long long>(std::floor(localBlockEndQn / patternLengthQn)) + 1;

    auto schedulePhrase = [&](const midiator::Phrase& phrase, int32 busIndex) {
        for (long long cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            const double cycleStartQn =
                transportAnchorQn_ + static_cast<double>(cycle) * patternLengthQn;
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
                    const double requestedOffQn =
                        onQn + static_cast<double>(std::max(1, note.lengthSteps)) *
                                   kStepQuarterNotes - kSustainGapQn;
                    const double offQn =
                        std::max(onQn, std::min(latestOffQn, requestedOffQn));
                    addScheduled(onQn, true, note.pitch, note.velocity, busIndex);
                    addScheduled(offQn, false, note.pitch, 0, busIndex);
                }
            }
        }
    };

    schedulePhrase(phrase_, kGuitarOutBus);
    schedulePhrase(bassPhrase_, kBassOutBus);

    auto scheduleDrums = [&]() {
        constexpr double kDrumGateQn = 1.0 / 16.0;
        for (long long cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            const double cycleStartQn =
                transportAnchorQn_ + static_cast<double>(cycle) * patternLengthQn;
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
                    addScheduled(onQn, true, pitch, hit.velocity, kDrumsOutBus);
                    addScheduled(offQn, false, pitch, 0, kDrumsOutBus);
                }
            }
        }
    };
    scheduleDrums();

    auto schedulePads = [&]() {
        constexpr double kPadReleaseGapQn = 1.0 / 64.0;
        for (long long cycle = firstCycle; cycle <= lastCycle; ++cycle) {
            const double cycleStartQn =
                transportAnchorQn_ + static_cast<double>(cycle) * patternLengthQn;
            for (int stepIndex = 0; stepIndex < padPhrase_.usedSteps(); ++stepIndex) {
                const auto& step = padPhrase_.steps[stepIndex];
                if (step.noteCount <= 0)
                    continue;
                const double onQn =
                    cycleStartQn + static_cast<double>(stepIndex) * kStepQuarterNotes;
                for (int n = 0; n < step.noteCount; ++n) {
                    const auto& note = step.notes[n];
                    const double durationQn =
                        static_cast<double>(std::max(1, note.lengthSteps)) *
                        kStepQuarterNotes;
                    const double offQn =
                        std::max(onQn, onQn + durationQn - kPadReleaseGapQn);
                    addScheduled(onQn, true, note.pitch, note.velocity, kPadOutBus);
                    addScheduled(offQn, false, note.pitch, 0, kPadOutBus);
                }
            }
        }
    };
    schedulePads();
    schedulePhrase(synthPhrase_, kSynthOutBus);

    if (schedulerOverflow) {
        // Never silently drop scheduled MIDI. A host-sized block that exceeds
        // the fixed realtime-safe staging buffer is reported explicitly.
        // Also release any notes carried in from the previous block so failure
        // cannot leave downstream instruments hanging.
        flushActiveNotes(data.outputEvents, blockStartQn);
        return kResultFalse;
    }

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

    for (int i = 0; i < scheduledCount; ++i) {
        const auto& s = scheduled[i];
        Event e{};
        e.busIndex = s.busIndex;
        e.sampleOffset = s.sampleOffset;
        e.ppqPosition = s.ppqPosition;

        if (s.noteOn) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = 0;
            e.noteOn.pitch = static_cast<int16>(s.pitch);
            e.noteOn.velocity = static_cast<float>(s.velocity) / 127.0f;
            e.noteOn.length = 0;
            e.noteOn.tuning = 0.0f;
            e.noteOn.noteId = -1;
            data.outputEvents->addEvent(e);
            activePitchesByBus_[static_cast<size_t>(s.busIndex)][s.pitch] = true;
        } else {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = 0;
            e.noteOff.pitch = static_cast<int16>(s.pitch);
            e.noteOff.velocity = 0.0f;
            e.noteOff.noteId = -1;
            data.outputEvents->addEvent(e);
            activePitchesByBus_[static_cast<size_t>(s.busIndex)][s.pitch] = false;
        }
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

    auto* style = new StringListParameter(STR16("Riff Style"), kStyleId);
    style->appendString(STR16("NDH / Industrial"));
    style->appendString(STR16("Dark Rock / Gothic"));
    style->appendString(STR16("Heavy Industrial"));
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

    auto* bars = new StringListParameter(STR16("Bars"), kBarsId);
    bars->appendString(STR16("1"));
    bars->appendString(STR16("2"));
    bars->appendString(STR16("4"));
    bars->appendString(STR16("8"));
    bars->getInfo().defaultNormalizedValue = 1.0 / 3.0;
    bars->setNormalized(bars->getInfo().defaultNormalizedValue);
    parameters.addParameter(bars);

    auto addPercent = [&](const char16_t* name, ParamID id, double defaultValue) {
        auto* p = new RangeParameter(name, id, STR16("%"), 0.0, 100.0, defaultValue, 0,
                                     ParameterInfo::kCanAutomate);
        p->setPrecision(0);
        parameters.addParameter(p);
    };

    addPercent(STR16("Density"), kDensityId, 56.0);
    addPercent(STR16("Complexity"), kComplexityId, 42.0);
    addPercent(STR16("Repetition"), kRepetitionId, 72.0);
    auto* powerChordsEnabled = new StringListParameter(STR16("Power Chords"), kPowerChordsEnabledId);
    powerChordsEnabled->appendString(STR16("OFF"));
    powerChordsEnabled->appendString(STR16("ON"));
    powerChordsEnabled->getInfo().defaultNormalizedValue = 1.0;
    powerChordsEnabled->setNormalized(1.0);
    parameters.addParameter(powerChordsEnabled);

    addPercent(STR16("Power Chords Amount"), kPowerChordId, 25.0);
    addPercent(STR16("Palm Mute"), kPalmMuteId, 70.0);
    addPercent(STR16("Variation Amount"), kVariationAmountId, 35.0);

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
    if (!readStateHeader(state, restored, variationAmount, seed,
                         manualRoot, midiRootSource, powerChordsEnabled))
        return kResultFalse;

    auto barsIndex = [](int bars) -> double {
        switch (bars) {
            case 1: return 0.0;
            case 2: return 1.0 / 3.0;
            case 4: return 2.0 / 3.0;
            case 8: return 1.0;
            default: return 1.0 / 3.0;
        }
    };

    setParamNormalized(kRootId, static_cast<double>(manualRoot) / 11.0);
    setParamNormalized(kRootSourceId, midiRootSource ? 1.0 : 0.0);
    setParamNormalized(kPowerChordsEnabledId, powerChordsEnabled ? 1.0 : 0.0);
    setParamNormalized(kStyleId, static_cast<double>(static_cast<int>(restored.style)) /
                                 static_cast<double>(static_cast<int>(midiator::StyleId::Count) - 1));
    setParamNormalized(kScaleId, static_cast<double>(static_cast<int>(restored.scale)) /
                                  static_cast<double>(static_cast<int>(midiator::ScaleId::Count) - 1));
    setParamNormalized(kBarsId, barsIndex(restored.bars));
    setParamNormalized(kDensityId, restored.density);
    setParamNormalized(kComplexityId, restored.complexity);
    setParamNormalized(kRepetitionId, restored.repetition);
    setParamNormalized(kPowerChordId, restored.powerChordChance);
    setParamNormalized(kPalmMuteId, restored.palmMuteChance);
    setParamNormalized(kVariationAmountId, variationAmount);

    return kResultOk;
}


tresult PLUGIN_API MidiatorController::setParamNormalized(ParamID tag, ParamValue value) {
    const auto r = EditControllerEx1::setParamNormalized(tag, value);
    if (tag == kRootId || tag == kScaleId)
        refreshTheory();
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
        }
    }

    refreshTheory();
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
