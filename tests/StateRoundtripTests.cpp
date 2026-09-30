#include "PluginProcessor.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/funknown.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Steinberg SDK's Windows support library expects the module handle symbol
// normally provided by dllmain.cpp in a VST3 module. The state test is a
// console executable, so a null stub is sufficient.
void* moduleHandle = nullptr;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

class MemoryStream final : public IBStream {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) SMTG_OVERRIDE {
        if (!obj)
            return kInvalidArgument;

        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(iid, IBStream::iid)) {
            *obj = static_cast<IBStream*>(this);
            return kResultOk;
        }

        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() SMTG_OVERRIDE { return 1; }
    uint32 PLUGIN_API release() SMTG_OVERRIDE { return 1; }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) SMTG_OVERRIDE {
        if (!buffer || numBytes < 0)
            return kInvalidArgument;

        const auto available = position_ < bytes_.size()
            ? bytes_.size() - position_
            : size_t{0};
        const auto wanted = static_cast<size_t>(numBytes);
        const auto count = std::min(available, wanted);

        if (count > 0)
            std::memcpy(buffer, bytes_.data() + position_, count);

        position_ += count;
        if (numBytesRead)
            *numBytesRead = static_cast<int32>(count);
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) SMTG_OVERRIDE {
        if ((!buffer && numBytes > 0) || numBytes < 0)
            return kInvalidArgument;

        const auto count = static_cast<size_t>(numBytes);
        const auto end = position_ + count;
        if (end > bytes_.size())
            bytes_.resize(end);

        if (count > 0)
            std::memcpy(bytes_.data() + position_, buffer, count);

        position_ = end;
        if (numBytesWritten)
            *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) SMTG_OVERRIDE {
        int64 base = 0;
        if (mode == kIBSeekCur)
            base = static_cast<int64>(position_);
        else if (mode == kIBSeekEnd)
            base = static_cast<int64>(bytes_.size());
        else if (mode != kIBSeekSet)
            return kInvalidArgument;

        const int64 next = base + pos;
        if (next < 0)
            return kInvalidArgument;

        position_ = static_cast<size_t>(next);
        if (result)
            *result = next;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) SMTG_OVERRIDE {
        if (!pos)
            return kInvalidArgument;
        *pos = static_cast<int64>(position_);
        return kResultOk;
    }

    void rewind() noexcept { position_ = 0; }

    std::vector<uint8_t>& bytes() noexcept { return bytes_; }
    const std::vector<uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<uint8_t> bytes_{};
    size_t position_ = 0;
};


template <typename T>
void appendFixtureValue(MemoryStream& stream, const T& value) {
    const auto oldSize = stream.bytes().size();
    stream.bytes().resize(oldSize + sizeof(T));
    std::memcpy(stream.bytes().data() + oldSize, &value, sizeof(T));
}

MemoryStream makeLegacyStateFixture(uint32_t version,
                                    int32 root,
                                    int32 manualRoot,
                                    bool midiRootSource,
                                    int32 style) {
    MemoryStream stream;
    constexpr uint32_t magic = 0x4D445231u; // MDR1

    const int32 scale = static_cast<int32>(midiator::ScaleId::Phrygian);
    const int32 bars = 2;
    const float density = 0.56f;
    const float complexity = 0.42f;
    const float repetition = 0.72f;
    const float powerChordChance = 0.25f;
    const float palmMuteChance = 0.70f;
    const float variationAmount = 0.35f;
    const uint32_t seed = 0x10203040u;

    appendFixtureValue(stream, magic);
    appendFixtureValue(stream, version);
    appendFixtureValue(stream, root);
    appendFixtureValue(stream, scale);
    appendFixtureValue(stream, bars);
    appendFixtureValue(stream, density);
    appendFixtureValue(stream, complexity);
    appendFixtureValue(stream, repetition);
    appendFixtureValue(stream, powerChordChance);
    appendFixtureValue(stream, palmMuteChance);
    appendFixtureValue(stream, variationAmount);
    appendFixtureValue(stream, seed);

    if (version >= 2u) {
        const int32 source = midiRootSource ? 1 : 0;
        appendFixtureValue(stream, manualRoot);
        appendFixtureValue(stream, source);
    }
    if (version >= 3u)
        appendFixtureValue(stream, style);

    // Legacy versions predate the V4 power-chord enabled flag.
    // The phrase payload format itself is already the fixed 128-step layout.
    const int32 phraseBars = 2;
    appendFixtureValue(stream, phraseBars);
    for (int i = 0; i < midiator::kMaxSteps; ++i) {
        const int32 noteCount = (i == 0) ? 1 : 0;
        appendFixtureValue(stream, noteCount);
        for (int n = 0; n < midiator::kMaxNotesPerStep; ++n) {
            const int32 pitch = (i == 0 && n == 0) ? 45 : 0;
            const int32 velocity = (i == 0 && n == 0) ? 90 : 0;
            const int32 lengthSteps = 1;
            appendFixtureValue(stream, pitch);
            appendFixtureValue(stream, velocity);
            appendFixtureValue(stream, lengthSteps);
        }
    }

    stream.rewind();
    return stream;
}

