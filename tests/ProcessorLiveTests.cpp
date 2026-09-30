#include "PluginProcessor.h"

#include "base/source/fobject.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
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

struct TestMessage final : FObject, IMessage {
    explicit TestMessage(const char* messageId) : id(messageId ? messageId : "") {}

    FIDString PLUGIN_API getMessageID() SMTG_OVERRIDE { return id.c_str(); }
    void PLUGIN_API setMessageID(FIDString messageId) SMTG_OVERRIDE {
        id = messageId ? messageId : "";
    }
    IAttributeList* PLUGIN_API getAttributes() SMTG_OVERRIDE { return nullptr; }

    std::string id;

    OBJ_METHODS(TestMessage, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IMessage)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};


ProcessContext makeContext(double projectTimeQn, bool playing) {
    ProcessContext context {};
    context.sampleRate = 48000.0;
    context.tempo = 120.0;
    context.projectTimeMusic = projectTimeQn;
    context.timeSigNumerator = 4;
    context.timeSigDenominator = 4;
    context.barPositionMusic = std::floor(projectTimeQn / 4.0) * 4.0;
    context.state = ProcessContext::kTempoValid |
                    ProcessContext::kProjectTimeMusicValid |
                    ProcessContext::kBarPositionValid |
                    ProcessContext::kTimeSigValid;
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

void testDedicatedInstrumentOutputBuses() {
    MidiatorProcessor processor;
    require(processor.initialize(nullptr) == kResultOk,
            "processor initialization must succeed for bus inspection");

    require(processor.getBusCount(kEvent, kOutput) == kEventOutputBusCount,
            "Midiator must expose one dedicated event output bus per instrument role");

    struct ExpectedBus { int32 index; const char16_t* name; };
    const ExpectedBus expected[] = {
        {kGuitarOutBus, STR16("Guitar Out")},
        {kBassOutBus,   STR16("Bass Out")},
        {kDrumsOutBus,  STR16("Drums Out")},
        {kPadOutBus,    STR16("Pad Out")},
        {kSynthOutBus,  STR16("Synth Out")}
    };

    for (const auto& item : expected) {
        BusInfo info{};
        require(processor.getBusInfo(kEvent, kOutput, item.index, info) == kResultOk,
                "dedicated output bus must be queryable");
        require(std::char_traits<char16_t>::compare(info.name, item.name,
                    std::char_traits<char16_t>::length(item.name)) == 0,
                "dedicated output bus must keep its role-specific name");
    }

    processor.terminate();
}

void testAllInstrumentRolesUseSeparateOutputBuses() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto context = makeContext(0.0, true);
    EventList output;
    auto data = makeProcessData(context, output, 192000);
    require(processor.process(data) == kResultOk,
            "multi-out routing process must succeed");

    int guitarOns = 0;
    int bassOns = 0;
    int drumOns = 0;
    int padOns = 0;
    int synthOns = 0;
    int unexpectedOns = 0;
    for (const auto& e : output.events) {
        if (e.type != Event::kNoteOnEvent)
            continue;
        if (e.busIndex == kGuitarOutBus)
            ++guitarOns;
        else if (e.busIndex == kBassOutBus) {
            ++bassOns;
            require(e.noteOn.pitch >= 24 && e.noteOn.pitch <= 60,
                    "Bass Out notes must stay in the bass register");
        } else if (e.busIndex == kDrumsOutBus) {
            ++drumOns;
            require(e.noteOn.pitch >= 0 && e.noteOn.pitch <= 127,
                    "Drums Out notes must be valid MIDI notes");
        } else if (e.busIndex == kPadOutBus) {
            ++padOns;
            require(e.noteOn.pitch >= 45 && e.noteOn.pitch <= 88,
                    "Pad Out notes must stay in the pad register");
        } else if (e.busIndex == kSynthOutBus) {
            ++synthOns;
            require(e.noteOn.pitch >= 48 && e.noteOn.pitch <= 96,
                    "Synth Out notes must stay in the melodic synth register");
        } else {
            ++unexpectedOns;
        }
    }

    require(guitarOns > 0, "Guitar Out must emit guitar notes");
    require(bassOns > 0, "Bass Out must emit bass notes");
    require(drumOns > 0, "Drums Out must emit mapped drum notes");
    require(padOns > 0, "Pad Out must emit polyphonic pad notes");
    require(synthOns > 0, "Synth Out must emit melodic synth notes");
    require(unexpectedOns == 0, "no undefined event bus may emit notes");
}

