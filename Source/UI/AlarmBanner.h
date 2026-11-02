#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/DisplayText.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // The main window's loud line. An engineer mid-show watches the strips, not
    // the header, so whatever threatens a take shows here, the one bright fill
    // on screen. It lies over the header's wordmark and status text and takes no
    // clicks, so it never moves a fader or stands between the engineer and STOP.
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

        void show (const juce::String& newText, Level newLevel)
        {
            if (newText == text && newLevel == level) return;
            const bool announce = newText.isNotEmpty() && newText != text;
            text = newText;
            level = newLevel;
            setDescription (text);
            setVisible (text.isNotEmpty());
            if (isVisible()) toFront (false);
            repaint();
            if (announce)
                juce::AccessibilityHandler::postAnnouncement (
                    text, juce::AccessibilityHandler::AnnouncementPriority::high);
        }

        const juce::String& getText() const noexcept { return text; }
        Level getLevel() const noexcept { return level; }

        void paint (juce::Graphics& g) override
        {
            const auto fill = level == Level::Danger ? brand::accentRecord : brand::alertAmber;
            g.setColour (fill);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), brand::radius::md);
            g.setColour (brand::onSignal (fill));
            g.setFont (brand::type::headline());
            g.drawFittedText (typeset (text), getLocalBounds().reduced (brand::space::lg, 0),
                              juce::Justification::centredLeft, 2, 0.75f);
        }

    private:
        juce::String text;
        Level level { Level::Danger };
    };
}
