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
    };
}
