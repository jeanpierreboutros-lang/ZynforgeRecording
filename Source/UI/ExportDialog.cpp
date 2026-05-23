#include "ExportDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        class ExportDialogContent final : public juce::Component
        {
        public:
            explicit ExportDialogContent (ExportDialog::ResultCallback cb)
                : callback (std::move (cb))
            {
                // Format
                formatLabel.setText ("Format", juce::dontSendNotification);
                formatLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (formatLabel);

                formatBox.addItem ("WAV",  1);
                formatBox.addItem ("AIFF", 2);
                formatBox.addItem ("FLAC", 3);
                formatBox.addItem ("MP3",  4);
                formatBox.setSelectedId (1);
                formatBox.onChange = [this] { refreshAvailability(); };
                addAndMakeVisible (formatBox);

                // Sample rate
                rateLabel.setText ("Sample rate", juce::dontSendNotification);
                rateLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (rateLabel);

                rateBox.addItem ("44.1 kHz",  1);
                rateBox.addItem ("48 kHz",    2);
                rateBox.addItem ("96 kHz",    3);
                rateBox.addItem ("192 kHz",   4);
                rateBox.setSelectedId (2);
                addAndMakeVisible (rateBox);

                // Bit depth
                bitsLabel.setText ("Bit depth", juce::dontSendNotification);
                bitsLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (bitsLabel);

                bitsBox.addItem ("16-bit PCM",   1);
                bitsBox.addItem ("24-bit PCM",   2);
                bitsBox.addItem ("32-bit float", 3);
                bitsBox.setSelectedId (2);
                addAndMakeVisible (bitsBox);

                // MP3 bitrate
                bitrateLabel.setText ("MP3 bitrate", juce::dontSendNotification);
                bitrateLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (bitrateLabel);

                bitrateBox.addItem ("128 kbps", 1);
                bitrateBox.addItem ("256 kbps", 2);
                bitrateBox.addItem ("320 kbps", 3);
                bitrateBox.setSelectedId (2);
                addAndMakeVisible (bitrateBox);

                // Buttons
                okButton.setButtonText ("Export");
                okButton.onClick = [this] { onAccept(); };
                addAndMakeVisible (okButton);

                cancelButton.setButtonText ("Cancel");
                cancelButton.onClick = [this] { onCancel(); };
                addAndMakeVisible (cancelButton);

                refreshAvailability();
                setSize (380, 240);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (16);
                const int rowH = 28;
                const int labelW = 110;

                auto row = [&] (juce::Label& label, juce::ComboBox& box)
                {
                    auto rr = r.removeFromTop (rowH);
                    label.setBounds (rr.removeFromLeft (labelW));
                    box  .setBounds (rr);
                    r.removeFromTop (brand::space::md);
                };

                row (formatLabel,  formatBox);
                row (rateLabel,    rateBox);
                row (bitsLabel,    bitsBox);
                row (bitrateLabel, bitrateBox);

                r.removeFromTop (brand::space::lg);
                auto btnRow = r.removeFromBottom (32);
                okButton    .setBounds (btnRow.removeFromRight (110));
                btnRow.removeFromRight (brand::space::md);
                cancelButton.setBounds (btnRow.removeFromRight (110));
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat();
                g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.08f, 0.14f));
                g.fillRect (r);
            }

        private:
            void refreshAvailability()
            {
                const int sel = formatBox.getSelectedId();
                const bool isMp3  = (sel == 4);
                const bool isFlac = (sel == 3);

                bitsLabel.setEnabled (! isMp3);
                bitsBox  .setEnabled (! isMp3);

                // FLAC: only 16-bit or 24-bit valid.
                // Disable the 32-bit-float item by rebuilding the combo state.
                bitsBox.clear (juce::dontSendNotification);
                bitsBox.addItem ("16-bit PCM",   1);
                bitsBox.addItem ("24-bit PCM",   2);
                if (! isFlac && ! isMp3)
                    bitsBox.addItem ("32-bit float", 3);
                if (bitsBox.getSelectedId() == 0) bitsBox.setSelectedId (2);

                bitrateLabel.setEnabled (isMp3);
                bitrateBox  .setEnabled (isMp3);
            }

            void onAccept()
            {
                ExportOptions opts;

                switch (formatBox.getSelectedId())
                {
                    case 1: opts.format = ExportFormat::Wav24;  break;
                    case 2: opts.format = ExportFormat::Aiff24; break;
                    case 3: opts.format = ExportFormat::Flac24; break;
                    case 4: opts.format = ExportFormat::Mp3;    break;
                }

                switch (rateBox.getSelectedId())
                {
                    case 1: opts.sampleRate = 44100.0;  break;
                    case 2: opts.sampleRate = 48000.0;  break;
                    case 3: opts.sampleRate = 96000.0;  break;
                    case 4: opts.sampleRate = 192000.0; break;
                }

                switch (bitsBox.getSelectedId())
                {
                    case 1: opts.bitsPerSample = 16; break;
                    case 2: opts.bitsPerSample = 24; break;
                    case 3: opts.bitsPerSample = 32; break;
                    default: opts.bitsPerSample = 24;
                }

                switch (bitrateBox.getSelectedId())
                {
                    case 1: opts.mp3Bitrate = 128; break;
                    case 2: opts.mp3Bitrate = 256; break;
                    case 3: opts.mp3Bitrate = 320; break;
                }

                if (callback) callback (opts);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (1);
            }

            void onCancel()
            {
                if (callback) callback (std::nullopt);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            }

            ExportDialog::ResultCallback callback;

            juce::Label    formatLabel, rateLabel, bitsLabel, bitrateLabel;
            juce::ComboBox formatBox,   rateBox,   bitsBox,   bitrateBox;
            juce::TextButton okButton, cancelButton;
        };
    }

    void ExportDialog::launch (const juce::String& title, ResultCallback cb)
    {
        // We wrap the callback so the dialog can only fire it once.
        auto shared = std::make_shared<ResultCallback> (std::move (cb));
        auto fireOnce = [shared] (std::optional<ExportOptions> result)
        {
            if (shared && *shared) { (*shared)(result); *shared = nullptr; }
        };

        auto content = std::make_unique<ExportDialogContent> (fireOnce);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (content.release());
        opts.dialogTitle                  = title;
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
