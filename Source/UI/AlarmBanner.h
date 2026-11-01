#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // The main window's loud line. An engineer mid-show watches the strips, not
    // the header, so whatever threatens a take shows here, big enough to read
    // from the mix position. It has its own row and takes no clicks, so it can
    // never stand between the engineer and STOP.
    class AlarmBanner final : public juce::Component
    {
    public:
        enum class Level { Notice, Danger };

        AlarmBanner()
        {
            setInterceptsMouseClicks (false, false);
            setTitle ("Capture alarm");
            setAccessible (true);
        }

        // True when the banner appeared or went, so the host can lay out again.
        bool show (const juce::String& newText, Level newLevel)
        {
            if (newText == text && newLevel == level) return false;
            const bool visibilityChanged = newText.isEmpty() != text.isEmpty();
            const bool announce = newText.isNotEmpty() && newText != text;
            text = newText;
            level = newLevel;
            setDescription (text);
            setVisible (text.isNotEmpty());
            repaint();
            if (announce)
                juce::AccessibilityHandler::postAnnouncement (
                    text, juce::AccessibilityHandler::AnnouncementPriority::high);
            return visibilityChanged;
        }

        const juce::String& getText() const noexcept { return text; }
        Level getLevel() const noexcept { return level; }

        void paint (juce::Graphics& g) override
        {
            const auto fill = level == Level::Danger ? brand::accentRecord : brand::alertAmber;
            g.setColour (fill);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), brand::radius::md);
            g.setColour (brand::onSignal (fill));
            g.setFont (brand::type::ui (20.0f, true));
            g.drawFittedText (text, getLocalBounds().reduced (brand::space::lg, brand::space::xs),
                              juce::Justification::centredLeft, 2, 0.75f);
        }

    private:
        juce::String text;
        Level level { Level::Danger };
    };
}
