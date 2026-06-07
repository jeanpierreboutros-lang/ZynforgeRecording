#include "Meterbridge.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "LedMeter.h"
#include "FloatingLaunch.h"
#include "DismissOnOutsideClick.h"

namespace zynforge
{
    namespace
    {
        class MeterbridgeContent final : public juce::Component
        {
        public:
            explicit MeterbridgeContent (AudioEngine& eng) : engine (eng)
            {
                rebuild();
                setSize (juce::jmax (640, (int) meters.size() * 60 + 24), 380);
            }

            void rebuild()
            {
                meters.clear();
                names .clear();
                const int n = engine.getRecorder().getNumTracks();
                for (int i = 0; i < n; ++i)
                {
                    auto& t = engine.getRecorder().getTrack (i);
                    auto m = std::make_unique<LedMeter> (t);
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
            }

        private:
            AudioEngine& engine;
            std::vector<std::unique_ptr<LedMeter>>    meters;
            std::vector<std::unique_ptr<juce::Label>> names;
            // Click anywhere outside the meterbridge → it closes itself.
            DismissOnOutsideClick dismisser { *this };
        };
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
