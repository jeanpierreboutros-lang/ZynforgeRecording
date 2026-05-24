#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/TrackState.h"
#include "LedMeter.h"
#include "MiniSpectrum.h"

#include <functional>
#include <memory>

namespace zynforge
{
    class ChannelStrip final : public juce::Component
    {
    public:
        using ColourCallback = std::function<void (juce::Colour)>;
        using NameCallback   = std::function<void (juce::String)>;
        using FloatCallback  = std::function<void (float)>;
        using IntCallback    = std::function<void (int)>;

        // Optional callbacks for context-menu actions that go beyond a
        // single strip's state (delete / add / stereo link). The strip
        // doesn't know about the engine; MainComponent wires these.
        using VoidCallback = std::function<void()>;

        ChannelStrip (int index, TrackState& state,
                      ColourCallback onColourPicked  = {},
                      NameCallback   onRename        = {},
                      FloatCallback  onGainDb        = {},
                      FloatCallback  onPan           = {},
                      IntCallback    onInputRouted   = {},
                      IntCallback    onOutputRouted  = {},
                      TrackState*    stereoPartner   = nullptr,
                      FloatCallback  onPanR          = {});
        ~ChannelStrip() override;

        void setMenuCallbacks (VoidCallback onDelete,
                               VoidCallback onAdd,
                               VoidCallback onLinkStereo,
                               IntCallback  onLinkToOther);

        // Populate routing combo boxes -- call after device topology changes.
        void setAvailableInputs  (int n);
        void setAvailableOutputs (int n);

        // Re-reads TrackState routing atomics and updates the combo
        // selection without firing onChange. Used to keep the strip
        // combos in sync when the PATCH page mutates routing.
        void refreshRoutingSelection();

        // Re-reads name + colour from TrackState so changes made in the
        // EDIT view (rename, colour swatch) propagate back to the mixer.
        void refreshAppearance();

        void mouseDown (const juce::MouseEvent&) override;

        // Multi-select state. Shift/Cmd-clicking the strip header
        // toggles selection through onToggleSelection so the host
        // (MainComponent) can build a set of selected strips for
        // bulk actions (delete / colour).
        void setSelected (bool isSelected);
        bool isSelected() const noexcept { return selected; }
        int  getStripIndex() const noexcept { return stripIndex; }
        bool isStereo()     const noexcept { return pairState != nullptr; }
        std::function<void (bool /*additive*/)> onToggleSelection;

        // Fired when the engineer assigns this strip to a VCA bus via
        // the right-click menu. The host should persist by calling
        // engine.setTrackVcaGroup so the assignment survives relaunch.
        IntCallback onVcaGroupChanged;

        // Aux send wiring -- host provides the live bus list
        // (busTrackIndex, displayName) so the right-click menu can
        // populate the 'Send to bus' submenu, and a callback the
        // strip fires when the engineer picks a new target for
        // send slot 0.
        std::function<std::vector<std::pair<int, juce::String>>()> getBusList;
        std::function<void (int /*targetBus*/)> onSendTargetChanged;

        // Automation state surfaced to the strip header. Host polls
        // engine state on a slow timer and pushes any changes here
        // so the LED stays in sync without the strip needing a
        // direct engine pointer. writeArmed = WRITE-mode active AND
        // playback rolling; safeOn = per-track Safe lock.
        void setAutomationLed (bool writeArmed, bool safeOn);

        // Fired when the engineer toggles 'Automation Safe' in the
        // strip context menu. Host wires this to
        // engine.setTrackAutomationSafe.
        std::function<void (bool /*safeOn*/)> onAutomationSafeChanged;

        juce::Colour getResolvedColour() const;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit  (const juce::MouseEvent&) override;

    private:
        bool hovered { false };

        class Swatch;

        void openColourPicker();
        void openRenameDialog();
        void showContextMenu();

        int          stripIndex;
        TrackState&  state;
        TrackState*  pairState { nullptr };   // R partner when this is a stereo strip
        juce::Colour personality;
        ColourCallback colourCb;
        NameCallback   renameCb;
        FloatCallback  gainCb;
        FloatCallback  panCb;
        FloatCallback  panRCb;
        IntCallback    inputCb;
        IntCallback    outputCb;
        VoidCallback   deleteCb;
        VoidCallback   addCb;
        VoidCallback   linkStereoCb;
        IntCallback    linkOtherCb;

        class DbRuler;
        std::unique_ptr<DbRuler> dbRuler;

        juce::Label   nameLabel;
        juce::Label   outLabel;
        juce::ComboBox inputCombo;
        juce::ComboBox outputCombo;
        // Single-letter glyphs match the reference: R = record arm,
        // I = input monitor, M = mute, S = solo.
        juce::ToggleButton armButton   { "R" };
        juce::ToggleButton monButton   { "I" };
        juce::ToggleButton muteButton  { "M" };
        juce::ToggleButton soloButton  { "S" };
        juce::Label   dbLabel;
        juce::Label   clipLabel;
        MiniSpectrum  spectrum;
        // Mono: panSlider + panLabel ('pan  <  0  >'). Stereo: two
        // knobs panSlider/panSliderR, two compact labels ('<  100  >').
        juce::Slider  panSlider;
        juce::Slider  panSliderR;
        juce::Label   panLabel;
        juce::Label   panLabelR;
        juce::Slider  gainFader;
        LedMeter      meter;
        std::unique_ptr<Swatch> swatch;

        class StripTimer;
        std::unique_ptr<StripTimer> stripTimer;

        bool selected { false };
        // Automation LED state -- painted as a tiny 6 px badge in the
        // strip header. writeArmed lights brand-red; safeOn lights
        // amber. Both together = safe overrides (writes are blocked).
        bool autoWriteArmed { false };
        bool autoSafeOn     { false };
        // Cached ARGB of the last colour pushed into the swatch + sliders
        // so refreshAppearance only re-applies it on actual change.
        // Zero is a safe "not yet applied" sentinel because any real
        // colour has a non-zero alpha.
        juce::uint32 lastAppliedColour { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
    };
}