void testNewRiffChangesRhythmMask() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto captureOnsets = [&](double startQn, IParameterChanges* changes) {
        auto context = makeContext(startQn, true);
        EventList output;
        auto data = makeProcessData(context, output, 192000, changes); // 8 QN at 120 BPM
        require(processor.process(data) == kResultOk, "full-phrase capture must succeed");

        std::array<bool, 32> hits{};
        for (const auto& event : output.events) {
            if (event.type != Event::kNoteOnEvent || event.busIndex != kGuitarOutBus)
                continue;
            const double localQn = event.ppqPosition - startQn;
            const int step = static_cast<int>(std::lround(localQn / 0.25));
            if (step >= 0 && step < 32)
                hits[static_cast<size_t>(step)] = true;
        }
        return hits;
    };

    const auto first = captureOnsets(0.0, nullptr);

    ParameterChanges changes;
    int32 queueIndex = 0;
    auto* queue = changes.addParameterData(kNewRiffId, queueIndex);
    require(queue != nullptr, "NEW RIFF queue must be created");
    int32 pointIndex = 0;
    require(queue->addPoint(0, 1.0, pointIndex) == kResultOk,
            "NEW RIFF toggle value must be accepted");

    // Start a new transport phase so the complete replacement riff is captured
    // from step zero rather than from the middle of an old phrase.
    auto stopped = makeContext(8.0, false);
    EventList stoppedOutput;
    auto stoppedData = makeProcessData(stopped, stoppedOutput, 64, &changes);
    require(processor.process(stoppedData) == kResultOk, "NEW RIFF stop block must succeed");

    const auto second = captureOnsets(8.0, nullptr);

    int intersection = 0;
    int unionCount = 0;
    for (size_t i = 0; i < first.size(); ++i) {
        if (first[i] || second[i])
            ++unionCount;
        if (first[i] && second[i])
            ++intersection;
    }
    require(unionCount > 0, "NEW RIFF rhythm comparison needs active onsets");

    const double jaccard = static_cast<double>(intersection) /
                           static_cast<double>(unionCount);
    require(jaccard <= 0.52,
            "NEW RIFF must create a strongly different onset/rest rhythm mask");
}

void testNewRiffToggleZeroValueStillCommands() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto capture = [&](double startQn) {
        auto context = makeContext(startQn, true);
        EventList output;
        auto data = makeProcessData(context, output, 192000);
        require(processor.process(data) == kResultOk, "toggle command capture must succeed");
        std::array<bool, 32> hits{};
        for (const auto& e : output.events) {
            if (e.type != Event::kNoteOnEvent || e.busIndex != kGuitarOutBus)
                continue;
            const int step = static_cast<int>(std::lround((e.ppqPosition - startQn) / 0.25));
            if (step >= 0 && step < 32)
                hits[static_cast<size_t>(step)] = true;
        }
        return hits;
    };

    const auto first = capture(0.0);

    // Simulate a coalescing host's second toggle click: only final value 0 arrives.
    ParameterChanges changes;
    int32 qi = 0;
    auto* q = changes.addParameterData(kNewRiffId, qi);
    int32 pi = 0;
    require(q && q->addPoint(0, 0.0, pi) == kResultOk,
            "zero-valued NEW RIFF toggle must be accepted");

    auto stopped = makeContext(8.0, false);
    EventList stoppedOut;
    auto stoppedData = makeProcessData(stopped, stoppedOut, 64, &changes);
    require(processor.process(stoppedData) == kResultOk,
            "zero-valued NEW RIFF command must process");

    const auto second = capture(8.0);
    require(first != second,
            "zero-valued toggle transition must still generate a new riff");
}

void testHeavyIndustrialStaysLockedToHostGrid() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    // Begin at a real bar boundary that is not song zero: QN 4 = bar 2 in 4/4.
    ParameterChanges styleChanges;
    int32 qi = 0;
    auto* styleQueue = styleChanges.addParameterData(kStyleId, qi);
    int32 pi = 0;
    require(styleQueue && styleQueue->addPoint(0, 1.0, pi) == kResultOk,
            "Heavy Industrial style must be accepted");

    auto firstContext = makeContext(4.0, true);
    EventList firstOut;
    auto firstData = makeProcessData(firstContext, firstOut, 192000, &styleChanges);
    require(processor.process(firstData) == kResultOk,
            "Heavy Industrial full phrase capture must succeed");

    auto assertGrid = [](const EventList& out, double anchorQn) {
        int noteOns = 0;
        for (const auto& e : out.events) {
            if (e.type != Event::kNoteOnEvent || e.busIndex != kGuitarOutBus)
                continue;
            ++noteOns;
            const double steps = (e.ppqPosition - anchorQn) / 0.25;
            require(std::abs(steps - std::round(steps)) < 1e-8,
                    "every generated Heavy Industrial note-on must stay on the host 16th grid");
        }
        require(noteOns > 0, "grid test needs generated note-ons");
    };
    assertGrid(firstOut, 4.0);

    // Trigger NEW RIFF while transport continues at QN 12 (same 2-bar phrase boundary).
    ParameterChanges newChanges;
    qi = 0;
    auto* newQueue = newChanges.addParameterData(kNewRiffId, qi);
    pi = 0;
    require(newQueue && newQueue->addPoint(0, 1.0, pi) == kResultOk,
            "NEW RIFF toggle value must be accepted");

    auto secondContext = makeContext(12.0, true);
    EventList secondOut;
    auto secondData = makeProcessData(secondContext, secondOut, 192000, &newChanges);
    require(processor.process(secondData) == kResultOk,
            "running NEW RIFF capture must succeed");
    assertGrid(secondOut, 4.0);

    // A style change mid-run must also preserve the original host phase.
    ParameterChanges styleAgain;
    qi = 0;
    styleQueue = styleAgain.addParameterData(kStyleId, qi);
    pi = 0;
    require(styleQueue && styleQueue->addPoint(0, 0.0, pi) == kResultOk,
            "NDH style change must be accepted");

    auto thirdContext = makeContext(20.0, true);
    EventList thirdOut;
    auto thirdData = makeProcessData(thirdContext, thirdOut, 192000, &styleAgain);
    require(processor.process(thirdData) == kResultOk,
            "running style-change capture must succeed");
    assertGrid(thirdOut, 4.0);
}

