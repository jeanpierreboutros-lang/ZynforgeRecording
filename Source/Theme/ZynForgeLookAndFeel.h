#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    class ZynForgeLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        ZynForgeLookAndFeel();

        // Returns Inter / JetBrains Mono from BinaryData when the
        // engineer-facing typeface names (SF Pro / SF Mono -- see
        // BrandTokens.h) are requested. Falls back to the system
        // typeface for anything else.
        juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

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

        // Heated Steel pan knob: a small machined-steel knob with a single
        // forge-orange pointer line. Replaces JUCE's default "C" ring so the
        // pan control matches the Direction-C mock.
        void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                               float sliderPosProportional,
                               float rotaryStartAngle, float rotaryEndAngle,
                               juce::Slider&) override;

        // Wide pill-shaped toggle button: dark gradient body, accent-coloured
        // bold text whose colour reflects the toggle state.
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                               bool over, bool down) override;

        // AlertWindow chrome -- paints the same orange-stripe title bar,
        // gradient bgPanel background, and footer divider that every
        // first-party dialog (AudioDevice, NewSession, Export, ...)
        // uses. Any juce::AlertWindow::showAsync (...) now reads as a
        // first-party ZynForge prompt.
        void drawAlertBox (juce::Graphics&, juce::AlertWindow&,
                           const juce::Rectangle<int>& textArea,
                           juce::TextLayout&) override;
        int  getAlertBoxWindowFlags() override;
        juce::Array<int> getWidthsForTextButtons (juce::AlertWindow&,
                                                  const juce::Array<juce::TextButton*>&) override;

    private:
        // Bundled faces, loaded lazily on first getTypefaceForFont call.
        // Members (not function-local statics) so they're released when
        // this LookAndFeel is destroyed during app shutdown -- while
        // CoreText is still alive -- rather than at __cxa_finalize after
        // CoreText is gone (which terminate()s the process on quit).
        juce::Typeface::Ptr interReg, interBold, monoReg, monoBold;
    };
}
