#include "BigClockPanel.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    BigClockPanel::BigClockPanel() = default;

    void BigClockPanel::setMode (Mode m)
    {
        if (mode != m) { mode = m; syncPulseTimer(); repaint(); }
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
        freeGB           = gb;
        lastWriteMs      = ms;
        missed           = mis;
        remainingSeconds = remainingSec;
        repaint();
    }

    void BigClockPanel::setArmedReady (bool ready)
    {
        if (armedReady != ready) { armedReady = ready; syncPulseTimer(); repaint(); }
    }

    void BigClockPanel::syncPulseTimer()
    {
        const bool wantPulse = (mode == Mode::Recording) || armedReady;
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
        if (mode == Mode::Recording)
        {
            const float pulse = 0.5f + 0.5f * std::sin (pulsePhase);  // 0..1
            const float a = 0.12f + 0.08f * pulse;                    // 0.12..0.20
            bg = brand::accentRecord.withAlpha (a);
        }
        else if (mode == Mode::Playing) bg = brand::accentPlay.withAlpha (0.14f);
        auto bgRect = r.reduced (2.0f);
        g.setGradientFill (juce::ColourGradient (
            bg.brighter (0.12f), bgRect.getCentreX(), bgRect.getY(),
            bg.darker   (0.18f), bgRect.getCentreX(), bgRect.getBottom(),
            false));
        g.fillRoundedRectangle (bgRect, 6.0f);

        // Soft top highlight -- reinforces the glass / depth feel and
        // makes the timer read as raised metal under stage lights.
        auto hi = bgRect.withTrimmedBottom (bgRect.getHeight() * 0.60f);
        g.setGradientFill (juce::ColourGradient (
            juce::Colours::white.withAlpha (0.06f), hi.getCentreX(), hi.getY(),
            juce::Colours::white.withAlpha (0.0f),  hi.getCentreX(), hi.getBottom(),
            false));
        g.fillRoundedRectangle (hi, 6.0f);

        // Armed-but-not-rolling border. Uses signalArmedReady
        // (engagedAmber -- the BYPASS / LIVE / LOCK token) so the
        // 'primed but not active' colour doesn't collide with the
        // mute / brand-assertion claims on brandOrange. Pulses at
        // 1 Hz so the call-to-action draws the eye.
        if (armedReady && mode == Mode::Idle)
        {
            const float pulse = 0.5f + 0.5f * std::sin (pulsePhase);
            const auto  col   = brand::signalArmedReady().withMultipliedBrightness (0.85f + 0.25f * pulse);
            g.setColour (col);
            g.drawRoundedRectangle (r.reduced (2.0f), 6.0f, 2.0f);
        }
        else
        {
            g.setColour (brand::edge);
            g.drawRoundedRectangle (r.reduced (2.0f), 6.0f, 1.0f);
        }

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
                                juce::Colour labelCol = brand::textTertiary,
                                juce::Colour valueCol = brand::textSecondary)
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
                     missed > 0 ? brand::accentRecord : brand::textTertiary,
                     missed > 0 ? brand::accentRecord : brand::textSecondary);
            drawRow (right.removeFromTop (14.0f), "MARKERS",
                     juce::String (markerCount));
        }

        // Centre: huge timer
        g.setColour (mode == Mode::Recording ? brand::accentRecord
                   : mode == Mode::Playing   ? brand::accentPlay
                                             : brand::accentStatus);
        // Tabular numerals -- the timer string can't shift width as
        // seconds tick. Hero scale (h_hero=44) pinned via brand::type
        // so the timer's visual weight is system-managed, not bespoke.
        g.setFont (brand::type::mono (juce::jmin (inner.getHeight() * 0.95f,
                                                  brand::type::h_hero + 12.0f),
                                      true));
        g.drawText (elapsedText, inner, juce::Justification::centred, false);
    }
}
