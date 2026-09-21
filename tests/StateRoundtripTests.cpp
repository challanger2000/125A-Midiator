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

} // namespace

int main() {
    MidiatorProcessor original;

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

    std::cout << "Midiator processor-state roundtrip test: PASS\n";
    return 0;
}
