#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Pro Tools-style Min:Secs time ruler. Paints across the top of
    // the EDIT view's wave-pane area showing 0:00, 0:10, 0:20, 1:00...
    // ticks scaled by the current zoom level. The left "Min:Secs"
    // label column matches the strip-header column width so columns
    // line up.
    //
    // Tick density auto-adapts: at high zoom we show 1s ticks; at low
    // zoom we coalesce to 10s / 30s / 1m so labels never collide.
    class EditTimeRuler final : public juce::Component, private juce::Timer
    {
    public:
        explicit EditTimeRuler (AudioEngine& eng) : engine (eng)
        {
            startTimerHz (4);   // recheck the session length 4x per second
        }

        // Width of the left header column (matches TrackRow's headerW).
        void setHeaderWidth (int w) { headerW = w; repaint(); }
        // Total content width including the off-screen overflow when
        // zoomed in. Tells the ruler how many pixels correspond to
        // the full session length.
        void setContentWidth (int w) { contentW = juce::jmax (1, w); repaint(); }

    private:
        void timerCallback() override { repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setGradientFill (brand::verticalGradient (brand::bgDeep, r, 0.06f, 0.12f));
            g.fillRect (r);
            g.setColour (brand::edge);
            g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

            // Left "Min:Secs" label column.
            auto label = getLocalBounds().withWidth (headerW).reduced (8, 0);
            g.setColour (brand::accentStatus);
            g.setFont (brand::type::captionBold());
            g.drawText ("Min:Secs", label, juce::Justification::centredLeft, false);
            g.setColour (brand::edge);
            g.drawVerticalLine (headerW - 1, 0.0f, (float) getHeight());

            // Decide tick spacing in seconds based on pixels-per-second.
            // total samples / sample rate = total seconds across contentW.
            const auto& player = engine.getPlayer();
            const auto total = player.getTotalLengthSamples();
            const double sr  = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            // If the session has no length yet (no audio loaded), fall
            // back to a notional 5-minute span so the ruler still
            // shows meaningful gradations.
            const double totalSec = total > 0 ? (double) total / sr : 300.0;
            const int waveW = juce::jmax (1, contentW - headerW);
            const double secPerPx = totalSec / (double) waveW;
            const double pxPerSec = (double) waveW / juce::jmax (0.001, totalSec);

            // Pick a tick interval whose label spacing >= 70 px.
            const double minPxBetweenLabels = 70.0;
            const int candidates[] = { 1, 2, 5, 10, 30, 60, 120, 300, 600, 1800 };
            int tickSec = 60;
            for (int c : candidates)
                if ((double) c * pxPerSec >= minPxBetweenLabels) { tickSec = c; break; }

            // Sub-tick is 1/5 of major tick, but only painted if it's
            // wide enough to be visually distinct.
            const int subSec = juce::jmax (1, tickSec / 5);
            const double subPx = (double) subSec * pxPerSec;

            const int rulerTop = 0;
            const int rulerH   = getHeight();
            const int majorTickH = juce::jmax (8,  (int) (rulerH * 0.55f));
            const int minorTickH = juce::jmax (4,  (int) (rulerH * 0.30f));

            g.setFont (brand::type::mono (10.5f, true));
            for (double tSec = 0.0; tSec <= totalSec + 0.5; tSec += subSec)
            {
                const int x = headerW + (int) (tSec * pxPerSec);
                if (x >= getWidth()) break;
                const bool major = std::fmod (tSec + 0.0001, (double) tickSec) < 0.001;
                const int hPx = major ? majorTickH : minorTickH;
                g.setColour (major ? brand::textSecondary : brand::textMuted);
                g.drawVerticalLine (x, (float) (rulerTop + rulerH - hPx), (float) (rulerTop + rulerH));

                if (major)
                {
                    // Format as M:SS so 0..59s reads "0:00".."0:59",
                    // then "1:00", "1:10", etc.
                    const int m = (int) (tSec / 60.0);
                    const int s = (int) (std::fmod (tSec, 60.0) + 0.0001);
                    juce::String text = juce::String (m) + ":"
                                      + (s < 10 ? "0" : "") + juce::String (s);
                    g.setColour (brand::textSecondary);
                    g.drawText (text,
                                juce::Rectangle<int> (x + 3, rulerTop, 60, rulerH - majorTickH - 2),
                                juce::Justification::topLeft, false);
                }
            }
            juce::ignoreUnused (secPerPx);
        }

        AudioEngine& engine;
        int headerW   { 240 };
        int contentW  { 1024 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditTimeRuler)
    };
}
