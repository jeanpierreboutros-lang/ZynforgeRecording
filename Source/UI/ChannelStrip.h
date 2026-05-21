#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/TrackState.h"
#include "LedMeter.h"

namespace zynforge
{
    class ChannelStrip final : public juce::Component
    {
    public:
        ChannelStrip (int index, TrackState& state);
        ~ChannelStrip() override = default;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        int          stripIndex;
        TrackState&  state;
        juce::Colour personality;

        juce::Label   nameLabel;
        juce::ToggleButton armButton  { "ARM" };
        juce::ToggleButton monButton  { "MON" };
        LedMeter      meter;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
    };
}
