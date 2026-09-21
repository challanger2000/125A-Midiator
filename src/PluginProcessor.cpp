#include "PluginProcessor.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "vstgui/lib/controls/ctextlabel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Steinberg::Vst {
namespace {

constexpr double kStepQuarterNotes = 0.25;
constexpr int kMaxScheduledEvents = 2048;
constexpr uint32_t kStateMagic = 0x4D445231u; // "MDR1"
constexpr uint32_t kStateVersion = 1u;

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
                     uint32_t& seed) {
    uint32_t magic = 0;
    uint32_t version = 0;
    int32 root = 0;
    int32 scale = 0;
    int32 bars = 0;

    if (!readValue(state, magic) || !readValue(state, version) ||
        magic != kStateMagic || version != kStateVersion ||
        !readValue(state, root) || !readValue(state, scale) || !readValue(state, bars) ||
        !readValue(state, settings.density) || !readValue(state, settings.complexity) ||
        !readValue(state, settings.repetition) || !readValue(state, settings.powerChordChance) ||
        !readValue(state, settings.palmMuteChance) || !readValue(state, variationAmount) ||
        !readValue(state, seed)) {
        return false;
    }

    settings.rootPitchClass = std::clamp<int32>(root, 0, 11);
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
    generateNew();
}

tresult PLUGIN_API MidiatorProcessor::initialize(FUnknown* context) {
    auto r = AudioEffect::initialize(context);
    if (r != kResultOk)
        return r;

    addEventInput(STR16("MIDI In"), 16, kMain, BusInfo::kDefaultActive);
    addEventOutput(STR16("MIDI Out"), 16, kMain, BusInfo::kDefaultActive);
    return kResultOk;
}

tresult PLUGIN_API MidiatorProcessor::setActive(TBool state) {
    if (state) {
        activePitches_.fill(false);
        wasPlaying_ = false;
        haveExpectedProjectTime_ = false;
        expectedProjectTimeQn_ = 0.0;
    }
    return AudioEffect::setActive(state);
}

tresult PLUGIN_API MidiatorProcessor::setProcessing(TBool state) {
    // Midiator has no audio DSP resources to start/stop, but hosts and
    // stress-testers legitimately expect the VST3 processing transition
    // to be implemented instead of inheriting kNotImplemented.
    activePitches_.fill(false);
    wasPlaying_ = false;
    haveExpectedProjectTime_ = false;
    expectedProjectTimeQn_ = 0.0;
    return kResultOk;
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

    if (!readStateHeader(state, restored, restoredVariation, restoredSeed))
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
    phrase_ = restoredPhrase;
    activePitches_.fill(false);
    wasPlaying_ = false;
    haveExpectedProjectTime_ = false;
    expectedProjectTimeQn_ = 0.0;
    return kResultOk;
}

void MidiatorProcessor::generateNew() {
    seed_ = nextSeed(seed_);
    phrase_ = midiator::RiffEngine::generate(settings_, seed_);
}

void MidiatorProcessor::generateVariation() {
    seed_ = nextSeed(seed_);
    phrase_ = midiator::RiffEngine::vary(phrase_, settings_, variationAmount_, seed_);
}

void MidiatorProcessor::applyParameterChanges(ProcessData& data) {
    if (!data.inputParameterChanges)
        return;

    bool tonalFrameChanged = false;

    for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
        auto* queue = data.inputParameterChanges->getParameterData(i);
        if (!queue || queue->getPointCount() <= 0)
            continue;

        int32 sampleOffset = 0;
        ParamValue v = 0.0;
        if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, v) != kResultOk)
            continue;

        v = std::clamp(v, 0.0, 1.0);
        const auto id = queue->getParameterId();

        switch (id) {
            case kRootId: {
                const int next = normalizedIndex(v, 12);
                if (next != settings_.rootPitchClass) {
                    settings_.rootPitchClass = next;
                    tonalFrameChanged = true;
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
                if (next != settings_.bars) {
                    settings_.bars = next;
                    tonalFrameChanged = true;
                }
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
            case kPalmMuteId:
                settings_.palmMuteChance = static_cast<float>(v);
                break;
            case kVariationAmountId:
                variationAmount_ = static_cast<float>(v);
                break;
            case kNewRiffId:
                if (newRiffTrigger_.update(v))
                    generateNew();
                break;
            case kVariationId:
                if (variationTrigger_.update(v))
                    generateVariation();
                break;
            default:
                break;
        }
    }

    if (tonalFrameChanged)
        generateNew();
}

void MidiatorProcessor::flushActiveNotes(IEventList* output) {
    if (!output)
        return;

    for (int pitch = 0; pitch < 128; ++pitch) {
        if (!activePitches_[pitch])
            continue;

        Event e{};
        e.busIndex = 0;
        e.sampleOffset = 0;
        e.type = Event::kNoteOffEvent;
        e.noteOff.channel = 0;
        e.noteOff.pitch = static_cast<int16>(pitch);
        e.noteOff.velocity = 0.0f;
        e.noteOff.noteId = -1;
        output->addEvent(e);
        activePitches_[pitch] = false;
    }
}