void testRepeatedNewRiffStaysDistinct() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto capture = [&](double startQn) {
        auto context = makeContext(startQn, true);
        EventList output;
        auto data = makeProcessData(context, output, 192000);
        require(processor.process(data) == kResultOk, "repeated NEW capture must succeed");
        std::array<bool, 32> hits{};
        for (const auto& e : output.events) {
            if (e.type != Event::kNoteOnEvent || e.busIndex != kGuitarOutBus)
                continue;
            const int step = static_cast<int>(std::lround((e.ppqPosition - startQn) / 0.25));
            if (step >= 0 && step < 32)
                hits[static_cast<size_t>(step)] = true;
        }
        return hits;
    };

    auto previous = capture(0.0);
    double totalJaccard = 0.0;

    for (int round = 0; round < 6; ++round) {
        ParameterChanges changes;
        int32 queueIndex = 0;
        auto* q = changes.addParameterData(kNewRiffId, queueIndex);
        int32 pointIndex = 0;
        const double toggleValue = (round % 2 == 0) ? 1.0 : 0.0;
        require(q && q->addPoint(0, toggleValue, pointIndex) == kResultOk,
                "repeated NEW RIFF toggle must be accepted");

        auto stopped = makeContext(8.0 + round * 8.0, false);
        EventList stoppedOutput;
        auto stoppedData = makeProcessData(stopped, stoppedOutput, 64, &changes);
        require(processor.process(stoppedData) == kResultOk,
                "repeated NEW RIFF stop block must succeed");

        const double nextStart = 16.0 + round * 8.0;
        auto current = capture(nextStart);

        int intersection = 0;
        int unionCount = 0;
        for (size_t i = 0; i < previous.size(); ++i) {
            if (previous[i] || current[i]) ++unionCount;
            if (previous[i] && current[i]) ++intersection;
        }
        require(unionCount > 0, "repeated NEW comparison needs active onsets");
        const double jaccard = static_cast<double>(intersection) /
                               static_cast<double>(unionCount);
        require(jaccard <= 0.55,
                "every consecutive NEW RIFF must move to a clearly different groove");
        totalJaccard += jaccard;
        previous = current;
    }

    require(totalJaccard / 6.0 <= 0.48,
            "successive NEW RIFF clicks must have low average rhythm overlap");
}

void testBarsResizePreservesExistingRiff() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto capture = [&](double startQn, int samples, IParameterChanges* changes = nullptr) {
        auto context = makeContext(startQn, true);
        EventList output;
        auto data = makeProcessData(context, output, samples, changes);
        require(processor.process(data) == kResultOk, "bars resize capture must succeed");

        std::vector<std::pair<int, int>> notes;
        for (const auto& e : output.events) {
            if (e.type != Event::kNoteOnEvent || e.busIndex != kGuitarOutBus)
                continue;
            const int step = static_cast<int>(std::lround((e.ppqPosition - startQn) / 0.25));
            notes.emplace_back(step, e.noteOn.pitch);
        }
        return notes;
    };

    const auto original = capture(0.0, 192000); // default 2 bars

    ParameterChanges grow;
    int32 qi = 0;
    auto* q = grow.addParameterData(kBarsId, qi);
    int32 pi = 0;
    require(q && q->addPoint(0, 2.0 / 3.0, pi) == kResultOk,
            "4-bar value must be accepted");

    auto stopped = makeContext(8.0, false);
    EventList stoppedOut;
    auto stoppedData = makeProcessData(stopped, stoppedOut, 64, &grow);
    require(processor.process(stoppedData) == kResultOk, "4-bar resize must succeed");

    const auto extended = capture(8.0, 384000); // 4 bars
    require(!original.empty() && !extended.empty(), "bars resize fixture needs generated notes");

    // First two bars must be bit-for-bit the same note-on pattern.
    std::vector<std::pair<int, int>> extendedFirstHalf;
    std::vector<std::pair<int, int>> extendedSecondHalf;
    for (const auto& n : extended) {
        if (n.first < 32)
            extendedFirstHalf.push_back(n);
        else
            extendedSecondHalf.emplace_back(n.first - 32, n.second);
    }
    require(original == extendedFirstHalf,
            "extending 2 to 4 bars must preserve the existing riff exactly");
    require(original == extendedSecondHalf,
            "extending 2 to 4 bars must tile the existing riff instead of composing a new one");

    ParameterChanges shrink;
    qi = 0;
    q = shrink.addParameterData(kBarsId, qi);
    pi = 0;
    require(q && q->addPoint(0, 1.0 / 3.0, pi) == kResultOk,
            "2-bar value must be accepted");

    auto stopped2 = makeContext(24.0, false);
    EventList stoppedOut2;
    auto stoppedData2 = makeProcessData(stopped2, stoppedOut2, 64, &shrink);
    require(processor.process(stoppedData2) == kResultOk, "2-bar shrink must succeed");

    const auto shortened = capture(24.0, 192000);
    require(shortened == original,
            "shrinking back to 2 bars must restore the unchanged original riff span");
}

