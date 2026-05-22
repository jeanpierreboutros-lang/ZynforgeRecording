#include "PatchPage.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    namespace
    {
        // One half of the patch page — either input or output routing.
        class PatchMatrix final : public juce::Component, private juce::Timer
        {
        public:
            PatchMatrix (AudioEngine& eng, bool inputSide)
                : engine (eng), isInput (inputSide) { startTimerHz (10); }
            ~PatchMatrix() override { stopTimer(); }

        private:
            void timerCallback() override { repaint(); }
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

            // Returns the list of logical strips — each entry is the L track
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
                const int avail = getWidth() - L.rowHeaderW;
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
                g.setFont (juce::Font (juce::FontOptions().withHeight (13.0f).withStyle ("Bold")));
                g.drawText (isInput ? "HW IN" : "HW OUT",
                            juce::Rectangle<int> (0, 14, L.rowHeaderW, 18),
                            juce::Justification::centred, false);
                g.setFont (juce::Font (juce::FontOptions().withHeight (12.0f)));
                g.drawText ("CH ->",
                            juce::Rectangle<int> (0, 34, L.rowHeaderW, 18),
                            juce::Justification::centred, false);

                // ─── Column headers (coloured band + M button band)
                for (int c = 0; c < numStrips; ++c)
                {
                    auto& t = engine.getRecorder().getTrack (logical[(std::size_t) c].trackIndex);
                    const bool stereoCol = logical[(std::size_t) c].stereo;
                    const auto stripCol = t.colourARGB.load() != 0
                                          ? juce::Colour ((juce::uint32) t.colourARGB.load())
                                          : brand::stripColour (logical[(std::size_t) c].trackIndex);

                    // Coloured band
                    juce::Rectangle<int> head (L.rowHeaderW + c * L.colW, 0, L.colW, L.colorBandH);
                    auto inner = head.reduced (3, 3);
                    juce::ColourGradient grad (stripCol.brighter (0.10f), inner.getX(), inner.getY(),
                                               stripCol.darker  (0.10f), inner.getX(), inner.getBottom(), false);
                    g.setGradientFill (grad);
                    g.fillRoundedRectangle (inner.toFloat(), 5.0f);
                    g.setColour (stripCol.brighter (0.30f).withAlpha (0.60f));
                    g.drawRoundedRectangle (inner.toFloat(), 5.0f, 1.0f);

                    // Channel number / pair label
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions().withHeight (22.0f).withStyle ("Bold")));
                    g.drawText (juce::String (c + 1),
                                juce::Rectangle<int> (inner.getX(), inner.getY() + 4, inner.getWidth(), 26),
                                juce::Justification::centred, false);

                    // INS label — "INS N" for mono, "INS N L+R" for stereo
                    g.setFont (juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold")));
                    const auto subLabel = stereoCol
                        ? juce::String ("INS ") + juce::String (c + 1) + " (L+R)"
                        : juce::String ("INS ") + juce::String (c + 1);
                    g.drawText (subLabel,
                                juce::Rectangle<int> (inner.getX(), inner.getY() + 32, inner.getWidth(), 18),
                                juce::Justification::centred, false);

                    // M (mute) pill under header
                    juce::Rectangle<int> muteCell (L.rowHeaderW + c * L.colW, L.colorBandH, L.colW, L.muteBandH);
                    auto pill = muteCell.reduced (10, 4);
                    const bool isMuted = t.muted.load();
                    if (isMuted)
                    {
                        g.setColour (brand::accentRecord);
                        g.fillRoundedRectangle (pill.toFloat(), pill.getHeight() * 0.5f);
                        g.setColour (juce::Colours::white);
                    }
                    else
                    {
                        g.setColour (brand::bgElevated);
                        g.fillRoundedRectangle (pill.toFloat(), pill.getHeight() * 0.5f);
                        g.setColour (stripCol.brighter (0.20f).withAlpha (0.60f));
                        g.drawRoundedRectangle (pill.toFloat(), pill.getHeight() * 0.5f, 1.0f);
                        g.setColour (brand::textSecondary);
                    }
                    g.setFont (juce::Font (juce::FontOptions().withHeight (12.0f).withStyle ("Bold")));
                    g.drawText ("M", pill, juce::Justification::centred, false);
                }

                // ─── Rows
                for (int row = 0; row < numHw; ++row)
                {
                    const int y = L.colHeaderH + row * L.rowH;
                    const auto rowRect = juce::Rectangle<int> (0, y, getWidth(), L.rowH);

                    if (row % 2 == 0)
                    {
                        g.setColour (brand::bgStrip.withAlpha (0.45f));
                        g.fillRect (rowRect);
                    }

                    // Row label
                    const bool isActiveRow = isRowRoutedToAnyStrip (row, logical);
                    g.setColour (isActiveRow ? brand::textPrimary : brand::textSecondary);
                    g.setFont (juce::Font (juce::FontOptions().withHeight (14.0f).withStyle ("Bold")));
                    g.drawText ((isInput ? "IN " : "OUT ") + juce::String (row + 1),
                                juce::Rectangle<int> (14, y, L.rowHeaderW - 18, L.rowH),
                                juce::Justification::centredLeft, false);

                    // Dots
                    for (int c = 0; c < numStrips; ++c)
                    {
                        const int x = L.rowHeaderW + c * L.colW;
                        const auto cell = juce::Rectangle<int> (x, y, L.colW, L.rowH);
                        const auto dot  = cell.withSizeKeepingCentre (22, 22).toFloat();

                        const int trackIdx = logical[(std::size_t) c].trackIndex;
                        const bool stereoCol = logical[(std::size_t) c].stereo;
                        const int rtL = currentRoutingForTrack (trackIdx);
                        const int rtR = stereoCol ? currentRoutingForTrack (trackIdx + 1) : -99;
                        const bool activeL = (rtL == row);
                        const bool activeR = (rtR == row);

                        if (activeL || activeR)
                        {
                            auto& t = engine.getRecorder().getTrack (trackIdx);
                            const auto stripCol = t.colourARGB.load() != 0
                                                  ? juce::Colour ((juce::uint32) t.colourARGB.load())
                                                  : brand::stripColour (trackIdx);
                            g.setColour (stripCol);
                            g.fillEllipse (dot);
                            g.setColour (juce::Colours::white);
                            g.drawEllipse (dot, 2.0f);

                            // L / R glyph on stereo dots so users see which
                            // side of the pair this row feeds.
                            if (stereoCol)
                            {
                                g.setColour (juce::Colours::white);
                                g.setFont (juce::Font (juce::FontOptions().withHeight (10.0f).withStyle ("Bold")));
                                g.drawText (activeL ? "L" : "R",
                                            dot.toNearestInt(),
                                            juce::Justification::centred, false);
                            }
                        }
                        else
                        {
                            g.setColour (brand::textTertiary.withAlpha (0.55f));
                            g.drawEllipse (dot, 1.6f);
                        }
                    }
                }
            }

            void mouseDown (const juce::MouseEvent& e) override
            {
                const auto logical  = logicalStrips();
                const int numStrips = (int) logical.size();
                const int numHw     = isInput ? engine.getCurrentDeviceInputCount()
                                              : engine.getCurrentDeviceOutputCount();
                const auto L = computeLayout();

                // Mute pill click?
                if (e.y >= L.colorBandH && e.y < L.colHeaderH)
                {
                    const int x = e.x - L.rowHeaderW;
                    if (x < 0) return;
                    const int col = x / L.colW;
                    if (col < 0 || col >= numStrips) return;
                    const auto& ls = logical[(std::size_t) col];
                    auto& t = engine.getRecorder().getTrack (ls.trackIndex);
                    const bool newMute = ! t.muted.load();
                    t.muted.store (newMute);
                    if (ls.stereo)
                        engine.getRecorder().getTrack (ls.trackIndex + 1).muted.store (newMute);
                    dragActive = false;
                    repaint();
                    return;
                }

                const int x = e.x - L.rowHeaderW;
                const int y = e.y - L.colHeaderH;
                if (x < 0 || y < 0) { dragActive = false; return; }

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

                const int y = e.y - L.colHeaderH;
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
                for (auto& ls : logical)
                {
                    if (currentRoutingForTrack (ls.trackIndex) == row) return true;
                    if (ls.stereo && currentRoutingForTrack (ls.trackIndex + 1) == row) return true;
                }
                return false;
            }

        private:
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

            // Drag-patch state — set in mouseDown, consumed in mouseDrag.
            bool dragActive   = false;
            int  dragStartCol = 0;
            int  dragStartRow = 0;
            int  dragLastDr   = 0;
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

    void PatchPage::launch (AudioEngine& engine)
    {
        auto content = std::make_unique<PatchPageContent> (engine);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (content.release());
        opts.dialogTitle                  = "Patch";
        opts.dialogBackgroundColour       = brand::bgDeep;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = true;
        opts.launchAsync();
    }
}
