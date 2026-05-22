#include "AudioDeviceDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        // ── The container component
        // ───────────────────────────────────────────
        class DialogContent final : public juce::Component
        {
        public:
            explicit DialogContent (AudioEngine& eng) : engine (eng)
            {
                auto& dm = engine.getDeviceManager();

                // Snapshot current state — restored if the user cancels.
                snapshot = dm.createStateXml();

                selector = std::make_unique<juce::AudioDeviceSelectorComponent> (
                    dm,
                    0, 256,    // input channels min/max
                    0, 64,     // output channels min/max
                    false,     // showMidiInputOptions
                    false,     // showMidiOutputSelector
                    true,      // showChannelsAsStereoPairs
                    false);    // hideAdvancedOptionsWithButton
                addAndMakeVisible (*selector);

                applyButton.setColour (juce::TextButton::buttonColourId,
                                       brand::accentStatus.withAlpha (0.85f));
                applyButton.setColour (juce::TextButton::textColourOffId,
                                       brand::textPrimary);
                applyButton.onClick = [this] { applyAndClose(); };
                addAndMakeVisible (applyButton);

                cancelButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                cancelButton.setColour (juce::TextButton::textColourOffId, brand::textSecondary);
                cancelButton.onClick = [this] { cancelAndClose(); };
                addAndMakeVisible (cancelButton);

                statusLabel.setText ("Pick output, input, sample rate, buffer — Apply commits.",
                                     juce::dontSendNotification);
                statusLabel.setJustificationType (juce::Justification::centredLeft);
                statusLabel.setColour (juce::Label::textColourId, brand::textTertiary);
                statusLabel.setFont (brand::type::statusBar());
                addAndMakeVisible (statusLabel);

                setSize (600, 540);
            }

            ~DialogContent() override
            {
                if (! applied && snapshot != nullptr)
                {
                    // User dismissed without applying — restore previous state.
                    engine.getDeviceManager().initialise (0, 256, snapshot.get(), true);
                }
            }

            void paint (juce::Graphics& g) override
            {
                // Vertical gradient backdrop in the panel colour.
                auto r = getLocalBounds().toFloat();
                g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.05f, 0.15f));
                g.fillAll();

                // Title block at the top
                auto title = getLocalBounds().removeFromTop (44).reduced (brand::space::md, 0);
                g.setColour (brand::brandOrange);
                g.fillRect (title.removeFromLeft (3));
                g.setColour (brand::textPrimary);
                g.setFont (brand::type::sectionTitle());
                g.drawText ("AUDIO DEVICE", title.translated (brand::space::md, 0),
                            juce::Justification::centredLeft, false);

                // Divider above buttons.
                const auto footY = getHeight() - 56;
                g.setColour (brand::edge);
                g.drawHorizontalLine (footY, 0.0f, (float) getWidth());
            }

            void resized() override
            {
                auto r = getLocalBounds();
                r.removeFromTop (44);                  // title block
                auto footer = r.removeFromBottom (56).reduced (brand::space::md);
                r.reduce (brand::space::md, brand::space::sm);

                if (selector != nullptr)
                    selector->setBounds (r);

                // Footer: status label on the left, Cancel + Apply on the right.
                applyButton .setBounds (footer.removeFromRight (110)
                                              .reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::sm);
                cancelButton.setBounds (footer.removeFromRight (90)
                                              .reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::md);
                statusLabel.setBounds (footer);
            }

        private:
            void applyAndClose()
            {
                applied = true;
                statusLabel.setText ("Applied.", juce::dontSendNotification);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (1);
            }

            void cancelAndClose()
            {
                applied = false;
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            }

            AudioEngine&                                          engine;
            std::unique_ptr<juce::AudioDeviceSelectorComponent>   selector;
            juce::TextButton                                      applyButton  { "Apply"  };
            juce::TextButton                                      cancelButton { "Cancel" };
            juce::Label                                           statusLabel;
            std::unique_ptr<juce::XmlElement>                     snapshot;
            bool                                                  applied { false };
        };
    }

    void AudioDeviceDialog::launch (AudioEngine& engine)
    {
        auto content = std::make_unique<DialogContent> (engine);

        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Audio Device";
        opts.content.setOwned (content.release());
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = true;
        opts.launchAsync();
    }
}
