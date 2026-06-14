#include "ZynForgeLookAndFeel.h"
#include "BrandColors.h"
#include "BrandTokens.h"
#include "DialogChrome.h"
#include "BinaryData.h"

namespace zynforge
{
    namespace
    {
        // The ZynForge forge-mark badge -- the app-icon glyph (forged-steel
        // squircle + orange hex + rising double-chevron). Drawn programmatically
        // so it scales crisply at any size and needs no embedded PNG. Used as the
        // icon on every prompt in place of JUCE's stock warning/question glyph.
        void drawForgeBadge (juce::Graphics& g, juce::Rectangle<float> r)
        {
            // Forged-steel squircle plate.
            g.setColour (brand::controlBg);
            g.fillRoundedRectangle (r, brand::radius::lg);
            g.setColour (brand::gloss (0.10f));
            g.drawRoundedRectangle (r.reduced (0.5f), brand::radius::lg, 1.0f);

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
    }

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
        // Prompts read as neutral grey (bgElevated), not the blue-tinted
        // bgPanel/bgDeep -- so dialogs match the rest of the app.
        setColour (juce::AlertWindow::backgroundColourId,     brand::bgElevated);
        setColour (juce::AlertWindow::textColourId,           brand::textPrimary);
        setColour (juce::AlertWindow::outlineColourId,        brand::borderSubtle);
    }

    juce::Typeface::Ptr ZynForgeLookAndFeel::getTypefaceForFont (const juce::Font& f)
    {
        // Load Inter / JetBrains Mono from the BinaryData target the
        // moment any font requests those typeface names, then hold them
        // as MEMBERS (not function-local statics). Members die with the
        // LookAndFeel -- which MainComponent owns and destroys during
        // shutdown(), while CoreText is still alive. A function-local
        // static Typeface::Ptr would instead survive to __cxa_finalize
        // and run its CoreText destructor after the framework is gone,
        // crashing the process on quit. Called on the message thread
        // only (paint), so the lazy init needs no lock.
        if (interReg == nullptr)
        {
            interReg  = juce::Typeface::createSystemTypefaceFor (
                BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize);
            interBold = juce::Typeface::createSystemTypefaceFor (
                BinaryData::InterBold_ttf,    BinaryData::InterBold_ttfSize);
            monoReg   = juce::Typeface::createSystemTypefaceFor (
                BinaryData::JetBrainsMonoRegular_ttf, BinaryData::JetBrainsMonoRegular_ttfSize);
            monoBold  = juce::Typeface::createSystemTypefaceFor (
                BinaryData::JetBrainsMonoBold_ttf,    BinaryData::JetBrainsMonoBold_ttfSize);
        }

        const auto name  = f.getTypefaceName();
        const bool bold  = f.isBold();

        // brand::uiFamily resolves to 'Inter' in BrandTokens.h. The
        // 'SF Pro' / 'SF Mono' fallbacks keep any legacy call sites
        // (pre-2026-05-23 rename) wiring through to the bundled face.
        if (name.containsIgnoreCase ("inter") || name == "SF Pro")
            return bold ? interBold : interReg;
        if (name.containsIgnoreCase ("jetbrains")
            || name.containsIgnoreCase ("mono")
            || name == "SF Mono")
            return bold ? monoBold : monoReg;

        return juce::LookAndFeel_V4::getTypefaceForFont (f);
    }

    juce::Font ZynForgeLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
    {
        return brand::type::ui ((float) juce::jmin (16, h - 8), true);
    }

