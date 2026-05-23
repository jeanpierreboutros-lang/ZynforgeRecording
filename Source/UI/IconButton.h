#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Theme/Icons.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Header button styled with an icon on the left + label on the right.
    // Reuses juce::TextButton paint for the pill background; the icon is
    // overlaid in paintButton(). Keeps every header control on the same
    // visual grid even when we swap text for a glyph.
    class IconButton final : public juce::TextButton
    {
    public:
        IconButton (zynforge::icons::Glyph g, const juce::String& label)
            : juce::TextButton (label), glyph (g) {}

        void paintButton (juce::Graphics& g,
                          bool over,
                          bool down) override
        {
            // Let the LookAndFeel paint the pill background; we'll paint
            // icon + label on top so the label centres in the icon-free
            // remainder of the button.
            getLookAndFeel().drawButtonBackground (
                g, *this,
                findColour (getToggleState()
                                ? juce::TextButton::buttonOnColourId
                                : juce::TextButton::buttonColourId),
                over, down);

            const auto bounds = getLocalBounds().toFloat();
            const float iconSize = juce::jmin (16.0f, bounds.getHeight() - 6.0f);
            const float iconX    = 6.0f;
            const float iconY    = bounds.getCentreY() - iconSize * 0.5f;

            const auto colour = getToggleState()
                ? findColour (juce::TextButton::textColourOnId)
                : findColour (juce::TextButton::textColourOffId);

            zynforge::icons::draw (g, glyph,
                                   juce::Rectangle<float> (iconX, iconY, iconSize, iconSize),
                                   colour, 1.3f);

            // Label centred in the bounds *minus* the icon column so
            // they don't overlap.
            auto textArea = bounds.withTrimmedLeft (iconSize + 10.0f).reduced (2.0f, 0.0f);
            g.setColour (colour);
            g.setFont (brand::type::uiLabel());
            g.drawText (getButtonText(), textArea,
                        juce::Justification::centredLeft, false);
        }

    private:
        zynforge::icons::Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
    };
}
