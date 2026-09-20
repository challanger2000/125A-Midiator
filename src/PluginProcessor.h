#pragma once

#include "RiffEngine.h"

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <array>
#include <cstdint>

namespace Steinberg::Vst {

enum : ParamID {
    kRootId = 100,
    kScaleId = 101,
    kBarsId = 102,
    kDensityId = 103,
    kComplexityId = 104,
    kRepetitionId = 105,
    kPowerChordId = 106,
    kPalmMuteId = 107,
    kVariationAmountId = 108,
    kNewRiffId = 109,
    kVariationId = 110
};

static const FUID ProcessorUID(0x125A4001, 0x6D494449, 0x41544F52, 0x00000100);
static const FUID ControllerUID(0x125A4002, 0x6D494449, 0x41544F52, 0x00000100);

class MidiatorProcessor final : public AudioEffect {
public:
    MidiatorProcessor();

    static FUnknown* createInstance(void*) {
        return static_cast<IAudioProcessor*>(new MidiatorProcessor());
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE;

private:
    struct ScheduledEvent {
        int32 sampleOffset = 0;
        bool noteOn = false;
        int pitch = 0;
        int velocity = 0;
    };

    midiator::GeneratorSettings settings_{};
    midiator::Phrase phrase_{};
    uint32_t seed_ = 0x125A2026u;

    double sampleRate_ = 44100.0;
    bool wasPlaying_ = false;
    std::array<bool, 128> activePitches_{};

    double lastNewRiffValue_ = 0.0;
    double lastVariationValue_ = 0.0;
    float variationAmount_ = 0.35f;

    void generateNew();
    void generateVariation();
    void applyParameterChanges(ProcessData& data);
    void flushActiveNotes(IEventList* output);
};

class MidiatorController final : public EditControllerEx1 {
public:
    static FUnknown* createInstance(void*) {
        return static_cast<IEditController*>(new MidiatorController());
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API setComponentState(IBStream* state) SMTG_OVERRIDE;
};

} // namespace Steinberg::Vst
