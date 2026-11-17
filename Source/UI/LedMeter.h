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

        // The dBFS scale is the strip's one labelled scale: signed marks
        // (0, -6, -12, -18, -30, -60) in a column left of the bar. The host
        // picks the type rung (brand::type::label() by default, caption() on
        // M and L strips); with no room for it the scale falls back to
        // label(), then to ticks on the bar.
        void setScaleFont (const juce::Font& f);
        // Width the scale column takes in that font, gap to the bar included.
        static int scaleColumnWidth (const juce::Font& f);
        // A scale mark as drawn: "0", "-6" ... "-60".
        static juce::String scaleLabel (float dB);
        // The marks, top to bottom.
        static constexpr float kScaleMarksDb[] = { 0.0f, -6.0f, -12.0f, -18.0f, -30.0f, -60.0f };

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        // Stop reading the (about-to-be-freed) TrackState. Called from
        // ChannelStrip::invalidate() when the owning strip is condemned but
        // not yet destroyed -- the meter's own timer would otherwise keep
        // sampling freed memory for up to a rebuild interval. paint() reads
        // only smoothed member copies, so it stays safe after detach.
        void detach() noexcept { detached = true; stopTimer(); }

    private:
        void timerCallback() override;
        void paintBar (juce::Graphics& g, juce::Rectangle<float> r,
                       float displayPeak, float displayRms) const;

        TrackState& state;
        // Keeps `state` valid after the recorder drops it, until this
        // surface is rebuilt. Empty for a TrackState the recorder doesn't own.
        const std::shared_ptr<TrackState> keepAlive;
        TrackState* stereoR     { nullptr };
        bool        showLabels  { true };
        juce::Font  scaleFont;            // set in the constructor (brand::type::label())
        int         scaleColW   { 0 };    // scaleColumnWidth (scaleFont)
        float displayPeak       { 0.0f };
        float displayRms        { 0.0f };
        float displayPeakR      { 0.0f };
        float displayRmsR       { 0.0f };
        bool  showClip          { false };
        bool  detached          { false };
    };
}
