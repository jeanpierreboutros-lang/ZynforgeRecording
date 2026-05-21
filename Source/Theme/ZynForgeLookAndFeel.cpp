#include "ZynForgeLookAndFeel.h"
#include "BrandColors.h"

namespace zynforge
{
    ZynForgeLookAndFeel::ZynForgeLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, brand::bgDeep);
        setColour (juce::DocumentWindow::backgroundColourId,  brand::bgDeep);
        setColour (juce::Label::textColourId,                 brand::textPrimary);
        setColour (juce::TextButton::buttonColourId,          brand::bgStrip);
        setColour (juce::TextButton::textColourOnId,          brand::textPrimary);
        setColour (juce::TextButton::textColourOffId,         brand::textPrimary);
        setColour (juce::ComboBox::backgroundColourId,        brand::bgStrip);
        setColour (juce::ComboBox::textColourId,              brand::textPrimary);
        setColour (juce::ComboBox::outlineColourId,           brand::edge);
        setColour (juce::PopupMenu::backgroundColourId,       brand::bgPanel);
        setColour (juce::PopupMenu::textColourId,             brand::textPrimary);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, brand::edge);
    }

    juce::Font ZynForgeLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
    {
        return juce::Font (juce::FontOptions().withHeight ((float) juce::jmin (16, h - 8))
                                              .withStyle ("Bold"));
    }

    void ZynForgeLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                                    const juce::Colour& bg,
                                                    bool over, bool down)
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        auto fill = bg;
        if (down) fill = fill.brighter (0.15f);
        else if (over) fill = fill.brighter (0.07f);

        g.setColour (fill);
        g.fillRoundedRectangle (r, 4.0f);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }
}
