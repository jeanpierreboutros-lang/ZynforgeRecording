#include "ZynForgeLookAndFeel.h"
#include "BrandColors.h"
#include "BrandTokens.h"

namespace zynforge
{
    ZynForgeLookAndFeel::ZynForgeLookAndFeel()
    {
        // Per Live's design system: bg-elevated (#303030) is the default
        // chrome colour for buttons / combos. The Recording app was using
        // bg-strip for these, which made buttons read as part of the dark
        // panel instead of standing forward.
        setColour (juce::ResizableWindow::backgroundColourId, brand::bgDeep);
        setColour (juce::DocumentWindow::backgroundColourId,  brand::bgDeep);
        setColour (juce::Label::textColourId,                 brand::textPrimary);
        setColour (juce::TextButton::buttonColourId,          brand::bgElevated);
        setColour (juce::TextButton::textColourOnId,          brand::textPrimary);
        setColour (juce::TextButton::textColourOffId,         brand::textPrimary);
        setColour (juce::ComboBox::backgroundColourId,        brand::bgElevated);
        setColour (juce::ComboBox::textColourId,              brand::textPrimary);
        setColour (juce::ComboBox::outlineColourId,           brand::edge);
        setColour (juce::PopupMenu::backgroundColourId,       brand::bgElevated);
        setColour (juce::PopupMenu::textColourId,             brand::textPrimary);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, brand::edge);
        setColour (juce::AlertWindow::backgroundColourId,     brand::bgPanel);
        setColour (juce::AlertWindow::textColourId,           brand::textPrimary);
        setColour (juce::AlertWindow::outlineColourId,        brand::edge);
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
        auto base = bg;
        if (down) base = base.brighter (0.15f);
        else if (over) base = base.brighter (0.07f);

        // Every button surface gets a subtle vertical gradient so the chrome
        // reads as 3D, matching ZynForge Live's button finish.
        g.setGradientFill (brand::verticalGradient (base, r));
        g.fillRoundedRectangle (r, brand::radius::md);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
    }

    void ZynForgeLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                                                bool over, bool down)
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);

        // Pill body — dark gradient, brightens on hover / press.
        const float lift   = down ? 0.18f : (over ? 0.08f : 0.0f);
        const auto  base   = brand::controlBg.brighter (lift);
        g.setGradientFill (brand::verticalGradient (base, r, 0.20f, 0.30f));
        g.fillRoundedRectangle (r, brand::radius::md);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);

        // Letter / label — coloured by toggle state.
        const auto active = b.findColour (juce::ToggleButton::tickColourId);
        const auto txt    = b.getToggleState() ? active : brand::textMuted;
        g.setColour (txt);
        g.setFont (brand::type::uiLabel());
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
    }

    void ZynForgeLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                                float sliderPos, float /*minPos*/, float /*maxPos*/,
                                                juce::Slider::SliderStyle style, juce::Slider& s)
    {
        const juce::Rectangle<float> bounds ((float) x, (float) y, (float) w, (float) h);

        if (style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearBar)
        {
            // Pan-style horizontal: thin trough + small centred indicator.
            auto track = bounds.withSizeKeepingCentre ((float) w - 4.0f, 4.0f);
            g.setColour (brand::bgDeep);
            g.fillRoundedRectangle (track, 2.0f);
            g.setColour (brand::edge);
            g.drawRoundedRectangle (track, 2.0f, 1.0f);

            // Centre tick.
            g.setColour (brand::textMuted);
            const float cx = track.getCentreX();
            g.drawVerticalLine ((int) cx, track.getY() - 2.0f, track.getBottom() + 2.0f);

            const float thumbW = 8.0f;
            const float thumbH = (float) h - 6.0f;
            g.setColour (s.findColour (juce::Slider::thumbColourId));
            g.fillRoundedRectangle (sliderPos - thumbW * 0.5f,
                                    bounds.getCentreY() - thumbH * 0.5f,
                                    thumbW, thumbH, 2.0f);
            return;
        }

        // Vertical fader (the channel-strip gain fader).
        const float trackW   = 12.0f;
        const float trackX   = bounds.getCentreX() - trackW * 0.5f;
        const float trackY   = bounds.getY() + 6.0f;
        const float trackB   = bounds.getBottom() - 6.0f;
        const float trackH   = trackB - trackY;

        // Background track.
        g.setColour (brand::bgDeep);
        g.fillRoundedRectangle (trackX, trackY, trackW, trackH, 2.0f);

        // Fill from the thumb down to the bottom — gradient in the
        // channel's colour (taken from the slider's thumbColourId).
        const float thumbY  = juce::jlimit (trackY, trackB, sliderPos);
        const auto  baseCol = s.findColour (juce::Slider::thumbColourId);
        g.setGradientFill (juce::ColourGradient (
            baseCol.brighter (0.25f), trackX, thumbY,
            baseCol.darker  (0.30f), trackX, trackB,
            false));
        g.fillRoundedRectangle (trackX, thumbY, trackW, trackB - thumbY, 2.0f);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (trackX, trackY, trackW, trackH, 2.0f, 1.0f);

        // Long thumb with grip lines and a brighter centre stripe.
        const float thumbW = juce::jmin (28.0f, bounds.getWidth() - 4.0f);
        const float thumbH = 38.0f;
        const auto  thumb  = juce::Rectangle<float> (
                                bounds.getCentreX() - thumbW * 0.5f,
                                thumbY - thumbH * 0.5f,
                                thumbW, thumbH);

        // Body — gradient between the two fader-thumb tokens.
        g.setGradientFill (juce::ColourGradient (
            brand::faderThumbHi, thumb.getCentreX(), thumb.getY(),
            brand::faderThumbLo, thumb.getCentreX(), thumb.getBottom(),
            false));
        g.fillRoundedRectangle (thumb, brand::radius::sm);

        g.setColour (brand::faderThumbEdge);
        g.drawRoundedRectangle (thumb, brand::radius::sm, 1.0f);

        // Grip lines.
        g.setColour (brand::faderThumbGrip);
        const int   numGrips = 5;
        const float gripPad  = 6.0f;
        for (int i = 0; i < numGrips; ++i)
        {
            const float ly = thumb.getY() + 8.0f + (float) i * 4.5f;
            g.drawHorizontalLine ((int) ly,
                                  thumb.getX() + gripPad,
                                  thumb.getRight() - gripPad);
        }

        // Centre highlight stripe.
        g.setColour (juce::Colours::white);
        const float stripeH = 3.0f;
        g.fillRect (juce::Rectangle<float> (thumb.getX() + 2.0f,
                                            thumb.getCentreY() - stripeH * 0.5f,
                                            thumb.getWidth() - 4.0f, stripeH));
    }
}
