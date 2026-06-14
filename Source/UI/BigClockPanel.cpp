#include "BigClockPanel.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

// ── Heated Steel (Direction C) -- forge-mark glyph ────────────────────
namespace
{
    namespace steel
    {
        inline void drawForgeMark (juce::Graphics& g, juce::Rectangle<float> r, bool hot)
        {
            const auto orange = zynforge::brand::brandOrange;
            const auto xf = juce::AffineTransform::scale (r.getWidth(), r.getHeight()).translated (r.getX(), r.getY());
            juce::Path hex;
            hex.startNewSubPath (0.50f, 0.05f); hex.lineTo (0.92f, 0.28f); hex.lineTo (0.92f, 0.72f);
            hex.lineTo (0.50f, 0.95f); hex.lineTo (0.08f, 0.72f); hex.lineTo (0.08f, 0.28f); hex.closeSubPath();
            juce::Path chv;
            chv.startNewSubPath (0.28f, 0.64f); chv.lineTo (0.50f, 0.41f); chv.lineTo (0.72f, 0.64f);
            chv.startNewSubPath (0.36f, 0.73f); chv.lineTo (0.50f, 0.58f); chv.lineTo (0.64f, 0.73f);
            hex.applyTransform (xf); chv.applyTransform (xf);
            g.setColour (hot ? orange.withAlpha (zynforge::brand::alpha::chrome) : zynforge::brand::debossInk.withAlpha (0.28f)); g.fillPath (hex);
            g.setColour (hot ? orange.withAlpha (zynforge::brand::alpha::muted) : zynforge::brand::gloss (0.10f)); g.strokePath (hex, juce::PathStrokeType (1.0f));
            g.setColour (hot ? orange : zynforge::brand::gloss (0.24f));
            g.strokePath (chv, juce::PathStrokeType (juce::jmax (1.0f, r.getWidth() * 0.07f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            const float s = r.getWidth() * 0.05f;
            g.setColour (hot ? orange : zynforge::brand::gloss (0.30f));
            g.fillEllipse (r.getCentreX() - s, r.getY() + r.getHeight() * 0.27f - s, s * 2.0f, s * 2.0f);
        }
    }
}

namespace zynforge
{
    BigClockPanel::BigClockPanel() = default;

    void BigClockPanel::setMode (Mode m)
    {
        if (mode != m)
        {
            const bool igniting = (m == Mode::Recording && mode != Mode::Recording);
            mode = m;
            if (igniting) igniteLevel = 0.0f;   // start cold, ramp the heat in
            syncPulseTimer(); repaint();
        }
    }

    void BigClockPanel::setElapsed (juce::int64 samples, double sr)
    {
        if (sr <= 0) sr = 48000.0;
        const auto totalSec = (juce::int64) (samples / sr);
        const auto h = totalSec / 3600;
        const auto m = (totalSec / 60) % 60;
        const auto s = totalSec % 60;
        auto text = juce::String::formatted ("%02lld:%02lld:%02lld", h, m, s);
        if (text != elapsedText) { elapsedText = text; repaint(); }
    }

    void BigClockPanel::setMarkers (int count)
    {
        if (count != markerCount) { markerCount = count; repaint(); }
    }

    void BigClockPanel::setDiskInfo (double gb, int ms, juce::int64 mis, double remainingSec)
    {
        // Called at the MainComponent timer rate (24 Hz). Without
        // change detection, the unconditional repaint here drives
        // the BigClock to repaint 24 times a second even when the
        // shown numbers haven't moved a single digit. Round each
        // value to the visible precision so trivial float jitter
        // doesn't defeat the check.
        const double newFreeGB    = std::round (gb * 10.0) / 10.0;   // 1 dp
        const int    newRemainSec = (int) std::round (remainingSec);
        const bool changed = newFreeGB    != freeGB
                          || ms           != lastWriteMs
                          || mis          != missed
                          || newRemainSec != (int) std::round (remainingSeconds);
        freeGB           = newFreeGB;
        lastWriteMs      = ms;
        missed           = mis;
        remainingSeconds = remainingSec;
        if (changed) repaint();
    }

    void BigClockPanel::setLoudness (float integ, float momentary, float tp)
    {
        // 0.1-unit change detection so meter jitter doesn't force repaints.
        auto q = [] (float v) { return std::round (v * 10.0f) / 10.0f; };
        const bool changed = q (integ)     != q (lufsIntegrated)
                          || q (momentary) != q (lufsMomentary)
                          || q (tp)        != q (truePeakDb);
        lufsIntegrated = integ;
        lufsMomentary  = momentary;
        truePeakDb     = tp;
        if (changed) repaint();
    }

    void BigClockPanel::setArmedReady (bool ready)
    {
        if (armedReady != ready) { armedReady = ready; syncPulseTimer(); repaint(); }
    }

    void BigClockPanel::syncPulseTimer()
    {
        const bool wantPulse = ! brand::reduceMotion() && ((mode == Mode::Recording) || armedReady);
        if (wantPulse)
        {
            if (! isTimerRunning())
                startTimerHz (30);
        }
        else
        {
            stopTimer();
            pulsePhase = 0.0f;
        }
    }

    void BigClockPanel::timerCallback()
    {
        // 1 Hz breathe: a full sine cycle every second. Phase wraps at
        // 2π so the float stays bounded over a long recording session.
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        pulsePhase += twoPi / 30.0f;
        if (pulsePhase > twoPi) pulsePhase -= twoPi;
        // Heat ignite: ramp 0→1 over ~9 frames (300ms) at record start.
        igniteLevel = juce::jlimit (0.0f, 1.0f,
                                    igniteLevel + (mode == Mode::Recording ? 1.0f : -1.0f) / 9.0f);
        repaint();
    }

    static juce::String formatRemaining (double seconds)
    {
        if (seconds <= 0 || std::isinf (seconds)) return "--";
        const auto h = (int) (seconds / 3600.0);
        const auto m = ((int) (seconds / 60.0)) % 60;
        if (h > 99) return ">99h";
        return juce::String::formatted ("%dh%02dm", h, m);
    }

    void BigClockPanel::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();

        // Background -- gradient fill in the state-tinted colour so the
        // hero panel reads as a glassy plate rather than a flat block.
        // While recording, the red tint breathes at 1 Hz so the engineer
        // can read 'rolling' from across the room without looking at
        // the timer value.
        juce::Colour bg = brand::bgPanel;
        const bool  rm     = brand::reduceMotion();
        const float pulse  = rm ? 0.5f : 0.5f + 0.5f * std::sin (pulsePhase);   // 0..1 (static under reduced-motion)
        const float ignite = rm ? 1.0f : igniteLevel;                          // full heat, no ramp
        if (mode == Mode::Recording)
            bg = brand::meterEmber.withAlpha ((0.14f + 0.10f * pulse) * ignite); // ember, breathing + igniting
        else if (mode == Mode::Playing) bg = brand::accentPlay.withAlpha (0.14f);
        auto bgRect = r.reduced (2.0f);
        g.setGradientFill (juce::ColourGradient (
            brand::lift (bg, 0.12f), bgRect.getCentreX(), bgRect.getY(),
            bg.darker   (0.18f), bgRect.getCentreX(), bgRect.getBottom(),
            false));
        g.fillRoundedRectangle (bgRect, brand::radius::lg);

        // Forge ember-underglow -- while recording, heat rises from the base of
        // the plate behind the timecode (breathing at 1 Hz), so the centrepiece
        // reads as metal being worked, not just a red lamp.
        if (mode == Mode::Recording)
        {
            g.setGradientFill (juce::ColourGradient (
                brand::meterHot.withAlpha (0.0f),                                bgRect.getCentreX(), bgRect.getCentreY(),
                brand::meterHot.withAlpha ((0.16f + 0.12f * pulse) * ignite), bgRect.getCentreX(), bgRect.getBottom(),
                false));
            g.fillRoundedRectangle (bgRect, brand::radius::lg);
        }

        // Soft top highlight -- reinforces the glass / depth feel and
        // makes the timer read as raised metal under stage lights.
        auto hi = bgRect.withTrimmedBottom (bgRect.getHeight() * 0.60f);
        g.setGradientFill (juce::ColourGradient (
            brand::gloss (0.06f), hi.getCentreX(), hi.getY(),
            brand::gloss (0.0f),  hi.getCentreX(), hi.getBottom(),
            false));
        g.fillRoundedRectangle (hi, brand::radius::lg);

        // Armed-but-not-rolling border. Uses signalArmedReady
        // (engagedAmber -- the BYPASS / LIVE / LOCK token) so the
        // 'primed but not active' colour doesn't collide with the
        // mute / brand-assertion claims on brandOrange. Pulses at
        // 1 Hz so the call-to-action draws the eye.
        if (armedReady && mode == Mode::Idle)
        {
            const float pulse = rm ? 0.5f : 0.5f + 0.5f * std::sin (pulsePhase);
            const auto  col   = brand::signalArmedReady().withMultipliedBrightness (0.85f + 0.25f * pulse);
            g.setColour (col);
            g.drawRoundedRectangle (r.reduced (2.0f), brand::radius::lg, 2.0f);
        }
        else
        {
            g.setColour (brand::edge);
            g.drawRoundedRectangle (r.reduced (2.0f), brand::radius::lg, 1.0f);
        }

        // ── Heated Steel: structural orange frame + forge-mark ─────────────
        g.setColour (brand::brandOrange.withAlpha (brand::alpha::muted));
        g.drawRoundedRectangle (r.reduced (3.0f), brand::radius::lg, 2.0f);
        // Orange corner brackets (top-left / top-right) -- target detail.
        {
            const float br = 20.0f, bw = 2.0f, off = 8.0f;
            g.setColour (brand::brandOrange.withAlpha (brand::alpha::prominent));
            // top-left
            g.fillRect (r.getX() + off, r.getY() + off, br, bw);
            g.fillRect (r.getX() + off, r.getY() + off, bw, br);
            // top-right
            g.fillRect (r.getRight() - off - br, r.getY() + off, br, bw);
            g.fillRect (r.getRight() - off - bw, r.getY() + off, bw, br);
        }
        steel::drawForgeMark (g, { r.getX() + 14.0f, r.getBottom() - 34.0f, 24.0f, 24.0f },
                              mode == Mode::Recording);

        auto inner = r.reduced (16.0f, 10.0f);

        // Left: state + lamp
        auto left = inner.removeFromLeft (juce::jmin (220.0f, inner.getWidth() * 0.30f));
        {
            juce::Colour lamp = brand::textMuted;
            juce::String label = "IDLE";
            if (mode == Mode::Recording) { lamp = brand::accentRecord; label = "REC"; }
            else if (mode == Mode::Playing) { lamp = brand::accentPlay;  label = "PLAY"; }

            const float lampRadius = 9.0f;
            g.setColour (lamp);
            g.fillEllipse (left.getX(), left.getY() + 14.0f, lampRadius * 2.0f, lampRadius * 2.0f);

            g.setColour (brand::textPrimary);
            g.setFont (brand::type::headline());
            g.drawText (label,
                        left.withTrimmedLeft (lampRadius * 2.0f + 12.0f),
                        juce::Justification::centredLeft, false);
        }

        // Right: disk/health column
        auto right = inner.removeFromRight (juce::jmin (260.0f, inner.getWidth() * 0.40f));
        {
            // Each row is [label left, value right]. Label uses the UI
            // font; value uses tabular mono so the digits don't dance
            // while the engineer's eye is on the timer.
            auto drawRow = [&] (juce::Rectangle<float> line,
                                const char* label, const juce::String& value,
                                juce::Colour labelCol = brand::textSecondary,
                                juce::Colour valueCol = brand::textPrimary)
            {
                g.setColour (labelCol);
                g.setFont (brand::type::caption());
                g.drawText (label, line, juce::Justification::topLeft, false);
                g.setColour (valueCol);
                g.setFont (brand::type::mono (11.0f, true));
                g.drawText (value, line, juce::Justification::topRight, false);
            };

            drawRow (right.removeFromTop (14.0f), "FREE",
                     juce::String::formatted ("%.1f GB", freeGB));
            drawRow (right.removeFromTop (14.0f), "RECORD TIME LEFT",
                     formatRemaining (remainingSeconds));
            drawRow (right.removeFromTop (14.0f), "LAST WRITE",
                     juce::String (lastWriteMs) + " ms");
            drawRow (right.removeFromTop (14.0f),
                     missed > 0 ? "MISSED SAMPLES" : "NO MISSED WRITES",
                     missed > 0 ? juce::String (missed) : juce::String(),
                     missed > 0 ? brand::accentRecord : brand::textSecondary,
                     missed > 0 ? brand::accentRecord : brand::textPrimary);
            drawRow (right.removeFromTop (14.0f), "MARKERS",
                     juce::String (markerCount));

            // Master loudness (BS.1770). Integrated program loudness +
            // true-peak; true-peak turns red as it approaches 0 dBTP.
            auto lufsStr = [] (float v)
            { return v <= -119.0f ? juce::String ("--") : juce::String (v, 1) + " LUFS"; };
            drawRow (right.removeFromTop (14.0f), "LOUDNESS (I)", lufsStr (lufsIntegrated));
            drawRow (right.removeFromTop (14.0f), "MOMENTARY",    lufsStr (lufsMomentary),
                     brand::textSecondary, brand::textPrimary);
            const bool tpHot = truePeakDb > -1.0f;
            drawRow (right.removeFromTop (14.0f), "TRUE PEAK",
                     truePeakDb <= -119.0f ? juce::String ("--")
                                           : juce::String (truePeakDb, 1) + " dBTP",
                     tpHot ? brand::accentRecord : brand::textSecondary,
                     tpHot ? brand::accentRecord : brand::textPrimary);
        }

        // Centre: huge timer. Tabular numerals -- the timer string can't
        // shift width as seconds tick. Hero scale (h_hero=44) pinned via
        // brand::type so the timer's weight is system-managed, not bespoke.
        const float timerPt = juce::jmin (inner.getHeight() * 0.95f,
                                          brand::type::h_hero + 12.0f);
        const auto timerCol = mode == Mode::Recording ? brand::accentRecord
                            : mode == Mode::Playing   ? brand::accentPlay
                                                      : brand::accentStatus;
        // Legibility first: a soft orange bloom around the digits (the old
        // approach) fringed every glyph and made the timer read as a blurry
        // smear against the warm, breathing ember background. Instead anchor
        // the digits with a CRISP 1 px dark contour -- a hard edge separates
        // each glyph from the bg, so the readout stays sharp at a glance --
        // then stamp the bright face on top. The "heat" now comes from the
        // panel's ember underglow, not from blurring the numerals.
        g.setFont (brand::type::mono (timerPt, true));
        {
            g.setColour (brand::debossInk.withAlpha (brand::alpha::muted));
            const float o = 1.0f;
            for (auto d : { juce::Point<float> (-o, 0.0f), {  o, 0.0f },
                            {  0.0f, -o },                 {  0.0f, o },
                            { -o, -o }, { o, -o }, { -o, o }, { o, o } })
                g.drawText (elapsedText, inner.translated (d.x, d.y),
                            juce::Justification::centred, false);
        }
        // Recording stays incandescent but brighter/cleaner so it pops off the
        // dark contour; play/idle keep their accent face.
        g.setColour (mode == Mode::Recording ? brand::lift (timerCol, brand::tint::hover)
                                             : timerCol);
        g.drawText (elapsedText, inner, juce::Justification::centred, false);
    }
}
