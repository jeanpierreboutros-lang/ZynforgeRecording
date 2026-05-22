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

    void ZynForgeLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                                                bool over, bool down)
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);

        // Dark gradient pill body.
        const auto top = juce::Colour (0x35, 0x35, 0x39);
        const auto bot = juce::Colour (0x18, 0x18, 0x1c);
        g.setGradientFill (juce::ColourGradient (top, r.getCentreX(), r.getY(),
                                                 bot, r.getCentreX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 4.0f);

        if (down)
            g.setColour (juce::Colours::white.withAlpha (0.06f));
        else if (over)
            g.setColour (juce::Colours::white.withAlpha (0.03f));
        else
            g.setColour (juce::Colours::transparentBlack);
        g.fillRoundedRectangle (r, 4.0f);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);

        // Letter / label — coloured by toggle state.
        const auto active = b.findColour (juce::ToggleButton::tickColourId);
        const auto txt    = b.getToggleState() ? active : brand::textMuted;
        g.setColour (txt);
        g.setFont (juce::Font (juce::FontOptions().withHeight (11.5f).withStyle ("Bold")));
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

        // Green fill from the thumb down to the bottom.
        const float thumbY = juce::jlimit (trackY, trackB, sliderPos);
        g.setColour (brand::accentPlay);
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

        // Body
        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0x33, 0x35, 0x3c), thumb.getCentreX(), thumb.getY(),
            juce::Colour (0x1c, 0x1e, 0x23), thumb.getCentreX(), thumb.getBottom(),
            false));
        g.fillRoundedRectangle (thumb, 3.0f);

        g.setColour (juce::Colour (0x55, 0x57, 0x60));
        g.drawRoundedRectangle (thumb, 3.0f, 1.0f);

        // Grip lines.
        g.setColour (juce::Colour (0xa0, 0xa3, 0xad));
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
