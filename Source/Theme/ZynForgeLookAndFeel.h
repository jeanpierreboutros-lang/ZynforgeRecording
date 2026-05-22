#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    class ZynForgeLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        ZynForgeLookAndFeel();

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        void drawButtonBackground (juce::Graphics&, juce::Button&,
                                   const juce::Colour& bg,
                                   bool over, bool down) override;

        // Live-style linear fader: thin track with green fill below the
        // thumb, long dark thumb with white grip lines + a brighter centre
        // stripe.
        void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPos, float minSliderPos, float maxSliderPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;

        // Wide pill-shaped toggle button: dark gradient body, accent-coloured
        // bold text whose colour reflects the toggle state.
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                               bool over, bool down) override;
    };
}
