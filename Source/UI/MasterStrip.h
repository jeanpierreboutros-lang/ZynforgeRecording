#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "LedMeter.h"

#include <memory>

namespace zynforge
{
    // Fixed master strip — sits to the right of the channel-strip viewport
    // and never scrolls. Controls the master gain / mute / output routing
    // and meters the master bus.
    class MasterStrip final : public juce::Component, private juce::Timer
    {
    public:
        explicit MasterStrip (AudioEngine&);
        ~MasterStrip() override;

        void paint    (juce::Graphics&) override;
        void resized  () override;

        // Re-populate the output combos when device topology changes.
        void refreshOutputs();

    private:
        void timerCallback() override;

        AudioEngine&     engine;
        juce::Label      title { {}, "MASTER" };
        juce::Label      gainLabel;
        juce::ComboBox   outLCombo;
        juce::ComboBox   outRCombo;
        juce::ToggleButton muteButton { "MUTE" };
        juce::Slider     fader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
        LedMeter         meter;
        int lastNumOutputs { -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
    };
}
