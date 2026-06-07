#include "WelcomeDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    namespace
    {
        constexpr int kSideRailW   = 210;
        constexpr int kSideItemH   = 44;
        constexpr int kFooterH     = 60;
        constexpr int kTabRowH     = 52;
        constexpr int kContentPad  = 28;

        // ─── Helpers identical to the ones in NewSessionDialog ─────
        static juce::String compactPath (const juce::File& f)
        {
            auto p = f.getFullPathName();
            const auto home = juce::File::getSpecialLocation
                                 (juce::File::userHomeDirectory).getFullPathName();
            if (p.startsWith (home))
                p = p.substring (home.length());
            if (! p.endsWithChar ('/'))
                p += "/";
            return "~" + p;
        }

        static int fileTypeIdFromFormat (CaptureFormat f)
        {
            if (f == CaptureFormat::Aiff16 || f == CaptureFormat::Aiff24 || f == CaptureFormat::Aiff32Float) return 2;
            if (f == CaptureFormat::Flac16 || f == CaptureFormat::Flac24) return 3;
            return 1;
        }
        static int bitDepthIdFromFormat (CaptureFormat f)
        {
            switch (f)
            {
                case CaptureFormat::Wav16:      case CaptureFormat::Aiff16:      case CaptureFormat::Flac16: return 1;
                case CaptureFormat::Wav24:      case CaptureFormat::Aiff24:      case CaptureFormat::Flac24: return 2;
                case CaptureFormat::Wav32Float: case CaptureFormat::Aiff32Float:                             return 3;
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
        static double hzFromSampleRateId (int id)
        {
            switch (id) { case 1: return 44100.0; case 3: return 88200.0; case 4: return 96000.0;
                          case 5: return 176400.0; case 6: return 192000.0; }
            return 48000.0;
        }
        static CaptureFormat formatFromUi (int fileId, int bitId)
        {
            if (fileId == 2) return bitId == 1 ? CaptureFormat::Aiff16
                                  : bitId == 3 ? CaptureFormat::Aiff32Float
                                                : CaptureFormat::Aiff24;
            if (fileId == 3) return bitId == 1 ? CaptureFormat::Flac16 : CaptureFormat::Flac24;
            return bitId == 1 ? CaptureFormat::Wav16
                 : bitId == 3 ? CaptureFormat::Wav32Float
                              : CaptureFormat::Wav24;
        }

        // ─── Side-rail item row ──────────────────────────────────
        class SideRailItem final : public juce::Component
        {
        public:
            SideRailItem (juce::String labelText, juce::Path glyphPath)
                : label (std::move (labelText)), glyph (std::move (glyphPath)) {}

            void setSelected (bool s) { selected = s; repaint(); }
            void mouseEnter (const juce::MouseEvent&) override { hovered = true;  repaint(); }
            void mouseExit  (const juce::MouseEvent&) override { hovered = false; repaint(); }
            void mouseUp    (const juce::MouseEvent& e) override
            {
                if (e.mouseWasClicked() && onClick) onClick();
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat();
                // Background: selected = bgElevated; hover = bgPanel; idle = transparent.
                if (selected)
                {
                    g.setGradientFill (brand::verticalGradient (brand::bgElevated, r, 0.05f, 0.10f));
                    g.fillRect (r);
                    // Brand-orange selection stripe on the left edge.
                    g.setColour (brand::brandOrange);
                    g.fillRect (r.withWidth (3.0f));
                }
                else if (hovered)
                {
                    g.setColour (brand::bgPanel.brighter (0.04f));
                    g.fillRect (r);
                }

                // Glyph
                const float glyphSize = 18.0f;
                const float glyphX = 22.0f;
                const float glyphY = r.getCentreY() - glyphSize * 0.5f;
                g.setColour (selected ? brand::textPrimary : brand::textSecondary);
                g.fillPath (glyph, glyph.getTransformToScaleToFit
                    (juce::Rectangle<float> (glyphX, glyphY, glyphSize, glyphSize), true));

                // Label
                g.setColour (selected ? brand::textPrimary : brand::textSecondary);
                g.setFont (brand::type::ui (15.0f, selected));
                g.drawText (label,
                            r.withTrimmedLeft (glyphX + glyphSize + 14.0f).toNearestInt(),
                            juce::Justification::centredLeft, true);
            }

            std::function<void()> onClick;
        private:
            juce::String label;
            juce::Path   glyph;
            bool selected { false };
            bool hovered  { false };
        };

        // ─── New panel ───────────────────────────────────────────
        class NewPanel final : public juce::Component
        {
        public:
            NewPanel (const juce::File& defaultLocation,
                      double            lastSR,
                      CaptureFormat     lastFmt,
                      const WelcomeDialog::DeviceChoices& dev)
                : storageDir (defaultLocation)
            {
                addAndMakeVisible (nameLabel);
                nameLabel.setText ("Name", juce::dontSendNotification);
                styleFieldLabel (nameLabel);

                addAndMakeVisible (nameEditor);
                dialog::styleTextEditor (nameEditor);
                nameEditor.setText ("Untitled", juce::dontSendNotification);
                nameEditor.setFont (brand::type::ui (16.0f, false));
                nameEditor.setSelectAllWhenFocused (true);
                nameEditor.grabKeyboardFocus();

                addAndMakeVisible (localRadio);
                localRadio.setButtonText ("Local Storage:");
                localRadio.setToggleState (true, juce::dontSendNotification);
                localRadio.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                localRadio.setColour (juce::ToggleButton::tickColourId, brand::accentStatus);

                addAndMakeVisible (pathButton);
                pathButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                pathButton.setColour (juce::TextButton::textColourOffId, brand::accentStatus);
                pathButton.setButtonText (compactPath (storageDir));
                pathButton.onClick = [this] { pickFolder(); };

                addAndMakeVisible (sectionHeader);
                sectionHeader.setText ("Session Configuration", juce::dontSendNotification);
                sectionHeader.setFont (brand::type::ui (14.0f, true));
                sectionHeader.setColour (juce::Label::textColourId, brand::textPrimary);

                addAndMakeVisible (fileTypeLabel);
                fileTypeLabel.setText ("File Type", juce::dontSendNotification);
                styleFieldLabel (fileTypeLabel);
                addAndMakeVisible (fileTypeCombo);
                dialog::styleCombo (fileTypeCombo);
                fileTypeCombo.addItem ("BWF (.WAV)", 1);
                fileTypeCombo.addItem ("AIFF",       2);
                fileTypeCombo.addItem ("FLAC",       3);
                fileTypeCombo.setSelectedId (fileTypeIdFromFormat (lastFmt), juce::dontSendNotification);

                addAndMakeVisible (sampleRateLabel);
                sampleRateLabel.setText ("Sample Rate", juce::dontSendNotification);
                styleFieldLabel (sampleRateLabel);
                addAndMakeVisible (sampleRateCombo);
                dialog::styleCombo (sampleRateCombo);
                static const std::pair<int, const char*> srs[] = {
                    { 1, "44.1 kHz" }, { 2, "48 kHz" }, { 3, "88.2 kHz" },
                    { 4, "96 kHz"   }, { 5, "176.4 kHz" }, { 6, "192 kHz" }
                };
                for (auto& s : srs) sampleRateCombo.addItem (s.second, s.first);
                sampleRateCombo.setSelectedId (sampleRateIdFromHz (lastSR), juce::dontSendNotification);

                addAndMakeVisible (bitDepthLabel);
                bitDepthLabel.setText ("Bit Depth", juce::dontSendNotification);
                styleFieldLabel (bitDepthLabel);
                addAndMakeVisible (bitDepthCombo);
                dialog::styleCombo (bitDepthCombo);
                bitDepthCombo.addItem ("16-bit",       1);
                bitDepthCombo.addItem ("24-bit",       2);
                bitDepthCombo.addItem ("32-bit float", 3);
                bitDepthCombo.setSelectedId (bitDepthIdFromFormat (lastFmt), juce::dontSendNotification);

                addAndMakeVisible (ioLabel);
                ioLabel.setText ("I/O Settings", juce::dontSendNotification);
                styleFieldLabel (ioLabel);
                addAndMakeVisible (ioCombo);
                dialog::styleCombo (ioCombo);
                ioCombo.addItem ("Last Used",  1);
                ioCombo.addItem ("Stereo Mix", 2);
                ioCombo.setSelectedId (1, juce::dontSendNotification);

                // Input / Output device pickers -- choose what to record from
                // and monitor through, right here when creating the session.
                addAndMakeVisible (inputDevLabel);
                inputDevLabel.setText ("Input Device (record from)", juce::dontSendNotification);
                styleFieldLabel (inputDevLabel);
                addAndMakeVisible (inputDevCombo);
                dialog::styleCombo (inputDevCombo);
                for (int i = 0; i < dev.inputs.size(); ++i)
                    inputDevCombo.addItem (dev.inputs[i], i + 1);
                {
                    const int idx = dev.inputs.indexOf (dev.currentInput);
                    inputDevCombo.setSelectedId (juce::jmax (1, idx + 1), juce::dontSendNotification);
                }

                addAndMakeVisible (outputDevLabel);
                outputDevLabel.setText ("Output Device (monitor)", juce::dontSendNotification);
                styleFieldLabel (outputDevLabel);
                addAndMakeVisible (outputDevCombo);
                dialog::styleCombo (outputDevCombo);
                for (int i = 0; i < dev.outputs.size(); ++i)
                    outputDevCombo.addItem (dev.outputs[i], i + 1);
                {
                    const int idx = dev.outputs.indexOf (dev.currentOutput);
                    outputDevCombo.setSelectedId (juce::jmax (1, idx + 1), juce::dontSendNotification);
                }

                addAndMakeVisible (interleaved);
                interleaved.setButtonText ("Interleaved");
                interleaved.setToggleState (true, juce::dontSendNotification);
                interleaved.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                interleaved.setColour (juce::ToggleButton::tickColourId, brand::accentStatus);

                addAndMakeVisible (fromTemplate);
                fromTemplate.setButtonText ("Create From Template");
                fromTemplate.setToggleState (false, juce::dontSendNotification);
                fromTemplate.setColour (juce::ToggleButton::textColourId, brand::textSecondary);
                fromTemplate.setColour (juce::ToggleButton::tickColourId, brand::accentStatus);

                addAndMakeVisible (templatesHeader);
                templatesHeader.setText ("Templates", juce::dontSendNotification);
                templatesHeader.setFont (brand::type::ui (14.0f, true));
                templatesHeader.setColour (juce::Label::textColourId, brand::textMuted);

                addAndMakeVisible (templatesHint);
                templatesHint.setText ("(no templates -- save one via Session > Save Current as Template)",
                                       juce::dontSendNotification);
                templatesHint.setFont (brand::type::caption());
                templatesHint.setColour (juce::Label::textColourId, brand::textMuted);
            }

            juce::String  getName()        const { return nameEditor.getText().trim(); }
            juce::File    getLocation()    const { return storageDir; }
            CaptureFormat getFormat()      const { return formatFromUi (fileTypeCombo.getSelectedId(),
                                                                        bitDepthCombo.getSelectedId()); }
            double        getSampleRate() const { return hzFromSampleRateId (sampleRateCombo.getSelectedId()); }
            bool          isInterleaved() const { return interleaved.getToggleState(); }
            juce::String  getIoSettings() const { return ioCombo.getText(); }
            juce::String  getInputDevice()  const { return inputDevCombo.getNumItems()  > 0 ? inputDevCombo.getText()  : juce::String(); }
            juce::String  getOutputDevice() const { return outputDevCombo.getNumItems() > 0 ? outputDevCombo.getText() : juce::String(); }

            void resized() override
            {
                auto b = getLocalBounds().reduced (kContentPad, 16);

                nameLabel.setBounds (b.removeFromTop (18));
                b.removeFromTop (brand::space::xs);
                nameEditor.setBounds (b.removeFromTop (36));
                b.removeFromTop (brand::space::btnH);

                auto local = b.removeFromTop (28);
                localRadio.setBounds (local.removeFromLeft (150));
                local.removeFromLeft (brand::space::sm);
                pathButton.setBounds (local.removeFromLeft (juce::jmin (local.getWidth(), 360)));
                b.removeFromTop (28);

                sectionHeader.setBounds (b.removeFromTop (brand::space::ctrlH));
                b.removeFromTop (brand::space::md);

                auto row1 = b.removeFromTop (60);
                const int colW = (row1.getWidth() - 18) / 2;
                {
                    auto left = row1.removeFromLeft (colW);
                    fileTypeLabel.setBounds (left.removeFromTop (18));
                    left.removeFromTop (brand::space::xs);
                    fileTypeCombo.setBounds (left.removeFromTop (32));
                }
                row1.removeFromLeft (18);
                {
                    auto right = row1;
                    sampleRateLabel.setBounds (right.removeFromTop (18));
                    right.removeFromTop (brand::space::xs);
                    sampleRateCombo.setBounds (right.removeFromTop (32));
                }
                b.removeFromTop (18);

                auto row2 = b.removeFromTop (60);
                {
                    auto left = row2.removeFromLeft (colW);
                    bitDepthLabel.setBounds (left.removeFromTop (18));
                    left.removeFromTop (brand::space::xs);
                    bitDepthCombo.setBounds (left.removeFromTop (32));
                }
                row2.removeFromLeft (18);
                {
                    auto right = row2;
                    ioLabel.setBounds (right.removeFromTop (18));
                    right.removeFromTop (brand::space::xs);
                    ioCombo.setBounds (right.removeFromTop (32));
                }
                b.removeFromTop (18);

                auto row3 = b.removeFromTop (60);
                {
                    auto left = row3.removeFromLeft (colW);
                    inputDevLabel.setBounds (left.removeFromTop (18));
                    left.removeFromTop (brand::space::xs);
                    inputDevCombo.setBounds (left.removeFromTop (32));
                }
                row3.removeFromLeft (18);
                {
                    auto right = row3;
                    outputDevLabel.setBounds (right.removeFromTop (18));
                    right.removeFromTop (brand::space::xs);
                    outputDevCombo.setBounds (right.removeFromTop (32));
                }
                b.removeFromTop (12);

                interleaved.setBounds (b.removeFromTop (brand::space::rowH).removeFromLeft (180));
                b.removeFromTop (18);

                fromTemplate.setBounds (b.removeFromTop (brand::space::rowH).removeFromLeft (240));
                b.removeFromTop (14);

                templatesHeader.setBounds (b.removeFromTop (brand::space::ctrlH));
                b.removeFromTop (brand::space::xs);
                templatesHint.setBounds (b.removeFromTop (brand::space::ioH));
            }

        private:
            static void styleFieldLabel (juce::Label& l)
            {
                l.setFont (brand::type::ui (13.0f, true));
                l.setColour (juce::Label::textColourId, brand::textSecondary);
            }

            void pickFolder()
            {
                folderChooser = std::make_unique<juce::FileChooser>
                    ("Choose session storage location", storageDir);
                folderChooser->launchAsync (
                    juce::FileBrowserComponent::canSelectDirectories
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

            juce::File         storageDir;
            juce::Label        nameLabel;
            juce::TextEditor   nameEditor;
            juce::ToggleButton localRadio;
            juce::TextButton   pathButton;
            juce::Label        sectionHeader;
            juce::Label        fileTypeLabel;
            juce::ComboBox     fileTypeCombo;
            juce::Label        sampleRateLabel;
            juce::ComboBox     sampleRateCombo;
            juce::Label        bitDepthLabel;
            juce::ComboBox     bitDepthCombo;
            juce::Label        ioLabel;
            juce::ComboBox     ioCombo;
            juce::Label        inputDevLabel;
            juce::ComboBox     inputDevCombo;
            juce::Label        outputDevLabel;
            juce::ComboBox     outputDevCombo;
            juce::ToggleButton interleaved;
            juce::ToggleButton fromTemplate;
            juce::Label        templatesHeader;
            juce::Label        templatesHint;
            std::unique_ptr<juce::FileChooser> folderChooser;
        };

        // ─── Open panel ──────────────────────────────────────────
        class OpenPanel final : public juce::Component
        {
        public:
            OpenPanel (const juce::File& defaultLocation,
                       std::function<void (const juce::File&)> onChoose)
                : defaultDir (defaultLocation), chooseCallback (std::move (onChoose))
            {
                addAndMakeVisible (header);
                header.setText ("Open Session", juce::dontSendNotification);
                header.setFont (brand::type::ui (16.0f, true));
                header.setColour (juce::Label::textColourId, brand::textPrimary);

                addAndMakeVisible (hint);
                hint.setText ("Pick a .zfproj session folder.",
                              juce::dontSendNotification);
                hint.setFont (brand::type::caption());
                hint.setColour (juce::Label::textColourId, brand::textSecondary);

                addAndMakeVisible (pickButton);
                pickButton.setButtonText ("Choose Session Folder...");
                dialog::stylePrimary (pickButton);
                pickButton.onClick = [this] { pickSession(); };

                addAndMakeVisible (recentHeader);
                recentHeader.setText ("Recent", juce::dontSendNotification);
                recentHeader.setFont (brand::type::ui (14.0f, true));
                recentHeader.setColour (juce::Label::textColourId, brand::textMuted);

                addAndMakeVisible (recentHint);
                recentHint.setText ("(recent sessions appear here after a save)",
                                    juce::dontSendNotification);
                recentHint.setFont (brand::type::caption());
                recentHint.setColour (juce::Label::textColourId, brand::textMuted);
            }

            void resized() override
            {
                auto b = getLocalBounds().reduced (kContentPad, 16);
                header.setBounds (b.removeFromTop (28));
                b.removeFromTop (brand::space::xs);
                hint.setBounds (b.removeFromTop (brand::space::ioH));
                b.removeFromTop (brand::space::ioH);
                pickButton.setBounds (b.removeFromTop (40).removeFromLeft (260));
                b.removeFromTop (30);
                recentHeader.setBounds (b.removeFromTop (brand::space::ctrlH));
                b.removeFromTop (brand::space::xs);
                recentHint.setBounds (b.removeFromTop (brand::space::ioH));
            }

        private:
            void pickSession()
            {
                folderChooser = std::make_unique<juce::FileChooser>
                    ("Open Zynforge session", defaultDir);
                folderChooser->launchAsync (
                    juce::FileBrowserComponent::canSelectDirectories
                    | juce::FileBrowserComponent::openMode,
                    [this] (const juce::FileChooser& fc)
                    {
                        const auto chosen = fc.getResult();
                        if (chosen.isDirectory() && chooseCallback)
                            chooseCallback (chosen);
                    });
            }

            juce::File defaultDir;
            std::function<void (const juce::File&)> chooseCallback;
            juce::Label header, hint, recentHeader, recentHint;
            juce::TextButton pickButton;
            std::unique_ptr<juce::FileChooser> folderChooser;
        };

        // ─── Main content ────────────────────────────────────────
        class Content final : public juce::Component
        {
        public:
            Content (const juce::File& defaultLocation,
                     double            lastSR,
                     CaptureFormat     lastFmt,
                     const WelcomeDialog::DeviceChoices& dev,
                     WelcomeDialog::OnCreateNew    onCreate_,
                     WelcomeDialog::OnOpenExisting onOpen_)
                : onCreate (std::move (onCreate_)),
                  onOpen   (std::move (onOpen_)),
                  newPanel (std::make_unique<NewPanel> (defaultLocation, lastSR, lastFmt, dev)),
                  openPanel (std::make_unique<OpenPanel> (defaultLocation,
                      [this] (const juce::File& f)
                      {
                          if (onOpen) onOpen (f);
                          dismiss();
                      }))
            {
                // Side rail items.
                newItem  = std::make_unique<SideRailItem> ("New",  makeNewGlyph());
                openItem = std::make_unique<SideRailItem> ("Open", makeOpenGlyph());
                newItem ->setSelected (true);
                newItem ->onClick = [this] { selectPanel (Panel::New);  };
                openItem->onClick = [this] { selectPanel (Panel::Open); };
                addAndMakeVisible (*newItem);
                addAndMakeVisible (*openItem);

                // Title label in the side rail.
                addAndMakeVisible (railTitle);
                railTitle.setText ("Zynforge Recording", juce::dontSendNotification);
                railTitle.setFont (brand::type::ui (16.0f, true));
                railTitle.setColour (juce::Label::textColourId, brand::textPrimary);

                // Tab strip at the top of the content pane (Session only,
                // per the brief -- no Sketch).
                addAndMakeVisible (tabLabel);
                tabLabel.setText ("Session", juce::dontSendNotification);
                tabLabel.setFont (brand::type::ui (16.0f, true));
                tabLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                tabLabel.setJustificationType (juce::Justification::centred);

                addAndMakeVisible (*newPanel);
                addAndMakeVisible (*openPanel);
                openPanel->setVisible (false);

                // Footer buttons.
                addAndMakeVisible (createButton);
                createButton.setButtonText ("Create");
                dialog::stylePrimary (createButton);
                createButton.onClick = [this] { commitNew(); };

                addAndMakeVisible (cancelButton);
                cancelButton.setButtonText ("Cancel");
                dialog::styleSecondary (cancelButton);
                cancelButton.onClick = [this] { dismiss(); };

                setSize (920, 760);
            }

            void paint (juce::Graphics& g) override
            {
                // Side rail
                const auto rail = juce::Rectangle<float> (0.0f, 0.0f,
                                                          (float) kSideRailW,
                                                          (float) getHeight());
                g.setGradientFill (brand::verticalGradient (brand::bgDeep, rail, 0.04f, 0.10f));
                g.fillRect (rail);
                g.setColour (brand::edge);
                g.drawVerticalLine (kSideRailW - 1, 0.0f, (float) getHeight());

                // Content pane background
                const auto content = juce::Rectangle<float> ((float) kSideRailW, 0.0f,
                                                             (float) (getWidth() - kSideRailW),
                                                             (float) getHeight());
                g.setGradientFill (brand::verticalGradient (brand::bgPanel, content, 0.04f, 0.12f));
                g.fillRect (content);

                // Tab row divider under the "Session" tab title.
                g.setColour (brand::edge);
                g.drawHorizontalLine (kTabRowH, (float) kSideRailW, (float) getWidth());

                // Footer divider above the buttons.
                g.setColour (brand::edge);
                g.drawHorizontalLine (getHeight() - kFooterH,
                                      (float) kSideRailW, (float) getWidth());
            }

            void resized() override
            {
                // Side rail
                auto rail = getLocalBounds().withWidth (kSideRailW);
                railTitle.setBounds (rail.removeFromTop (60).reduced (22, 16));
                rail.removeFromTop (6);
                newItem ->setBounds (rail.removeFromTop (kSideItemH));
                rail.removeFromTop (2);
                openItem->setBounds (rail.removeFromTop (kSideItemH));

                // Content pane
                auto content = getLocalBounds().withTrimmedLeft (kSideRailW);
                tabLabel.setBounds (content.removeFromTop (kTabRowH));

                auto footer = content.removeFromBottom (kFooterH).reduced (kContentPad, 12);
                createButton.setBounds (footer.removeFromRight (110).reduced (0, 4));
                footer.removeFromRight (brand::space::md);
                cancelButton.setBounds (footer.removeFromRight (90).reduced (0, 4));

                newPanel ->setBounds (content);
                openPanel->setBounds (content);
            }

        private:
            enum class Panel { New, Open };

            void selectPanel (Panel p)
            {
                newItem ->setSelected (p == Panel::New);
                openItem->setSelected (p == Panel::Open);
                newPanel ->setVisible (p == Panel::New);
                openPanel->setVisible (p == Panel::Open);
                createButton.setVisible (p == Panel::New);
            }

            void commitNew()
            {
                if (! onCreate) { dismiss(); return; }
                WelcomeDialog::NewResult r;
                r.name          = newPanel->getName().isEmpty() ? juce::String ("Untitled")
                                                                : newPanel->getName();
                r.location      = newPanel->getLocation();
                r.captureFormat = newPanel->getFormat();
                r.sampleRate    = newPanel->getSampleRate();
                r.interleaved   = newPanel->isInterleaved();
                r.ioSettings    = newPanel->getIoSettings();
                r.inputDeviceName  = newPanel->getInputDevice();
                r.outputDeviceName = newPanel->getOutputDevice();
                onCreate (r);
                dismiss();
            }

            void dismiss()
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            }

            static juce::Path makeNewGlyph()
            {
                // [+] in a rounded rectangle.
                juce::Path p;
                p.addRoundedRectangle (1.0f, 2.0f, 16.0f, 14.0f, 2.0f);
                juce::Path inner;
                inner.startNewSubPath (9.0f, 6.0f); inner.lineTo (9.0f, 12.0f);
                inner.startNewSubPath (6.0f, 9.0f); inner.lineTo (12.0f, 9.0f);
                p.addPath (inner);
                return p;
            }

            static juce::Path makeOpenGlyph()
            {
                // Folder shape.
                juce::Path p;
                p.startNewSubPath (1.0f, 5.0f);
                p.lineTo (6.0f, 5.0f);
                p.lineTo (8.0f, 3.0f);
                p.lineTo (17.0f, 3.0f);
                p.lineTo (17.0f, 15.0f);
                p.lineTo (1.0f, 15.0f);
                p.closeSubPath();
                return p;
            }

            WelcomeDialog::OnCreateNew    onCreate;
            WelcomeDialog::OnOpenExisting onOpen;
            std::unique_ptr<NewPanel>     newPanel;
            std::unique_ptr<OpenPanel>    openPanel;
            std::unique_ptr<SideRailItem> newItem, openItem;
            juce::Label                   railTitle, tabLabel;
            juce::TextButton              createButton, cancelButton;
        };
    } // anonymous namespace

    void WelcomeDialog::launch (const juce::File&    defaultLocation,
                                double               lastSR,
                                CaptureFormat        lastFmt,
                                const DeviceChoices& devices,
                                OnCreateNew          onCreate,
                                OnOpenExisting       onOpen)
    {
        auto content = std::make_unique<Content> (defaultLocation, lastSR, lastFmt, devices,
                                                  std::move (onCreate), std::move (onOpen));
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Welcome";
        opts.content.setOwned (content.release());
        opts.dialogBackgroundColour       = brand::bgDeep;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
