#include "PatchPage.h"
#include "PlaceholderView.h"
#include "AudioDeviceDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "FloatingLaunch.h"

namespace zynforge
{
    namespace
    {
        // One half of the patch page -- either input or output routing.
        class PatchMatrix final : public juce::Component,
                                  private juce::Timer,
                                  private juce::ScrollBar::Listener
        {
        public:
            // Reserved gutter on the right edge for the vertical scrollbar.
            static constexpr int kScrollW = 12;

            PatchMatrix (AudioEngine& eng, bool inputSide)
                : engine (eng), isInput (inputSide)
            {
                vscroll.setAutoHide (false);
                vscroll.addListener (this);
                addAndMakeVisible (vscroll);
                hscroll.setAutoHide (false);
                hscroll.addListener (this);
                addAndMakeVisible (hscroll);
                addChildComponent (placeholder);   // empty-state overlay
                startTimerHz (10);
            }
            ~PatchMatrix() override { stopTimer(); }

        private:
            void timerCallback() override
            {
                // Compute a cheap state hash for the routing surface:
                // track count, plus each track's routing + stereo +
                // colour atomics. Repaint only when the hash differs
                // from last tick. Without this guard the PATCH page
                // repaints 10 Hz unconditionally even when nothing
                // has moved.
                auto& rec = engine.getRecorder();
                const int n = rec.getNumTracks();
                const int numHw = isInput ? engine.getCurrentDeviceInputCount()
                                          : engine.getCurrentDeviceOutputCount();
                std::size_t h = (std::size_t) n;
                h = h * 1315423911u ^ (std::size_t) (numHw + 3);
                for (int i = 0; i < n; ++i)
                {
                    auto& t = rec.getTrack (i);
                    const auto r = isInput ? engine.getEffectiveInputRouting (i)
                                            : t.outputRouting.load (std::memory_order_relaxed);
                    const auto s = t.isStereo .load (std::memory_order_relaxed) ? 1 : 0;
                    const auto c = (std::size_t) t.colourARGB.load (std::memory_order_relaxed);
                    h = h * 1315423911u ^ (std::size_t) (r + 7);
                    h = h * 1315423911u ^ (std::size_t) (s + 11);
                    h = h * 1315423911u ^ c;
                    // Include the NAME too -- the column headers draw t.name, so
                    // a rename in MIXER/EDIT must repaint the open PATCH window.
                    h = h * 1315423911u ^ (std::size_t) t.getNameThreadSafe().hashCode();
                }
                if (h != lastHash)
                {
                    lastHash = h;
                    updateScrollRange();   // HW count / track count may have changed
                    syncPlaceholder();
                    repaint();
                }
            }
            std::size_t lastHash { 0 };
        public:

            struct Layout
            {
                int rowHeaderW;
                int colorBandH;
                int muteBandH;
                int colHeaderH;
                int rowH;
                int colW;
            };

            // Returns the list of logical strips -- each entry is the L track
            // index of that strip, and `stereo` is true when the strip
            // represents a stereo pair (L, L+1).
            struct LogicalStrip { int trackIndex; bool stereo; };
            std::vector<LogicalStrip> logicalStrips() const
            {
                std::vector<LogicalStrip> out;
                const int n = engine.getRecorder().getNumTracks();
                for (int i = 0; i < n; )
                {
                    const bool stereo = engine.getRecorder().getTrack (i).isStereo.load()
                                     && (i + 1 < n);
                    out.push_back ({ i, stereo });
                    i += stereo ? 2 : 1;
                }
                return out;
            }

            Layout computeLayout() const
            {
                const int numStrips = (int) logicalStrips().size();
                Layout L;
                L.rowHeaderW = 90;
                L.colorBandH = 60;
                L.muteBandH  = 32;
                L.colHeaderH = L.colorBandH + L.muteBandH;
                L.rowH       = 56;
                const int avail = getWidth() - L.rowHeaderW - kScrollW;
                L.colW = juce::jmax (60, avail / juce::jmax (1, numStrips));
                return L;
            }

            void paint (juce::Graphics& g) override
            {
                g.fillAll (brand::bgDeep);

                const auto logical  = logicalStrips();
                const int numStrips = (int) logical.size();
                const int numHw     = isInput ? engine.getCurrentDeviceInputCount()
                                              : engine.getCurrentDeviceOutputCount();
                const auto L = computeLayout();

                // ─── Top-left label (HW IN / CH ->)
                g.setColour (brand::textSecondary);
                g.setFont (brand::type::channelName());
                g.drawText (isInput ? "HW IN" : "HW OUT",
                            juce::Rectangle<int> (0, 14, L.rowHeaderW, 18),
                            juce::Justification::centred, false);
                g.setFont (brand::type::caption());
                g.drawText ("CH ->",
                            juce::Rectangle<int> (0, 34, L.rowHeaderW, 18),
                            juce::Justification::centred, false);

                // Frozen-pane geometry. The HW-channel label column (left)
                // and the strip-header band (top) stay put; the dot body
                // scrolls in both axes, the header band scrolls with X, the
                // label column scrolls with Y. Gutters on the right/bottom
                // hold the two scrollbars.
                const int bodyX = L.rowHeaderW;
                const int bodyY = L.colHeaderH;
                const int bodyW = juce::jmax (0, getWidth()  - bodyX - kScrollW);
                const int bodyH = juce::jmax (0, getHeight() - bodyY - kScrollW);

                // ─── Column headers -- frozen vertically, scroll with X.
                {
                    juce::Graphics::ScopedSaveState clip (g);
                    g.reduceClipRegion (bodyX, 0, bodyW, L.colHeaderH);
                    for (int c = 0; c < numStrips; ++c)
                    {
                        const int colX = bodyX + c * L.colW - scrollX;
                        if (colX + L.colW <= bodyX || colX >= bodyX + bodyW) continue;

                        const auto& ls = logical[(std::size_t) c];
                        auto& t = engine.getRecorder().getTrack (ls.trackIndex);
                        const bool stereoCol = ls.stereo;
                        const auto stripCol = t.colourARGB.load() != 0
                                              ? juce::Colour ((juce::uint32) t.colourARGB.load())
                                              : brand::stripColour (ls.trackIndex);

                        juce::Rectangle<int> head (colX, 0, L.colW, L.colorBandH);
                        auto inner = head.reduced (3, 3);
                        g.setColour (stripCol);   // FLAT: solid colour band (no gradient)
                        g.fillRoundedRectangle (inner.toFloat(), brand::radius::lg);
                        g.setColour (brand::lift (stripCol, 0.30f).withAlpha (zynforge::brand::alpha::muted));
                        g.drawRoundedRectangle (inner.toFloat(), brand::radius::lg, 1.0f);

                        g.setColour (brand::onSignal (stripCol));
                        g.setFont (brand::type::ui (22.0f, true));
                        // The strip's own number, as MIXER and EDIT show it. A column
                        // count drifts from it after the first stereo pair.
                        g.drawText (juce::String (ls.trackIndex + 1).paddedLeft ('0', 2),
                                    juce::Rectangle<int> (inner.getX(), inner.getY() + 4, inner.getWidth(), 26),
                                    juce::Justification::centred, false);

                        g.setFont (brand::type::uiLabel());
                        const auto displayName = t.name.isNotEmpty() ? t.name
                                                                      : juce::String ("In ") + juce::String (ls.trackIndex + 1);
                        g.drawText (displayName,
                                    juce::Rectangle<int> (inner.getX() + 2, inner.getY() + 32,
                                                          inner.getWidth() - 4, 18),
                                    juce::Justification::centred, false);

                        juce::Rectangle<int> pillCell (colX, L.colorBandH, L.colW, L.muteBandH);
                        auto pill = pillCell.reduced (10, 4);
                        if (stereoCol)
                        {
                            g.setColour (stripCol);
                            g.fillRoundedRectangle (pill.toFloat(), pill.getHeight() * 0.5f);
                            g.setColour (brand::onSignal (stripCol));
                        }
                        else
                        {
                            g.setColour (brand::bgElevated);
                            g.fillRoundedRectangle (pill.toFloat(), pill.getHeight() * 0.5f);
                            g.setColour (brand::lift (stripCol, 0.20f).withAlpha (zynforge::brand::alpha::muted));
                            g.drawRoundedRectangle (pill.toFloat(), pill.getHeight() * 0.5f, 1.0f);
                            g.setColour (brand::textSecondary);
                        }
                        g.setFont (brand::type::uiLabel());
                        g.drawText (stereoCol ? "ST" : "M", pill, juce::Justification::centred, false);
                    }
                }

                // ─── Row labels (HW channel) -- frozen horizontally, scroll Y.
                {
                    juce::Graphics::ScopedSaveState clip (g);
                    g.reduceClipRegion (0, bodyY, L.rowHeaderW, bodyH);
                    const auto rowInfo = PatchPage::rowsFor (engine, isInput);
                    for (int row = 0; row < (int) rowInfo.size(); ++row)
                    {
                        const int y = bodyY + row * L.rowH - scrollY;
                        if (y + L.rowH <= bodyY || y >= bodyY + bodyH) continue;
                        if (row % 2 == 0)
                        {
                            g.setColour (brand::bgStrip.withAlpha (zynforge::brand::alpha::scrim));
                            g.fillRect (juce::Rectangle<int> (0, y, L.rowHeaderW, L.rowH));
                        }
                        const auto& info = rowInfo[(size_t) row];
                        g.setColour (info.held ? brand::textPrimary : brand::textSecondary);
                        g.setFont (brand::type::sectionTitle());
                        g.drawText (info.label,
                                    juce::Rectangle<int> (14, y, L.rowHeaderW - 18, L.rowH),
                                    juce::Justification::centredLeft, false);
                    }
                }

                // Seams framing the frozen panes.
                g.setColour (brand::borderSubtle);
                g.fillRect (0, bodyY - 1, getWidth(), 1);   // under the column-header band
                g.fillRect (bodyX - 1, 0, 1, getHeight());  // right of the row-header column

                // ─── Dot body -- scrolls in both axes.
                {
                    juce::Graphics::ScopedSaveState clip (g);
                    g.reduceClipRegion (bodyX, bodyY, bodyW, bodyH);
                    for (int row = 0; row < numHw; ++row)
                    {
                        const int y = bodyY + row * L.rowH - scrollY;
                        if (y + L.rowH <= bodyY || y >= bodyY + bodyH) continue;
                        if (row % 2 == 0)
                        {
                            g.setColour (brand::bgStrip.withAlpha (zynforge::brand::alpha::scrim));
                            g.fillRect (juce::Rectangle<int> (bodyX, y, bodyW, L.rowH));
                        }
                        for (int c = 0; c < numStrips; ++c)
                        {
                            const int x = bodyX + c * L.colW - scrollX;
                            if (x + L.colW <= bodyX || x >= bodyX + bodyW) continue;
                            const auto cell = juce::Rectangle<int> (x, y, L.colW, L.rowH);
                            const auto dot  = cell.withSizeKeepingCentre (22, 22).toFloat();

                            const auto& ls     = logical[(std::size_t) c];
                            const int trackIdx = ls.trackIndex;
                            const bool active  = holdsRow (ls, row);
                            if (active)
                            {
                                auto& t = engine.getRecorder().getTrack (trackIdx);
                                const auto stripCol = t.colourARGB.load() != 0
                                                      ? juce::Colour ((juce::uint32) t.colourARGB.load())
                                                      : brand::stripColour (trackIdx);
                                g.setColour (stripCol);
                                // A pair's two dots are joined: one strip, two
                                // channels. Dots a wrap has parted stay apart.
                                if (ls.stereo && row + 1 < numHw && currentRoutingForTrack (trackIdx) == row
                                    && currentRoutingForTrack (trackIdx + 1) == row + 1)
                                    g.fillRect (dot.getCentreX() - 4.0f, dot.getCentreY(), 8.0f, (float) L.rowH);
                                g.fillEllipse (dot);
                                g.setColour (brand::onSignal (stripCol));
                                g.drawEllipse (dot, 2.0f);
                            }
                            else
                            {
                                g.setColour (brand::textTertiary.withAlpha (zynforge::brand::alpha::muted));
                                g.drawEllipse (dot, 1.6f);
                            }
                        }
                    }
                }
            }

            void resized() override
            {
                vscroll.setBounds (getWidth()  - kScrollW, 0, kScrollW, getHeight() - kScrollW);
                hscroll.setBounds (0, getHeight() - kScrollW, getWidth() - kScrollW, kScrollW);
                placeholder.setBounds (getLocalBounds().withTrimmedRight (kScrollW)
                                                       .withTrimmedBottom (kScrollW));
                updateScrollRange();
                syncPlaceholder();
            }

            // Show an empty-state overlay when there's nothing to patch:
            // no audio device (no hardware rows) or no channel strips
            // (no columns). The no-device case offers a jump to audio
            // settings. Guarded by lastPlaceholderKind so VoiceOver isn't
            // re-announced on every tick.
            void syncPlaceholder()
            {
                const int kind = (numRows() == 0)        ? 1     // no device
                               : (numStripsCount() == 0) ? 2     // no strips
                                                         : 0;    // patchable
                if (kind == lastPlaceholderKind) return;
                lastPlaceholderKind = kind;

                if (kind == 1)
                    placeholder.showEmpty ("No audio device",
                        "Select an input/output device to patch hardware channels.",
                        "Audio settings…",
                        [this] { AudioDeviceDialog::launch (engine, /*selfOwned*/ true); });
                else if (kind == 2)
                    placeholder.showEmpty ("No channels to patch",
                        "Add tracks or open a session first.");
                else
                    placeholder.clear();
            }

            void mouseWheelMove (const juce::MouseEvent&,
                                 const juce::MouseWheelDetails& w) override
            {
                const auto L = computeLayout();
                const int viewH = juce::jmax (0, getHeight() - L.colHeaderH - kScrollW);
                const int viewW = juce::jmax (0, getWidth()  - L.rowHeaderW - kScrollW);
                const int maxY  = juce::jmax (0, numRows()        * L.rowH - viewH);
                const int maxX  = juce::jmax (0, numStripsCount() * L.colW - viewW);
                bool did = false;
                if (maxY > 0 && w.deltaY != 0.0f)
                {
                    scrollY = juce::jlimit (0, maxY,
                                            scrollY - juce::roundToInt (w.deltaY * (float) L.rowH * 3.0f));
                    vscroll.setCurrentRangeStart ((double) scrollY);
                    did = true;
                }
                if (maxX > 0 && w.deltaX != 0.0f)
                {
                    scrollX = juce::jlimit (0, maxX,
                                            scrollX - juce::roundToInt (w.deltaX * (float) L.colW * 3.0f));
                    hscroll.setCurrentRangeStart ((double) scrollX);
                    did = true;
                }
                if (did) repaint();
            }

            void scrollBarMoved (juce::ScrollBar* sb, double newStart) override
            {
                if      (sb == &vscroll) { scrollY = juce::roundToInt (newStart); repaint(); }
                else if (sb == &hscroll) { scrollX = juce::roundToInt (newStart); repaint(); }
            }

            void mouseDown (const juce::MouseEvent& e) override
            {
                if (engine.areControlsLocked()) { dragActive = false; return; }
                const auto logical  = logicalStrips();
                const int numStrips = (int) logical.size();
                const int numHw     = isInput ? engine.getCurrentDeviceInputCount()
                                              : engine.getCurrentDeviceOutputCount();
                const auto L = computeLayout();

                // Mono / stereo pill click -- toggles isStereo on this column.
                if (e.y >= L.colorBandH && e.y < L.colHeaderH)
                {
                    const int x = e.x - L.rowHeaderW + scrollX;   // header scrolls in X
                    if (e.x < L.rowHeaderW || x < 0) return;
                    const int col = x / L.colW;
                    if (col < 0 || col >= numStrips) return;
                    const auto& ls = logical[(std::size_t) col];
                    const int totalTracks = engine.getRecorder().getNumTracks();
                    dragActive = false;
                    if (! ls.stereo)
                    {
                        // Going mono → stereo. Need a right partner.
                        if (ls.trackIndex + 1 >= totalTracks) return;
                        if (engine.getRecorder().getTrack (ls.trackIndex + 1).isStereo.load()) return;
                    }
                    if (engine.linkStereoPair (ls.trackIndex, ! ls.stereo) && ! ls.stereo)
                    {
                        const auto wrapped = engine.pairWrapNotice (ls.trackIndex);
                        if (wrapped.isNotEmpty()) engine.getRemotes().showHostStatus (wrapped);
                    }
                    repaint();
                    return;
                }

                const int x = e.x - L.rowHeaderW + scrollX;   // body scrolls in X
                const int y = e.y - L.colHeaderH + scrollY;   // body scrolls in Y
                if (e.x < L.rowHeaderW || e.y < L.colHeaderH || x < 0 || y < 0)
                    { dragActive = false; return; }

                const int col = x / L.colW;
                const int row = y / L.rowH;
                if (col < 0 || col >= numStrips) { dragActive = false; return; }
                if (row < 0 || row >= numHw)     { dragActive = false; return; }

                const auto& ls = logical[(std::size_t) col];
                const int newVal = holdsRow (ls, row) ? -1 : row;
                setRoutingLogical (ls, newVal);

                dragActive   = (newVal >= 0);
                dragStartCol = col;
                dragStartRow = row;
                dragLastDr   = 0;

                repaint();
            }

            void mouseDrag (const juce::MouseEvent& e) override
            {
                if (! dragActive) return;

                const auto logical  = logicalStrips();
                const int numStrips = (int) logical.size();
                const int numHw     = isInput ? engine.getCurrentDeviceInputCount()
                                              : engine.getCurrentDeviceOutputCount();
                const auto L = computeLayout();

                const int y = e.y - L.colHeaderH + scrollY;   // body is scrolled
                if (y < 0) return;
                int row = y / L.rowH;
                row = juce::jlimit (0, numHw - 1, row);

                const int dr = row - dragStartRow;
                if (dr == dragLastDr) return;
                dragLastDr = dr;

                std::vector<bool> stereo;
                for (const auto& ls : logical) stereo.push_back (ls.stereo);
                for (const auto& [targetCol, targetRow] : PatchPage::dragTargets (stereo, dragStartCol, dragStartRow, dr, numHw))
                    setRoutingLogical (logical[(std::size_t) targetCol], targetRow);
                repaint();
            }

            void mouseUp (const juce::MouseEvent&) override { dragActive = false; }

        private:
            int numRows() const
            {
                return isInput ? engine.getCurrentDeviceInputCount()
                               : engine.getCurrentDeviceOutputCount();
            }
            int numStripsCount() const { return (int) logicalStrips().size(); }

            // Sync both scrollbars to the current content/viewport sizes.
            // Called from resized() and whenever the HW count / track set
            // changes (timer). Each bar hides when its axis already fits.
            void updateScrollRange()
            {
                const auto L = computeLayout();
                const int viewH    = juce::jmax (0, getHeight() - L.colHeaderH - kScrollW);
                const int viewW    = juce::jmax (0, getWidth()  - L.rowHeaderW - kScrollW);
                const int contentH = numRows()        * L.rowH;
                const int contentW = numStripsCount() * L.colW;

                scrollY = juce::jlimit (0, juce::jmax (0, contentH - viewH), scrollY);
                scrollX = juce::jlimit (0, juce::jmax (0, contentW - viewW), scrollX);

                vscroll.setRangeLimits (0.0, (double) juce::jmax (contentH, viewH));
                vscroll.setCurrentRange ((double) scrollY, (double) viewH);
                vscroll.setVisible (contentH > viewH);

                hscroll.setRangeLimits (0.0, (double) juce::jmax (contentW, viewW));
                hscroll.setCurrentRange ((double) scrollX, (double) viewW);
                hscroll.setVisible (contentW > viewW);
            }

            // Whether a strip occupies this row. Both halves of a pair do, each
            // where it really is: R need not be the row after L.
            bool holdsRow (const LogicalStrip& ls, int row) const
            {
                return currentRoutingForTrack (ls.trackIndex) == row
                    || (ls.stereo && currentRoutingForTrack (ls.trackIndex + 1) == row);
            }

            int currentRoutingForTrack (int trackIndex) const
            {
                // Inputs: what the strip hears on this device; a saved patch
                // past a smaller device's inputs wraps live, unchanged on disk.
                auto& t = engine.getRecorder().getTrack (trackIndex);
                return isInput ? engine.getEffectiveInputRouting (trackIndex)
                               : t.outputRouting.load (std::memory_order_relaxed);
            }
            void setRoutingLogical (const LogicalStrip& ls, int hwChannel)
            {
                // Clearing an output's dot sends the strip back to the master,
                // which is not a channel, so the input stays where it is.
                if (! isInput && hwChannel < 0)
                {
                    engine.sendStripToMaster (ls.trackIndex, ls.stereo);
                    return;
                }
                // Linked: clicking a dot in either matrix patches both
                // sides of the strip's I/O to the same hardware channel.
                engine.setStripLinkedRouting (ls.trackIndex, ls.stereo, hwChannel, numRows());
            }

            AudioEngine& engine;
            bool         isInput;

            // Scroll over the HW-channel rows (up to 128) and the strip
            // columns. The HW-channel labels and strip headers stay frozen;
            // only their respective bodies scroll.
            juce::ScrollBar vscroll { true };   // vertical   -- rows
            juce::ScrollBar hscroll { false };  // horizontal -- columns
            int             scrollY = 0;
            int             scrollX = 0;

            // Drag-patch state -- set in mouseDown, consumed in mouseDrag.
            bool dragActive   = false;
            int  dragStartCol = 0;
            int  dragStartRow = 0;
            int  dragLastDr   = 0;

            PlaceholderView placeholder;
            int             lastPlaceholderKind { -1 };   // -1 forces first sync
        };

