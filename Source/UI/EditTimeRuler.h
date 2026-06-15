#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Min:Secs time ruler for the EDIT view. Paints a Markers strip
    // across the top and a graduated Min:Secs scale underneath. The left
    // header column matches the strip-header width so columns line up.
    //
    // No Bars|Beats: this is a live recorder, not a DAW. Engineers
    // navigate by wall-clock + markers, not by bars. Tempo math is
    // still alive for the click track + cue tempo ramps; the ruler
    // just doesn't visualise it.
    //
    // The scale is built like a Reaper / Pro Tools time ruler:
    //   - A 1-2-5 progression (0.1s -> 2h) picks a labelled "major"
    //     interval whose labels never collide, subdivided into mid +
    //     minor ticks of graduated height so the spacing reads at a
    //     glance. Labels switch to tenths below 1s/major and to
    //     H:MM:SS past an hour.
    //   - The transport playhead draws a bright line + a time-readout
    //     bubble; the edit cursor draws its own line; the loop region
    //     and the automation punch range shade as bands. All share the
    //     wave-pane's colours + sample->x mapping so the ruler lines up
    //     with the lanes below.
    class EditTimeRuler final : public juce::Component, private juce::Timer
    {
    public:
        explicit EditTimeRuler (AudioEngine& eng) : engine (eng)
        {
            startTimerHz (idleHz);
        }

        // Width of the left header column (matches TrackRow's headerW).
        void setHeaderWidth (int w) { headerW = w; repaint(); }
        // Total content width including the off-screen overflow when
        // zoomed in. Tells the ruler how many pixels correspond to
        // the full session length.
        void setContentWidth (int w) { contentW = juce::jmax (1, w); repaint(); }

        // Sample/second <-> screen-x mapping. The wave LANES inset their clip
        // content by brand::space::xs on each side (TrackRow paints into
        // `withTrimmedLeft(headerW).reduced(xs, sm)`, and the lane playhead is
        // `headerW + xs + frac*(waveW - 2*xs)`). The ruler MUST use the exact
        // same mapping or its ticks / markers / playhead drift a few px from
        // the audio they label. Route every ruler mapping through these.
        static constexpr int kWaveInset = brand::space::editWaveInset;
        double rulerPxPerSec (double totalSec) const noexcept
        {
            const int effW = juce::jmax (1, contentW - headerW - 2 * kWaveInset);
            return (double) effW / juce::jmax (0.001, totalSec);
        }
        int rulerTimeToX (double sec, double pxPerSec) const noexcept
        {
            return headerW + kWaveInset + (int) (sec * pxPerSec);
        }
        double rulerXToTime (int xPx, double pxPerSec) const noexcept
        {
            return juce::jmax (0.0, (double) (xPx - headerW - kWaveInset)
                                        / juce::jmax (1.0, pxPerSec));
        }

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
            const double pxPerSec = rulerPxPerSec (totalSec);

            auto& markers = engine.getMarkers();
            const auto& list = markers.getAll();
            for (int i = 0; i < (int) list.size(); ++i)
            {
                const double tSec = (double) list[(size_t) i].sampleOffset / sr;
                if (tSec < 0.0 || tSec > totalSec) continue;
                const int markerX = rulerTimeToX (tSec, pxPerSec);

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

        // Idle: poll the session length a few times a second. Playing:
        // step up so the playhead + time bubble glide instead of jumping.
        void timerCallback() override
        {
            const bool active = engine.isPlaying() || engine.isRecording();
            if (active != fastTimer)
            {
                fastTimer = active;
                startTimerHz (active ? playHz : idleHz);
            }
            // While the transport rolls, the playhead moves every tick --
            // always repaint. At idle, repaint only when something the ruler
            // DRAWS changed (seek, marker add/move, loop edits, session
            // length): the unconditional 4 Hz repaint re-shaped the ruler's
            // text labels forever at idle.
            if (active) { repaint(); return; }
            juce::int64 sig = engine.getPlayer().getPositionSamples()
                            ^ (engine.getPlayer().getTotalLengthSamples() << 1)
                            ^ (engine.getPlayer().getLoopStart() << 2)
                            ^ (engine.getPlayer().getLoopEnd()   << 3)
                            ^ (engine.getEditCursorSample()      << 4);
            const auto& list = engine.getMarkers().getAll();
            sig ^= (juce::int64) list.size() << 40;
            for (const auto& m : list) sig += m.sampleOffset;
            if (sig != lastIdleSig)
            {
                lastIdleSig = sig;
                repaint();
            }
        }

        juce::int64 lastIdleSig { -1 };

        static constexpr int idleHz = 4;
        static constexpr int playHz = 30;
        bool fastTimer { false };

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
            g.setColour (brand::edge.withAlpha (zynforge::brand::alpha::muted));
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

            // Time mapping: pixels per second across the (possibly
            // zoomed) content width. While recording, the player isn't
            // loaded yet, so drive the scale from the elapsed record time
            // (grow-to-fit) -- the same timebase the EDIT lanes' live
            // capture envelope uses, so the ruler lines up with it.
            const auto& player = engine.getPlayer();
            const bool recording = engine.isRecording();
            const juce::int64 recSamples = recording
                ? engine.getRecorder().getRecordTimelineSamples() : 0;
            const auto total = player.getTotalLengthSamples();
            const double sr  = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const double totalSec = recording ? juce::jmax ((double) recSamples / sr, 1.0)
                                              : (total > 0 ? (double) total / sr : 300.0);
            const double pxPerSec = rulerPxPerSec (totalSec);
            const double rulerBottom = (double) (rulerTop + rulerH);
            const auto secToX = [&] (double s) { return rulerTimeToX (s, pxPerSec); };

            // Pick a labelled "major" interval whose labels won't collide,
            // then subdivide it into minor + mid ticks. A 1-2-5 progression
            // (sub-second through hours) keeps the numbers readable, the way
            // Reaper / Pro Tools rulers step. `decimals` switches the label
            // to tenths once we're below a second per major.
            const double minLabelPx = 64.0;
            static const double steps[] = { 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30,
                                            60, 120, 300, 600, 900, 1800, 3600, 7200 };
            double majorSec = 3600.0;
            for (double s : steps)
                if (s * pxPerSec >= minLabelPx) { majorSec = s; break; }
            // Minor = major / 5, but collapse to /2 (or off) if the ticks
            // would crowd. Mid tick sits at the half-major when there are
            // an even number of minors, giving the eye a coarse anchor.
            double minorSec = majorSec / 5.0;
            if (minorSec * pxPerSec < 5.0) minorSec = majorSec / 2.0;
            if (minorSec * pxPerSec < 5.0) minorSec = majorSec;          // too dense -> majors only
            const double midSec     = majorSec / 2.0;
            const int    decimals   = majorSec < 1.0 ? 1 : 0;

            const int majorTickH = juce::jmax (10, (int) (rulerH * 0.62f));
            const int midTickH   = juce::jmax (7,  (int) (rulerH * 0.42f));
            const int minorTickH = juce::jmax (4,  (int) (rulerH * 0.24f));

            // Walk ticks by an integer index, not a `tSec += minorSec`
            // accumulator: repeated float addition drifts, so an integer-
            // second tick could land at 1.9999998 and floor() to the
            // wrong label (duplicate / skipped seconds). The index also
            // classifies major/mid exactly via modulo.
            const int minorPerMajor = juce::jmax (1, (int) std::lround (majorSec / minorSec));
            const int minorPerMid   = juce::jmax (1, (int) std::lround (midSec   / minorSec));

            // -------- Loop-region shading (mirrors the wave-pane overlay) --------
            if (player.hasLoopRegion() && player.isLoaded())
            {
                const int xA = juce::jmax (headerW, secToX ((double) player.getLoopStart() / sr));
                const int xB = juce::jmin (getWidth(), secToX ((double) player.getLoopEnd()   / sr));
                if (xB > xA)
                {
                    g.setColour (brand::accentEdit.withAlpha (zynforge::brand::alpha::subtle));
                    g.fillRect (juce::Rectangle<int> (xA, rulerTop, xB - xA, rulerH));
                    g.setColour (brand::accentEdit.withAlpha (zynforge::brand::alpha::strong));
                    g.drawVerticalLine (xA,     (float) rulerTop, (float) rulerBottom);
                    g.drawVerticalLine (xB - 1, (float) rulerTop, (float) rulerBottom);
                }

                // Numeric selection readout (Pro Tools-style): Start > End
                // (Length) in M:SS.mmm. Lives in the MARKER strip at the very
                // top -- NOT the ruler strip, where it used to collide with the
                // major time-tick labels and become unreadable. An opaque
                // pill behind it keeps it legible over markers / ticks / shading.
                auto fmt = [sr] (juce::int64 s)
                {
                    const double t = juce::jmax (0.0, (double) s / sr);
                    const int    m = (int) (t / 60.0);
                    return juce::String::formatted ("%d:%06.3f", m, t - 60.0 * m);
                };
                const auto label = "Sel " + fmt (player.getLoopStart())
                                 + " \xe2\x96\xb8 " + fmt (player.getLoopEnd())
                                 + "  (" + fmt (player.getLoopEnd() - player.getLoopStart()) + ")";
                const auto font = brand::type::label();
                const int  tw   = juce::GlyphArrangement::getStringWidthInt (font, label) + 12;
                const int  tx   = juce::jlimit (headerW + 4,
                                                juce::jmax (headerW + 4, getWidth() - tw - 4),
                                                xA + 3);
                juce::Rectangle<int> pill (tx, 2, tw, juce::jmax (12, markerStripH - 4));
                g.setColour (brand::bgDeep.withAlpha (zynforge::brand::alpha::bold));
                g.fillRoundedRectangle (pill.toFloat(), brand::radius::sm);
                g.setColour (brand::accentEdit.withAlpha (zynforge::brand::alpha::scrim));
                g.drawRoundedRectangle (pill.toFloat().reduced (0.5f), brand::radius::sm, 1.0f);
                g.setColour (brand::lift (brand::accentEdit, 0.35f));
                g.setFont (font);
                g.drawText (label, pill.reduced (6, 0),
                            juce::Justification::centredLeft, false);
            }

            // -------- Punch range overlay --------
            // Translucent band representing the engine's stored automation
            // punch range; bright when PUNCH is armed (writes gated), dim
            // when it's just remembered.
            const auto pIn  = engine.getAutomationPunchIn();
            const auto pOut = engine.getAutomationPunchOut();
            if (pIn >= 0 && pOut > pIn)
            {
                const int clampedIn  = juce::jmax (headerW,    secToX ((double) pIn  / sr));
                const int clampedOut = juce::jmin (getWidth(), secToX ((double) pOut / sr));
                if (clampedOut > clampedIn)
                {
                    const bool armed = engine.isAutomationPunchEnabled();
                    g.setColour (brand::accentStatus.withAlpha (armed ? 0.22f : 0.08f));
                    g.fillRect (juce::Rectangle<float> ((float) clampedIn, (float) rulerTop,
                                                        (float) (clampedOut - clampedIn), (float) rulerH));
                    g.setColour (brand::accentStatus.withAlpha (armed ? 1.0f : 0.45f));
                    g.drawVerticalLine (clampedIn,      (float) rulerTop, (float) rulerBottom);
                    g.drawVerticalLine (clampedOut - 1, (float) rulerTop, (float) rulerBottom);
                }
            }

            // -------- Graduated tick scale --------
            // Draw minors first (short, dim), then majors on top (tall +
            // labelled), with a mid tier so the spacing reads at a glance.
            const int lastTick = (int) std::floor (totalSec / minorSec) + 1;
            g.setFont (brand::type::mono (10.5f, true));
            for (int k = 0; k <= lastTick; ++k)
            {
                const double tSec = (double) k * minorSec;
                const int x = secToX (tSec);
                if (x >= getWidth()) break;
                if (x < headerW) continue;

                const bool major = (k % minorPerMajor) == 0;
                const bool mid   = ! major && (k % minorPerMid) == 0;
                const int  hPx   = major ? majorTickH : (mid ? midTickH : minorTickH);
                g.setColour (major ? brand::textSecondary
                                   : (mid ? brand::textMuted
                                          : brand::edge.withAlpha (zynforge::brand::alpha::prominent)));
                g.drawVerticalLine (x, (float) (rulerBottom - hPx), (float) rulerBottom);

                if (major)
                {
                    // Label from the exact major multiple (k / minorPerMajor)
                    // so it never inherits the tick's float drift.
                    const double labelSec = (double) (k / minorPerMajor) * majorSec;
                    g.setColour (brand::textSecondary);
                    g.drawText (formatTime (labelSec, decimals),
                                juce::Rectangle<int> (x + 3, rulerTop, 64,
                                                      rulerH - majorTickH - 1),
                                juce::Justification::topLeft, false);
                }
            }

            // -------- Edit cursor (Pro Tools insertion point) --------
            const auto cursorSample = engine.getEditCursorSample();
            if (cursorSample >= 0)
            {
                const int cx = secToX ((double) cursorSample / sr);
                if (cx >= headerW && cx < getWidth())
                {
                    g.setColour (brand::textPrimary.withAlpha (zynforge::brand::alpha::bold));
                    g.drawVerticalLine (cx, (float) rulerTop, (float) rulerBottom);
                }
            }

            // -------- Playhead / record head + time bubble --------
            // A bright transport line plus a readout chip so the engineer
            // always knows the exact time without doing tick math. While
            // recording it turns red and tracks the elapsed-record head
            // (pinned to the right edge as the take grows); otherwise it
            // follows the playhead whenever a session is loaded.
            const bool showHead = recording || player.isLoaded();
            if (showHead)
            {
                const juce::int64 headSample = recording ? recSamples
                                                         : player.getPositionSamples();
                const juce::Colour headCol = recording ? brand::accentRecord : brand::accentPlay;
                const int px = secToX ((double) headSample / sr);
                if (px >= headerW && px <= getWidth())
                {
                    g.setColour (headCol.withAlpha (zynforge::brand::alpha::prominent));
                    g.fillRect (juce::Rectangle<int> (px - 1, rulerTop, 2, rulerH));

                    const auto label = formatTime ((double) headSample / sr,
                                                   pxPerSec > 120.0 ? 1 : 0);
                    g.setFont (brand::type::mono (10.5f, true));
                    const int bw = juce::jmax (38, g.getCurrentFont().getStringWidth (label) + 10);
                    const int bh = juce::jmin (14, rulerH - 2);
                    int bx = px + 2;
                    if (bx + bw > getWidth()) bx = px - bw - 2;   // flip to the left near the edge
                    bx = juce::jlimit (headerW, getWidth() - bw, bx);
                    auto bubble = juce::Rectangle<int> (bx, rulerTop + 1, bw, bh).toFloat();
                    g.setColour (headCol);
                    g.fillRoundedRectangle (bubble, brand::radius::sm);
                    g.setColour (brand::onSignal (headCol));
                    g.drawText (label, bubble, juce::Justification::centred, false);
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
                const int x = rulerTimeToX (tSec, pxPerSec);
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
                g.setColour (brand::sink (brand::brandOrange, 0.40f));
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

        // Wall-clock label. Below an hour: M:SS; an hour or more:
        // H:MM:SS. `decimals` appends fractional seconds (tenths) so the
        // scale stays readable when zoomed past one major-tick / second.
        static juce::String formatTime (double sec, int decimals)
        {
            if (sec < 0.0) sec = 0.0;
            const int whole = (int) std::floor (sec);
            const int h = whole / 3600;
            const int m = (whole % 3600) / 60;
            const int s =  whole % 60;
            const auto two = [] (int v) { return (v < 10 ? "0" : "") + juce::String (v); };
            juce::String out = h > 0 ? juce::String (h) + ":" + two (m) + ":" + two (s)
                                     : juce::String (m) + ":" + two (s);
            if (decimals > 0)
            {
                const double frac = sec - whole;
                out << "." << juce::String ((int) (frac * 10.0 + 0.5));
            }
            return out;
        }

        // Maps a pixel x on the time-scale strip back to a session
        // sample position. Returns 0 when x is left of the header.
        juce::int64 pixelToSample (int xPx) const
        {
            const auto& player = engine.getPlayer();
            const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const auto total = player.getTotalLengthSamples();
            const double totalSec = total > 0 ? (double) total / sr : 300.0;
            const double pxPerSec = rulerPxPerSec (totalSec);
            const double sec = rulerXToTime (xPx, pxPerSec);
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
