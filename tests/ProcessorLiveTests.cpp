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
                            IParameterChanges* changes = nullptr) {
    ProcessData data {};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numSamples;
    data.processContext = &context;
    data.outputEvents = &output;
    data.inputParameterChanges = changes;
    return data;
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
    testLiveDownbeatAndStopFlush();
    testNewRiffFlushesHeldNotes();
    testVariationPressReleaseFlushesHeldNotes();
    testTransportJumpFlushesHeldNotes();

    std::cout << "Midiator live processor tests: PASS\n";
    return 0;
}
