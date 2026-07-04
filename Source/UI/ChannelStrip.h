#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/TrackState.h"
#include "FineFader.h"
#include "LedMeter.h"
#include "MiniSpectrum.h"
#include "RenameLabel.h"

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

        // Tab / Shift+Tab in the name field: ask the host to commit and
        // open the next / previous strip's name editor (shiftDown =
        // previous). MainComponent wires this to walk the strip list.
        std::function<void (int /*fromStripIndex*/, bool /*shiftDown*/)> onTabRename;

        // Open this strip's inline name editor (scrolling it into view is
        // the host's job). Used as the Tab-rename destination.
        void beginRename() { openRenameDialog(); }

        // Fired when the engineer assigns this strip to a VCA bus via
        // the right-click menu. The host should persist by calling
        // engine.setTrackVcaGroup so the assignment survives relaunch.
        IntCallback onVcaGroupChanged;

        // Fired when the engineer assigns this strip to an Edit
        // Group (-1 = unlinked). Host wires to engine.setTrackEditGroup
        // so the assignment persists.
        IntCallback onEditGroupChanged;

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

        // Read-mode automation display. When set, the slow poll asks these
        // for the gain / pan to SHOW (passing the stored manual value as
        // fallback). MainComponent wires them to engine.automationValueAt so
        // that while playback rolls the MIXER fader + pan track the EDIT
        // automation curve at the playhead -- keeping the two views linked --
        // and snap back to the manual value when stopped. Null = always show
        // the manual value.
        std::function<float (float /*manualDb*/)>  displayGainDb;
        std::function<float (float /*manualPan*/)> displayPanL;
        std::function<float (float /*manualPan*/)> displayPanR;

        // Fired when the engineer toggles 'Automation Safe' in the
        // strip context menu. Host wires this to
        // engine.setTrackAutomationSafe.
        std::function<void (bool /*safeOn*/)> onAutomationSafeChanged;

        // Pro Tools-style Edit Group broadcast hook. Fired after the
        // user toggles arm / monitor / mute / solo, so the host can
        // mirror the new value to every other strip in the same
        // edit group. The strip passes its own track index; the host
        // looks up the group + peers via the engine.
        std::function<void (int /*srcTrackIndex*/)> onAfterArmedToggle;
        std::function<void (int /*srcTrackIndex*/)> onAfterMonitorToggle;
        std::function<void (int /*srcTrackIndex*/)> onAfterMuteToggle;
        std::function<void (int /*srcTrackIndex*/)> onAfterSoloToggle;

        juce::Colour getResolvedColour() const;

        // Called just before the strip's backing TrackState is destroyed
        // (an in-strip right-click Delete runs engine.removeStripAt, which
        // frees the TrackState while this ChannelStrip + its 10 Hz StripTimer
        // live on until the next rebuild ~100 ms later). Stops the timer and
        // marks the strip invalid so StripTimer::timerCallback can never
        // dereference the freed TrackState&.
        void invalidate();

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit  (const juce::MouseEvent&) override;

    private:
        bool hovered { false };
        // Tracks whether a VCA / edit-group / bus badge is currently shown so
        // the slow poll can re-lay-out the name label (which reserves room for
        // the badge) the moment an assignment changes. -1 = uninitialised.
        int  lastBadgeShown { -1 };

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

        RenameLabel   nameLabel;
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
        FineFader     gainFader;   // fixed 0.5 dB/notch wheel (Shift = 0.1)
        LedMeter      meter;
        std::unique_ptr<Swatch> swatch;

        class StripTimer;
        std::unique_ptr<StripTimer> stripTimer;

        bool selected { false };
        // False once invalidate() has run (the backing TrackState is being
        // destroyed). Guards StripTimer::timerCallback against a dangling ref.
        bool stripValid { true };
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
