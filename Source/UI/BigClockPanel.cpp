#include "BigClockPanel.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    BigClockPanel::BigClockPanel() = default;

    void BigClockPanel::setMode (Mode m)
    {
        if (mode != m) { mode = m; repaint(); }
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

    static juce::String formatRemaining (double seconds)
    {
        if (seconds <= 0 || std::isinf (seconds)) return "—";
        const auto h = (int) (seconds / 3600.0);
        const auto m = ((int) (seconds / 60.0)) % 60;
        if (h > 99) return ">99h";
        return juce::String::formatted ("%dh%02dm", h, m);
    }

    void BigClockPanel::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();

        // Background
        juce::Colour bg = brand::bgPanel;
        if (mode == Mode::Recording) bg = brand::accentRecord.withAlpha (0.16f);
        else if (mode == Mode::Playing) bg = brand::accentPlay.withAlpha (0.14f);
        g.setColour (bg);
        g.fillRoundedRectangle (r.reduced (2.0f), 6.0f);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r.reduced (2.0f), 6.0f, 1.0f);

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
            g.setFont (juce::FontOptions().withHeight (28.0f).withStyle ("Bold"));
            g.drawText (label,
                        left.withTrimmedLeft (lampRadius * 2.0f + 12.0f),
                        juce::Justification::centredLeft, false);
        }

        // Right: disk/health column
        auto right = inner.removeFromRight (juce::jmin (260.0f, inner.getWidth() * 0.40f));
        {
            g.setColour (brand::textMuted);
            g.setFont (juce::FontOptions().withHeight (11.0f));

            auto line = right.removeFromTop (14.0f);
            g.drawText ("FREE", line, juce::Justification::topLeft, false);
            g.drawText (juce::String::formatted ("%.1f GB", freeGB),
                        line, juce::Justification::topRight, false);

            line = right.removeFromTop (14.0f);
            g.drawText ("RECORD TIME LEFT", line, juce::Justification::topLeft, false);
            g.drawText (formatRemaining (remainingSeconds),
                        line, juce::Justification::topRight, false);

            line = right.removeFromTop (14.0f);
            g.drawText ("LAST WRITE", line, juce::Justification::topLeft, false);
            g.drawText (juce::String (lastWriteMs) + " ms",
                        line, juce::Justification::topRight, false);

            line = right.removeFromTop (14.0f);
            const auto missLabel = missed > 0 ? "MISSED SAMPLES" : "NO MISSED WRITES";
            g.setColour (missed > 0 ? brand::accentRecord : brand::textMuted);
            g.drawText (missLabel, line, juce::Justification::topLeft, false);
            if (missed > 0)
                g.drawText (juce::String (missed), line, juce::Justification::topRight, false);

            g.setColour (brand::textMuted);
            line = right.removeFromTop (14.0f);
            g.drawText ("MARKERS", line, juce::Justification::topLeft, false);
            g.drawText (juce::String (markerCount), line, juce::Justification::topRight, false);
        }

        // Centre: huge timer
        g.setColour (mode == Mode::Recording ? brand::accentRecord
                   : mode == Mode::Playing   ? brand::accentPlay
                                             : brand::textPrimary);
        g.setFont (juce::FontOptions().withHeight (juce::jmin (inner.getHeight() * 0.95f, 56.0f))
                                       .withStyle ("Bold"));
        g.drawText (elapsedText, inner, juce::Justification::centred, false);
    }
}