tresult PLUGIN_API MidiatorProcessor::process(ProcessData& data) {
    if (data.processContext && data.processContext->sampleRate > 0.0)
        sampleRate_ = data.processContext->sampleRate;

    applyParameterChanges(data);

    if (!data.outputEvents)
        return kResultOk;

    const auto* context = data.processContext;
    const bool hasTempo = context && (context->state & ProcessContext::kTempoValid) && context->tempo > 0.0;
    const bool hasProjectTime = context && (context->state & ProcessContext::kProjectTimeMusicValid);
    const bool playing = context && (context->state & ProcessContext::kPlaying);

    if (!playing || !hasTempo || !hasProjectTime || data.numSamples <= 0) {
        if (wasPlaying_)
            flushActiveNotes(data.outputEvents);
        wasPlaying_ = playing;
        haveExpectedProjectTime_ = false;
        return kResultOk;
    }

    const double tempo = context->tempo;
    const double qnPerSample = tempo / (60.0 * sampleRate_);
    if (qnPerSample <= 0.0)
        return kResultOk;

    const double blockStartQn = context->projectTimeMusic;
    const double blockEndQn = blockStartQn + static_cast<double>(data.numSamples) * qnPerSample;

    if (wasPlaying_ && haveExpectedProjectTime_) {
        const double tolerance = std::max(1e-6, qnPerSample * 4.0);
        if (std::abs(blockStartQn - expectedProjectTimeQn_) > tolerance)
            flushActiveNotes(data.outputEvents);
    }

    wasPlaying_ = true;
    haveExpectedProjectTime_ = true;
    expectedProjectTimeQn_ = blockEndQn;
    const double patternLengthQn = static_cast<double>(phrase_.bars) * 4.0;

    if (patternLengthQn <= 0.0)
        return kResultOk;

    std::array<ScheduledEvent, kMaxScheduledEvents> scheduled{};
    int scheduledCount = 0;

    auto addScheduled = [&](double eventQn, bool noteOn, int pitch, int velocity) {
        constexpr double eps = 1e-9;
        if (eventQn + eps < blockStartQn || eventQn >= blockEndQn - eps)
            return;
        if (scheduledCount >= kMaxScheduledEvents)
            return;

        const double relSamples = (eventQn - blockStartQn) / qnPerSample;
        ScheduledEvent e{};
        e.sampleOffset = std::clamp<int32>(
            static_cast<int32>(std::floor(relSamples + 1e-9)),
            0,
            std::max<int32>(0, data.numSamples - 1));
        e.noteOn = noteOn;
        e.pitch = std::clamp(pitch, 0, 127);
        e.velocity = std::clamp(velocity, 0, 126);
        scheduled[scheduledCount++] = e;
    };

    const long long firstCycle = static_cast<long long>(std::floor(blockStartQn / patternLengthQn)) - 1;
    const long long lastCycle = static_cast<long long>(std::floor(blockEndQn / patternLengthQn)) + 1;

    for (long long cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        const double cycleStartQn = static_cast<double>(cycle) * patternLengthQn;

        for (int stepIndex = 0; stepIndex < phrase_.usedSteps(); ++stepIndex) {
            const auto& step = phrase_.steps[stepIndex];
            if (step.noteCount <= 0)
                continue;

            const double onQn = cycleStartQn + static_cast<double>(stepIndex) * kStepQuarterNotes;

            for (int n = 0; n < step.noteCount; ++n) {
                const auto& note = step.notes[n];
                const double offQn = onQn + static_cast<double>(std::max(1, note.lengthSteps)) * kStepQuarterNotes * 0.90;

                addScheduled(onQn, true, note.pitch, note.velocity);
                addScheduled(offQn, false, note.pitch, 0);
            }
        }
    }

    std::sort(scheduled.begin(), scheduled.begin() + scheduledCount,
              [](const ScheduledEvent& a, const ScheduledEvent& b) {
                  if (a.sampleOffset != b.sampleOffset)
                      return a.sampleOffset < b.sampleOffset;
                  if (a.noteOn != b.noteOn)
                      return !a.noteOn; // NoteOff before NoteOn at the same sample.
                  return a.pitch < b.pitch;
              });

    for (int i = 0; i < scheduledCount; ++i) {
        const auto& s = scheduled[i];
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = s.sampleOffset;

        if (s.noteOn) {
            e.type = Event::kNoteOnEvent;
            e.noteOn.channel = 0;
            e.noteOn.pitch = static_cast<int16>(s.pitch);
            e.noteOn.velocity = static_cast<float>(s.velocity) / 127.0f;
            e.noteOn.length = 0;
            e.noteOn.tuning = 0.0f;
            e.noteOn.noteId = -1;
            data.outputEvents->addEvent(e);
            activePitches_[s.pitch] = true;
        } else {
            e.type = Event::kNoteOffEvent;
            e.noteOff.channel = 0;
            e.noteOff.pitch = static_cast<int16>(s.pitch);
            e.noteOff.velocity = 0.0f;
            e.noteOff.noteId = -1;
            data.outputEvents->addEvent(e);
            activePitches_[s.pitch] = false;
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
    addPercent(STR16("Power Chords"), kPowerChordId, 25.0);
    addPercent(STR16("Palm Mute"), kPalmMuteId, 70.0);
    addPercent(STR16("Variation Amount"), kVariationAmountId, 35.0);

    auto* newRiff = new RangeParameter(STR16("NEW Riff"), kNewRiffId, STR16(""),
                                       0.0, 1.0, 0.0, 1, 0);
    parameters.addParameter(newRiff);

    auto* variation = new RangeParameter(STR16("VARIATION"), kVariationId, STR16(""),
                                         0.0, 1.0, 0.0, 1, 0);
    parameters.addParameter(variation);

    return kResultOk;
}

tresult PLUGIN_API MidiatorController::setComponentState(IBStream* state) {
    if (!state)
        return kInvalidArgument;

    midiator::GeneratorSettings restored{};
    float variationAmount = 0.35f;
    uint32_t seed = 0;
    if (!readStateHeader(state, restored, variationAmount, seed))
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

    setParamNormalized(kRootId, static_cast<double>(restored.rootPitchClass) / 11.0);
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