void testPowerChordTogglePreservesRiffOnsetsAndPitches() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto capturePrimary = [&](double startQn, IParameterChanges* changes = nullptr) {
        auto context = makeContext(startQn, true);
        EventList output;
        auto data = makeProcessData(context, output, 192000, changes);
        require(processor.process(data) == kResultOk, "power-chord capture must succeed");

        std::vector<std::pair<int, int>> primary;
        std::vector<double> seenTimes;
        for (const auto& e : output.events) {
            if (e.type != Event::kNoteOnEvent || e.busIndex != kGuitarOutBus)
                continue;
            const int step = static_cast<int>(std::lround((e.ppqPosition - startQn) / 0.25));
            bool already = false;
            for (double t : seenTimes)
                if (std::abs(t - e.ppqPosition) < 1e-9) { already = true; break; }
            if (!already) {
                primary.emplace_back(step, e.noteOn.pitch);
                seenTimes.push_back(e.ppqPosition);
            }
        }
        return primary;
    };

    const auto before = capturePrimary(0.0);

    ParameterChanges off;
    int32 qi = 0, pi = 0;
    auto* q = off.addParameterData(kPowerChordsEnabledId, qi);
    require(q && q->addPoint(0, 0.0, pi) == kResultOk, "Power Chords OFF must be accepted");
    auto stopped = makeContext(8.0, false);
    EventList stoppedOut;
    auto stoppedData = makeProcessData(stopped, stoppedOut, 64, &off);
    require(processor.process(stoppedData) == kResultOk, "Power Chords OFF process must succeed");

    const auto afterOff = capturePrimary(8.0);
    require(before == afterOff,
            "Power Chords OFF must preserve the existing riff's primary onset/pitch pattern");

    ParameterChanges on;
    qi = 0; pi = 0;
    q = on.addParameterData(kPowerChordsEnabledId, qi);
    require(q && q->addPoint(0, 1.0, pi) == kResultOk, "Power Chords ON must be accepted");
    auto stopped2 = makeContext(16.0, false);
    EventList stoppedOut2;
    auto stoppedData2 = makeProcessData(stopped2, stoppedOut2, 64, &on);
    require(processor.process(stoppedData2) == kResultOk, "Power Chords ON process must succeed");

    const auto afterOn = capturePrimary(16.0);
    require(before == afterOn,
            "Power Chords ON must preserve the existing riff's primary onset/pitch pattern");
}

void testLargeOfflineBlockKeepsNoteEventsBalanced() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    auto context = makeContext(0.0, true);
    EventList output;
    auto data = makeProcessData(context, output, 1000000);
    data.processMode = kOffline;
    require(processor.process(data) == kResultOk, "large offline block must process");

    std::array<std::array<int, 128>, kEventOutputBusCount> balance{};
    std::array<int, kEventOutputBusCount> noteOns{};
    std::array<int, kEventOutputBusCount> noteOffs{};

    for (const auto& e : output.events) {
        require(e.busIndex >= 0 && e.busIndex < kEventOutputBusCount,
                "offline scheduler must never emit an invalid bus index");

        const auto bus = static_cast<size_t>(e.busIndex);
        if (e.type == Event::kNoteOnEvent) {
            ++balance[bus][static_cast<size_t>(e.noteOn.pitch)];
            ++noteOns[bus];
        } else if (e.type == Event::kNoteOffEvent) {
            --balance[bus][static_cast<size_t>(e.noteOff.pitch)];
            ++noteOffs[bus];
        }
    }

    for (int bus : {kGuitarOutBus, kBassOutBus, kDrumsOutBus, kPadOutBus, kSynthOutBus}) {
        require(noteOns[static_cast<size_t>(bus)] > 20,
                "large offline fixture must exercise every active instrument bus");
        require(noteOffs[static_cast<size_t>(bus)] > 20,
                "large offline fixture must emit note-offs on every active instrument bus");
        for (int v : balance[static_cast<size_t>(bus)])
            require(std::abs(v) <= 1,
                    "large offline scheduling must not silently lose per-bus note-on/off pairs");
    }


}

