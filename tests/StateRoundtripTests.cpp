#include "PluginProcessor.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/funknown.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Steinberg SDK's Windows support library expects the module handle symbol
// normally provided by dllmain.cpp in a VST3 module. The state test is a
// console executable, so a null stub is sufficient.
void* moduleHandle = nullptr;

namespace {

constexpr int kLegacyStateSteps = 128;

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
void patchFixtureValue(MemoryStream& stream, size_t offset, const T& value) {
    require(offset + sizeof(T) <= stream.bytes().size(),
            "state patch offset must stay inside serialized state");
    std::memcpy(stream.bytes().data() + offset, &value, sizeof(T));
}

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
                                    int32 style,
                                    bool powerChordsEnabled = true) {
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
    if (version >= 4u) {
        const int32 enabled = powerChordsEnabled ? 1 : 0;
        appendFixtureValue(stream, enabled);
    }

    // V1-V4 stored only the Guitar phrase in the fixed 128-step layout.
    const int32 phraseBars = 2;
    appendFixtureValue(stream, phraseBars);
    for (int i = 0; i < kLegacyStateSteps; ++i) {
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
                                     double expectedStyleNormalized,
                                     bool expectedPowerChordsEnabled = true) {
    auto fixture = makeLegacyStateFixture(
        version, root, manualRoot, midiRootSource, style, expectedPowerChordsEnabled);

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
            "legacy style migration must preserve the original 3-choice normalization");
    const double expectedMetalStyle =
        static_cast<double>(std::clamp<int32>(style, 0, 2)) /
        static_cast<double>(static_cast<int>(midiator::StyleId::Count) - 1);
    require(std::abs(controller.getParamNormalized(kMetalStyleId) - expectedMetalStyle) < 1e-9,
            "legacy style migration must also populate the new Metal Style selector");
    const double expectedPower = expectedPowerChordsEnabled ? 1.0 : 0.0;
    require(std::abs(controller.getParamNormalized(kPowerChordsEnabledId) - expectedPower) < 1e-9,
            "legacy Power Chords Enabled migration must preserve/default correctly");
    require(std::abs(controller.getParamNormalized(kDrumMapId)) < 1e-9,
            "legacy Drum Map migration must default to General MIDI");
    constexpr double kRoleEpsilon = 1e-6;
    require(std::abs(controller.getParamNormalized(kBassFollowId) - 0.72) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kBassMovementId) - 0.34) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kDrumDensityId) - 0.48) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kDrumComplexityId) - 0.30) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kPadSpreadId) - 0.42) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kPadTensionId) - 0.18) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kSynthActivityId) - 0.46) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kSynthMovementId) - 0.42) < kRoleEpsilon &&
            std::abs(controller.getParamNormalized(kFillIntensityId) - 0.50) < kRoleEpsilon,
            "legacy role controls must migrate to documented defaults");

    fixture.rewind();
    MidiatorProcessor processor;
    require(processor.setState(&fixture) == kResultOk,
            "legacy state must migrate through processor");

    MemoryStream migrated;
    require(processor.getState(&migrated) == kResultOk,
            "migrated legacy state must serialize as current state");
    require(migrated.bytes().size() > fixture.bytes().size(),
            "current V7 state must include fields absent from legacy fixture");
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

    // V7 must restore the exact companion-role payload, not regenerate it.
    // Layout is fixed-width: 64-byte header, then Guitar/Bass/Drums/Pad/Synth.
    MemoryStream companionPatched;
    companionPatched.bytes() = first.bytes();

    constexpr size_t kHeaderBytes = 108;
    constexpr size_t kPhraseBytes =
        sizeof(int32) + midiator::kMaxSteps *
        (sizeof(int32) + midiator::kMaxNotesPerStep * 3 * sizeof(int32));
    constexpr size_t kDrumPhraseBytes =
        sizeof(int32) + midiator::kMaxSteps *
        (sizeof(int32) + midiator::kMaxDrumHitsPerStep * 2 * sizeof(int32));
    constexpr size_t kPadPhraseBytes =
        sizeof(int32) + midiator::kMaxSteps *
        (sizeof(int32) + midiator::kMaxPadVoices * 3 * sizeof(int32));
    constexpr size_t kLegacyPhraseBytes =
        sizeof(int32) + kLegacyStateSteps *
        (sizeof(int32) + midiator::kMaxNotesPerStep * 3 * sizeof(int32));
    constexpr size_t kLegacyDrumPhraseBytes =
        sizeof(int32) + kLegacyStateSteps *
        (sizeof(int32) + midiator::kMaxDrumHitsPerStep * 2 * sizeof(int32));
    constexpr size_t kLegacyPadPhraseBytes =
        sizeof(int32) + kLegacyStateSteps *
        (sizeof(int32) + midiator::kMaxPadVoices * 3 * sizeof(int32));

    const size_t guitarOffset = kHeaderBytes;
    const size_t bassOffset = guitarOffset + kPhraseBytes;
    const size_t drumOffset = bassOffset + kPhraseBytes;
    const size_t padOffset = drumOffset + kDrumPhraseBytes;
    const size_t synthOffset = padOffset + kPadPhraseBytes;

    // Patch valid data fields in every non-Guitar role. Even if a particular
    // step is currently unused, V7 promises exact payload recall.
    const int32 bassPitch = 59;
    const int32 drumVelocity = 77;
    const int32 padPitch = 83;
    const int32 synthPitch = 91;

    patchFixtureValue(companionPatched,
                      bassOffset + sizeof(int32) + sizeof(int32),
                      bassPitch);
    patchFixtureValue(companionPatched,
                      drumOffset + sizeof(int32) + sizeof(int32) + sizeof(int32),
                      drumVelocity);
    patchFixtureValue(companionPatched,
                      padOffset + sizeof(int32) + sizeof(int32),
                      padPitch);
    patchFixtureValue(companionPatched,
                      synthOffset + sizeof(int32) + sizeof(int32),
                      synthPitch);

    companionPatched.rewind();
    MidiatorProcessor exactRestore;
    require(exactRestore.setState(&companionPatched) == kResultOk,
            "V7 companion-payload fixture must restore");

    MemoryStream exactReserialized;
    require(exactRestore.getState(&exactReserialized) == kResultOk,
            "V7 companion-payload fixture must serialize again");
    require(companionPatched.bytes() == exactReserialized.bytes(),
            "V7 must preserve exact Bass/Drums/Pad/Synth payload bytes without regeneration");

    auto expectRejectedPatch = [&](size_t offset, int32 value, const char* message) {
        MemoryStream damaged;
        damaged.bytes() = first.bytes();
        patchFixtureValue(damaged, offset, value);
        damaged.rewind();
        MidiatorProcessor target;
        require(target.setState(&damaged) != kResultOk, message);
    };

    // Current V7 header values are exact too. Invalid enums/ranges,
    // non-boolean flags and non-finite controls must be rejected.
    expectRejectedPatch(8, 99,
                        "V7 invalid Root must be rejected");
    expectRejectedPatch(12, 99,
                        "V7 invalid Scale must be rejected");
    expectRejectedPatch(16, 3,
                        "V7 invalid Bars must be rejected");
    expectRejectedPatch(48, 99,
                        "V7 invalid Manual Root must be rejected");
    expectRejectedPatch(52, 2,
                        "V7 invalid Root Source must be rejected");
    expectRejectedPatch(56, 99,
                        "V7 invalid Style must be rejected");
    expectRejectedPatch(60, 2,
                        "V7 invalid Power Chords Enabled flag must be rejected");
    expectRejectedPatch(64, static_cast<int32>(midiator::DrumMapId::SuperiorDrummer3),
                        "V7 unverified Drum Map id must be rejected");
    {
        MemoryStream damaged;
        damaged.bytes() = first.bytes();
        const float nanValue = std::numeric_limits<float>::quiet_NaN();
        patchFixtureValue(damaged, 68, nanValue);
        damaged.rewind();
        MidiatorProcessor target;
        require(target.setState(&damaged) != kResultOk,
                "V7 NaN Bass Follow must be rejected");
    }

    {
        MemoryStream damaged;
        damaged.bytes() = first.bytes();
        const float nanValue = std::numeric_limits<float>::quiet_NaN();
        patchFixtureValue(damaged, 100, nanValue);
        damaged.rewind();
        MidiatorProcessor target;
        require(target.setState(&damaged) != kResultOk,
                "V10 NaN Fill Intensity must be rejected");
    }
    expectRejectedPatch(104, 99,
                        "V10 invalid Section Type must be rejected");

    {
        MemoryStream damaged;
        damaged.bytes() = first.bytes();
        const float nanValue = std::numeric_limits<float>::quiet_NaN();
        patchFixtureValue(damaged, 20, nanValue);
        damaged.rewind();
        MidiatorProcessor target;
        require(target.setState(&damaged) != kResultOk,
                "V7 NaN Density must be rejected");
    }

    // Current V7 states are exact payloads. Structural corruption must be
    // rejected instead of silently clamped into a different arrangement.
    expectRejectedPatch(bassOffset, 3,
                        "V7 invalid Bass bars must be rejected");
    expectRejectedPatch(bassOffset + sizeof(int32), 99,
                        "V7 invalid Bass noteCount must be rejected");
    expectRejectedPatch(drumOffset + sizeof(int32), 99,
                        "V7 invalid Drum hitCount must be rejected");
    expectRejectedPatch(drumOffset + sizeof(int32) + sizeof(int32), 99,
                        "V7 invalid Drum voice must be rejected");
    expectRejectedPatch(padOffset + sizeof(int32), 99,
                        "V7 invalid Pad noteCount must be rejected");
    expectRejectedPatch(synthOffset + sizeof(int32) + sizeof(int32), 200,
                        "V7 invalid Synth pitch must be rejected");

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

    // V4: exact pre-V7 layout includes Power Chords Enabled but only Guitar payload.
    verifyLegacyControllerMigration(
        4u, 9, 8, true, static_cast<int32>(midiator::StyleId::DarkRockGothic),
        8.0 / 11.0, 1.0, 0.5, false);

    // Legacy states must reset Section to FREE even when the target processor
    // previously restored a modern non-FREE state.
    {
        MidiatorProcessor target;

        MemoryStream modern;
        require(target.getState(&modern) == kResultOk,
                "modern-section fixture must serialize");
        const int32 chorus =
            static_cast<int32>(midiator::SectionType::Chorus);
        patchFixtureValue(modern, 104, chorus);
        modern.rewind();
        require(target.setState(&modern) == kResultOk,
                "modern CHORUS state must restore before legacy migration");

        auto legacy = makeLegacyStateFixture(
            4u, 9, 8, true,
            static_cast<int32>(midiator::StyleId::DarkRockGothic), true);
        legacy.rewind();
        require(target.setState(&legacy) == kResultOk,
                "V4 state must restore over an active modern Section");

        MemoryStream migrated;
        require(target.getState(&migrated) == kResultOk,
                "legacy-over-section state must serialize");
        int32 section = -1;
        std::memcpy(&section, migrated.bytes().data() + 104, sizeof(section));
        require(section == static_cast<int32>(midiator::SectionType::Free),
                "legacy state must reset an active modern Section to FREE");
    }

    // V5 was the first exact five-role payload format. It had a 64-byte
    // header and no Drum Map or role-shaping values. Reconstruct that exact
    // historical byte layout from a valid current state by removing the
    // V6/V7-only header extension, then prove that V7 preserves all five
    // serialized role payloads while supplying the documented defaults.
    {
        MemoryStream current;
        MidiatorProcessor source;
        require(source.getState(&current) == kResultOk,
                "current state fixture for V5 migration must serialize");

        MemoryStream v5;
        v5.bytes().insert(v5.bytes().end(),
                          current.bytes().begin(),
                          current.bytes().begin() + 64);
        const size_t curGuitar = 108;
        const size_t curBass = curGuitar + kPhraseBytes;
        const size_t curDrums = curBass + kPhraseBytes;
        const size_t curPads = curDrums + kDrumPhraseBytes;
        const size_t curSynth = curPads + kPadPhraseBytes;
        auto appendLegacy = [&](size_t offset, size_t bytes) {
            v5.bytes().insert(v5.bytes().end(),
                              current.bytes().begin() + offset,
                              current.bytes().begin() + offset + bytes);
        };
        appendLegacy(curGuitar, kLegacyPhraseBytes);
        appendLegacy(curBass, kLegacyPhraseBytes);
        appendLegacy(curDrums, kLegacyDrumPhraseBytes);
        appendLegacy(curPads, kLegacyPadPhraseBytes);
        appendLegacy(curSynth, kLegacyPhraseBytes);
        const uint32_t v5Version = 5u;
        patchFixtureValue(v5, sizeof(uint32_t), v5Version);

        const std::vector<uint8_t> oldPayload(
            v5.bytes().begin() + 64, v5.bytes().end());

        v5.rewind();
        MidiatorProcessor migrated;
        require(migrated.setState(&v5) == kResultOk,
                "frozen V5 five-role state must migrate to V8");

        MemoryStream upgraded;
        require(migrated.getState(&upgraded) == kResultOk,
                "migrated V5 state must serialize as V8");
        require(upgraded.bytes().size() > 100 + oldPayload.size(),
                "upgraded V5 state must contain expanded V8 payload");

        size_t legacyOffset = 0;
        auto requireLegacyPrefix = [&](size_t upgradedOffset, size_t bytes) {
            require(std::equal(oldPayload.begin() + legacyOffset,
                               oldPayload.begin() + legacyOffset + bytes,
                               upgraded.bytes().begin() + upgradedOffset),
                    "V5 migration must preserve every legacy role payload prefix");
            legacyOffset += bytes;
        };
        requireLegacyPrefix(108, kLegacyPhraseBytes);
        requireLegacyPrefix(108 + kPhraseBytes, kLegacyPhraseBytes);
        requireLegacyPrefix(108 + 2 * kPhraseBytes, kLegacyDrumPhraseBytes);
        requireLegacyPrefix(108 + 2 * kPhraseBytes + kDrumPhraseBytes,
                            kLegacyPadPhraseBytes);
        requireLegacyPrefix(108 + 2 * kPhraseBytes + kDrumPhraseBytes + kPadPhraseBytes,
                            kLegacyPhraseBytes);
        require(legacyOffset == oldPayload.size(),
                "V5 legacy payload comparison must cover all five roles");

        // V5 predates Drum Map and role shaping.
        const int32 expectedMap =
            static_cast<int32>(midiator::DrumMapId::GeneralMidi);
        int32 storedMap = -1;
        std::memcpy(&storedMap, upgraded.bytes().data() + 64, sizeof(storedMap));
        require(storedMap == expectedMap,
                "V5 migration must default Drum Map to General MIDI");

        const float expectedRoleDefaults[] = {
            0.72f, 0.34f, 0.48f, 0.30f,
            0.42f, 0.18f, 0.46f, 0.42f
        };
        for (size_t i = 0; i < std::size(expectedRoleDefaults); ++i) {
            float value = 0.0f;
            std::memcpy(&value,
                        upgraded.bytes().data() + 68 + i * sizeof(float),
                        sizeof(value));
            require(std::abs(value - expectedRoleDefaults[i]) < 1e-6f,
                    "V5 migration must install documented role-control defaults");
        }
        float fillIntensity = 0.0f;
        std::memcpy(&fillIntensity, upgraded.bytes().data() + 100, sizeof(fillIntensity));
        require(std::abs(fillIntensity - 0.50f) < 1e-6f,
                "V5 migration must default Fill Intensity to 50%");
        int32 sectionType = -1;
        std::memcpy(&sectionType, upgraded.bytes().data() + 104, sizeof(sectionType));
        require(sectionType == static_cast<int32>(midiator::SectionType::Free),
                "legacy migration must default Section Type to FREE");
    }

    // V6 added the verified Drum Map at byte 64 but still predates the eight
    // V7 role-shaping floats. Its five-role payload therefore begins at byte
    // 68. Preserve both the selected verified map and the complete payload.
    {
        MemoryStream current;
        MidiatorProcessor source;
        require(source.getState(&current) == kResultOk,
                "current state fixture for V6 migration must serialize");

        // Use EZdrummer 3 rather than the default so map preservation is
        // actually tested.
        const int32 ezd3 =
            static_cast<int32>(midiator::DrumMapId::EZdrummer3);
        patchFixtureValue(current, 64, ezd3);

        MemoryStream v6;
        v6.bytes().insert(v6.bytes().end(),
                          current.bytes().begin(),
                          current.bytes().begin() + 68);
        const size_t curGuitar = 108;
        const size_t curBass = curGuitar + kPhraseBytes;
        const size_t curDrums = curBass + kPhraseBytes;
        const size_t curPads = curDrums + kDrumPhraseBytes;
        const size_t curSynth = curPads + kPadPhraseBytes;
        auto appendLegacy = [&](size_t offset, size_t bytes) {
            v6.bytes().insert(v6.bytes().end(),
                              current.bytes().begin() + offset,
                              current.bytes().begin() + offset + bytes);
        };
        appendLegacy(curGuitar, kLegacyPhraseBytes);
        appendLegacy(curBass, kLegacyPhraseBytes);
        appendLegacy(curDrums, kLegacyDrumPhraseBytes);
        appendLegacy(curPads, kLegacyPadPhraseBytes);
        appendLegacy(curSynth, kLegacyPhraseBytes);
        const uint32_t v6Version = 6u;
        patchFixtureValue(v6, sizeof(uint32_t), v6Version);

        const std::vector<uint8_t> oldPayload(
            v6.bytes().begin() + 68, v6.bytes().end());

        v6.rewind();
        MidiatorProcessor migrated;
        require(migrated.setState(&v6) == kResultOk,
                "frozen V6 Drum-Map state must migrate to V8");

        MemoryStream upgraded;
        require(migrated.getState(&upgraded) == kResultOk,
                "migrated V6 state must serialize as V8");

        size_t legacyOffset = 0;
        auto requireLegacyPrefix = [&](size_t upgradedOffset, size_t bytes) {
            require(std::equal(oldPayload.begin() + legacyOffset,
                               oldPayload.begin() + legacyOffset + bytes,
                               upgraded.bytes().begin() + upgradedOffset),
                    "V6 migration must preserve every legacy role payload prefix");
            legacyOffset += bytes;
        };
        requireLegacyPrefix(108, kLegacyPhraseBytes);
        requireLegacyPrefix(108 + kPhraseBytes, kLegacyPhraseBytes);
        requireLegacyPrefix(108 + 2 * kPhraseBytes, kLegacyDrumPhraseBytes);
        requireLegacyPrefix(108 + 2 * kPhraseBytes + kDrumPhraseBytes,
                            kLegacyPadPhraseBytes);
        requireLegacyPrefix(108 + 2 * kPhraseBytes + kDrumPhraseBytes + kPadPhraseBytes,
                            kLegacyPhraseBytes);
        require(legacyOffset == oldPayload.size(),
                "V6 legacy payload comparison must cover all five roles");

        int32 storedMap = -1;
        std::memcpy(&storedMap, upgraded.bytes().data() + 64, sizeof(storedMap));
        require(storedMap == ezd3,
                "V6 migration must preserve the selected verified Drum Map");
    }

    {
        MemoryStream current;
        MidiatorProcessor source;
        require(source.getState(&current) == kResultOk,
                "current V8 fixture for V7 migration must serialize");

        MemoryStream v7;
        v7.bytes().insert(v7.bytes().end(),
                          current.bytes().begin(),
                          current.bytes().begin() + 100);
        const size_t curGuitar = 108;
        const size_t curBass = curGuitar + kPhraseBytes;
        const size_t curDrums = curBass + kPhraseBytes;
        const size_t curPads = curDrums + kDrumPhraseBytes;
        const size_t curSynth = curPads + kPadPhraseBytes;
        auto appendLegacy = [&](size_t offset, size_t bytes) {
            v7.bytes().insert(v7.bytes().end(),
                              current.bytes().begin() + offset,
                              current.bytes().begin() + offset + bytes);
        };
        appendLegacy(curGuitar, kLegacyPhraseBytes);
        appendLegacy(curBass, kLegacyPhraseBytes);
        appendLegacy(curDrums, kLegacyDrumPhraseBytes);
        appendLegacy(curPads, kLegacyPadPhraseBytes);
        appendLegacy(curSynth, kLegacyPhraseBytes);
        const uint32_t v7Version = 7u;
        patchFixtureValue(v7, sizeof(uint32_t), v7Version);

        const size_t oldSize = v7.bytes().size();
        v7.rewind();
        MidiatorProcessor migrated;
        require(migrated.setState(&v7) == kResultOk,
                "frozen V7 128-step state must migrate to V8");

        MemoryStream upgraded;
        require(migrated.getState(&upgraded) == kResultOk,
                "migrated V7 state must serialize as V8");
        require(upgraded.bytes().size() > oldSize,
                "V8 state must expand the old 128-step payload to 256 steps");
    }

    {
        // V8 already used 256-step role payloads but did not yet store Fill
        // Intensity. Reconstruct that exact 100-byte-header layout and verify
        // the V9 loader supplies the musical 50% default without touching the
        // 16-bar-capable payload.
        MemoryStream current;
        MidiatorProcessor source;
        require(source.getState(&current) == kResultOk,
                "current V9 fixture for V8 migration must serialize");

        MemoryStream v8;
        v8.bytes().insert(v8.bytes().end(),
                          current.bytes().begin(),
                          current.bytes().begin() + 100);
        v8.bytes().insert(v8.bytes().end(),
                          current.bytes().begin() + 108,
                          current.bytes().end());
        const uint32_t v8Version = 8u;
        patchFixtureValue(v8, sizeof(uint32_t), v8Version);

        v8.rewind();
        MidiatorProcessor migrated;
        require(migrated.setState(&v8) == kResultOk,
                "frozen V8 256-step state must migrate to V9");

        MemoryStream upgraded;
        require(migrated.getState(&upgraded) == kResultOk,
                "migrated V8 state must serialize as V9");
        float fillIntensity = 0.0f;
        std::memcpy(&fillIntensity,
                    upgraded.bytes().data() + 100,
                    sizeof(fillIntensity));
        require(std::abs(fillIntensity - 0.50f) < 1e-6f,
                "V8 migration must default Fill Intensity to 50%");
    }

    {
        // V9 stored Fill Intensity at byte 100 and then immediately started
        // the 256-step phrase payload. V10 appends the persistent Section Type
        // at byte 104; old V9 projects must therefore default to FREE.
        MemoryStream current;
        MidiatorProcessor source;
        require(source.getState(&current) == kResultOk,
                "current V10 fixture for V9 migration must serialize");

        MemoryStream v9;
        v9.bytes().insert(v9.bytes().end(),
                          current.bytes().begin(),
                          current.bytes().begin() + 104);
        v9.bytes().insert(v9.bytes().end(),
                          current.bytes().begin() + 108,
                          current.bytes().end());
        const uint32_t v9Version = 9u;
        patchFixtureValue(v9, sizeof(uint32_t), v9Version);

        v9.rewind();
        MidiatorProcessor migrated;
        require(migrated.setState(&v9) == kResultOk,
                "frozen V9 state must migrate to V10");

        MemoryStream upgraded;
        require(migrated.getState(&upgraded) == kResultOk,
                "migrated V9 state must serialize as V10");
        int32 sectionType = -1;
        std::memcpy(&sectionType,
                    upgraded.bytes().data() + 104,
                    sizeof(sectionType));
        require(sectionType == static_cast<int32>(midiator::SectionType::Free),
                "V9 migration must default Section Type to FREE");
    }


    {
        // V10 had exactly three valid style IDs. Newer state readers must not
        // reinterpret a corrupt/impossible V10 value as a newly-added style.
        MemoryStream impossibleV10;
        impossibleV10.bytes() = first.bytes();
        const uint32_t v10Version = 10u;
        const int32 impossibleOldStyle =
            static_cast<int32>(midiator::StyleId::ClassicHeavy);
        patchFixtureValue(impossibleV10, sizeof(uint32_t), v10Version);
        patchFixtureValue(impossibleV10, 56, impossibleOldStyle);
        impossibleV10.rewind();

        MidiatorProcessor target;
        require(target.setState(&impossibleV10) != kResultOk,
                "V10 state must reject style IDs that did not exist in V10");
    }

    {
        // V12 is the authoritative expanded-style state. The byte layout stays
        // compatible with V10 while style IDs 0..11 are now valid.
        MemoryStream deathV12;
        deathV12.bytes() = first.bytes();
        const int32 deathStyle =
            static_cast<int32>(midiator::StyleId::Death);
        patchFixtureValue(deathV12, 56, deathStyle);
        deathV12.rewind();

        MidiatorProcessor restoredDeath;
        require(restoredDeath.setState(&deathV12) == kResultOk,
                "V12 state must accept expanded metal style IDs");

        MemoryStream serializedDeath;
        require(restoredDeath.getState(&serializedDeath) == kResultOk,
                "V12 expanded style state must serialize again");
        int32 storedStyle = -1;
        std::memcpy(&storedStyle,
                    serializedDeath.bytes().data() + 56,
                    sizeof(storedStyle));
        require(storedStyle == deathStyle,
                "V12 expanded style ID must survive exact state roundtrip");

        deathV12.rewind();
        MidiatorController deathController;
        require(deathController.initialize(nullptr) == kResultOk,
                "V12 expanded style controller must initialize");
        require(deathController.setComponentState(&deathV12) == kResultOk,
                "V12 expanded style controller must restore state");
        require(std::abs(
                    deathController.getParamNormalized(kMetalStyleId) -
                    6.0 / 11.0) < 1e-9,
                "V12 Death Metal state must restore the 12-style selector exactly");
    }

    {
        // V11 was used by two unreleased development lines: early expanded
        // styles and the now-retired Song Mode. Preserve both safely. The old
        // Song Mode flag lived in Section bit 0x100; migration discards that
        // flag and keeps the selected manual Section.
        MemoryStream retiredSongV11;
        retiredSongV11.bytes() = first.bytes();
        const uint32_t v11Version = 11u;
        const int32 oldStyle =
            static_cast<int32>(midiator::StyleId::HeavyIndustrial);
        const int32 chorusWithRetiredAutoFlag =
            0x100 | static_cast<int32>(midiator::SectionType::Chorus);
        patchFixtureValue(retiredSongV11, sizeof(uint32_t), v11Version);
        patchFixtureValue(retiredSongV11, 56, oldStyle);
        patchFixtureValue(retiredSongV11, 104, chorusWithRetiredAutoFlag);
        retiredSongV11.rewind();

        MidiatorProcessor migratedSong;
        require(migratedSong.setState(&retiredSongV11) == kResultOk,
                "retired Song Mode V11 state must migrate without ambiguity");

        MemoryStream migratedV12;
        require(migratedSong.getState(&migratedV12) == kResultOk,
                "retired V11 state must resave as V12");
        uint32_t storedVersion = 0;
        int32 storedSection = -1;
        std::memcpy(&storedVersion,
                    migratedV12.bytes().data() + sizeof(uint32_t),
                    sizeof(storedVersion));
        std::memcpy(&storedSection,
                    migratedV12.bytes().data() + 104,
                    sizeof(storedSection));
        require(storedVersion == 12u,
                "retired V11 state must upgrade to V12");
        require(storedSection ==
                    static_cast<int32>(midiator::SectionType::Chorus),
                "retired V11 AUTO flag must be discarded while preserving the manual Section");

        // The brief expanded-style V11 development build used the same plain
        // header layout. Keep those test projects readable as well.
        MemoryStream expandedV11;
        expandedV11.bytes() = first.bytes();
        const int32 deathStyle =
            static_cast<int32>(midiator::StyleId::Death);
        patchFixtureValue(expandedV11, sizeof(uint32_t), v11Version);
        patchFixtureValue(expandedV11, 56, deathStyle);
        expandedV11.rewind();
        MidiatorProcessor migratedExpanded;
        require(migratedExpanded.setState(&expandedV11) == kResultOk,
                "expanded-style development V11 state must remain readable");
    }

    std::cout << "Midiator processor-state roundtrip test: PASS\n";
    return 0;
}
