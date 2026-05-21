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

        ChannelStrip (int index, TrackState& state,
                      ColourCallback onColourPicked = {},
                      NameCallback   onRename       = {});
        ~ChannelStrip() override;

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

        juce::Label   nameLabel;
        juce::ToggleButton armButton  { "ARM" };
        juce::ToggleButton monButton  { "MON" };
        juce::Label   dbLabel;
        juce::Label   clipLabel;
        MiniSpectrum  spectrum;
        LedMeter      meter;
        std::unique_ptr<Swatch> swatch;

        class StripTimer;
        std::unique_ptr<StripTimer> stripTimer;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
    };
}