void testStopFlushesEveryActiveInstrumentBus() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    // Use a tiny running block at the downbeat so Guitar, Bass, Drums, Pads and Synth all
    // have active notes whose scheduled NoteOff lies outside this block.
    auto runningContext = makeContext(0.0, true);
    EventList running;
    auto runningData = makeProcessData(runningContext, running, 64);
    require(processor.process(runningData) == kResultOk,
            "active-bus flush setup must process");

    std::array<bool, kEventOutputBusCount> hadOn{};
    for (const auto& e : running.events)
        if (e.type == Event::kNoteOnEvent &&
            e.busIndex >= 0 && e.busIndex < kEventOutputBusCount)
            hadOn[static_cast<size_t>(e.busIndex)] = true;

    require(hadOn[kGuitarOutBus], "flush fixture must hold a Guitar note");
    require(hadOn[kBassOutBus], "flush fixture must hold a Bass note");
    require(hadOn[kDrumsOutBus], "flush fixture must hold a Drum note");
    require(hadOn[kPadOutBus], "flush fixture must hold a Pad note");
    require(hadOn[kSynthOutBus], "flush fixture must hold a Synth note");

    auto stoppedContext = makeContext(64.0 / 24000.0, false);
    EventList stopped;
    auto stoppedData = makeProcessData(stoppedContext, stopped, 64);
    require(processor.process(stoppedData) == kResultOk,
            "transport stop must flush active buses");

    std::array<bool, kEventOutputBusCount> flushed{};
    for (const auto& e : stopped.events)
        if (e.type == Event::kNoteOffEvent &&
            e.busIndex >= 0 && e.busIndex < kEventOutputBusCount)
            flushed[static_cast<size_t>(e.busIndex)] = true;

    require(flushed[kGuitarOutBus], "transport stop must flush Guitar Out");
    require(flushed[kBassOutBus], "transport stop must flush Bass Out");
    require(flushed[kDrumsOutBus], "transport stop must flush Drums Out");
    require(flushed[kPadOutBus], "transport stop must flush Pad Out");
    require(flushed[kSynthOutBus], "transport stop must flush Synth Out");
}

void testHugeOfflineBlockChunksSchedulerSafely() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk,
            "huge offline scheduler fixture must start");

    // Use a one-bar dense phrase so this block emits far more than the old
    // fixed 8192-event whole-block staging limit.
    ParameterChanges setup;
    int32 qi = 0;
    auto* bars = setup.addParameterData(kBarsId, qi);
    int32 pi = 0;
    require(bars && bars->addPoint(0, 0.0, pi) == kResultOk,
            "one-bar setup must be accepted");
    auto* density = setup.addParameterData(kDensityId, qi);
    require(density && density->addPoint(0, 1.0, pi) == kResultOk,
            "dense setup must be accepted");
    auto* complexity = setup.addParameterData(kComplexityId, qi);
    require(complexity && complexity->addPoint(0, 1.0, pi) == kResultOk,
            "complex setup must be accepted");

    auto stopped = makeContext(0.0, false);
    EventList stoppedOutput;
    auto stoppedData = makeProcessData(stopped, stoppedOutput, 64, &setup);
    require(processor.process(stoppedData) == kResultOk,
            "huge offline setup block must succeed");

    auto context = makeContext(0.0, true);
    EventList output;
    auto data = makeProcessData(context, output, 50000000);
    data.processMode = kOffline;

    require(processor.process(data) == kResultOk,
            "huge offline block must be chunked instead of overflowing");
    require(output.events.size() > 8192,
            "huge offline fixture must exceed the former whole-block staging capacity");

    std::array<long long, kEventOutputBusCount> ons{};
    std::array<long long, kEventOutputBusCount> offs{};
    for (const auto& e : output.events) {
        require(e.busIndex >= 0 && e.busIndex < kEventOutputBusCount,
                "chunked scheduler must emit only valid role buses");
        if (e.type == Event::kNoteOnEvent)
            ++ons[static_cast<size_t>(e.busIndex)];
        else if (e.type == Event::kNoteOffEvent)
            ++offs[static_cast<size_t>(e.busIndex)];
    }

    for (int bus = 0; bus < kEventOutputBusCount; ++bus) {
        require(ons[static_cast<size_t>(bus)] > 0,
                "huge offline block must emit every musical role");
        require(std::llabs(ons[static_cast<size_t>(bus)] -
                           offs[static_cast<size_t>(bus)]) <= 128,
                "chunked huge-block note balance must remain bounded by active pitches");
    }

    auto endContext = makeContext(
        static_cast<double>(data.numSamples) * 120.0 / (60.0 * 48000.0), false);
    EventList endOutput;
    auto endData = makeProcessData(endContext, endOutput, 64);
    require(processor.process(endData) == kResultOk,
            "stop after huge offline block must succeed");
}

