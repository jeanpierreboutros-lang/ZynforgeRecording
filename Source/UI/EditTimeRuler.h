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

        // Paint splits vertically into THREE strips:
        //   top    : marker strip       (kMarkerStripH px tall)
        //   middle : Bars|Beats overlay (kBarsBeatsH  px tall)
        //   bottom : Min:Secs scale     (remaining height)
        // EditPage should size the ruler to ~64 px so all three rows
        // have room. The Bars|Beats row reads tempo + time signature
        // from the engine and lights bar lines + beat sub-ticks.
        static constexpr int kMarkerStripH = 20;
        static constexpr int kBarsBeatsH   = 18;

        void paint (juce::Graphics& g) override
        {
            const int totalH        = getHeight();
            const int markerStripH  = juce::jmin (kMarkerStripH, totalH / 3);
            const int barsBeatsH    = juce::jmin (kBarsBeatsH,   (totalH - markerStripH) / 2);
            const int barsBeatsTop  = markerStripH;
            const int rulerTop      = markerStripH + barsBeatsH;
            const int rulerH        = totalH - rulerTop;

            auto r = getLocalBounds().toFloat();
            g.setGradientFill (brand::verticalGradient (brand::bgDeep, r, 0.06f, 0.12f));
            g.fillRect (r);
            g.setColour (brand::edge);
            g.drawHorizontalLine (totalH - 1, 0.0f, (float) getWidth());
            // Separators between the three strips.
            g.setColour (brand::edge.withAlpha (0.6f));
            g.drawHorizontalLine (markerStripH, 0.0f, (float) getWidth());
            g.drawHorizontalLine (rulerTop,     0.0f, (float) getWidth());

            // Left header column -- three stacked labels matching the
            // three-row layout: 'Markers', 'Bars|Beats', 'Min:Secs'.
            const int headerInsetX = 8;
            g.setColour (brand::textMuted);
            g.setFont (brand::type::caption());
            g.drawText ("Markers",
                        juce::Rectangle<int> (headerInsetX, 0,
                                              headerW - headerInsetX, markerStripH),
                        juce::Justification::centredLeft, false);
            g.setColour (brand::engagedAmber);
            g.setFont (brand::type::captionBold());
            g.drawText ("Bars" + juce::String (juce::CharPointer_UTF8 ("\xc2\xa6")) + "Beats",
                        juce::Rectangle<int> (headerInsetX, barsBeatsTop,
                                              headerW - headerInsetX, barsBeatsH),
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

            // -------- Bars|Beats strip --------
            // Walks the engine's tempo map so accelerandi /
            // ritardandi produce a correct grid (the previous pass
            // used a single tempo across the whole session and got
            // the bar positions wrong after any tempo change).
            // Algorithm: step beat by beat, switching the active
            // bpm whenever we cross the next tempo change.
            {
                const int sigNum = juce::jmax (1, engine.getTimeSignatureNumerator());
                const auto& tempoMap = engine.getTempoMap();
                double curBpm = (double) engine.getSessionTempoBpm();
                if (curBpm < 1.0) curBpm = 120.0;

                // Find the smallest beat width on the timeline to
                // decide whether to draw sub-beats. Worst case (the
                // fastest bpm in the map) sets the show-beats gate.
                double maxBpm = curBpm;
                for (const auto& tc : tempoMap)
                    if ((double) tc.bpm > maxBpm) maxBpm = (double) tc.bpm;
                const double minBarSec = 60.0 / juce::jmax (1.0, maxBpm) * (double) sigNum;
                const bool   showBeats = (minBarSec * pxPerSec) >= 24.0;

                const int barTickH  = juce::jmax (8, (int) (barsBeatsH * 0.65f));
                const int beatTickH = juce::jmax (4, (int) (barsBeatsH * 0.35f));
                g.setFont (brand::type::mono (10.0f, true));

                // Walk samples-as-time, switching bpm at each map entry.
                const double initialBpm =
                    (! tempoMap.empty() && tempoMap.front().samplePos == 0)
                        ? (double) tempoMap.front().bpm
                        : curBpm;
                double bpm = initialBpm;
                double beatSec = 60.0 / bpm;
                size_t nextTempoIdx = 0;
                // Advance past any tempo events at samplePos 0 -- they're
                // the initial bpm, already consumed.
                while (nextTempoIdx < tempoMap.size()
                       && tempoMap[nextTempoIdx].samplePos == 0)
                    ++nextTempoIdx;

                int bar = 1, beat = 1;
                double t = 0.0;
                while (t <= totalSec + 0.0001)
                {
                    const auto sampleAtT = (juce::int64) (t * sr);
                    // Cross any pending tempo changes that fall on or
                    // before this sample.
                    while (nextTempoIdx < tempoMap.size()
                           && tempoMap[nextTempoIdx].samplePos <= sampleAtT)
                    {
                        bpm = juce::jmax (1.0, (double) tempoMap[nextTempoIdx].bpm);
                        beatSec = 60.0 / bpm;
                        ++nextTempoIdx;
                    }

                    const int x = headerW + (int) (t * pxPerSec);
                    if (x >= getWidth()) break;
                    const bool isBar = (beat == 1);
                    if (isBar || showBeats)
                    {
                        const int hPx = isBar ? barTickH : beatTickH;
                        g.setColour (isBar ? brand::engagedAmber : brand::textMuted);
                        g.drawVerticalLine (x, (float) (barsBeatsTop + barsBeatsH - hPx),
                                            (float) (barsBeatsTop + barsBeatsH));
                    }
                    if (isBar)
                    {
                        g.setColour (brand::engagedAmber);
                        g.drawText (juce::String (bar),
                                    juce::Rectangle<int> (x + 3, barsBeatsTop,
                                                          60, barsBeatsH - barTickH - 2),
                                    juce::Justification::topLeft, false);
                    }

                    t += beatSec;
                    if (++beat > sigNum) { beat = 1; ++bar; }
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
