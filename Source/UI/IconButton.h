#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include "../Theme/Icons.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Header button styled with an icon on the left + label on the right.
    // Reuses juce::TextButton paint for the pill background; the icon is
    // overlaid in paintButton(). Keeps every header control on the same
    // visual grid even when we swap text for a glyph.
    // Not final: a bar with a right-click menu wraps it in LeftClickOnly
    // (the setlist bar's + CUE).
    class IconButton : public juce::TextButton
    {
    public:
        IconButton (zynforge::icons::Glyph g, const juce::String& label)
            : juce::TextButton (label), glyph (g) {}

        // A button only a click (or VoiceOver's press) presses: it never holds
        // keyboard focus, and a key that reaches it anyway goes on to the window.
        void setKeyboardClicks (bool allowed)
        {
            keyboardClicks = allowed;
            setWantsKeyboardFocus (allowed);
            if (! allowed) setMouseClickGrabsKeyboardFocus (false);
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            return keyboardClicks && juce::TextButton::keyPressed (key);
        }

        // A small state dot at the right end, like a status LED. The chip stays
        // neutral; only the dot carries colour. Empty = no dot.
        void setStatusDot (std::optional<juce::Colour> colour)
        {
            if (colour == statusDot) return;
            statusDot = colour;
            repaint();
        }
        std::optional<juce::Colour> getStatusDot() const noexcept { return statusDot; }
        static constexpr float kDotSize = 7.0f;

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
            if (statusDot.has_value())
            {
                auto dotColumn = textArea.removeFromRight (kDotSize + (float) brand::space::xs);
                g.setColour (*statusDot);
                g.fillEllipse (dotColumn.withSizeKeepingCentre (kDotSize, kDotSize));
            }
            g.setColour (colour);
            g.setFont (brand::type::uiLabel());
            g.drawText (getButtonText(), textArea,
                        juce::Justification::centredLeft, false);
        }

    private:
        zynforge::icons::Glyph glyph;
        bool keyboardClicks = true;
        std::optional<juce::Colour> statusDot;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
    };
}
