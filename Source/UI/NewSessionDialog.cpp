#include "NewSessionDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        // Field-label style used above every editor / combo.
        static void styleFieldLabel (juce::Label& l)
        {
            l.setFont (brand::type::uiBody().withHeight (13.0f));
            l.setColour (juce::Label::textColourId, brand::textSecondary);
        }

        // Black-bg / outlined editor, matches the screenshot.
        static void styleTextField (juce::TextEditor& t)
        {
            t.setFont (brand::type::uiBody().withHeight (15.0f));
            t.setColour (juce::TextEditor::backgroundColourId,   juce::Colour (0xff000000));
            t.setColour (juce::TextEditor::textColourId,         brand::textPrimary);
            t.setColour (juce::TextEditor::outlineColourId,      brand::edge);
            t.setColour (juce::TextEditor::focusedOutlineColourId, brand::accentStatus);
            t.setColour (juce::TextEditor::highlightColourId,    brand::accentStatus.withAlpha (0.35f));
        }

        static void styleCombo (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff000000));
            c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
            c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
            c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
        }

        class Content final : public juce::Component
        {
        public:
            explicit Content (const juce::File& defaultLocation,
                              double            lastSampleRate,
                              CaptureFormat     lastCaptureFormat,
                              NewSessionDialog::ResultCallback cb)
                : storageDir (defaultLocation),
                  callback (std::move (cb))
            {
                // ─── Name ───
                addAndMakeVisible (nameLabel);
                nameLabel.setText ("Name", juce::dontSendNotification);
                styleFieldLabel (nameLabel);

                addAndMakeVisible (nameEditor);
                styleTextField   (nameEditor);
                nameEditor.setText ("Untitled-1", juce::dontSendNotification);
                nameEditor.setSelectAllWhenFocused (true);
                nameEditor.grabKeyboardFocus();

                // ─── Storage radios ───
                addAndMakeVisible (localRadio);
                localRadio.setButtonText ("Local Storage:");
                localRadio.setRadioGroupId (1);
                localRadio.setToggleState (true, juce::dontSendNotification);
                localRadio.setColour (juce::ToggleButton::textColourId,      brand::textPrimary);
                localRadio.setColour (juce::ToggleButton::tickColourId,      brand::accentStatus);
                localRadio.setColour (juce::ToggleButton::tickDisabledColourId, brand::textMuted);

                addAndMakeVisible (pathButton);
                pathButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                pathButton.setColour (juce::TextButton::textColourOffId, brand::accentStatus);
                pathButton.setButtonText (compactPath (storageDir));
                pathButton.setTooltip ("Choose the folder to create the session in.");
                pathButton.onClick = [this] { pickFolder(); };

                addAndMakeVisible (cloudRadio);
                cloudRadio.setButtonText ("Cloud Project");
                cloudRadio.setRadioGroupId (1);
                cloudRadio.setEnabled (false);
                cloudRadio.setColour (juce::ToggleButton::textColourId,      brand::textMuted);
                cloudRadio.setColour (juce::ToggleButton::tickColourId,      brand::accentStatus);
                cloudRadio.setColour (juce::ToggleButton::tickDisabledColourId, brand::textMuted);

                addAndMakeVisible (cloudHelp);
                cloudHelp.setButtonText ("?");
                cloudHelp.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                cloudHelp.setColour (juce::TextButton::textColourOffId, brand::textMuted);
                cloudHelp.setTooltip ("Cloud project storage is not available in this build.");

                // ─── Section header ───
                addAndMakeVisible (sectionHeader);
                sectionHeader.setText ("v  Session Configuration", juce::dontSendNotification);
                sectionHeader.setFont (brand::type::sectionTitle().withHeight (15.0f));
                sectionHeader.setColour (juce::Label::textColourId, brand::textPrimary);

                // ─── File Type ───
                addAndMakeVisible (fileTypeLabel);
                fileTypeLabel.setText ("File Type", juce::dontSendNotification);
                styleFieldLabel (fileTypeLabel);

                addAndMakeVisible (fileTypeCombo);
                styleCombo (fileTypeCombo);
                fileTypeCombo.addItem ("BWF (.WAV)", 1);
                fileTypeCombo.addItem ("AIFF",       2);
                fileTypeCombo.addItem ("FLAC",       3);
                fileTypeCombo.setSelectedId (fileTypeIdFromFormat (lastCaptureFormat), juce::dontSendNotification);
                fileTypeCombo.onChange = [this] { syncBitDepthForFormat(); };

                // ─── Sample Rate ───
                addAndMakeVisible (sampleRateLabel);
                sampleRateLabel.setText ("Sample Rate", juce::dontSendNotification);
                styleFieldLabel (sampleRateLabel);

                addAndMakeVisible (sampleRateCombo);
                styleCombo (sampleRateCombo);
                static const std::pair<int, const char*> srs[] = {
                    { 1, "44.1 kHz" }, { 2, "48 kHz" }, { 3, "88.2 kHz" },
                    { 4, "96 kHz"   }, { 5, "176.4 kHz" }, { 6, "192 kHz" }
                };
                for (auto& s : srs) sampleRateCombo.addItem (s.second, s.first);
                sampleRateCombo.setSelectedId (sampleRateIdFromHz (lastSampleRate), juce::dontSendNotification);

                // ─── Bit Depth ───
                addAndMakeVisible (bitDepthLabel);
                bitDepthLabel.setText ("Bit Depth", juce::dontSendNotification);
                styleFieldLabel (bitDepthLabel);

                addAndMakeVisible (bitDepthCombo);
                styleCombo (bitDepthCombo);
                bitDepthCombo.addItem ("16-bit",        1);
                bitDepthCombo.addItem ("24-bit",        2);
                bitDepthCombo.addItem ("32-bit float",  3);
                bitDepthCombo.setSelectedId (bitDepthIdFromFormat (lastCaptureFormat), juce::dontSendNotification);

                // ─── I/O Settings ───
                addAndMakeVisible (ioLabel);
                ioLabel.setText ("I/O Settings", juce::dontSendNotification);
                styleFieldLabel (ioLabel);

                addAndMakeVisible (ioCombo);
                styleCombo (ioCombo);
                ioCombo.addItem ("Last Used", 1);
                ioCombo.addItem ("Stereo Mix", 2);
                ioCombo.addItem ("5.1 Film",   3);
                ioCombo.setSelectedId (1, juce::dontSendNotification);

                // ─── Interleaved ───
                addAndMakeVisible (interleavedToggle);
                interleavedToggle.setButtonText ("Interleaved");
                interleavedToggle.setToggleState (true, juce::dontSendNotification);
                interleavedToggle.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                interleavedToggle.setColour (juce::ToggleButton::tickColourId, brand::accentStatus);
                interleavedToggle.setColour (juce::ToggleButton::tickDisabledColourId, brand::textMuted);

                // ─── Buttons ───
                addAndMakeVisible (cancelButton);
                cancelButton.setButtonText ("Cancel");
                cancelButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                cancelButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
                cancelButton.onClick = [this] { dismiss (false); };

                addAndMakeVisible (createButton);
                createButton.setButtonText ("Create");
                createButton.setColour (juce::TextButton::buttonColourId, brand::accentStatus);
                createButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
                createButton.onClick = [this] { dismiss (true); };

                setSize (520, 460);
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat();
                g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.04f, 0.12f));
                g.fillAll();

                g.setColour (brand::edge);
                g.drawHorizontalLine (getHeight() - 60, 0.0f, (float) getWidth());
            }

            void resized() override
            {
                auto b = getLocalBounds().reduced (24, 20);

                // Name
                nameLabel.setBounds (b.removeFromTop (18));
                b.removeFromTop (brand::space::xs);
                nameEditor.setBounds (b.removeFromTop (32));
                b.removeFromTop (brand::space::xl);

                // Local Storage row: [○] Local Storage:  [path]
                auto local = b.removeFromTop (24);
                localRadio.setBounds (local.removeFromLeft (130));
                local.removeFromLeft (brand::space::xs);
                pathButton.setBounds (local.removeFromLeft (juce::jmin (local.getWidth(), 280)));
                b.removeFromTop (brand::space::sm);

                // Cloud row
                auto cloud = b.removeFromTop (24);
                cloudRadio.setBounds (cloud.removeFromLeft (130));
                cloudHelp .setBounds (cloud.removeFromLeft (22));
                b.removeFromTop (18);

                // Section header
                sectionHeader.setBounds (b.removeFromTop (22));
                b.removeFromTop (brand::space::md);

                // Row: File Type / Sample Rate
                auto row1 = b.removeFromTop (54);
                auto colW = (row1.getWidth() - 16) / 2;
                {
                    auto left  = row1.removeFromLeft (colW);
                    fileTypeLabel.setBounds (left.removeFromTop (18));
                    left.removeFromTop (brand::space::xs);
                    fileTypeCombo.setBounds (left.removeFromTop (28));
                }
                row1.removeFromLeft (brand::space::xl);
                {
                    auto right = row1;
                    sampleRateLabel.setBounds (right.removeFromTop (18));
                    right.removeFromTop (brand::space::xs);
                    sampleRateCombo.setBounds (right.removeFromTop (28));
                }
                b.removeFromTop (brand::space::xl);

                // Row: Bit Depth / I/O Settings
                auto row2 = b.removeFromTop (54);
                {
                    auto left  = row2.removeFromLeft (colW);
                    bitDepthLabel.setBounds (left.removeFromTop (18));
                    left.removeFromTop (brand::space::xs);
                    bitDepthCombo.setBounds (left.removeFromTop (28));
                }
                row2.removeFromLeft (brand::space::xl);
                {
                    auto right = row2;
                    ioLabel.setBounds (right.removeFromTop (18));
                    right.removeFromTop (brand::space::xs);
                    ioCombo.setBounds (right.removeFromTop (28));
                }
                b.removeFromTop (14);

                interleavedToggle.setBounds (b.removeFromTop (24).removeFromLeft (160));

                // Footer
                auto footer = getLocalBounds().removeFromBottom (52).reduced (20, 10);
                createButton.setBounds (footer.removeFromRight (96));
                footer.removeFromRight (brand::space::lg);
                cancelButton.setBounds (footer.removeFromRight (96));
            }

        private:
            static int fileTypeIdFromFormat (CaptureFormat f)
            {
                if (f == CaptureFormat::Aiff16 || f == CaptureFormat::Aiff24 || f == CaptureFormat::Aiff32Float) return 2;
                if (f == CaptureFormat::Flac16 || f == CaptureFormat::Flac24)                                   return 3;
                return 1;
            }

            static int bitDepthIdFromFormat (CaptureFormat f)
            {
                switch (f)
                {
                    case CaptureFormat::Wav16:       case CaptureFormat::Aiff16:      case CaptureFormat::Flac16: return 1;
                    case CaptureFormat::Wav24:       case CaptureFormat::Aiff24:      case CaptureFormat::Flac24: return 2;
                    case CaptureFormat::Wav32Float:  case CaptureFormat::Aiff32Float:                             return 3;
                }
                return 2;
            }

            static int sampleRateIdFromHz (double hz)
            {
                if (hz <= 44150.0)  return 1;
                if (hz <= 48050.0)  return 2;
                if (hz <= 88250.0)  return 3;
                if (hz <= 96050.0)  return 4;
                if (hz <= 176450.0) return 5;
                return 6;
            }

            static double hzFromId (int id)
            {
                switch (id)
                {
                    case 1: return 44100.0;
                    case 2: return 48000.0;
                    case 3: return 88200.0;
                    case 4: return 96000.0;
                    case 5: return 176400.0;
                    case 6: return 192000.0;
                }
                return 48000.0;
            }

            static CaptureFormat formatFrom (int fileTypeId, int bitDepthId)
            {
                if (fileTypeId == 2) // AIFF
                {
                    if (bitDepthId == 1) return CaptureFormat::Aiff16;
                    if (bitDepthId == 3) return CaptureFormat::Aiff32Float;
                    return CaptureFormat::Aiff24;
                }
                if (fileTypeId == 3) // FLAC — no 32-bit float
                {
                    if (bitDepthId == 1) return CaptureFormat::Flac16;
                    return CaptureFormat::Flac24;
                }
                // WAV
                if (bitDepthId == 1) return CaptureFormat::Wav16;
                if (bitDepthId == 3) return CaptureFormat::Wav32Float;
                return CaptureFormat::Wav24;
            }

            void syncBitDepthForFormat()
            {
                // FLAC doesn't support 32-bit float — silently fall back to 24-bit.
                if (fileTypeCombo.getSelectedId() == 3 && bitDepthCombo.getSelectedId() == 3)
                    bitDepthCombo.setSelectedId (2, juce::sendNotification);
            }

            void pickFolder()
            {
                folderChooser = std::make_unique<juce::FileChooser> (
                    "Choose where to create the session",
                    storageDir);

                folderChooser->launchAsync (juce::FileBrowserComponent::canSelectDirectories
                                            | juce::FileBrowserComponent::openMode,
                    [this] (const juce::FileChooser& fc)
                {
                    const auto chosen = fc.getResult();
                    if (chosen.isDirectory())
                    {
                        storageDir = chosen;
                        pathButton.setButtonText (compactPath (storageDir));
                    }
                });
            }

            static juce::String compactPath (const juce::File& f)
            {
                auto p = f.getFullPathName();
                const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
                if (p.startsWith (home)) p = "~" + p.substring (home.length());
                if (! p.endsWith ("/")) p += "/";
                return p;
            }

            void dismiss (bool create)
            {
                if (create && callback)
                {
                    NewSessionDialog::Result r;
                    r.name          = nameEditor.getText().trim().isNotEmpty()
                                          ? nameEditor.getText().trim()
                                          : juce::String ("Untitled-1");
                    r.location      = storageDir;
                    r.captureFormat = formatFrom (fileTypeCombo.getSelectedId(),
                                                  bitDepthCombo.getSelectedId());
                    r.sampleRate    = hzFromId (sampleRateCombo.getSelectedId());
                    r.interleaved   = interleavedToggle.getToggleState();
                    r.ioSettings    = ioCombo.getText();
                    callback (r);
                }

                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (create ? 1 : 0);
            }

            juce::File storageDir;
            std::unique_ptr<juce::FileChooser> folderChooser;
            NewSessionDialog::ResultCallback callback;

            juce::Label        nameLabel;
            juce::TextEditor   nameEditor;
            juce::ToggleButton localRadio, cloudRadio;
            juce::TextButton   pathButton, cloudHelp;
            juce::Label        sectionHeader;
            juce::Label        fileTypeLabel, sampleRateLabel, bitDepthLabel, ioLabel;
            juce::ComboBox     fileTypeCombo, sampleRateCombo, bitDepthCombo, ioCombo;
            juce::ToggleButton interleavedToggle;
            juce::TextButton   createButton, cancelButton;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Content)
        };
    }

    void NewSessionDialog::launch (const juce::File& defaultLocation,
                                   double            lastSampleRate,
                                   CaptureFormat     lastCaptureFormat,
                                   ResultCallback    onCreate)
    {
        auto* content = new Content (defaultLocation, lastSampleRate, lastCaptureFormat, std::move (onCreate));

        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "New Session";
        opts.content.setOwned (content);
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
