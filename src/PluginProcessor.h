#pragma once

#include "RiffEngine.h"
#include "BassBrain.h"
#include "DrumBrain.h"
#include "PadBrain.h"
#include "SynthBrain.h"

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/lib/controls/icontrollistener.h"
#include "vstgui/uidescription/uiattributes.h"
#include "pluginterfaces/base/ibstream.h"

#include <array>
#include <memory>
#include <atomic>
#include <cstdint>
#include <string>

namespace VSTGUI {
class CTextLabel;
class CControl;
class CTextButton;
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
    kPowerChordsEnabledId = 113,
    kDrumMapId = 114,
    kBassFollowId = 115,
    kBassMovementId = 116,
    kDrumDensityId = 117,
    kDrumComplexityId = 118,
    kPadSpreadId = 119,
    kPadTensionId = 120,
    kSynthActivityId = 121,
    kSynthMovementId = 122,
    // New host/UI length contract. kBarsId=102 is frozen for legacy
    // automation with the historic {1,2,4,8} normalization.
    kSectionLengthId = 123,
    kFillIntensityId = 124,
    kSectionTypeId = 125,
    // kStyleId=112 stays frozen with the original 3-choice normalization.
    // The expanded 12-style selector uses a new ID for automation safety.
    kMetalStyleId = 126,
    kPalmMuteVelocityId = 127,
    kTriggerModeId = 128,
    kGuitarLockId = 129,
    kBassLockId = 130,
    kDrumsLockId = 131,
    kPadLockId = 132,
    kSynthLockId = 133,
    kHumanizeId = 134,
    // kDrumMapId=114 stays frozen with the historical verified 3-map normalization.
    kVerifiedDrumMapId = 135,
    kLiveRootId = 136 // read-only processor->host->GUI effective root
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
    static constexpr int kMaxScheduledEvents = 8192;
    struct ScheduledEvent {
        int32 sampleOffset = 0;
        double ppqPosition = 0.0;
        bool noteOn = false;
        int pitch = 0;
        int velocity = 0;
        int32 busIndex = kGuitarOutBus;
        int32 noteLengthSamples = 0;
        int stepIndex = -1;
        int noteIndex = -1;
    };

    // Reused realtime scheduler buffer: allocate once at construction, not
    // once per MIDI root slice. No callback heap allocation or 300KB reset.
    std::unique_ptr<std::array<ScheduledEvent, kMaxScheduledEvents>> scheduledBuffer_;
    midiator::GeneratorSettings settings_{};
    midiator::Phrase phrase_{};
    midiator::BassSettings bassSettings_{};
    midiator::Phrase bassPhrase_{};
    midiator::DrumSettings drumSettings_{};
    midiator::DrumPhrase drumPhrase_{};
    midiator::DrumMapId drumMapId_ = midiator::DrumMapId::GeneralMidi;
    midiator::DrumMidiMap drumMap_ = midiator::DrumMidiMap::preset(drumMapId_);
    midiator::PadSettings padSettings_{};
    midiator::PadPhrase padPhrase_{};
    midiator::SynthSettings synthSettings_{};
    midiator::Phrase synthPhrase_{};
    // Immutable note-register references for fully reversible key changes.
    // Refreshed only when an individual role is musically regenerated;
    // reconstructed from the exact phrase on project load (no V17 migration).
    midiator::Phrase bassRootReference_{};
    midiator::PadPhrase padRootReference_{};
    midiator::Phrase synthRootReference_{};
    int bassReferenceRootClass_ = 9;
    int padReferenceRootClass_ = 9;
    int synthReferenceRootClass_ = 9;
    uint32_t seed_ = 0x125A2026u;

    double sampleRate_ = 44100.0;
    bool wasPlaying_ = false;
    bool haveExpectedProjectTime_ = false;
    double expectedProjectTimeQn_ = 0.0;
    bool haveTransportAnchor_ = false;
    double transportAnchorQn_ = 0.0;
    std::array<std::array<bool, 128>, kEventOutputBusCount> activePitchesByBus_{};
    std::array<std::array<int, 128>, kEventOutputBusCount> activeVelocitiesByBus_{};
    // Provenance of every sounding note, including polyphonic Pad voices.
    // The source step/note is needed to retune the EXACT held pitch even
    // when a registered octave fold would otherwise alias two Pad voices.
    std::array<std::array<int, 128>, kEventOutputBusCount> activeStepByBus_{};
    std::array<std::array<int, 128>, kEventOutputBusCount> activeVoiceByBus_{};
    bool smoothRootShiftPending_ = false;
    int smoothGuitarSemitones_ = 0;
    int smoothCompanionSemitones_ = 0;
    std::array<bool, 128> triggerHeldPitches_{};
    int triggerHeldCount_ = 0;
    bool midiNoteTrigger_ = false;
    std::array<bool, kEventOutputBusCount> roleLocks_{};
    float humanizeAmount_ = 0.0f;
    int lastPublishedRootPitchClass_ = -1;
    int panicNoteOffCursor_ = 0;

