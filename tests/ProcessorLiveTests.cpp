#include "PluginProcessor.h"

#include "base/source/fobject.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Console-test stub normally provided by the VST3 module entry point.
void* moduleHandle = nullptr;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

struct EventList final : FObject, IEventList {
    std::vector<Event> events;

    int32 PLUGIN_API getEventCount() SMTG_OVERRIDE {
        return static_cast<int32>(events.size());
    }

    tresult PLUGIN_API getEvent(int32 index, Event& event) SMTG_OVERRIDE {
        if (index < 0 || index >= static_cast<int32>(events.size()))
            return kInvalidArgument;
        event = events[static_cast<size_t>(index)];
        return kResultOk;
    }

    tresult PLUGIN_API addEvent(Event& event) SMTG_OVERRIDE {
        events.push_back(event);
        return kResultOk;
    }

    OBJ_METHODS(EventList, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IEventList)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

struct ParamQueue final : FObject, IParamValueQueue {
    explicit ParamQueue(ParamID id) : id(id) {}

    ParamID id {};
    std::vector<std::pair<int32, ParamValue>> points;

    ParamID PLUGIN_API getParameterId() SMTG_OVERRIDE { return id; }
    int32 PLUGIN_API getPointCount() SMTG_OVERRIDE {
        return static_cast<int32>(points.size());
    }

    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) SMTG_OVERRIDE {
        if (index < 0 || index >= static_cast<int32>(points.size()))
            return kInvalidArgument;
        sampleOffset = points[static_cast<size_t>(index)].first;
        value = points[static_cast<size_t>(index)].second;
        return kResultOk;
    }

    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) SMTG_OVERRIDE {
        index = static_cast<int32>(points.size());
        points.emplace_back(sampleOffset, value);
        return kResultOk;
    }

    OBJ_METHODS(ParamQueue, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IParamValueQueue)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

struct ParameterChanges final : FObject, IParameterChanges {
    std::vector<ParamQueue> queues;

    int32 PLUGIN_API getParameterCount() SMTG_OVERRIDE {
        return static_cast<int32>(queues.size());
    }

    IParamValueQueue* PLUGIN_API getParameterData(int32 index) SMTG_OVERRIDE {
        if (index < 0 || index >= static_cast<int32>(queues.size()))
            return nullptr;
        return &queues[static_cast<size_t>(index)];
    }

    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) SMTG_OVERRIDE {
        for (size_t i = 0; i < queues.size(); ++i) {
            if (queues[i].id == id) {
                index = static_cast<int32>(i);
                return &queues[i];
            }
        }
        queues.emplace_back(id);
        index = static_cast<int32>(queues.size() - 1);
        return &queues.back();
    }

    OBJ_METHODS(ParameterChanges, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IParameterChanges)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

ProcessContext makeContext(double projectTimeQn, bool playing) {
    ProcessContext context {};
    context.sampleRate = 48000.0;
    context.tempo = 120.0;
    context.projectTimeMusic = projectTimeQn;
    context.state = ProcessContext::kTempoValid |
                    ProcessContext::kProjectTimeMusicValid;
    if (playing)
        context.state |= ProcessContext::kPlaying;
    return context;
}

bool containsType(const EventList& list, Event::EventTypes type) {
    for (const auto& event : list.events)
        if (event.type == type)
            return true;
    return false;
}

ProcessData makeProcessData(ProcessContext& context,
                            EventList& output,
                            int32 numSamples,
                            IParameterChanges* changes = nullptr,
                            IEventList* inputEvents = nullptr) {
    ProcessData data {};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numSamples;
    data.processContext = &context;
    data.outputEvents = &output;
    data.inputEvents = inputEvents;
    data.inputParameterChanges = changes;
    return data;
}

void testGeneratedNotesSustainToOne64BeforeNextHit() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    // One quarter note at 120 BPM/48k = 24000 samples. Process enough time to
    // capture several generated hits and their note-offs.
    auto context = makeContext(0.0, true);
    EventList output;
    auto data = makeProcessData(context, output, 48000);
    require(processor.process(data) == kResultOk, "sustain scheduling process must succeed");

    std::vector<double> ons;
    std::vector<double> offs;
    for (const auto& e : output.events) {
        if (e.type == Event::kNoteOnEvent)
            ons.push_back(e.ppqPosition);
        else if (e.type == Event::kNoteOffEvent)
            offs.push_back(e.ppqPosition);
    }

    require(ons.size() >= 2, "sustain fixture requires at least two generated hits");
    require(!offs.empty(), "sustain fixture requires generated note-offs");

    // Find the first distinct next onset after the first onset. Chord members
    // may share the same onset and must therefore be ignored here.
    const double firstOn = ons.front();
    double nextOn = -1.0;
    for (double on : ons) {
        if (on > firstOn + 1e-9) {
            nextOn = on;
            break;
        }
    }
    require(nextOn > firstOn, "fixture must contain a later distinct hit");

    const double expectedOff = nextOn - (1.0 / 16.0);
    bool foundExpectedOff = false;
    for (double off : offs) {
        if (std::abs(off - expectedOff) < 1e-9) {
            foundExpectedOff = true;
            break;
        }
    }
    require(foundExpectedOff,
            "generated note/chord must end exactly one 1/64 note before the next hit");
}

