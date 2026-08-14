#include "ExportDialog.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include "../Theme/DialogChrome.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        class ExportDialogContent final : public juce::Component
        {
        public:
            ExportDialogContent (ExportDialog::ResultCallback cb, double sessionRate)
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

                // Standard rates, plus the session's own rate when it isn't one
                // of them (88.2 kHz sessions exist). The session rate is
                // preselected and marked, so "just export it" is a straight copy
                // and choosing a conversion is a deliberate act.
                rateHz = { 44100.0, 48000.0, 96000.0, 192000.0 };
                const double sr = sessionRate > 0.0 ? sessionRate : 48000.0;
                if (std::find_if (rateHz.begin(), rateHz.end(),
                                  [sr] (double r) { return std::abs (r - sr) < 1.0; })
                    == rateHz.end())
                    rateHz.push_back (sr);
                std::sort (rateHz.begin(), rateHz.end());

                int selectId = 1;
                for (size_t i = 0; i < rateHz.size(); ++i)
                {
                    const bool isSession = std::abs (rateHz[i] - sr) < 1.0;
                    auto label = juce::String (rateHz[i] / 1000.0, rateHz[i] < 100000.0 ? 1 : 0)
                                 + " kHz" + (isSession ? "  (session)" : "");
                    rateBox.addItem (label, (int) i + 1);
                    if (isSession) selectId = (int) i + 1;
                }
                rateBox.setSelectedId (selectId);
                rateBox.onChange = [this] { refreshAvailability(); };
                addAndMakeVisible (rateBox);

                srNote.setFont (brand::type::caption());
                srNote.setJustificationType (juce::Justification::centredLeft);
                addAndMakeVisible (srNote);

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
                dialog::stylePrimary (okButton);
                okButton.setButtonText ("Export");
                okButton.onClick = [this] { onAccept(); };
                addAndMakeVisible (okButton);

                dialog::styleSecondary (cancelButton);
                cancelButton.setButtonText ("Cancel");
                cancelButton.onClick = [this] { onCancel(); };
                addAndMakeVisible (cancelButton);

                sessionRateHz = sr;
                refreshAvailability();

                // Return = Export, Escape = Cancel, matching every other prompt
                // in the app (result code 1 for the primary action).
                okButton.setWantsKeyboardFocus (true);
                juce::MessageManager::callAsync (
                    [safe = juce::Component::SafePointer<ExportDialogContent> (this)]
                    { if (safe != nullptr) safe->okButton.grabKeyboardFocus(); });

                setSize (380, 268);
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

                srNote.setBounds (r.removeFromTop (18));
                r.removeFromTop (brand::space::sm);
                auto btnRow = r.removeFromBottom (32);
                okButton    .setBounds (btnRow.removeFromRight (110));
                btnRow.removeFromRight (brand::space::md);
                cancelButton.setBounds (btnRow.removeFromRight (110));
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "EXPORT");
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
                // Disable the 32-bit-float item by rebuilding the combo state,
                // but PRESERVE the engineer's current bit-depth across the
                // rebuild -- clear() wiped it, so flipping WAV->FLAC (both do
                // 16-bit) silently jumped a chosen 16-bit back to 24-bit.
                const int prevId = bitsBox.getSelectedId();
                bitsBox.clear (juce::dontSendNotification);
                bitsBox.addItem ("16-bit PCM",   1);
                bitsBox.addItem ("24-bit PCM",   2);
                if (! isFlac && ! isMp3)
                    bitsBox.addItem ("32-bit float", 3);
                const bool prevStillValid = prevId == 1 || prevId == 2
                                            || (prevId == 3 && ! isFlac && ! isMp3);
                bitsBox.setSelectedId (prevStillValid ? prevId : 2, juce::dontSendNotification);

                bitrateLabel.setEnabled (isMp3);
                bitrateBox  .setEnabled (isMp3);

                const int rateIdx = rateBox.getSelectedId() - 1;
                const double chosen = (rateIdx >= 0 && rateIdx < (int) rateHz.size())
                                          ? rateHz[(size_t) rateIdx] : sessionRateHz;
                const bool converting = std::abs (chosen - sessionRateHz) > 1.0;
                srNote.setText (converting
                                  ? "Sample-rate conversion from "
                                    + juce::String (sessionRateHz / 1000.0, 1) + " kHz"
                                  : "No sample-rate conversion",
                                juce::dontSendNotification);
                srNote.setColour (juce::Label::textColourId,
                                  converting ? brand::accentSolo : brand::textTertiary);
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

                const int rateIdx = rateBox.getSelectedId() - 1;
                opts.sampleRate = (rateIdx >= 0 && rateIdx < (int) rateHz.size())
                                      ? rateHz[(size_t) rateIdx]
                                      : sessionRateHz;

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
            std::vector<double> rateHz;
            double              sessionRateHz { 48000.0 };
            juce::Label         srNote;

            juce::Label    formatLabel, rateLabel, bitsLabel, bitrateLabel;
            juce::ComboBox formatBox,   rateBox,   bitsBox,   bitrateBox;
            juce::TextButton okButton, cancelButton;
        };
    }

    void ExportDialog::launch (const juce::String& title,
                               double sessionSampleRate,
                               ResultCallback cb)
    {
        // We wrap the callback so the dialog can only fire it once.
        auto shared = std::make_shared<ResultCallback> (std::move (cb));
        auto fireOnce = [shared] (std::optional<ExportOptions> result)
        {
            if (shared && *shared) { (*shared)(result); *shared = nullptr; }
        };

        auto content = std::make_unique<ExportDialogContent> (fireOnce, sessionSampleRate);

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
