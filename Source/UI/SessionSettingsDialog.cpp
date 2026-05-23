#include "SessionSettingsDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        class SettingsContent final : public juce::Component
        {
        public:
            explicit SettingsContent (AudioEngine& eng) : engine (eng)
            {
                using F = CaptureFormat;
                const auto cur = engine.getRecorder().getCaptureFormat();
                const double currentSr = engine.getDeviceManager().getAudioDeviceSetup().sampleRate;

                // Format
                formatLabel.setText ("Audio Format", juce::dontSendNotification);
                formatLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (formatLabel);
                formatBox.addItem ("WAV",  1);
                formatBox.addItem ("AIFF", 2);
                formatBox.addItem ("FLAC", 3);
                const int container = (cur == F::Wav16 || cur == F::Wav24 || cur == F::Wav32Float) ? 1
                                     : (cur == F::Aiff16 || cur == F::Aiff24 || cur == F::Aiff32Float) ? 2
                                     : 3;
                formatBox.setSelectedId (container, juce::dontSendNotification);
                formatBox.onChange = [this] { refreshBitDepths(); };
                addAndMakeVisible (formatBox);

                // Sample rate
                rateLabel.setText ("Sample Rate", juce::dontSendNotification);
                rateLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (rateLabel);
                rateBox.addItem ("44.1 kHz", 1);
                rateBox.addItem ("48 kHz",   2);
                rateBox.addItem ("96 kHz",   3);
                rateBox.addItem ("192 kHz",  4);
                const int rId = juce::approximatelyEqual (currentSr, 44100.0)  ? 1
                              : juce::approximatelyEqual (currentSr, 96000.0)  ? 3
                              : juce::approximatelyEqual (currentSr, 192000.0) ? 4 : 2;
                rateBox.setSelectedId (rId, juce::dontSendNotification);
                addAndMakeVisible (rateBox);

                // Bit depth
                bitsLabel.setText ("Bit Depth", juce::dontSendNotification);
                bitsLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (bitsLabel);
                addAndMakeVisible (bitsBox);
                refreshBitDepths();

                // Pre-select current bit depth.
                const int bitId = (cur == F::Wav16 || cur == F::Aiff16 || cur == F::Flac16) ? 1
                                 : (cur == F::Wav32Float || cur == F::Aiff32Float)          ? 3
                                                                                            : 2;
                if (bitsBox.getItemId (juce::jmax (0, bitId - 1)) > 0)
                    bitsBox.setSelectedId (bitId, juce::dontSendNotification);

                // Buttons
                applyButton .setButtonText ("Apply");
                applyButton .onClick = [this] { onApply(); };
                addAndMakeVisible (applyButton);

                cancelButton.setButtonText ("Cancel");
                cancelButton.onClick = [this] { closeDialog (false); };
                addAndMakeVisible (cancelButton);

                setSize (380, 230);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (16);
                const int rowH = 30;
                const int labelW = 120;
                auto row = [&] (juce::Label& l, juce::ComboBox& b)
                {
                    auto rr = r.removeFromTop (rowH);
                    l.setBounds (rr.removeFromLeft (labelW));
                    b.setBounds (rr);
                    r.removeFromTop (brand::space::sm);
                };
                row (formatLabel, formatBox);
                row (rateLabel,   rateBox);
                row (bitsLabel,   bitsBox);

                r.removeFromTop (brand::space::lg);
                auto btnRow = r.removeFromBottom (32);
                applyButton .setBounds (btnRow.removeFromRight (110));
                btnRow.removeFromRight (brand::space::md);
                cancelButton.setBounds (btnRow.removeFromRight (110));
            }

            void paint (juce::Graphics& g) override { g.fillAll (brand::bgPanel); }

        private:
            void refreshBitDepths()
            {
                const int container = formatBox.getSelectedId();  // 1 wav, 2 aiff, 3 flac
                const int prev = bitsBox.getSelectedId();
                bitsBox.clear (juce::dontSendNotification);
                bitsBox.addItem ("16-bit PCM",   1);
                bitsBox.addItem ("24-bit PCM",   2);
                if (container != 3)                       // FLAC has no 32-bit float
                    bitsBox.addItem ("32-bit float", 3);
                if (prev > 0) bitsBox.setSelectedId (prev, juce::dontSendNotification);
                if (bitsBox.getSelectedId() == 0)
                    bitsBox.setSelectedId (2, juce::dontSendNotification);
            }

            void onApply()
            {
                if (engine.isRecording())
                {
                    juce::AlertWindow::showAsync (
                        juce::MessageBoxOptions()
                            .withIconType (juce::MessageBoxIconType::WarningIcon)
                            .withTitle ("Recording active")
                            .withMessage ("Stop recording before changing session settings.")
                            .withButton ("OK"),
                        nullptr);
                    return;
                }

                using F = CaptureFormat;
                const int container = formatBox.getSelectedId();   // 1=Wav, 2=Aiff, 3=Flac
                const int bits      = bitsBox.getSelectedId();      // 1=16, 2=24, 3=32f

                F fmt = F::Wav24;
                if (container == 1)
                    fmt = bits == 1 ? F::Wav16 : bits == 2 ? F::Wav24 : F::Wav32Float;
                else if (container == 2)
                    fmt = bits == 1 ? F::Aiff16 : bits == 2 ? F::Aiff24 : F::Aiff32Float;
                else
                    fmt = bits == 1 ? F::Flac16 : F::Flac24;

                engine.getRecorder().setCaptureFormat (fmt);

                double sr = 48000.0;
                switch (rateBox.getSelectedId())
                {
                    case 1: sr = 44100.0;  break;
                    case 2: sr = 48000.0;  break;
                    case 3: sr = 96000.0;  break;
                    case 4: sr = 192000.0; break;
                }
                auto setup = engine.getDeviceManager().getAudioDeviceSetup();
                setup.sampleRate = sr;
                const auto err = engine.getDeviceManager().setAudioDeviceSetup (setup, true);

                juce::Logger::writeToLog ("[ZF] Session settings applied — format="
                                         + juce::String ((int) fmt)
                                         + " sr=" + juce::String (sr)
                                         + (err.isEmpty() ? juce::String() : juce::String (" err=") + err));

                if (err.isNotEmpty())
                {
                    juce::AlertWindow::showAsync (
                        juce::MessageBoxOptions()
                            .withIconType (juce::MessageBoxIconType::WarningIcon)
                            .withTitle ("Could not apply sample rate")
                            .withMessage ("Audio device error:\n\n" + err
                                + "\n\nThe capture format has still been changed; the device kept its previous sample rate.")
                            .withButton ("OK"),
                        nullptr);
                }

                closeDialog (true);
            }

            void closeDialog (bool /*applied*/)
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            }

            AudioEngine& engine;
            juce::Label  formatLabel, rateLabel, bitsLabel;
            juce::ComboBox formatBox, rateBox, bitsBox;
            juce::TextButton applyButton, cancelButton;
        };
    }

    void SessionSettingsDialog::launch (AudioEngine& engine)
    {
        // Direct file breadcrumb — bypasses juce::Logger entirely so we
        // can prove the call site is wired even if writeToLog is muted
        // in Release builds.
        juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-settings-launch.txt")
                .replaceWithText (juce::Time::getCurrentTime().toISO8601 (true));

        auto content = std::make_unique<SettingsContent> (engine);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (content.release());
        opts.dialogTitle                  = "Session Settings";
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        auto* dw = opts.launchAsync();
        if (dw != nullptr)
        {
            dw->setAlwaysOnTop (true);
            dw->toFront (true);
        }
    }
}