void testMidiRootSourceTransposesRiff() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    EventList input;
    Event rootEvent{};
    rootEvent.busIndex = 0;
    rootEvent.sampleOffset = 0;
    rootEvent.ppqPosition = 0.0;
    rootEvent.type = Event::kNoteOnEvent;
    rootEvent.noteOn.channel = 0;
    rootEvent.noteOn.pitch = 36; // C2 -> pitch class C
    rootEvent.noteOn.velocity = 1.0f;
    rootEvent.noteOn.noteId = -1;
    require(input.addEvent(rootEvent) == kResultOk, "MIDI root fixture must accept C note");

    auto context = makeContext(0.0, true);
    EventList output;
    auto data = makeProcessData(context, output, 100, nullptr, &input);
    require(processor.process(data) == kResultOk, "MIDI-root process call must succeed");

    bool foundGeneratedC = false;
    bool leakedControlNote = false;
    for (const auto& event : output.events) {
        if (event.type != Event::kNoteOnEvent)
            continue;
        if ((event.noteOn.pitch % 12 + 12) % 12 == 0)
            foundGeneratedC = true;
        if (event.noteOn.pitch == 36 && event.noteOn.velocity == 1.0f)
            leakedControlNote = true;
    }

    require(foundGeneratedC,
            "C input in MIDI root mode must transpose the generated downbeat to C");
    require(!leakedControlNote,
            "MIDI root control note must not be passed through as a played guitar note");
}

void testManualRootSourceIgnoresMidiRootNotes() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    ParameterChanges changes;
    int32 queueIndex = 0;
    auto* sourceQueue = changes.addParameterData(kRootSourceId, queueIndex);
    require(sourceQueue != nullptr, "Root Source queue must be created");
    int32 pointIndex = 0;
    require(sourceQueue->addPoint(0, 0.0, pointIndex) == kResultOk,
            "Manual Root Source value must be accepted");

    EventList input;
    Event rootEvent{};
    rootEvent.busIndex = 0;
    rootEvent.sampleOffset = 0;
    rootEvent.ppqPosition = 0.0;
    rootEvent.type = Event::kNoteOnEvent;
    rootEvent.noteOn.channel = 0;
    rootEvent.noteOn.pitch = 36; // C2
    rootEvent.noteOn.velocity = 1.0f;
    rootEvent.noteOn.noteId = -1;
    require(input.addEvent(rootEvent) == kResultOk, "manual-mode MIDI fixture must accept C note");

    auto context = makeContext(0.0, true);
    EventList output;
    auto data = makeProcessData(context, output, 100, &changes, &input);
    require(processor.process(data) == kResultOk, "manual-root process call must succeed");

    bool foundGeneratedA = false;
    for (const auto& event : output.events) {
        if (event.type == Event::kNoteOnEvent &&
            ((event.noteOn.pitch % 12 + 12) % 12) == 9) {
            foundGeneratedA = true;
            break;
        }
    }
    require(foundGeneratedA,
            "Manual root mode must keep the configured A root despite incoming C");
}

void testTransportStartAtBarTwoBeginsPhraseAtStepZero() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    // In 4/4, bar 2 begins at QN 4 when the host timeline starts at QN 0.
    auto context = makeContext(4.0, true);
    EventList output;
    auto data = makeProcessData(context, output, 100);

    require(processor.process(data) == kResultOk,
            "bar-2 transport-start process call must succeed");
    require(containsType(output, Event::kNoteOnEvent),
            "starting playback at bar 2 must emit phrase step 0 immediately");

    bool foundAnchoredDownbeat = false;
    for (const auto& event : output.events) {
        if (event.type == Event::kNoteOnEvent && std::abs(event.ppqPosition - 4.0) < 1e-9) {
            foundAnchoredDownbeat = true;
            break;
        }
    }
    require(foundAnchoredDownbeat,
            "phrase step 0 must be anchored exactly to the bar-2 play position");
}

