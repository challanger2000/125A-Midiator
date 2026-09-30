#pragma once

#include "RiffEngine.h"

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/lib/controls/icontrollistener.h"
#include "vstgui/uidescription/uiattributes.h"
#include "pluginterfaces/base/ibstream.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace VSTGUI {
class CTextLabel;
class CControl;
class CView;
class IUIDescription;
class VST3Editor;
}

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
    kVariationId = 110,
    kRootSourceId = 111,
    kStyleId = 112,
    kPowerChordsEnabledId = 113
};

static const FUID ProcessorUID(0x125A4001, 0x6D494449, 0x41544F52, 0x00000100);
static const FUID ControllerUID(0x125A4002, 0x6D494449, 0x41544F52, 0x00000100);

enum EventOutputBus : int32 {
    kGuitarOutBus = 0,
    kBassOutBus = 1,
    kDrumsOutBus = 2,
    kPadOutBus = 3,
    kSynthOutBus = 4,
    kEventOutputBusCount = 5
};

class MidiatorProcessor final : public AudioEffect {
public:
    MidiatorProcessor();

    static FUnknown* createInstance(void*) {
        return static_cast<IAudioProcessor*>(new MidiatorProcessor());
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API setProcessing(TBool state) SMTG_OVERRIDE;
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE;
    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE;
    tresult PLUGIN_API notify(IMessage* message) SMTG_OVERRIDE;

private:
    struct ScheduledEvent {
        int32 sampleOffset = 0;
        double ppqPosition = 0.0;
        bool noteOn = false;
        int pitch = 0;
        int velocity = 0;
    };

    midiator::GeneratorSettings settings_{};
    midiator::Phrase phrase_{};
    uint32_t seed_ = 0x125A2026u;

    double sampleRate_ = 44100.0;
    bool wasPlaying_ = false;
    bool haveExpectedProjectTime_ = false;
    double expectedProjectTimeQn_ = 0.0;
    bool haveTransportAnchor_ = false;
    double transportAnchorQn_ = 0.0;
    std::array<bool, 128> activePitches_{};

    float variationAmount_ = 0.35f;
    bool phraseChangedNeedsFlush_ = false;
    bool midiRootSource_ = true;
    int manualRootPitchClass_ = 9;
    std::atomic<uint32_t> pendingNewRiffCommands_{0};
    std::atomic<uint32_t> pendingVariationCommands_{0};

    void generateNew();
    void generateVariation();
    void resizePhraseBars(int newBars);
    void applyPowerChordMode(bool enabled);
    void transposePhraseToRoot(int newRootPitchClass);
    void applyParameterChanges(ProcessData& data);
    void applyMidiRootInput(ProcessData& data);
    void flushActiveNotes(IEventList* output, double ppqPosition = 0.0);
};

class MidiatorController final : public EditControllerEx1,
                                public VSTGUI::VST3EditorDelegate,
                                public VSTGUI::IControlListener {
public:
    static FUnknown* createInstance(void*) {
        return static_cast<IEditController*>(new MidiatorController());
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE;
    tresult PLUGIN_API setComponentState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) SMTG_OVERRIDE;
    IPlugView* PLUGIN_API createView(FIDString name) SMTG_OVERRIDE;

    void valueChanged(VSTGUI::CControl* control) SMTG_OVERRIDE;

    VSTGUI::CView* verifyView(VSTGUI::CView* view,
                              const VSTGUI::UIAttributes& attributes,
                              const VSTGUI::IUIDescription* description,
                              VSTGUI::VST3Editor* editor) SMTG_OVERRIDE;
    void willClose(VSTGUI::VST3Editor* editor) SMTG_OVERRIDE;

private:
    void refreshTheory() noexcept;

    VSTGUI::CTextLabel* theoryKey_ = nullptr;
    VSTGUI::CTextLabel* theoryNotes_ = nullptr;
    VSTGUI::CTextLabel* theoryCharacter_ = nullptr;
    VSTGUI::CTextLabel* theoryInterval_ = nullptr;
    VSTGUI::CControl* newRiffButton_ = nullptr;
    VSTGUI::CControl* variationButton_ = nullptr;
    double fallbackNewRiffState_ = 0.0;
    double fallbackVariationState_ = 0.0;
};

} // namespace Steinberg::Vst