        class PatchPageContent final : public juce::Component
        {
        public:
            explicit PatchPageContent (AudioEngine& eng)
                : tabs (juce::TabbedButtonBar::TabsAtTop)
            {
                tabs.addTab ("INPUT PATCH",  brand::bgDeep, new PatchMatrix (eng, true),  true);
                tabs.addTab ("OUTPUT PATCH", brand::bgDeep, new PatchMatrix (eng, false), true);
                tabs.setOutline (0);
                tabs.setColour (juce::TabbedComponent::backgroundColourId, brand::bgDeep);
                addAndMakeVisible (tabs);

                setSize (1180, 700);
            }
            void resized() override { tabs.setBounds (getLocalBounds()); }
            void paint (juce::Graphics& g) override { g.fillAll (brand::bgDeep); }

        private:
            juce::TabbedComponent tabs;
        };
    }

    std::vector<std::pair<int, int>> PatchPage::dragTargets (const std::vector<bool>& stripIsStereo,
                                                             int startCol, int startRow,
                                                             int rowsDragged, int numRows)
    {
        std::vector<std::pair<int, int>> targets;
        const int numCols = (int) stripIsStereo.size();
        const int sign = rowsDragged >= 0 ? 1 : -1;
        int col = startCol, row = startRow;
        while (col >= 0 && col < numCols && row >= 0 && row < numRows
               && std::abs (row - startRow) <= std::abs (rowsDragged))
        {
            if (stripIsStereo[(size_t) col] && row + 1 >= numRows && ! targets.empty()) break;
            targets.emplace_back (col, row);
            const int width = stripIsStereo[(size_t) col] ? 2 : 1;
            col += sign;
            if (sign > 0)                         row += width;
            else if (col >= 0 && col < numCols)   row -= stripIsStereo[(size_t) col] ? 2 : 1;
        }
        return targets;
    }

