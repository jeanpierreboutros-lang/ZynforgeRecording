#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/TrackState.h"

namespace zynforge
{
    // Vertical LED-segment meter, peak + RMS, with brand styling.
    class LedMeter final : public juce::Component,
                           public juce::SettableTooltipClient,
                           private juce::Timer
    {
    public:
        explicit LedMeter (TrackState& s);
        ~LedMeter() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;

        TrackState& state;
        float displayPeak { 0.0f };
        float displayRms  { 0.0f };
        bool  showClip    { false };
    };
}