    float variationAmount_ = 0.35f;
    bool phraseChangedNeedsFlush_ = false;
    bool lifecyclePanicPending_ = false;
    bool midiRootSource_ = true;
    int manualRootPitchClass_ = 9;
    int guitarRootMidi_ = 33; // actual guitar root for transposition and V16 state
    std::atomic<uint32_t> pendingNewRiffCommands_{0};
    std::atomic<uint32_t> pendingVariationCommands_{0};

    void generateNew();
    void generateVariation();
    void regenerateBass();
    void regenerateDrums();
    void regeneratePads();
    void regenerateSynth();
    void regenerateBassOnly();
    void regenerateDrumsOnly();
    void regeneratePadsOnly();
    void regenerateSynthOnly();
    void regenerateUnlockedCompanions();
    void regenerateSectionFromCurrentSeed();
    void resizePhraseBars(int newBars, bool regenerateCompanions = true);
    void applyPowerChordMode(bool enabled, bool regenerateCompanions = true);
    void applyPalmMuteVelocityThreshold(int threshold);
    void transposePhraseToRoot(int newRootPitchClass, int newRootMidi,
                               bool regenerateCompanions = true);
    tresult processBlock(ProcessData& data);
    void transposeCompanionPhrases(int semitones) noexcept;
    bool migrateHeldTonalNotes(IEventList* output, double ppqPosition) noexcept;
    void applyParameterChanges(ProcessData& data,
                               bool guiNewRiff = false,
                               bool guiVariation = false);
    bool flushActiveNotes(IEventList* output, double ppqPosition = 0.0,
                          bool forceAllNotes = false, int32 sampleOffset = 0);
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
    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE;
    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) SMTG_OVERRIDE;
    IPlugView* PLUGIN_API createView(FIDString name) SMTG_OVERRIDE;

    void valueChanged(VSTGUI::CControl* control) SMTG_OVERRIDE;

    VSTGUI::CView* verifyView(VSTGUI::CView* view,
                              const VSTGUI::UIAttributes& attributes,
                              const VSTGUI::IUIDescription* description,
                              VSTGUI::VST3Editor* editor) SMTG_OVERRIDE;
    void willClose(VSTGUI::VST3Editor* editor) SMTG_OVERRIDE;
    void onZoomChanged(VSTGUI::VST3Editor* editor, double zoom) SMTG_OVERRIDE;

private:
    void refreshTheory() noexcept;
    void refreshStyleHint() noexcept;
    void refreshToggleLabels() noexcept;
    void refreshDetailsPage() noexcept;
    void refreshZoomButton() noexcept;

    VSTGUI::CTextLabel* theoryKey_ = nullptr;
    VSTGUI::CTextLabel* theoryNotes_ = nullptr;
    VSTGUI::CTextLabel* theoryCharacter_ = nullptr;
    VSTGUI::CTextLabel* theoryInterval_ = nullptr;
    VSTGUI::CTextLabel* styleBpm_ = nullptr;
    VSTGUI::CControl* newRiffButton_ = nullptr;
    VSTGUI::CControl* variationButton_ = nullptr;
    std::array<VSTGUI::CTextButton*, 5> lockButtons_{};
    VSTGUI::CTextButton* powerChordToggle_ = nullptr;
    VSTGUI::CTextButton* zoomButton_ = nullptr;
    VSTGUI::CTextButton* detailsButton_ = nullptr;
    VSTGUI::CView* mainGeneratePanel_ = nullptr;
    VSTGUI::CView* mainLocksPanel_ = nullptr;
    VSTGUI::CView* detailsPanel_ = nullptr;
    bool detailsShown_ = false;
    VSTGUI::VST3Editor* activeEditor_ = nullptr;
    int zoomIndex_ = 1; // fresh instances open at 100%; saved project state wins
    double zoomFactor_ = 1.0;
    double fallbackNewRiffState_ = 0.0;
    double fallbackVariationState_ = 0.0;
};

} // namespace Steinberg::Vst
