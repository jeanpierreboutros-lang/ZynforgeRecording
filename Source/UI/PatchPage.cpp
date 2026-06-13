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
                    const auto r = isInput ? t.inputRouting.load (std::memory_order_relaxed)
                                            : t.outputRouting.load (std::memory_order_relaxed);
                    const auto s = t.isStereo .load (std::memory_order_relaxed) ? 1 : 0;
                    const auto c = (std::size_t) t.colourARGB.load (std::memory_order_relaxed);
                    h = h * 1315423911u ^ (std::size_t) (r + 7);
                    h = h * 1315423911u ^ (std::size_t) (s + 11);
                    h = h * 1315423911u ^ c;
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
                        juce::ColourGradient grad (stripCol.brighter (0.10f), inner.getX(), inner.getY(),
                                                   stripCol.darker  (0.10f), inner.getX(), inner.getBottom(), false);
                        g.setGradientFill (grad);
                        g.fillRoundedRectangle (inner.toFloat(), brand::radius::lg);
                        g.setColour (stripCol.brighter (0.30f).withAlpha (brand::alpha::muted));
                        g.drawRoundedRectangle (inner.toFloat(), brand::radius::lg, 1.0f);

                        g.setColour (brand::onSignal (stripCol));
                        g.setFont (brand::type::ui (22.0f, true));
                        g.drawText (juce::String (c + 1),
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
                            g.setColour (stripCol.brighter (0.20f).withAlpha (brand::alpha::muted));
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
                    for (int row = 0; row < numHw; ++row)
                    {
                        const int y = bodyY + row * L.rowH - scrollY;
                        if (y + L.rowH <= bodyY || y >= bodyY + bodyH) continue;
                        if (row % 2 == 0)
                        {
                            g.setColour (brand::bgStrip.withAlpha (brand::alpha::scrim));
                            g.fillRect (juce::Rectangle<int> (0, y, L.rowHeaderW, L.rowH));
                        }
                        const bool isActiveRow = isRowRoutedToAnyStrip (row, logical);
                        g.setColour (isActiveRow ? brand::textPrimary : brand::textSecondary);
                        g.setFont (brand::type::sectionTitle());
                        g.drawText ((isInput ? "IN " : "OUT ") + juce::String (row + 1),
                                    juce::Rectangle<int> (14, y, L.rowHeaderW - 18, L.rowH),
                                    juce::Justification::centredLeft, false);
                    }
                }

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
                            g.setColour (brand::bgStrip.withAlpha (brand::alpha::scrim));
                            g.fillRect (juce::Rectangle<int> (bodyX, y, bodyW, L.rowH));
                        }
                        for (int c = 0; c < numStrips; ++c)
                        {
                            const int x = bodyX + c * L.colW - scrollX;
                            if (x + L.colW <= bodyX || x >= bodyX + bodyW) continue;
                            const auto cell = juce::Rectangle<int> (x, y, L.colW, L.rowH);
                            const auto dot  = cell.withSizeKeepingCentre (22, 22).toFloat();

                            const int trackIdx = logical[(std::size_t) c].trackIndex;
                            const bool active  = (currentRoutingForTrack (trackIdx) == row);
                            if (active)
                            {
                                auto& t = engine.getRecorder().getTrack (trackIdx);
                                const auto stripCol = t.colourARGB.load() != 0
                                                      ? juce::Colour ((juce::uint32) t.colourARGB.load())
                                                      : brand::stripColour (trackIdx);
                                g.setColour (stripCol);
                                g.fillEllipse (dot);
                                g.setColour (brand::onSignal (stripCol));
                                g.drawEllipse (dot, 2.0f);
                            }
                            else
                            {
                                g.setColour (brand::textTertiary.withAlpha (brand::alpha::muted));
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
                        [this] { AudioDeviceDialog::launch (engine); });
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
                    if (! ls.stereo)
                    {
                        // Going mono → stereo. Need a right partner.
                        if (ls.trackIndex + 1 >= totalTracks) return;
                        engine.setTrackStereo (ls.trackIndex, true);
                        // Hard-pan L/R for the full stereo image (buses sum
                        // centred, so skip them). Matches the mixer + import.
                        if (! engine.getRecorder().getTrack (ls.trackIndex).isBus.load())
                        {
                            engine.setTrackPan (ls.trackIndex,     -1.0f);
                            engine.setTrackPan (ls.trackIndex + 1,  1.0f);
                        }
                    }
                    else
                    {
                        engine.setTrackStereo (ls.trackIndex, false);
                        // Back to mono -> recentre both halves.
                        if (! engine.getRecorder().getTrack (ls.trackIndex).isBus.load())
                        {
                            engine.setTrackPan (ls.trackIndex,     0.0f);
                            engine.setTrackPan (ls.trackIndex + 1, 0.0f);
                        }
                    }
                    dragActive = false;
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
                const int current = currentRoutingForTrack (ls.trackIndex);
                const int newVal  = (current == row) ? -1 : row;
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

                const int sign = (dr >= 0) ? 1 : -1;
                for (int i = 0; i <= std::abs (dr); ++i)
                {
                    const int targetCol = dragStartCol + i * sign;
                    const int targetRow = dragStartRow + i * sign;
                    if (targetCol < 0 || targetCol >= numStrips) break;
                    if (targetRow < 0 || targetRow >= numHw)     break;
                    setRoutingLogical (logical[(std::size_t) targetCol], targetRow);
                }
                repaint();
            }

            void mouseUp (const juce::MouseEvent&) override { dragActive = false; }

            bool isRowRoutedToAnyStrip (int row, const std::vector<LogicalStrip>& logical) const
            {
                // Only the L position of a stereo strip activates a row
                // label -- R is implicit, so leaving its row dim keeps the
                // visual in sync with the dot-rendering rule.
                for (auto& ls : logical)
                    if (currentRoutingForTrack (ls.trackIndex) == row) return true;
                return false;
            }

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

            int currentRoutingForTrack (int trackIndex) const
            {
                auto& t = engine.getRecorder().getTrack (trackIndex);
                return isInput ? t.inputRouting .load (std::memory_order_relaxed)
                               : t.outputRouting.load (std::memory_order_relaxed);
            }
            void setRoutingLogical (const LogicalStrip& ls, int hwChannel)
            {
                // Linked: clicking a dot in either matrix patches both
                // sides of the strip's I/O to the same hardware channel.
                // For stereo: L → hwChannel, R → hwChannel + 1.
                engine.setTrackLinkedRouting (ls.trackIndex, hwChannel);
                if (ls.stereo)
                    engine.setTrackLinkedRouting (ls.trackIndex + 1,
                                                  (hwChannel < 0) ? -1 : hwChannel + 1);
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
