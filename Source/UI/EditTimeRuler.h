#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Min:Secs time ruler for the EDIT view. Paints a Markers strip
    // across the top and a Min:Secs scale underneath (0:00, 0:10, 0:30,
    // 1:00 ...) scaled by the current zoom. The left header column
    // matches the strip-header width so columns line up.
    //
    // No Bars|Beats: this is a live recorder, not a DAW. Engineers
    // navigate by wall-clock + markers, not by bars. Tempo math is
    // still alive for the click track + cue tempo ramps; the ruler
    // just doesn't visualise it.
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
        // Shift-drag on the time-scale strip defines the automation
        // punch range. The drag's start x = punch in, end x = punch
        // out. The range is pushed straight into engine atomics so
        // PUNCH (when armed on the toolbar) gates writes immediately.
        // Plain mouseDown still goes through the wave pane's edit-
        // cursor handler -- only Shift-modified drags hit this path.
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! e.mods.isShiftDown()) return;
            if (e.y < kMarkerStripH)    return;     // marker strip ignores
            punchDragInSample  = pixelToSample (e.x);
            punchDragOutSample = punchDragInSample;
            engine.setAutomationPunchRange (punchDragInSample, punchDragInSample);
            repaint();
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! e.mods.isShiftDown() || punchDragInSample < 0) return;
            punchDragOutSample = pixelToSample (e.x);
            const auto lo = juce::jmin (punchDragInSample, punchDragOutSample);
            const auto hi = juce::jmax (punchDragInSample, punchDragOutSample);
            engine.setAutomationPunchRange (lo, hi);
            repaint();
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! e.mods.isShiftDown() || punchDragInSample < 0) return;
            // Collapse a no-drag click to "clear the range" so the
            // engineer can wipe the in/out by shift-clicking once.
            if (std::llabs ((long long) (punchDragOutSample - punchDragInSample)) < 16)
                engine.setAutomationPunchRange (-1, -1);
            punchDragInSample = punchDragOutSample = -1;
            repaint();
        }

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
            if (auto* ed = aw->getTextEditor ("n"))
            {
                ed->setSelectAllWhenFocused (true);
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::TextEditor> (ed)]
                {
                    if (safe != nullptr) safe->grabKeyboardFocus();
                });
            }
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

        // Paint splits vertically into TWO strips:
        //   top    : marker strip (kMarkerStripH px tall)
        //   bottom : Min:Secs scale (remaining height)
        // EditPage should size the ruler to ~46 px so both rows have
        // room.
        static constexpr int kMarkerStripH = 20;

        void paint (juce::Graphics& g) override
        {
            const int totalH        = getHeight();
            const int markerStripH  = juce::jmin (kMarkerStripH, totalH / 2);
            const int rulerTop      = markerStripH;
            const int rulerH        = totalH - rulerTop;

            auto r = getLocalBounds().toFloat();
            g.setGradientFill (brand::verticalGradient (brand::bgDeep, r, 0.06f, 0.12f));
            g.fillRect (r);
            g.setColour (brand::edge);
            g.drawHorizontalLine (totalH - 1, 0.0f, (float) getWidth());
            // Separator between the two strips.
            g.setColour (brand::edge.withAlpha (brand::alpha::muted));
            g.drawHorizontalLine (markerStripH, 0.0f, (float) getWidth());

            // Left header column -- two stacked labels matching the
            // two-row layout: 'Markers' and 'Min:Secs'.
            const int headerInsetX = 8;
            g.setColour (brand::textMuted);
            g.setFont (brand::type::caption());
            g.drawText ("Markers",
                        juce::Rectangle<int> (headerInsetX, 0,
                                              headerW - headerInsetX, markerStripH),
                        juce::Justification::centredLeft, false);
            g.setColour (brand::accentStatus);
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

            // -------- Time scale --------
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

            // -------- Punch range overlay --------
            // Drawn on the time-scale strip as a translucent green
            // band with two solid edges so the engineer can see the
            // in / out points at a glance. Painted regardless of
            // whether PUNCH is armed -- the band represents the
            // engine's stored range, the toolbar arms it.
            const auto pIn  = engine.getAutomationPunchIn();
            const auto pOut = engine.getAutomationPunchOut();
            if (pIn >= 0 && pOut > pIn)
            {
                const double inSec  = (double) pIn  / sr;
                const double outSec = (double) pOut / sr;
                const int xIn  = headerW + (int) (inSec  * pxPerSec);
                const int xOut = headerW + (int) (outSec * pxPerSec);
                const int clampedIn  = juce::jmax (headerW, xIn);
                const int clampedOut = juce::jmin (getWidth(), xOut);
                if (clampedOut > clampedIn)
                {
                    // Dim the band when PUNCH isn't armed so the
                    // engineer can tell at a glance whether the range
                    // is live (writes gated) or just remembered.
                    const bool armed = engine.isAutomationPunchEnabled();
                    auto band = juce::Rectangle<float> ((float) clampedIn,
                                                        (float) rulerTop,
                                                        (float) (clampedOut - clampedIn),
                                                        (float) rulerH);
                    g.setColour (brand::accentStatus.withAlpha (armed ? 0.22f : 0.08f));
                    g.fillRect (band);
                    g.setColour (brand::accentStatus.withAlpha (armed ? 1.0f : 0.45f));
                    g.drawVerticalLine (clampedIn,  (float) rulerTop, (float) (rulerTop + rulerH));
                    g.drawVerticalLine (clampedOut - 1, (float) rulerTop, (float) (rulerTop + rulerH));
                }
            }

            // -------- Marker strip (top strip) --------
            // Each marker gets a small downward-pointing flag at its
            // sample position plus its name to the right. Name leans
            // right, flag tip aligned to the exact marker x so it
            // lines up with the time scale tick.
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

        // Maps a pixel x on the time-scale strip back to a session
        // sample position. Returns 0 when x is left of the header.
        juce::int64 pixelToSample (int xPx) const
        {
            const auto& player = engine.getPlayer();
            const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const auto total = player.getTotalLengthSamples();
            const double totalSec = total > 0 ? (double) total / sr : 300.0;
            const int waveW = juce::jmax (1, contentW - headerW);
            const double pxPerSec = (double) waveW / juce::jmax (0.001, totalSec);
            const double sec = juce::jmax (0.0, (double) (xPx - headerW) / juce::jmax (1.0, pxPerSec));
            return (juce::int64) (sec * sr);
        }

        AudioEngine& engine;
        int headerW   { 380 };   // matches TrackRow::headerW
        int contentW  { 1024 };
        juce::int64 punchDragInSample  { -1 };
        juce::int64 punchDragOutSample { -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditTimeRuler)
    };
}
