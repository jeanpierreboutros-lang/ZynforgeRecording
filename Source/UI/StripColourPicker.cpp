#include "StripColourPicker.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace zynforge
{
    StripColourPicker::StripColourPicker (juce::Colour currentColour, Callback cb)
        : original (currentColour), pending (currentColour), callback (std::move (cb))
    {
        buildGradient();

        auto styleButton = [] (juce::TextButton& b, juce::Colour bg)
        {
            b.setColour (juce::TextButton::buttonColourId, bg);
            b.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
        };

        // OK -- confirm the live-previewed selection and close. The colour is
        // already applied (each swatch click previews it on the strip), so OK
        // simply re-applies the pending colour and dismisses the call-out.
        styleButton (okButton, brand::featureEngaged.withAlpha (brand::alpha::muted));
        okButton.onClick = [this]
        {
            if (callback) callback (pending);
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        };
        addAndMakeVisible (okButton);

        // Default -- revert to the neutral-grey default colour (alpha 0).
        styleButton (resetButton, brand::bgElevated);
        resetButton.onClick = [this]
        {
            if (callback) callback (juce::Colour ((juce::uint32) 0));
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        };
        addAndMakeVisible (resetButton);

        const int width  = kMargin * 2 + kCols * kSize + (kCols - 1) * kGap;
        const int gridH  = kRows * kSize + (kRows - 1) * kGap;
        const int height = kMargin * 2 + gridH + kGap + kBtnH;
        setSize (width, height);
    }

    void StripColourPicker::buildGradient()
    {
        // [0] = the strip's current / original colour, so the engineer always
        // has the default-of-the-original one click away. The rest form a 2-D
        // gradient: hue sweeps left→right across the columns, and each row
        // steps lighter→darker so warm/cool tints at several shades are all
        // reachable without opening the custom selector.
        presets.assign ((std::size_t) (kCols * kRows), juce::Colour());
        presets[0] = original.withAlpha (1.0f);
        for (int i = 1; i < kCols * kRows; ++i)
        {
            const int   col   = i % kCols;
            const int   row   = i / kCols;
            const float hue   = (float) col / (float) (kCols - 1);
            const float shade = (kRows > 1) ? (float) row / (float) (kRows - 1) : 0.0f;
            const float sat   = juce::jlimit (0.0f, 1.0f, 0.52f + 0.40f * shade);
            const float bri   = juce::jlimit (0.0f, 1.0f, 0.98f - 0.52f * shade);
            presets[(std::size_t) i] = juce::Colour::fromHSV (hue, sat, bri, 1.0f);
        }
    }

    juce::Rectangle<int> StripColourPicker::swatchBounds (int i) const
    {
        const int col = i % kCols;
        const int row = i / kCols;
        return { kMargin + col * (kSize + kGap),
                 kMargin + row * (kSize + kGap),
                 kSize, kSize };
    }

    void StripColourPicker::resized()
    {
        auto r = getLocalBounds().reduced (kMargin);
        r.removeFromTop (kRows * kSize + (kRows - 1) * kGap);
        r.removeFromTop (kGap);
        auto btnRow = r.removeFromTop (kBtnH);
        const int w = (btnRow.getWidth() - kGap) / 2;
        resetButton.setBounds (btnRow.removeFromLeft (w));
        btnRow.removeFromLeft (kGap);
        okButton   .setBounds (btnRow);
    }

    void StripColourPicker::paint (juce::Graphics& g)
    {
        auto fr = getLocalBounds().toFloat();
        g.setGradientFill (brand::verticalGradient (brand::bgPanel, fr, 0.10f, 0.16f));
        g.fillRect (fr);
        g.setColour (brand::edge);
        g.drawRect (getLocalBounds(), 1);

        for (int i = 0; i < (int) presets.size(); ++i)
        {
            auto r = swatchBounds (i).toFloat();
            g.setColour (presets[(std::size_t) i]);
            g.fillRoundedRectangle (r, brand::radius::md);

            // The currently-pending (live-previewed) swatch gets the accent
            // selection ring; swatch 0 (the original) carries a thin inner
            // marker so it reads as "the original colour" even when another
            // swatch is selected.
            const bool isPending = (presets[(std::size_t) i].withAlpha (1.0f).getARGB()
                                    == pending.withAlpha (1.0f).getARGB());
            if (i == 0 && ! isPending)
            {
                g.setColour (brand::gloss (brand::alpha::muted));
                g.drawRoundedRectangle (r.reduced (3.0f), brand::radius::sm, 1.0f);
            }
            g.setColour (isPending ? brand::accentStatus : brand::edge);
            g.drawRoundedRectangle (r, brand::radius::md, isPending ? 2.5f : 1.0f);
        }
    }

    void StripColourPicker::mouseDown (const juce::MouseEvent& e)
    {
        for (int i = 0; i < (int) presets.size(); ++i)
        {
            if (swatchBounds (i).contains (e.getPosition()))
            {
                pending = presets[(std::size_t) i];
                if (callback) callback (pending);   // live preview on the strip
                repaint();
                return;
            }
        }
    }
}