void testGeneratedNoteLengthsAffectMidiOutput() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    ParameterChanges setup;
    int32 qi = 0, pi = 0;
    auto* density = setup.addParameterData(kDensityId, qi);
    require(density && density->addPoint(0, 0.12, pi) == kResultOk,
            "low Density setup must be accepted");
    auto* palm = setup.addParameterData(kPalmMuteId, qi);
    require(palm && palm->addPoint(0, 0.0, pi) == kResultOk,
            "Palm Mute OFF setup must be accepted");

    auto stopped = makeContext(0.0, false);
    EventList stoppedOut;
    auto stoppedData = makeProcessData(stopped, stoppedOut, 64, &setup);
    require(processor.process(stoppedData) == kResultOk,
            "note-length setup block must succeed");

    bool foundShort = false;
    bool foundLong = false;

    for (int round = 0; round < 12 && !foundLong; ++round) {
        ParameterChanges command;
        qi = 0; pi = 0;
        auto* q = command.addParameterData(kNewRiffId, qi);
        require(q && q->addPoint(0, (round & 1) ? 0.0 : 1.0, pi) == kResultOk,
                "NEW RIFF command must be accepted");

        const double startQn = static_cast<double>(round) * 8.0;
        auto stopContext = makeContext(startQn, false);
        EventList stopOutput;
        auto stopData = makeProcessData(stopContext, stopOutput, 64, &command);
        require(processor.process(stopData) == kResultOk,
                "NEW RIFF note-length setup must succeed");

        auto context = makeContext(startQn, true);
        EventList output;
        auto data = makeProcessData(context, output, 192000);
        require(processor.process(data) == kResultOk,
                "note-length scheduling process must succeed");

        for (const auto& on : output.events) {
            if (on.type != Event::kNoteOnEvent || on.busIndex != kGuitarOutBus)
                continue;

            double bestOff = -1.0;
            for (const auto& off : output.events) {
                if (off.type != Event::kNoteOffEvent ||
                    off.busIndex != kGuitarOutBus ||
                    off.noteOff.pitch != on.noteOn.pitch)
                    continue;
                if (off.ppqPosition + 1e-9 < on.ppqPosition)
                    continue;
                if (bestOff < 0.0 || off.ppqPosition < bestOff)
                    bestOff = off.ppqPosition;
            }
            if (bestOff < 0.0)
                continue;

            const double duration = bestOff - on.ppqPosition;
            if (std::abs(duration - (0.25 - 1.0 / 16.0)) < 1e-8)
                foundShort = true;
            if (duration > (0.25 - 1.0 / 16.0) + 1e-8)
                foundLong = true;
        }
    }

    require(foundShort, "generated one-step notes must produce short MIDI durations");
    require(foundLong, "generated longer notes must produce audibly longer MIDI durations");
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

void testTransportUsesHostBarGrid() {
    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk, "processor must start");

    // Bar 2 starts at QN 4 in 4/4. Starting exactly there must begin at step 0.
    auto barContext = makeContext(4.0, true);
    EventList barOutput;
    auto barData = makeProcessData(barContext, barOutput, 100);
    require(processor.process(barData) == kResultOk,
            "bar-boundary transport start must succeed");

    bool foundBarDownbeat = false;
    for (const auto& event : barOutput.events) {
        if (event.type == Event::kNoteOnEvent &&
            std::abs(event.ppqPosition - 4.0) < 1e-9) {
            foundBarDownbeat = true;
            break;
        }
    }
    require(foundBarDownbeat,
            "starting exactly on a host bar boundary must emit phrase step zero");

    // Restart mid-bar at QN 5.25 (one quarter + one 16th into bar 2).
    // The phrase must preserve that host-grid phase instead of treating 5.25
    // as a new arbitrary step zero.
    require(processor.setProcessing(false) == kResultOk, "processor must stop");
    require(processor.setProcessing(true) == kResultOk, "processor must restart");

    auto midContext = makeContext(5.25, true);
    EventList midOutput;
    auto midData = makeProcessData(midContext, midOutput, 12000); // half a quarter note
    require(processor.process(midData) == kResultOk,
            "mid-bar transport start must succeed");

    for (const auto& event : midOutput.events) {
        if (event.type != Event::kNoteOnEvent)
            continue;
        const double hostSteps = (event.ppqPosition - 4.0) / 0.25;
        require(std::abs(hostSteps - std::round(hostSteps)) < 1e-8,
                "mid-bar start must remain locked to the host 16th-note grid");
    }
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



std::vector<std::pair<int,int>> captureGuitarPhrase(MidiatorProcessor& processor,
                                                    double startQn) {
    auto context = makeContext(startQn, true);
    EventList output;
    auto data = makeProcessData(context, output, 192000);
    require(processor.process(data) == kResultOk,
            "burst-coalescing phrase capture must succeed");

    std::vector<std::pair<int,int>> notes;
    for (const auto& e : output.events) {
        if (e.type != Event::kNoteOnEvent || e.busIndex != kGuitarOutBus)
            continue;
        const int step = static_cast<int>(
            std::lround((e.ppqPosition - startQn) / 0.25));
        notes.emplace_back(step, e.noteOn.pitch);
    }
    return notes;
}

void consumeStoppedBlock(MidiatorProcessor& processor, double qn) {
    auto context = makeContext(qn, false);
    EventList output;
    auto data = makeProcessData(context, output, 64);
    require(processor.process(data) == kResultOk,
            "burst-coalescing stopped block must succeed");
}