    std::vector<PatchPage::RowInfo> PatchPage::rowsFor (AudioEngine& engine, bool inputSide)
    {
        auto& rec = engine.getRecorder();
        const int n = rec.getNumTracks();
        const int numHw = inputSide ? engine.getCurrentDeviceInputCount() : engine.getCurrentDeviceOutputCount();
        const auto routing = [&] (int track)
        {
            return inputSide ? engine.getEffectiveInputRouting (track)
                             : rec.getTrack (track).outputRouting.load (std::memory_order_relaxed);
        };
        std::vector<RowInfo> rows;
        for (int row = 0; row < numHw; ++row)
        {
            RowInfo info;
            info.channel = row;
            info.label = (inputSide ? "IN " : "OUT ") + juce::String (row + 1);
            for (int i = 0; i < n; )
            {
                const bool stereo = rec.getTrack (i).isStereo.load() && i + 1 < n;
                if (routing (i) == row || (stereo && routing (i + 1) == row)) info.held = true;
                i += stereo ? 2 : 1;
            }
            rows.push_back (info);
        }
        return rows;
    }

    juce::DialogWindow* PatchPage::launch (AudioEngine& engine)
    {
        auto content = std::make_unique<PatchPageContent> (engine);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (content.release());
        opts.dialogTitle                  = "Patch";
        opts.dialogBackgroundColour       = brand::bgDeep;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = true;
        return launchFloating (opts);   // non-modal so the PATCH tab can toggle it
    }
}