void verifyLegacyControllerMigration(uint32_t version,
                                     int32 root,
                                     int32 manualRoot,
                                     bool midiRootSource,
                                     int32 style,
                                     double expectedRootNormalized,
                                     double expectedSourceNormalized,
                                     double expectedStyleNormalized) {
    auto fixture = makeLegacyStateFixture(
        version, root, manualRoot, midiRootSource, style);

    MidiatorController controller;
    require(controller.initialize(nullptr) == kResultOk,
            "legacy migration controller must initialize");
    require(controller.setComponentState(&fixture) == kResultOk,
            "legacy state must migrate through controller");

    require(std::abs(controller.getParamNormalized(kRootId) - expectedRootNormalized) < 1e-9,
            "legacy root migration must preserve intended manual root");
    require(std::abs(controller.getParamNormalized(kRootSourceId) - expectedSourceNormalized) < 1e-9,
            "legacy root-source migration must preserve/default correctly");
    require(std::abs(controller.getParamNormalized(kStyleId) - expectedStyleNormalized) < 1e-9,
            "legacy style migration must preserve/default correctly");
    require(controller.getParamNormalized(kPowerChordsEnabledId) > 0.999,
            "legacy states must default Power Chords Enabled to ON");

    fixture.rewind();
    MidiatorProcessor processor;
    require(processor.setState(&fixture) == kResultOk,
            "legacy state must migrate through processor");

    MemoryStream migrated;
    require(processor.getState(&migrated) == kResultOk,
            "migrated legacy state must serialize as current state");
    require(migrated.bytes().size() > fixture.bytes().size(),
            "current V4 state must include fields absent from legacy fixture");
}

} // namespace

int main() {
    MidiatorController controller;
    require(controller.initialize(nullptr) == kResultOk,
            "controller must initialize for parameter-contract test");

    bool foundNewRiff = false;
    bool foundVariation = false;
    for (int32 i = 0; i < controller.getParameterCount(); ++i) {
        ParameterInfo info{};
        require(controller.getParameterInfo(i, info) == kResultOk,
                "controller parameter info must be readable");

        if (info.id == kNewRiffId) {
            foundNewRiff = true;
            require(info.stepCount == 1,
                    "NEW RIFF must be a two-state momentary parameter");
            require((info.flags & ParameterInfo::kCanAutomate) != 0,
                    "NEW RIFF must be host-visible so GUI edits reach the processor");
        }
        if (info.id == kVariationId) {
            foundVariation = true;
            require(info.stepCount == 1,
                    "VARIATION must be a two-state momentary parameter");
            require((info.flags & ParameterInfo::kCanAutomate) != 0,
                    "VARIATION must be host-visible so GUI edits reach the processor");
        }
    }
    require(foundNewRiff && foundVariation,
            "controller must expose both action parameters");

    MidiatorProcessor original;

    require(original.setProcessing(true) == kResultOk,
            "setProcessing(true) must be implemented");
    require(original.setProcessing(false) == kResultOk,
            "setProcessing(false) must be implemented");

    require(original.canProcessSampleSize(kSample32) == kResultTrue,
            "32-bit sample mode must be supported");
    require(original.canProcessSampleSize(kSample64) == kResultTrue,
            "64-bit sample mode must be supported by this MIDI-only processor");

    const auto contextFlags = original.getProcessContextRequirements();
    require((contextFlags & IProcessContextRequirements::kNeedProjectTimeMusic) != 0,
            "process context requirements must request musical project time");
    require((contextFlags & IProcessContextRequirements::kNeedBarPositionMusic) != 0,
            "process context requirements must request host bar position");
    require((contextFlags & IProcessContextRequirements::kNeedTimeSignature) != 0,
            "process context requirements must request host time signature");
    require((contextFlags & IProcessContextRequirements::kNeedTempo) != 0,
            "process context requirements must request tempo");
    require((contextFlags & IProcessContextRequirements::kNeedTransportState) != 0,
            "process context requirements must request transport state");

    MemoryStream first;
    require(original.getState(&first) == kResultOk,
            "processor must serialize its complete state");
    require(first.bytes().size() > 100,
            "serialized state must contain more than a trivial parameter header");

    MidiatorProcessor restored;
    first.rewind();
    require(restored.setState(&first) == kResultOk,
            "processor must restore a previously serialized state");

    MemoryStream second;
    require(restored.getState(&second) == kResultOk,
            "restored processor must serialize again");
    require(first.bytes() == second.bytes(),
            "state roundtrip must be byte-identical, including generated phrase data");

    MemoryStream corrupted;
    corrupted.bytes() = first.bytes();
    require(!corrupted.bytes().empty(), "corruption fixture must contain state bytes");
    corrupted.bytes()[0] ^= 0xFFu;
    corrupted.rewind();

    MidiatorProcessor rejectTarget;
    require(rejectTarget.setState(&corrupted) != kResultOk,
            "corrupted state magic must be rejected");

    MemoryStream truncated;
    truncated.bytes() = first.bytes();
    truncated.bytes().resize(truncated.bytes().size() / 2);
    truncated.rewind();

    MidiatorProcessor truncatedTarget;
    require(truncatedTarget.setState(&truncated) != kResultOk,
            "truncated state must be rejected");


    // Frozen legacy byte layouts verify the actual V1/V2/V3 migration contract.
    // V1: one fixed root -> Manual source, NDH style, Power Chords enabled.
    verifyLegacyControllerMigration(
        1u, 4, 4, false, 0,
        4.0 / 11.0, 0.0, 0.0);

    // V2: explicit manual root + MIDI source, style still defaults to NDH.
    verifyLegacyControllerMigration(
        2u, 7, 2, true, 0,
        2.0 / 11.0, 1.0, 0.0);

    // V3: explicit style arrives; Power Chords enabled still defaults ON.
    verifyLegacyControllerMigration(
        3u, 9, 5, false, static_cast<int32>(midiator::StyleId::HeavyIndustrial),
        5.0 / 11.0, 0.0, 1.0);

    std::cout << "Midiator processor-state roundtrip test: PASS\n";
    return 0;
}