void testGuiMessageBurstsAreCoalescedPerBlock() {
    MidiatorProcessor singleNew;
    MidiatorProcessor burstNew;
    require(singleNew.setProcessing(true) == kResultOk &&
            burstNew.setProcessing(true) == kResultOk,
            "NEW RIFF burst fixtures must start");

    TestMessage newMessage("125A.Midiator.NewRiff");
    require(singleNew.notify(&newMessage) == kResultOk,
            "single NEW RIFF message must be accepted");
    for (int i = 0; i < 10; ++i)
        require(burstNew.notify(&newMessage) == kResultOk,
                "burst NEW RIFF message must be accepted");

    consumeStoppedBlock(singleNew, 0.0);
    consumeStoppedBlock(burstNew, 0.0);
    require(captureGuitarPhrase(singleNew, 0.0) ==
            captureGuitarPhrase(burstNew, 0.0),
            "same-block NEW RIFF message bursts must coalesce to one composition");

    MidiatorProcessor singleVariation;
    MidiatorProcessor burstVariation;
    require(singleVariation.setProcessing(true) == kResultOk &&
            burstVariation.setProcessing(true) == kResultOk,
            "VARIATION burst fixtures must start");

    TestMessage variationMessage("125A.Midiator.Variation");
    require(singleVariation.notify(&variationMessage) == kResultOk,
            "single VARIATION message must be accepted");
    for (int i = 0; i < 10; ++i)
        require(burstVariation.notify(&variationMessage) == kResultOk,
                "burst VARIATION message must be accepted");

    consumeStoppedBlock(singleVariation, 0.0);
    consumeStoppedBlock(burstVariation, 0.0);
    require(captureGuitarPhrase(singleVariation, 0.0) ==
            captureGuitarPhrase(burstVariation, 0.0),
            "same-block VARIATION message bursts must coalesce to one composition");
}



void testParameterQueueOrderCannotChangeCompositionCommands() {
    auto runCase = [](bool actionFirst, bool variation) {
        MidiatorProcessor processor;
        require(processor.setProcessing(true) == kResultOk,
                "queue-order fixture must start");

        ParameterChanges changes;
        int32 qi = 0;
        int32 pi = 0;

        auto addAction = [&]() {
            const ParamID id = variation ? kVariationId : kNewRiffId;
            auto* q = changes.addParameterData(id, qi);
            require(q && q->addPoint(0, 1.0, pi) == kResultOk,
                    "queue-order action must be accepted");
        };
        auto addStyle = [&]() {
            auto* q = changes.addParameterData(kStyleId, qi);
            require(q && q->addPoint(0, 1.0, pi) == kResultOk,
                    "queue-order Heavy Industrial style must be accepted");
        };
        auto addScale = [&]() {
            auto* q = changes.addParameterData(kScaleId, qi);
            // Harmonic Minor = index 3 of 7 -> normalized 0.5.
            require(q && q->addPoint(0, 0.5, pi) == kResultOk,
                    "queue-order scale must be accepted");
        };

        if (actionFirst) {
            addAction();
            addStyle();
            addScale();
        } else {
            addStyle();
            addScale();
            addAction();
        }

        auto stopped = makeContext(0.0, false);
        EventList stoppedOut;
        auto stoppedData = makeProcessData(stopped, stoppedOut, 64, &changes);
        require(processor.process(stoppedData) == kResultOk,
                "queue-order command block must succeed");

        return captureGuitarPhrase(processor, 0.0);
    };

    require(runCase(true, false) == runCase(false, false),
            "NEW RIFF result must not depend on VST3 parameter queue order");
    require(runCase(true, true) == runCase(false, true),
            "VARIATION result must not depend on VST3 parameter queue order");
}



