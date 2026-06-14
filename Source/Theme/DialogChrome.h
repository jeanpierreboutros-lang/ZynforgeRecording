#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrandColors.h"
#include "BrandTokens.h"

namespace zynforge::dialog
{
    // Standard chrome dimensions -- all dialogs (and the LookAndFeel
    // AlertBox override) read from these so a future re-skin only
    // touches this file.
    inline constexpr int titleH     = 44;
    inline constexpr int footerH    = 60;
    inline constexpr int stripeW    = 3;   // brand-orange title accent
    inline constexpr int btnPrimary = 110;
    inline constexpr int btnSecond  = 90;
    inline constexpr int btnH       = 28;

    // Background -- vertical gradient on bgPanel, identical to what
    // AudioDeviceDialog paints. Call this first inside paint().
    inline void paintBackground (juce::Graphics& g, juce::Component& host) noexcept
    {
        auto r = host.getLocalBounds().toFloat();
        // Neutral grey (bgElevated), matching the rest of the app's prompts
        // -- not the blue-tinted bgPanel.
        g.setGradientFill (brand::verticalGradient (brand::bgElevated, r, 0.05f, 0.15f));
        g.fillAll();
    }

    // The ZynForge forge-mark badge -- the app-icon glyph (forged-steel
    // squircle + orange hex + rising double-chevron), drawn programmatically
    // so it scales crisply and needs no embedded PNG. Mirrors the badge the
    // LookAndFeel stamps on AlertWindow prompts, so a custom dialog title bar
    // carries the same identity. Pass the square rect to draw into.
    inline void drawForgeBadge (juce::Graphics& g, juce::Rectangle<float> r) noexcept
    {
        g.setColour (brand::controlBg);
        g.fillRoundedRectangle (r, brand::radius::md);
        g.setColour (brand::gloss (0.10f));
        g.drawRoundedRectangle (r.reduced (0.5f), brand::radius::md, 1.0f);

        const auto glyph = r.reduced (r.getWidth() * 0.22f, r.getHeight() * 0.22f);
        const auto xf = juce::AffineTransform::scale (glyph.getWidth(), glyph.getHeight())
                                               .translated (glyph.getX(), glyph.getY());
        juce::Path hex;
        hex.startNewSubPath (0.50f, 0.05f); hex.lineTo (0.92f, 0.28f); hex.lineTo (0.92f, 0.72f);
        hex.lineTo (0.50f, 0.95f); hex.lineTo (0.08f, 0.72f); hex.lineTo (0.08f, 0.28f); hex.closeSubPath();
        juce::Path chv;
        chv.startNewSubPath (0.28f, 0.64f); chv.lineTo (0.50f, 0.41f); chv.lineTo (0.72f, 0.64f);
        chv.startNewSubPath (0.36f, 0.73f); chv.lineTo (0.50f, 0.58f); chv.lineTo (0.64f, 0.73f);
        hex.applyTransform (xf); chv.applyTransform (xf);

        g.setColour (brand::structuralForge().withAlpha (0.12f));
        g.fillPath (hex);
        g.setColour (brand::structuralForge().withAlpha (0.55f));
        g.strokePath (hex, juce::PathStrokeType (1.0f));
        g.setColour (brand::structuralForge());
        g.strokePath (chv, juce::PathStrokeType (juce::jmax (1.0f, glyph.getWidth() * 0.07f),
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    // Title bar -- 44 px tall, brand-orange 3 px stripe down the left
    // edge, forge-mark badge, then section-title text in textPrimary.
    // Returns the rect the title bar occupied so the caller can measure
    // remaining space.
    inline juce::Rectangle<int> paintTitle (juce::Graphics& g,
                                            juce::Component& host,
                                            const juce::String& title) noexcept
    {
        auto bar = host.getLocalBounds().removeFromTop (titleH)
                                        .reduced (brand::space::md, 0);
        g.setColour (brand::brandOrange);
        g.fillRect (bar.removeFromLeft (stripeW));
        bar.removeFromLeft (brand::space::md);

        // Forge-mark badge, vertically centred in the title bar.
        const float badgeSz = 26.0f;
        auto badge = bar.removeFromLeft ((int) badgeSz).toFloat()
                        .withSizeKeepingCentre (badgeSz, badgeSz);
        drawForgeBadge (g, badge);
        bar.removeFromLeft (brand::space::sm);

        g.setColour (brand::textPrimary);
        g.setFont (brand::type::sectionTitle());
        g.drawText (title.toUpperCase(), bar,
                    juce::Justification::centredLeft, false);
        return host.getLocalBounds().removeFromTop (titleH);
    }

    // Footer divider -- horizontal hairline brand::edge just above the
    // footer button row. paintTitle + paintFooterDivider together
    // bracket the dialog's working area.
    inline void paintFooterDivider (juce::Graphics& g, juce::Component& host) noexcept
    {
        g.setColour (brand::edge);
        g.drawHorizontalLine (host.getHeight() - footerH, 0.0f, (float) host.getWidth());
    }

    // One-call chrome -- call this from any dialog's paint(). Equivalent
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

    // Apply / primary action button -- accentStatus background, legible
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

    // Cancel / secondary action button -- bgElevated background,
    // textSecondary foreground. Matches AudioDeviceDialog Cancel.
    inline void styleSecondary (juce::TextButton& b) noexcept
    {
        b.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
        b.setColour (juce::TextButton::textColourOffId, brand::textSecondary);
        b.setColour (juce::TextButton::textColourOnId,  brand::textSecondary);
    }

    // Standard combo / text-editor / numeric-field treatment -- same
    // dark deep field with edge outline that AudioDeviceDialog uses
    // on every input control.
    inline void styleCombo (juce::ComboBox& c) noexcept
    {
        c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep);
        c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
        c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
        c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
    }
    // Prime an AlertWindow's text editor so the engineer can start
    // typing immediately when the dialog opens: all text selected,
    // keyboard focus on the field. Without the deferred callAsync
    // the focus grab happens before AlertWindow finishes its own
    // layout, gets swallowed, and the engineer still has to click.
    // Pass the AlertWindow and the editor's id (as passed to
    // addTextEditor). No-op if the editor isn't found.
    // okResultCode is the modal result the dialog's primary button returns
    // (every prompt in this app uses 1 for OK / Apply / Save / Rename, so it
    // defaults to 1). Pressing Return in the field exits the dialog with that
    // code -- a single-line TextEditor swallows Return, so without this the
    // dialog's return-key button never fires and Enter "does nothing".
    inline void primeNameEditor (juce::AlertWindow& aw, const juce::String& id,
                                 int okResultCode = 1) noexcept
    {
        if (auto* ed = aw.getTextEditor (id))
        {
            ed->setSelectAllWhenFocused (true);
            // Enter in the field = hit the primary button.
            ed->onReturnKey = [safeAw = juce::Component::SafePointer<juce::AlertWindow> (&aw),
                               okResultCode]
            {
                if (safeAw != nullptr) safeAw->exitModalState (okResultCode);
            };
            juce::MessageManager::callAsync (
                [safe = juce::Component::SafePointer<juce::TextEditor> (ed)]
                {
                    if (safe != nullptr) safe->grabKeyboardFocus();
                });
        }
    }

    inline void styleTextEditor (juce::TextEditor& t) noexcept
    {
        t.setColour (juce::TextEditor::backgroundColourId,     brand::bgDeep);
        t.setColour (juce::TextEditor::textColourId,           brand::textPrimary);
        t.setColour (juce::TextEditor::outlineColourId,        brand::edge);
        t.setColour (juce::TextEditor::focusedOutlineColourId, brand::accentStatus);
        t.setColour (juce::TextEditor::highlightColourId,
                     brand::accentStatus.withAlpha (brand::alpha::dimmed));
    }
}
