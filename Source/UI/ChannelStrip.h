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
                      IntCallback    onOutputRouted  = {});
        ~ChannelStrip() override;

        void setMenuCallbacks (VoidCallback onDelete,
                               VoidCallback onAdd,
                               VoidCallback onLinkStereo,
                               IntCallback  onLinkToOther);

        // Populate routing combo boxes — call after device topology changes.
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

        juce::Colour getResolvedColour() const;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        class Swatch;

        void openColourPicker();
        void openRenameDialog();
        void showContextMenu();

        int          stripIndex;
        TrackState&  state;
        juce::Colour personality;
        ColourCallback colourCb;
        NameCallback   renameCb;
        FloatCallback  gainCb;
        FloatCallback  panCb;
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
        juce::ToggleButton armButton   { "REC" };
        juce::ToggleButton monButton   { "MON" };
        juce::ToggleButton muteButton  { "MUTE" };
        juce::ToggleButton soloButton  { "SOLO" };
        juce::Label   dbLabel;
        juce::Label   clipLabel;
        MiniSpectrum  spectrum;
        juce::Slider  panSlider;
        juce::Slider  gainFader;
        LedMeter      meter;
        std::unique_ptr<Swatch> swatch;

        class StripTimer;
        std::unique_ptr<StripTimer> stripTimer;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
    };
}
