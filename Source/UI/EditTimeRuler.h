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

        // Double-click a marker flag (or its name label) -> rename
        // dialog. The hit-test mirrors the paint layout: the flag is
        // an 8 px-wide triangle at the marker's x; the name extends
        // 120 px to the right of the flag tip.
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (e.y >= kMarkerStripH) return;   // double-clicks on the time scale ignored

            const auto& player = engine.getPlayer();
            const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const auto totalLoaded = player.getTotalLengthSamples();
            const double totalSec = totalLoaded > 0 ? (double) totalLoaded / sr : 300.0;
            const int waveW = juce::jmax (1, contentW - headerW);
            const double pxPerSec = (double) waveW / juce::jmax (0.001, totalSec);

            auto& markers = engine.getMarkers();
            const auto& list = markers.getAll();
            for (int i = 0; i < (int) list.size(); ++i)
            {
                const double tSec = (double) list[(size_t) i].sampleOffset / sr;
                if (tSec < 0.0 || tSec > totalSec) continue;
                const int markerX = headerW + (int) (tSec * pxPerSec);

                // Flag is 8 px wide centered on markerX. Name label
                // extends ~120 px to the right. Use that whole span as
                // the hit zone so the engineer can grab anywhere they
                // can see for that marker.
                if (e.x < markerX - 6) continue;
                if (e.x > markerX + 6 + 120) continue;

                openRenameDialog (i);
                return;
            }
        }

    private:
        void openRenameDialog (int markerIndex)
        {
            auto& markers = engine.getMarkers();
            const auto current = markers.getMarker (markerIndex).name;
            auto* aw = new juce::AlertWindow ("Rename marker",
                                              "Name:",
                                              juce::MessageBoxIconType::NoIcon);
            aw->addTextEditor ("n", current, {});
            if (auto* ed = aw->getTextEditor ("n")) ed->selectAll();
            aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            juce::Component::SafePointer<EditTimeRuler> self (this);
            aw->enterModalState (true,
                juce::ModalCallbackFunction::create ([aw, self, markerIndex] (int r)
                {
                    std::unique_ptr<juce::AlertWindow> dispose (aw);
                    if (r != 1 || self == nullptr) return;
                    const auto typed = dispose->getTextEditorContents ("n").trim();
                    if (typed.isEmpty()) return;
                    self->engine.getMarkers().renameMarker (markerIndex, typed);
                    self->engine.getMarkers().save();
                    self->repaint();
                }),
                false);
        }

        void timerCallback() override { repaint(); }

        // Paint splits vertically into two strips:
        //   top  : marker strip      (kMarkerStripH px tall)
        //   bottom: time scale ticks (remaining height)
        // EditPage now sizes the ruler to 44 px so the marker strip
        // has room for a name + flag.
        static constexpr int kMarkerStripH = 20;

        void paint (juce::Graphics& g) override
        {
            const int totalH        = getHeight();
            const int markerStripH  = juce::jmin (kMarkerStripH, totalH / 2);
            const int rulerH        = totalH - markerStripH;
            const int rulerTop      = markerStripH;

            auto r = getLocalBounds().toFloat();
            g.setGradientFill (brand::verticalGradient (brand::bgDeep, r, 0.06f, 0.12f));
            g.fillRect (r);
            g.setColour (brand::edge);
            g.drawHorizontalLine (totalH - 1, 0.0f, (float) getWidth());
            // Separator between marker strip and time scale.
            g.setColour (brand::edge.withAlpha (0.6f));
            g.drawHorizontalLine (markerStripH, 0.0f, (float) getWidth());

            // Left header column -- two stacked labels matching the
            // two-row layout: 'Markers' on top, 'Min:Secs' below.
            const int headerInsetX = 8;
            g.setColour (brand::textMuted);
            g.setFont (brand::type::caption());
            g.drawText ("Markers",
                        juce::Rectangle<int> (headerInsetX, 0,
                                              headerW - headerInsetX, markerStripH),
                        juce::Justification::centredLeft, false);
            g.setColour (brand::accentStatus);
            g.setFont (brand::type::captionBold());
            g.drawText ("Min:Secs",
                        juce::Rectangle<int> (headerInsetX, rulerTop,
                                              headerW - headerInsetX, rulerH),
                        juce::Justification::centredLeft, false);
            g.setColour (brand::edge);
            g.drawVerticalLine (headerW - 1, 0.0f, (float) totalH);

            // Decide tick spacing in seconds based on pixels-per-second.
            const auto& player = engine.getPlayer();
            const auto total = player.getTotalLengthSamples();
            const double sr  = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const double totalSec = total > 0 ? (double) total / sr : 300.0;
            const int waveW = juce::jmax (1, contentW - headerW);
            const double pxPerSec = (double) waveW / juce::jmax (0.001, totalSec);

            const double minPxBetweenLabels = 70.0;
            const int candidates[] = { 1, 2, 5, 10, 30, 60, 120, 300, 600, 1800 };
            int tickSec = 60;
            for (int c : candidates)
                if ((double) c * pxPerSec >= minPxBetweenLabels) { tickSec = c; break; }
            const int subSec = juce::jmax (1, tickSec / 5);

            const int majorTickH = juce::jmax (8, (int) (rulerH * 0.55f));
            const int minorTickH = juce::jmax (4, (int) (rulerH * 0.30f));

            // -------- Time scale (bottom strip) --------
            g.setFont (brand::type::mono (10.5f, true));
            for (double tSec = 0.0; tSec <= totalSec + 0.5; tSec += subSec)
            {
                const int x = headerW + (int) (tSec * pxPerSec);
                if (x >= getWidth()) break;
                const bool major = std::fmod (tSec + 0.0001, (double) tickSec) < 0.001;
                const int hPx = major ? majorTickH : minorTickH;
                g.setColour (major ? brand::textSecondary : brand::textMuted);
                g.drawVerticalLine (x, (float) (rulerTop + rulerH - hPx),
                                    (float) (rulerTop + rulerH));

                if (major)
                {
                    const int m = (int) (tSec / 60.0);
                    const int s = (int) (std::fmod (tSec, 60.0) + 0.0001);
                    juce::String text = juce::String (m) + ":"
                                      + (s < 10 ? "0" : "") + juce::String (s);
                    g.setColour (brand::textSecondary);
                    g.drawText (text,
                                juce::Rectangle<int> (x + 3, rulerTop, 60,
                                                      rulerH - majorTickH - 2),
                                juce::Justification::topLeft, false);
                }
            }

            // -------- Marker strip (top strip) --------
            // Each marker gets a small downward-pointing flag at its
            // sample position plus its name to the right. Pro Tools-
            // style: name leans right, flag tip aligned to the exact
            // marker x so it lines up with the time scale tick.
            const auto& list = engine.getMarkers().getAll();
            g.setFont (brand::type::mono (10.5f, true));
            for (const auto& m : list)
            {
                const double tSec = (double) m.sampleOffset / sr;
                if (tSec < 0.0 || tSec > totalSec) continue;
                const int x = headerW + (int) (tSec * pxPerSec);
                if (x < headerW || x >= getWidth()) continue;

                // Flag: 8x8 px brand-orange downward triangle anchored
                // to the bottom of the marker strip.
                juce::Path flag;
                const float fy = (float) markerStripH;
                flag.addTriangle ((float) x - 4.0f, fy - 10.0f,
                                  (float) x + 4.0f, fy - 10.0f,
                                  (float) x,        fy);
                g.setColour (brand::brandOrange);
                g.fillPath (flag);
                g.setColour (brand::brandOrange.darker (0.40f));
                g.strokePath (flag, juce::PathStrokeType (0.75f));

                // Name to the right of the flag. Clip to ~120 px so a
                // long name doesn't bleed across the next marker.
                if (m.name.isNotEmpty())
                {
                    g.setColour (brand::textPrimary);
                    g.drawText (m.name,
                                juce::Rectangle<int> (x + 6, 1, 120, markerStripH - 2),
                                juce::Justification::centredLeft, false);
                }
            }
        }

        AudioEngine& engine;
        int headerW   { 380 };   // matches TrackRow::headerW
        int contentW  { 1024 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditTimeRuler)
    };
}
