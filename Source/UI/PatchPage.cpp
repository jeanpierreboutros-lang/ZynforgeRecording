#include "PatchPage.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    namespace
    {
        // One half of the patch page — either input or output routing.
        class PatchMatrix final : public juce::Component
        {
        public:
            PatchMatrix (AudioEngine& eng, bool inputSide)
                : engine (eng), isInput (inputSide) {}

            void paint (juce::Graphics& g) override
            {
                g.fillAll (brand::bgDeep);

                const int numStrips = engine.getRecorder().getNumTracks();
                const int numHw     = isInput ? engine.getCurrentDeviceInputCount()
                                              : engine.getCurrentDeviceOutputCount();

                auto bounds = getLocalBounds();
                const int rowHeaderW = 60;
                const int colHeaderH = 56;
                const int rowH       = 30;
                const int colW       = juce::jmax (44, (bounds.getWidth() - rowHeaderW) / juce::jmax (1, numStrips));

                // Column headers (channel strips, coloured)
                for (int c = 0; c < numStrips; ++c)
                {
                    auto& t = engine.getRecorder().getTrack (c);
                    const auto stripCol = t.colourARGB.load() != 0
                                          ? juce::Colour ((juce::uint32) t.colourARGB.load())
                                          : brand::stripColour (c);

                    juce::Rectangle<int> head (rowHeaderW + c * colW, 0, colW, colHeaderH);
                    g.setColour (stripCol);
                    g.fillRect (head.reduced (2));
                    g.setColour (juce::Colours::white);
                    g.setFont (juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold")));
                    g.drawText (juce::String (c + 1), head.removeFromTop (16),
                                juce::Justification::centred, false);
                    g.setFont (juce::Font (juce::FontOptions().withHeight (10.0f)));
                    g.drawText ("INS " + juce::String (c + 1), head.removeFromTop (14),
                                juce::Justification::centred, false);
                }

                // Top-left label
                g.setColour (brand::textMuted);
                g.setFont (juce::Font (juce::FontOptions().withHeight (10.0f)));
                g.drawText (isInput ? "HW IN" : "HW OUT",
                            juce::Rectangle<int> (4, 8, rowHeaderW - 8, 14),
                            juce::Justification::topLeft, false);
                g.drawText (isInput ? "CH ->" : "CH ->",
                            juce::Rectangle<int> (4, 26, rowHeaderW - 8, 14),
                            juce::Justification::topLeft, false);

                // Row headers + dots
                for (int row = 0; row < numHw; ++row)
                {
                    const int y = colHeaderH + row * rowH;
                    const auto rowRect = juce::Rectangle<int> (0, y, bounds.getWidth(), rowH);

                    // Alternating background
                    if (row % 2 == 0)
                    {
                        g.setColour (brand::bgPanel.withAlpha (0.5f));
                        g.fillRect (rowRect);
                    }

                    g.setColour (brand::textPrimary);
                    g.setFont (juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold")));
                    g.drawText ((isInput ? "IN " : "OUT ") + juce::String (row + 1),
                                juce::Rectangle<int> (8, y, rowHeaderW - 12, rowH),
                                juce::Justification::centredLeft, false);

                    // Dots
                    for (int c = 0; c < numStrips; ++c)
                    {
                        const int x = rowHeaderW + c * colW;
                        const auto cell = juce::Rectangle<int> (x, y, colW, rowH);
                        const auto dot  = cell.withSizeKeepingCentre (14, 14).toFloat();

                        const int rt = currentRouting (c);
                        const bool active = (rt == row);

                        g.setColour (active ? brand::accentStatus : brand::edge);
                        if (active) g.fillEllipse (dot);
                        else        g.drawEllipse (dot, 1.5f);
                    }
                }

                // Footer help text
                g.setColour (brand::textMuted);
                g.setFont (juce::Font (juce::FontOptions().withHeight (12.0f)));
                auto footer = bounds.removeFromBottom (28);
                g.drawText ("Click a circle to route a hardware "
                            + juce::String (isInput ? "input" : "output")
                            + " to a channel. Click the active circle again to clear.",
                            footer,
                            juce::Justification::centred, false);
            }

            void mouseDown (const juce::MouseEvent& e) override
            {
                const int numStrips = engine.getRecorder().getNumTracks();
                const int numHw     = isInput ? engine.getCurrentDeviceInputCount()
                                              : engine.getCurrentDeviceOutputCount();

                const auto bounds = getLocalBounds();
                const int rowHeaderW = 60;
                const int colHeaderH = 56;
                const int rowH       = 30;
                const int colW       = juce::jmax (44, (bounds.getWidth() - rowHeaderW) / juce::jmax (1, numStrips));

                const int x = e.x - rowHeaderW;
                const int y = e.y - colHeaderH;
                if (x < 0 || y < 0) return;

                const int col = x / colW;
                const int row = y / rowH;
                if (col < 0 || col >= numStrips) return;
                if (row < 0 || row >= numHw)     return;

                const int current = currentRouting (col);
                const int newVal  = (current == row) ? -1 : row;
                setRouting (col, newVal);
                repaint();
            }

        private:
            int currentRouting (int strip) const
            {
                auto& t = engine.getRecorder().getTrack (strip);
                return isInput ? t.inputRouting .load (std::memory_order_relaxed)
                               : t.outputRouting.load (std::memory_order_relaxed);
            }
            void setRouting (int strip, int hwChannel)
            {
                // Linked: clicking a dot in either matrix patches both
                // sides of the strip's I/O to the same hardware channel.
                engine.setTrackLinkedRouting (strip, hwChannel);
            }

            AudioEngine& engine;
            bool         isInput;
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