void testLiveDownbeatAndStopFlush() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto context = makeContext(0.0, true);
    EventList first;
    auto data = makeProcessData(context, first, 100);

    require(processor.process(data) == kResultOk, "live process call must succeed");
    require(containsType(first, Event::kNoteOnEvent),
            "running transport must emit a downbeat NoteOn");

    for (const auto& event : first.events) {
        if (event.type == Event::kNoteOnEvent) {
            require(event.noteOn.velocity > 0.0f && event.noteOn.velocity < 1.0f,
                    "generated NoteOn velocity must remain below MIDI 127");
            require(std::abs(event.ppqPosition) < 1e-9,
                    "downbeat output event must carry the correct PPQ position");
        }
    }

    auto stoppedContext = makeContext(100.0 / 24000.0, false);
    EventList stopped;
    auto stoppedData = makeProcessData(stoppedContext, stopped, 100);

    require(processor.process(stoppedData) == kResultOk, "stop process call must succeed");
    require(containsType(stopped, Event::kNoteOffEvent),
            "stopping transport must flush active generated notes");
}

void testNewRiffFlushesHeldNotes() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto context = makeContext(0.0, true);
    EventList first;
    auto firstData = makeProcessData(context, first, 100);
    require(processor.process(firstData) == kResultOk, "initial process call must succeed");
    require(containsType(first, Event::kNoteOnEvent),
            "fixture must create an active note before NEW RIFF");

    ParameterChanges changes;
    int32 queueIndex = 0;
    auto* queue = changes.addParameterData(kNewRiffId, queueIndex);
    require(queue != nullptr, "NEW RIFF parameter queue must be created");
    int32 pointIndex = 0;
    require(queue->addPoint(0, 1.0, pointIndex) == kResultOk,
            "NEW RIFF press point must be accepted");
    require(queue->addPoint(1, 0.0, pointIndex) == kResultOk,
            "NEW RIFF release point must be accepted");

    auto nextContext = makeContext(100.0 / 24000.0, true);
    EventList changed;
    auto changedData = makeProcessData(nextContext, changed, 100, &changes);

    require(processor.process(changedData) == kResultOk,
            "NEW RIFF process call must succeed");
    require(containsType(changed, Event::kNoteOffEvent),
            "NEW RIFF while playing must flush notes from the previous phrase");
}

void testVariationPressReleaseFlushesHeldNotes() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto context = makeContext(0.0, true);
    EventList first;
    auto firstData = makeProcessData(context, first, 100);
    require(processor.process(firstData) == kResultOk, "initial process call must succeed");
    require(containsType(first, Event::kNoteOnEvent),
            "fixture must create an active note before VARIATION");

    ParameterChanges changes;
    int32 queueIndex = 0;
    auto* queue = changes.addParameterData(kVariationId, queueIndex);
    require(queue != nullptr, "VARIATION parameter queue must be created");
    int32 pointIndex = 0;
    require(queue->addPoint(0, 1.0, pointIndex) == kResultOk,
            "VARIATION press point must be accepted");
    require(queue->addPoint(1, 0.0, pointIndex) == kResultOk,
            "VARIATION release point must be accepted");

    auto nextContext = makeContext(100.0 / 24000.0, true);
    EventList changed;
    auto changedData = makeProcessData(nextContext, changed, 100, &changes);

    require(processor.process(changedData) == kResultOk,
            "VARIATION process call must succeed");
    require(containsType(changed, Event::kNoteOffEvent),
            "VARIATION press+release in one block must still flush the previous phrase");
}

void testTransportJumpFlushesHeldNotes() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto context = makeContext(0.0, true);
    EventList first;
    auto firstData = makeProcessData(context, first, 100);
    require(processor.process(firstData) == kResultOk, "initial process call must succeed");
    require(containsType(first, Event::kNoteOnEvent),
            "fixture must create an active note before transport jump");

    auto jumpedContext = makeContext(8.0, true);
    EventList jumped;
    auto jumpedData = makeProcessData(jumpedContext, jumped, 100);

    require(processor.process(jumpedData) == kResultOk, "jump process call must succeed");
    require(containsType(jumped, Event::kNoteOffEvent),
            "timeline jump must flush notes that belong to the previous position");
}

} // namespace

int main() {
    testGeneratedNotesSustainToOne64BeforeNextHit();
    testMidiRootSourceTransposesRiff();
    testManualRootSourceIgnoresMidiRootNotes();
    testTransportStartAtBarTwoBeginsPhraseAtStepZero();
    testLiveDownbeatAndStopFlush();
    testNewRiffFlushesHeldNotes();
    testVariationPressReleaseFlushesHeldNotes();
    testTransportJumpFlushesHeldNotes();

    std::cout << "Midiator live processor tests: PASS\n";
    return 0;
}
