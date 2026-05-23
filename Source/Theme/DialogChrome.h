#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrandColors.h"
#include "BrandTokens.h"

namespace zynforge::dialog
{
    // Standard chrome dimensions — all dialogs (and the LookAndFeel
    // AlertBox override) read from these so a future re-skin only
    // touches this file.
    inline constexpr int titleH     = 44;
    inline constexpr int footerH    = 60;
    inline constexpr int stripeW    = 3;   // brand-orange title accent
    inline constexpr int btnPrimary = 110;
    inline constexpr int btnSecond  = 90;
    inline constexpr int btnH       = 28;

    // Background — vertical gradient on bgPanel, identical to what
    // AudioDeviceDialog paints. Call this first inside paint().
    inline void paintBackground (juce::Graphics& g, juce::Component& host) noexcept
    {
        auto r = host.getLocalBounds().toFloat();
        g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.05f, 0.15f));
        g.fillAll();
    }

    // Title bar — 44 px tall, brand-orange 3 px stripe down the left
    // edge, section-title text in textPrimary. Returns the rect the
    // title bar occupied so the caller can measure remaining space.
    inline juce::Rectangle<int> paintTitle (juce::Graphics& g,
                                            juce::Component& host,
                                            const juce::String& title) noexcept
    {
        auto bar = host.getLocalBounds().removeFromTop (titleH)
                                        .reduced (brand::space::md, 0);
        g.setColour (brand::brandOrange);
        g.fillRect (bar.removeFromLeft (stripeW));
        g.setColour (brand::textPrimary);
        g.setFont (brand::type::sectionTitle());
        g.drawText (title.toUpperCase(),
                    bar.translated (brand::space::md, 0),
                    juce::Justification::centredLeft, false);
        return host.getLocalBounds().removeFromTop (titleH);
    }

    // Footer divider — horizontal hairline brand::edge just above the
    // footer button row. paintTitle + paintFooterDivider together
    // bracket the dialog's working area.
    inline void paintFooterDivider (juce::Graphics& g, juce::Component& host) noexcept
    {
        g.setColour (brand::edge);
        g.drawHorizontalLine (host.getHeight() - footerH, 0.0f, (float) host.getWidth());
    }

    // One-call chrome — call this from any dialog's paint(). Equivalent
    // to paintBackground + paintTitle + paintFooterDivider.
    inline void paintChrome (juce::Graphics& g,
                             juce::Component& host,
                             const juce::String& title) noexcept
    {
        paintBackground       (g, host);
        paintTitle            (g, host, title);
        paintFooterDivider    (g, host);
    }

    // Working area between the title bar and the footer button row.
    // Use this in resized() instead of computing offsets by hand.
    inline juce::Rectangle<int> bodyBounds (juce::Component& host) noexcept
    {
        return host.getLocalBounds()
                   .withTrimmedTop (titleH)
                   .withTrimmedBottom (footerH);
    }
    inline juce::Rectangle<int> footerBounds (juce::Component& host) noexcept
    {
        return host.getLocalBounds()
                   .removeFromBottom (footerH)
                   .reduced (brand::space::md);
    }

    // Apply / primary action button — accentStatus background, legible
    // foreground via brand::onSignal. Matches AudioDeviceDialog's
    // primary action treatment.
    inline void stylePrimary (juce::TextButton& b) noexcept
    {
        b.setColour (juce::TextButton::buttonColourId,
                     brand::accentStatus.withAlpha (brand::alpha::prominent));
        b.setColour (juce::TextButton::textColourOffId,
                     brand::onSignal (brand::accentStatus));
        b.setColour (juce::TextButton::textColourOnId,
                     brand::onSignal (brand::accentStatus));
    }

    // Cancel / secondary action button — bgElevated background,
    // textSecondary foreground. Matches AudioDeviceDialog Cancel.
    inline void styleSecondary (juce::TextButton& b) noexcept
    {
        b.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
        b.setColour (juce::TextButton::textColourOffId, brand::textSecondary);
        b.setColour (juce::TextButton::textColourOnId,  brand::textSecondary);
    }

    // Standard combo / text-editor / numeric-field treatment — same
    // dark deep field with edge outline that AudioDeviceDialog uses
    // on every input control.
    inline void styleCombo (juce::ComboBox& c) noexcept
    {
        c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep);
        c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
        c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
        c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
    }
    inline void styleTextEditor (juce::TextEditor& t) noexcept
    {
        t.setColour (juce::TextEditor::backgroundColourId,     brand::bgDeep);
        t.setColour (juce::TextEditor::textColourId,           brand::textPrimary);
        t.setColour (juce::TextEditor::outlineColourId,        brand::edge);
        t.setColour (juce::TextEditor::focusedOutlineColourId, brand::accentStatus);
        t.setColour (juce::TextEditor::highlightColourId,
                     brand::accentStatus.withAlpha (0.35f));
    }
}
