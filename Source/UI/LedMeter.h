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

        // Attach a second TrackState for stereo display. nullptr (default)
        // leaves the meter mono. Safe to call after construction.
        void setStereoPartner (TrackState* r) noexcept { stereoR = r; repaint(); }

        // Toggle the dB-label gutter on the left edge. The EDIT view rows
        // turn this off because the meter widget is too narrow to fit
        // labels and a bar -- they want a pure-bar meter.
        void setShowDbLabels (bool s) noexcept { showLabels = s; repaint(); }

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        void paintBar (juce::Graphics& g, juce::Rectangle<float> r,
                       float displayPeak, float displayRms) const;

        TrackState& state;
        TrackState* stereoR     { nullptr };
        bool        showLabels  { true };
        float displayPeak       { 0.0f };
        float displayRms        { 0.0f };
        float displayPeakR      { 0.0f };
        float displayRmsR       { 0.0f };
        bool  showClip          { false };
    };
}