void testRoleSpecificGateRules() {
    constexpr double kSixteenthQn = 0.25;
    constexpr double kSixtyFourthQn = 1.0 / 16.0;
    constexpr double eps = 1e-8;

    MidiatorProcessor processor;
    require(processor.setProcessing(true) == kResultOk,
            "gate-rule fixture must start");

    bool sawDrum = false;
    bool sawGuitarBoundaryGap = false;
    bool sawBassBoundaryGap = false;
    bool sawPadBoundaryGap = false;
    bool sawSynthLegatoBoundary = false;

    for (int round = 0; round < 16 &&
         !(sawDrum && sawGuitarBoundaryGap && sawBassBoundaryGap &&
           sawPadBoundaryGap && sawSynthLegatoBoundary); ++round) {
        ParameterChanges command;
        int32 qi = 0, pi = 0;
        auto* q = command.addParameterData(kNewRiffId, qi);
        require(q && q->addPoint(0, (round & 1) ? 0.0 : 1.0, pi) == kResultOk,
                "gate-rule NEW RIFF command must be accepted");

        const double startQn = static_cast<double>(round) * 8.0;
        auto stopped = makeContext(startQn, false);
        EventList stoppedOut;
        auto stoppedData = makeProcessData(stopped, stoppedOut, 64, &command);
        require(processor.process(stoppedData) == kResultOk,
                "gate-rule setup block must succeed");

        auto context = makeContext(startQn, true);
        EventList output;
        auto data = makeProcessData(context, output, 192000);
        require(processor.process(data) == kResultOk,
                "gate-rule capture must succeed");

        auto earliestMatchingOff = [&](int bus, int pitch, double onQn) {
            double best = -1.0;
            for (const auto& e : output.events) {
                if (e.type != Event::kNoteOffEvent || e.busIndex != bus ||
                    e.noteOff.pitch != pitch || e.ppqPosition + eps < onQn)
                    continue;
                if (best < 0.0 || e.ppqPosition < best)
                    best = e.ppqPosition;
            }
            return best;
        };

        // Every drum NoteOn must have a matching NoteOff exactly one
        // 16th later. Do not pair by "earliest same-pitch NoteOff", because
        // repeated hits can legitimately place the previous NoteOff at the
        // same timestamp as the next NoteOn.
        for (const auto& e : output.events) {
            if (e.type != Event::kNoteOnEvent || e.busIndex != kDrumsOutBus)
                continue;
            const double expectedOff = e.ppqPosition + kSixteenthQn;
            const double blockEndQn =
                startQn + static_cast<double>(data.numSamples) *
                context.tempo / (60.0 * context.sampleRate);
            if (expectedOff >= blockEndQn - eps)
                continue;

            bool foundExactOff = false;
            for (const auto& off : output.events) {
                if (off.type == Event::kNoteOffEvent &&
                    off.busIndex == kDrumsOutBus &&
                    off.noteOff.pitch == e.noteOn.pitch &&
                    std::abs(off.ppqPosition - expectedOff) < eps) {
                    foundExactOff = true;
                    break;
                }
            }
            require(foundExactOff,
                    "every Drum note must be exactly one 16th note long");
            sawDrum = true;
        }

        auto checkGapRole = [&](int bus, bool& sawValidGap, bool requireExactBoundary) {
            std::vector<double> onsets;
            for (const auto& e : output.events) {
                if (e.type == Event::kNoteOnEvent && e.busIndex == bus) {
                    if (onsets.empty() || std::abs(onsets.back() - e.ppqPosition) > eps)
                        onsets.push_back(e.ppqPosition);
                }
            }
            for (size_t i = 1; i < onsets.size(); ++i) {
                const double nextOn = onsets[i];
                for (const auto& e : output.events) {
                    if (e.type != Event::kNoteOffEvent || e.busIndex != bus)
                        continue;
                    if (e.ppqPosition > onsets[i - 1] + eps &&
                        e.ppqPosition <= nextOn + eps) {
                        require(e.ppqPosition <= nextOn - kSixtyFourthQn + eps,
                                "Guitar/Bass/Pad must leave at least a 64th-note gap");
                        if (!requireExactBoundary ||
                            std::abs((nextOn - e.ppqPosition) - kSixtyFourthQn) < eps)
                            sawValidGap = true;
                    }
                }
            }
        };

        checkGapRole(kGuitarOutBus, sawGuitarBoundaryGap, true);
        checkGapRole(kBassOutBus, sawBassBoundaryGap, true);
        checkGapRole(kPadOutBus, sawPadBoundaryGap, false);

        // Synth is deliberately different: a generated note may end exactly
        // at the next monophonic onset, with NoteOff sorted before NoteOn.
        std::vector<double> synthOnsets;
        for (const auto& e : output.events)
            if (e.type == Event::kNoteOnEvent && e.busIndex == kSynthOutBus)
                synthOnsets.push_back(e.ppqPosition);

        for (size_t i = 1; i < synthOnsets.size(); ++i) {
            const double nextOn = synthOnsets[i];
            for (const auto& e : output.events) {
                if (e.type == Event::kNoteOffEvent &&
                    e.busIndex == kSynthOutBus &&
                    std::abs(e.ppqPosition - nextOn) < eps) {
                    sawSynthLegatoBoundary = true;
                    break;
                }
            }
        }
    }

    require(sawDrum, "gate-rule fixture must observe Drum events");
    require(sawGuitarBoundaryGap,
            "Guitar must demonstrate the configured 64th-note release gap");
    require(sawBassBoundaryGap,
            "Bass must demonstrate the configured 64th-note release gap");
    require(sawPadBoundaryGap,
            "Pad must demonstrate a release gap of at least one 64th note");
    require(sawSynthLegatoBoundary,
            "Synth must demonstrate independent legato-capable scheduling");
}

} // namespace

int main() {
    testAllInstrumentRolesUseSeparateOutputBuses();
    testDedicatedInstrumentOutputBuses();
    testNewRiffChangesRhythmMask();
    testNewRiffToggleZeroValueStillCommands();
    testHeavyIndustrialStaysLockedToHostGrid();
    testRepeatedNewRiffStaysDistinct();
    testBarsResizePreservesExistingRiff();
    testPowerChordTogglePreservesRiffOnsetsAndPitches();
    testLargeOfflineBlockKeepsNoteEventsBalanced();
    testStopFlushesEveryActiveInstrumentBus();
    testHugeOfflineBlockChunksSchedulerSafely();
    testGeneratedNoteLengthsAffectMidiOutput();
    testRoleSpecificGateRules();
    testMidiRootSourceTransposesRiff();
    testManualRootSourceIgnoresMidiRootNotes();
    testTransportUsesHostBarGrid();
    testLiveDownbeatAndStopFlush();
    testNewRiffFlushesHeldNotes();
    testVariationPressReleaseFlushesHeldNotes();
    testTransportJumpFlushesHeldNotes();
    testGuiMessageBurstsAreCoalescedPerBlock();
    testParameterQueueOrderCannotChangeCompositionCommands();

    std::cout << "Midiator live processor tests: PASS\n";
    return 0;
}
