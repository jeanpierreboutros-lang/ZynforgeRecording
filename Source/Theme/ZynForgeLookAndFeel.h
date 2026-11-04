#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    class ZynForgeLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        ZynForgeLookAndFeel();

        // Returns bundled JetBrains Mono for fixed-width text and delegates
        // proportional UI text to the system sans-serif. The previous Inter
        // assets were HTML error pages rather than font data.
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

        // A combo too narrow for its arrow gives the arrow's room to the text:
        // on a narrow strip the routing is what the box is there to show.
        void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                           int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
        void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

        // Wide pill-shaped toggle button: dark gradient body, accent-coloured
        // bold text whose colour reflects the toggle state.
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                               bool over, bool down) override;

        // AlertWindow chrome -- paints the same orange-stripe title bar,
        // gradient bgPanel background, and footer divider that every
        // first-party dialog (AudioDevice, NewSession, Export, ...)
        // uses. Any juce::AlertWindow::showAsync (...) now reads as a
        // first-party ZynForge prompt.
        // Every message on screen goes through typeset(); see DisplayText.h.
        void drawLabel (juce::Graphics&, juce::Label&) override;
        void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
        // Every scrollbar thumb draws like the EDIT timeline overview's view
        // box: an outline in the thumb colour (ember by default) over a faint
        // fill of it, so the two bars of EDIT read as one control family.
        void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                            bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                            bool isMouseOver, bool isMouseDown) override;
        juce::AlertWindow* createAlertWindow (const juce::String& title, const juce::String& message,
                                              const juce::String& button1, const juce::String& button2,
                                              const juce::String& button3, juce::MessageBoxIconType iconType,
                                              int numButtons, juce::Component* associatedComponent) override;

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
        juce::Typeface::Ptr monoReg, monoBold;
    };
}
