#include "Meterbridge.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "LedMeter.h"
#include "FloatingLaunch.h"
#include "DismissOnOutsideClick.h"

#include <algorithm>
#include <vector>

namespace zynforge
{
    namespace
    {
        class MeterbridgeContent final : public juce::Component,
                                         private juce::Timer
        {
        public:
            explicit MeterbridgeContent (AudioEngine& eng) : engine (eng)
            {
                liveBridges().push_back (this);
                rebuild();
                lastGen = engine.getRecorder().getTrackGeneration();
                setSize (juce::jmax (640, (int) meters.size() * 60 + 24), 380);
                // Watch for track-set changes. Each meter holds a TrackState&
                // that removeStripAt/setStripCount frees; without this rebuild
                // the bridge polled freed memory for the life of the window
                // (garbage meters or a crash). This is a floating window with
                // two launch paths, one of which stores no pointer, so it must
                // self-heal rather than rely on the owner to reap it.
                startTimerHz (12);
            }

            ~MeterbridgeContent() override
            {
                auto& live = liveBridges();
                live.erase (std::remove (live.begin(), live.end(), this), live.end());
            }

            // Every open bridge, so condemnAllMeters() can reach instances the
            // caller holds no pointer to. Message thread only, so no lock.
            static std::vector<MeterbridgeContent*>& liveBridges()
            {
                static std::vector<MeterbridgeContent*> v;
                return v;
            }

            // Stop reading TrackStates the caller is about to free. The 12 Hz
            // generation watch below still rebinds afterwards; this just closes
            // the window in between.
            void condemn()
            {
                for (auto& m : meters) if (m != nullptr) m->detach();
            }

            void timerCallback() override
            {
                const int gen = engine.getRecorder().getTrackGeneration();
                if (gen != lastGen)
                {
                    lastGen = gen;
                    rebuild();      // drop stale meters, rebind to live TrackStates
                    repaint();
                }
            }

            void rebuild()
            {
                meters.clear();
                names .clear();
                const int n = engine.getRecorder().getNumTracks();
                for (int i = 0; i < n; ++i)
                {
                    auto& t = engine.getRecorder().getTrack (i);

                    // Collapse a stereo pair into ONE stereo meter: skip the
                    // R half (shown on its L meter as the second bar).
                    if (i > 0 && engine.getRecorder().getTrack (i - 1)
                                       .isStereo.load (std::memory_order_relaxed))
                        continue;

                    auto m = std::make_unique<LedMeter> (t);
                    if (t.isStereo.load (std::memory_order_relaxed) && i + 1 < n)
                        m->setStereoPartner (&engine.getRecorder().getTrack (i + 1));
                    addAndMakeVisible (*m);
                    meters.push_back (std::move (m));

                    auto label = std::make_unique<juce::Label> (juce::String(), t.name);
                    label->setColour (juce::Label::textColourId, brand::textPrimary);
                    label->setJustificationType (juce::Justification::centred);
                    label->setFont (brand::type::uiLabel());
                    addAndMakeVisible (*label);
                    names.push_back (std::move (label));
                }
                resized();
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (12);
                if (meters.empty()) return;
                const int colW = juce::jmax (28, r.getWidth() / (int) meters.size());

                for (std::size_t i = 0; i < meters.size(); ++i)
                {
                    auto col = r.removeFromLeft (colW);
                    names[i] ->setBounds (col.removeFromTop (18));
                    meters[i]->setBounds (col.reduced (3, 4));
                }
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat();
                g.setGradientFill (brand::verticalGradient (brand::bgDeep, r, 0.08f, 0.12f));
                g.fillRect (r);

                // Heated Steel: near-black header band + orange seam behind the
                // track-name row, plus a stamped column number per meter.
                if (! meters.empty())
                {
                    auto inner = getLocalBounds().reduced (12);
                    auto band  = juce::Rectangle<int> (inner.getX(), inner.getY(), inner.getWidth(), 20);
                    g.setColour (brand::steelHeaderHi);   // FLAT: solid header band (no gradient)
                    g.fillRect (band);
                    g.setColour (brand::brandOrange.withAlpha (zynforge::brand::alpha::muted));
                    g.fillRect (band.getX(), band.getBottom(), band.getWidth(), 1);

                    // Stamped column numbers, aligned to the resized() columns.
                    const int colW = juce::jmax (28, inner.getWidth() / (int) meters.size());
                    g.setFont (zynforge::brand::type::monoStamp());
                    for (std::size_t i = 0; i < meters.size(); ++i)
                    {
                        auto numBox = juce::Rectangle<int> (inner.getX() + (int) i * colW + 3,
                                                            inner.getY() + 2, 22, 14);
                        g.setColour (brand::debossFace.withAlpha (zynforge::brand::alpha::prominent));
                        g.drawText (juce::String ((int) i + 1).paddedLeft ('0', 2),
                                    numBox, juce::Justification::centredLeft, false);
                    }
                }
            }

        private:
            AudioEngine& engine;
            int          lastGen { 0 };
            std::vector<std::unique_ptr<LedMeter>>    meters;
            std::vector<std::unique_ptr<juce::Label>> names;
            // Click anywhere outside the meterbridge → it closes itself.
            DismissOnOutsideClick dismisser { *this };
        };
    }

    void Meterbridge::condemnAllMeters()
    {
        for (auto* b : MeterbridgeContent::liveBridges())
            if (b != nullptr) b->condemn();
    }

    juce::DialogWindow* Meterbridge::launch (AudioEngine& engine)
    {
        auto content = std::make_unique<MeterbridgeContent> (engine);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (content.release());
        opts.dialogTitle                  = "Meterbridge";
        opts.dialogBackgroundColour       = brand::bgDeep;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = true;
        return launchFloating (opts);   // non-modal so the METERS tab can toggle it
    }
}