    void ZynForgeLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                                    const juce::Colour& bg,
                                                    bool over, bool down)
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        auto base = bg;
        if (down) base = base.brighter (0.15f);
        else if (over) base = base.brighter (0.07f);

        // Pronounced top-to-bottom gradient -- top is noticeably lighter
        // and bottom darker than the base so the button reads as a
        // glossy pill rather than a flat tinted rectangle.
        const auto top = base.brighter (down ? 0.08f : 0.16f);
        const auto bot = base.darker   (down ? 0.05f : 0.18f);
        g.setGradientFill (juce::ColourGradient (
            top, r.getCentreX(), r.getY(),
            bot, r.getCentreX(), r.getBottom(),
            false));
        g.fillRoundedRectangle (r, brand::radius::md);

        // Faint top sheen -- matte pill, like the mock (not glossy).
        auto hi = r.withTrimmedBottom (r.getHeight() * 0.55f);
        g.setGradientFill (juce::ColourGradient (
            juce::Colours::white.withAlpha (down ? 0.04f : 0.06f),
                hi.getCentreX(), hi.getY(),
            juce::Colours::white.withAlpha (0.0f),
                hi.getCentreX(), hi.getBottom(),
            false));
        g.fillRoundedRectangle (hi, brand::radius::md);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
    }

    void ZynForgeLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                                                bool over, bool down)
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        const bool on = b.getToggleState();

        // When toggled on the button glows in the colour pinned via
        // ToggleButton::buttonOnColourId (R → red, I → green, M → red,
        // S → yellow). Off state stays the dark control-bg pill.
        if (on)
        {
            // ToggleButton has no buttonOnColourId -- repurpose
            // tickColourId (already set per button: R red, I green,
            // M red, S yellow) as the active fill source.
            const auto base = b.findColour (juce::ToggleButton::tickColourId);
            const auto top  = base.brighter (down ? 0.50f : 0.30f);
            const auto bot  = base.darker   (down ? 0.10f : 0.30f);
            g.setGradientFill (juce::ColourGradient (top, r.getCentreX(), r.getY(),
                                                     bot, r.getCentreX(), r.getBottom(),
                                                     false));
            g.fillRoundedRectangle (r, brand::radius::md);

            // Soft inner highlight at the top so the pill has depth.
            auto hi = r.withTrimmedBottom (r.getHeight() * 0.55f);
            g.setGradientFill (juce::ColourGradient (
                juce::Colours::white.withAlpha (0.20f), hi.getCentreX(), hi.getY(),
                juce::Colours::white.withAlpha (0.0f),  hi.getCentreX(), hi.getBottom(),
                false));
            g.fillRoundedRectangle (hi, brand::radius::md);

            g.setColour (base.darker (0.55f));
            g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
        }
        else
        {
            const float lift = down ? 0.18f : (over ? 0.08f : 0.0f);
            const auto  base = brand::controlBg.brighter (lift);
            g.setGradientFill (brand::verticalGradient (base, r, 0.20f, 0.30f));
            g.fillRoundedRectangle (r, brand::radius::md);
            g.setColour (brand::edge);
            g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
        }

        // Letter -- white-ish when on for max contrast against the
        // saturated background; muted grey when off.
        const auto txt = on ? juce::Colours::white.withAlpha (0.95f)
                            : brand::textMuted;
        g.setColour (txt);
        // Heavier, slightly larger glyph than the default uiLabel so
        // the single-letter chips read at a glance.
        g.setFont (brand::type::ui (13.5f, true));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
    }

    void ZynForgeLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert,
                                            const juce::Rectangle<int>& textArea,
                                            juce::TextLayout& textLayout)
    {
        // macOS-native-style alert chrome. Two-tone body:
        //  - upper area (message)            : bgPanel (lighter)
        //  - lower footer area (buttons)     : bgDeep   (darker)
        // No icon column (engineers don't need a question mark to
        // tell them this is a dialog -- removing it via NoIcon at
        // call sites, the textArea now spans the full width).
        // No brand-orange title stripe -- the message itself is the
        // header, bold and large; the body text follows underneath.

        const auto full = alert.getLocalBounds().toFloat();

        // Whole panel base -- neutral grey (bgElevated), matching the rest
        // of the app's prompts. NOT the blue-tinted bgPanel.
        g.fillAll (brand::bgElevated);

        // Footer area -- last 64 px shaded a touch darker (still neutral
        // grey) for visual weight.
        const float footerH = 64.0f;
        const auto footerRect = full.withTop (full.getBottom() - footerH);
        g.setColour (brand::bgElevated.darker (0.30f));
        g.fillRect (footerRect);

        // Hairline divider between message body and footer.
        g.setColour (brand::borderSubtle);
        g.drawHorizontalLine ((int) footerRect.getY(),
                              full.getX(), full.getRight());

        // ZynForge forge-mark badge. JUCE reserves an 80px icon column in the
        // window WIDTH for any non-NoIcon alert (juce_AlertWindow: iconSpace =
        // iconWidth) but leaves the icon + text positioning to the LookAndFeel:
        // it lays the text out left-justified and the LAF draws text at
        // x = iconSpaceUsed. So we mirror that exactly -- draw the forge-mark in
        // the left column (replacing JUCE's stock triangle / question glyph) and
        // draw the text at x = iconSpace, where it was sized to fit (never
        // clips). NoIcon prompts keep their centred full-width text + no glyph.
        const bool hasIcon  = alert.getAlertType() != juce::MessageBoxIconType::NoIcon;
        const int  iconSpace = hasIcon ? 80 : 0;

        if (hasIcon)
        {
            const float badge = 46.0f;
            drawForgeBadge (g, juce::Rectangle<float> (full.getX() + 18.0f,
                                                       full.getY() + 26.0f,
                                                       badge, badge));
        }

        // Text positioned exactly as JUCE's LookAndFeel_V4::drawAlertBox does
        // (origin x = iconSpace, y = 30, full width) so the pre-sized layout
        // lands without re-flow or clipping.
        juce::ignoreUnused (textArea);
        const juce::Rectangle<int> alertBounds (
            (int) full.getX() + iconSpace, 30,
            (int) full.getWidth(),
            (int) full.getHeight() - getAlertWindowButtonHeight() - 20);
        textLayout.draw (g, alertBounds.toFloat());
    }

    int ZynForgeLookAndFeel::getAlertBoxWindowFlags()
    {
        return juce::ComponentPeer::windowAppearsOnTaskbar
             | juce::ComponentPeer::windowHasDropShadow;
    }

    juce::Array<int> ZynForgeLookAndFeel::getWidthsForTextButtons (
        juce::AlertWindow&, const juce::Array<juce::TextButton*>& btns)
    {
        // Keep alert buttons neutral grey + light grey (no coloured
        // accent) per the prompt-colour rule. The first/default button
        // gets a slightly brighter grey for subtle emphasis; the rest are
        // the standard secondary grey.
        for (int i = 0; i < btns.size(); ++i)
        {
            if (auto* b = btns[i])
            {
                dialog::styleSecondary (*b);
                if (i == 0)
                {
                    b->setColour (juce::TextButton::buttonColourId, brand::bgElevated.brighter (0.12f));
                    b->setColour (juce::TextButton::textColourOffId, brand::textPrimary);
                    b->setColour (juce::TextButton::textColourOnId,  brand::textPrimary);
                }
            }
        }
        juce::Array<int> widths;
        for (int i = 0; i < btns.size(); ++i)
            widths.add (i == 0 ? dialog::btnPrimary : dialog::btnSecond);
        return widths;
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

        // Fill from the thumb down to the bottom -- gradient in the
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

        // Mock cap (Direction C): a near-square machined-steel cap, slightly
        // TALLER than wide, with rounded shoulders and a single VERTICAL
        // forge-orange groove milled down its centre. Reads as a forged steel
        // handle, not a moulded pill.
        const float thumbW = juce::jmin (30.0f, bounds.getWidth() - 2.0f);
        const float thumbH = 36.0f;
        const auto  thumb  = juce::Rectangle<float> (
                                bounds.getCentreX() - thumbW * 0.5f,
                                thumbY - thumbH * 0.5f,
                                thumbW, thumbH);
        const float capR = 6.0f;

        // Drop shadow lifts the cap off the track.
        g.setColour (brand::shadow::elev2());
        g.fillRoundedRectangle (thumb.translated (0.0f, 2.0f).expanded (1.0f, 1.0f), capR);

        // Steel body -- vertical gradient, lighter at the top.
        g.setGradientFill (juce::ColourGradient (
            brand::faderThumbHi.brighter (0.06f), thumb.getCentreX(), thumb.getY(),
            brand::faderThumbLo,                  thumb.getCentreX(), thumb.getBottom(),
            false));
        g.fillRoundedRectangle (thumb, capR);

        // Inner top highlight + outer edge -- the bevel.
        g.setColour (brand::gloss (0.30f));
        g.drawRoundedRectangle (thumb.reduced (1.0f).withTrimmedBottom (thumb.getHeight() * 0.45f),
                                capR - 1.0f, 1.0f);
        g.setColour (brand::faderThumbEdge);
        g.drawRoundedRectangle (thumb, capR, 1.0f);

        // Signature: a single VERTICAL forge groove down the centre of the cap.
        {
            const float grooveW = 3.0f;
            const float inset   = 6.0f;
            auto groove = juce::Rectangle<float> (thumb.getCentreX() - grooveW * 0.5f,
                                                  thumb.getY() + inset,
                                                  grooveW, thumb.getHeight() - inset * 2.0f);
            // STRUCTURAL identity orange (not meterHot): the cap groove is
            // permanent chrome on every fader, so it must NOT borrow the
            // meter's "you're peaking" colour -- meterHot is reserved for live
            // signal so a hot meter still escalates beyond the resting UI.
            g.setColour (brand::structuralForge().withAlpha (0.32f));      // glow
            g.fillRoundedRectangle (groove.expanded (2.0f, 1.0f), 2.0f);
            g.setColour (brand::structuralForge().withAlpha (0.78f));      // forge line (subdued at rest)
            g.fillRoundedRectangle (groove, 1.5f);
        }

        // Milled top sheen -- thin specular line along the cap's top edge.
        g.setColour (brand::gloss (0.40f));
        g.drawHorizontalLine ((int) (thumb.getY() + 2.0f),
                              thumb.getX() + 4.0f, thumb.getRight() - 4.0f);
    }

    void ZynForgeLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                                float pos, float startA, float endA,
                                                juce::Slider& s)
    {
        const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
        const float d      = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto  centre = bounds.getCentre();
        const float radius = d * 0.5f;
        const float angle  = startA + pos * (endA - startA);
        const auto  fill   = s.findColour (juce::Slider::rotarySliderFillColourId);

        // Recessed seat under the knob.
        g.setColour (brand::shadow::elev2());
        g.fillEllipse (juce::Rectangle<float> (d + 3.0f, d + 3.0f).withCentre (centre.translated (0.0f, 1.5f)));

        // Machined-steel knob body -- radial gradient lit from top-left.
        juce::ColourGradient body (brand::faderThumbHi.brighter (0.10f),
                                   centre.x - radius * 0.4f, centre.y - radius * 0.5f,
                                   brand::faderThumbLo.darker (0.20f),
                                   centre.x + radius * 0.4f, centre.y + radius * 0.6f, true);
        g.setGradientFill (body);
        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (centre));

        // Rim.
        g.setColour (brand::faderThumbEdge);
        g.drawEllipse (juce::Rectangle<float> (d, d).withCentre (centre), 1.0f);
        // Top specular arc.
        g.setColour (brand::gloss (0.22f));
        g.drawEllipse (juce::Rectangle<float> (d - 4.0f, d - 4.0f).withCentre (centre.translated (0.0f, -0.5f)), 1.0f);

        // Forge-orange pointer line from mid-radius to the edge.
        const float p0 = radius * 0.42f, p1 = radius * 0.92f;
        juce::Point<float> a (centre.x + std::sin (angle) * p0, centre.y - std::cos (angle) * p0);
        juce::Point<float> b (centre.x + std::sin (angle) * p1, centre.y - std::cos (angle) * p1);
        g.setColour (fill.withMultipliedBrightness (1.15f));
        g.drawLine ({ a, b }, 2.6f);
        // Bright tip dot.
        g.fillEllipse (juce::Rectangle<float> (3.2f, 3.2f).withCentre (b));
    }
}
