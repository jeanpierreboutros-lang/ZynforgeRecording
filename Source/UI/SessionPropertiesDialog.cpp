#include "SessionPropertiesDialog.h"
#include "../Theme/DialogChrome.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        static void styleLabel (juce::Label& l)
        {
            l.setFont (brand::type::caption());
            l.setColour (juce::Label::textColourId, brand::textSecondary);
        }

        static void styleEditor (juce::TextEditor& t)
        {
            t.setFont (brand::type::ui (14.0f));
            t.setColour (juce::TextEditor::backgroundColourId,        brand::inputBg);
            t.setColour (juce::TextEditor::textColourId,              brand::textPrimary);
            t.setColour (juce::TextEditor::outlineColourId,           brand::edge);
            t.setColour (juce::TextEditor::focusedOutlineColourId,    brand::accentStatus);
            t.setColour (juce::TextEditor::highlightColourId,         brand::accentStatus.withAlpha (zynforge::brand::alpha::dimmed));
        }

        static void styleReadOnly (juce::Label& l)
        {
            l.setFont (brand::type::channelName());
            l.setColour (juce::Label::textColourId, brand::accentStatus);
        }

        class Content final : public juce::Component
        {
        public:
            Content (SessionPropertiesDialog::Fields initial,
                     SessionPropertiesDialog::SaveCallback cb)
                : initialFields (std::move (initial)), callback (std::move (cb))
            {
                title.setText ("Session Properties", juce::dontSendNotification);
                title.setFont (brand::type::headline());
                title.setColour (juce::Label::textColourId, brand::textPrimary);
                title.setJustificationType (juce::Justification::centred);
                addAndMakeVisible (title);

                auto setupRow = [&] (juce::Label& lbl, juce::TextEditor& ed,
                                     const juce::String& labelText,
                                     const juce::String& value)
                {
                    lbl.setText (labelText, juce::dontSendNotification);
                    styleLabel (lbl);
                    addAndMakeVisible (lbl);

                    styleEditor (ed);
                    ed.setText (value, juce::dontSendNotification);
                    addAndMakeVisible (ed);
                };

                // Session name is READ-ONLY here and shows the FOLDER name --
                // which is what the title bar, the status line, the recent list
                // and the .zfproj all use. The editable field wrote a `name`
                // property that nothing but this dialog ever read back, so
                // typing a new name looked like a rename and changed nothing.
                // Renaming a session means moving the folder and its .zfproj
                // while the engine holds it open; that belongs in Save As, not
                // in the dialog documented as "inspect".
                nameLabel.setText ("Session name", juce::dontSendNotification);
                styleLabel (nameLabel);
                addAndMakeVisible (nameLabel);
                nameValue.setText (initialFields.name, juce::dontSendNotification);
                styleReadOnly (nameValue);
                addAndMakeVisible (nameValue);
                nameHint.setText ("Use File > Save As to write the session under a different name",
                                  juce::dontSendNotification);
                nameHint.setFont (brand::type::caption());
                nameHint.setColour (juce::Label::textColourId, brand::textTertiary);
                addAndMakeVisible (nameHint);

                setupRow (artistLabel, artistEditor, "Artist / band",  initialFields.artist);
                setupRow (venueLabel,  venueEditor,  "Venue",          initialFields.venue);
                setupRow (fohLabel,    fohEditor,    "FOH engineer",   initialFields.fohEngineer);
                setupRow (dateLabel,   dateEditor,   "Date",           initialFields.date);

                notesLabel.setText ("Notes", juce::dontSendNotification);
                styleLabel (notesLabel);
                addAndMakeVisible (notesLabel);

                styleEditor (notesEditor);
                notesEditor.setMultiLine (true);
                notesEditor.setReturnKeyStartsNewLine (true);
                notesEditor.setText (initialFields.notes, juce::dontSendNotification);
                addAndMakeVisible (notesEditor);

                // Read-only audio config block.
                configHeader.setText ("Audio configuration", juce::dontSendNotification);
                configHeader.setFont (brand::type::captionBold());
                configHeader.setColour (juce::Label::textColourId, brand::textMuted);
                addAndMakeVisible (configHeader);

                auto setupRO = [&] (juce::Label& cap, juce::Label& val,
                                    const juce::String& capText, const juce::String& valText)
                {
                    cap.setText (capText, juce::dontSendNotification);
                    styleLabel (cap);
                    addAndMakeVisible (cap);

                    val.setText (valText.isNotEmpty() ? valText : juce::String ("--"),
                                 juce::dontSendNotification);
                    styleReadOnly (val);
                    addAndMakeVisible (val);
                };
                setupRO (sampleRateCap, sampleRateVal, "Sample rate",   initialFields.sampleRate);
                setupRO (bitDepthCap,   bitDepthVal,   "Bit depth",     initialFields.bitDepth);
                setupRO (formatCap,     formatVal,     "File type",     initialFields.captureFormat);

                saveButton.setButtonText ("Save");
                dialog::stylePrimary (saveButton);
                saveButton.onClick = [this] { commit(); };
                addAndMakeVisible (saveButton);

                cancelButton.setButtonText ("Cancel");
                dialog::styleSecondary (cancelButton);
                cancelButton.onClick = [this] { dismiss (false); };
                addAndMakeVisible (cancelButton);

                // Custom Component dialogs do by hand what dialog::primeNameEditor
                // does for AlertWindows: focus the first field with its text
                // selected, and make Return commit. Six editors and none of it.
                for (auto* ed : { &artistEditor, &venueEditor, &fohEditor, &dateEditor })
                {
                    ed->setSelectAllWhenFocused (true);
                    ed->onReturnKey = [this] { commit(); };
                }
                // notesEditor is multi-line: Return there inserts a newline.
                juce::MessageManager::callAsync (
                    [safe = juce::Component::SafePointer<Content> (this)]
                    { if (safe != nullptr) safe->artistEditor.grabKeyboardFocus(); });

                setSize (520, 556);
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "SESSION PROPERTIES");
            }

            void resized() override
            {
                auto b = getLocalBounds();
                title.setBounds (b.removeFromTop (40));
                b.reduce (24, 12);

                auto row = [&] (juce::Label& lbl, juce::Component& ed, int h = 30)
                {
                    auto line = b.removeFromTop (h + 18);
                    lbl.setBounds (line.removeFromTop (brand::space::xl));
                    ed .setBounds (line.removeFromTop (h));
                    b.removeFromTop (brand::space::sm);
                };

                {
                    auto line = b.removeFromTop (30 + 18);
                    nameLabel.setBounds (line.removeFromTop (brand::space::xl));
                    nameValue.setBounds (line.removeFromTop (30));
                    nameHint .setBounds (b.removeFromTop (16));
                    b.removeFromTop (brand::space::sm);
                }
                row (artistLabel, artistEditor);
                row (venueLabel,  venueEditor);
                row (fohLabel,    fohEditor);
                row (dateLabel,   dateEditor);
                row (notesLabel,  notesEditor, 76);

                b.removeFromTop (brand::space::md);
                configHeader.setBounds (b.removeFromTop (18));
                b.removeFromTop (brand::space::xs);

                auto cfg = b.removeFromTop (44);
                const int colW = cfg.getWidth() / 3;
                {
                    auto col = cfg.removeFromLeft (colW);
                    sampleRateCap.setBounds (col.removeFromTop (brand::space::xl));
                    sampleRateVal.setBounds (col.removeFromTop (brand::space::btnH));
                }
                {
                    auto col = cfg.removeFromLeft (colW);
                    bitDepthCap.setBounds (col.removeFromTop (brand::space::xl));
                    bitDepthVal.setBounds (col.removeFromTop (brand::space::btnH));
                }
                {
                    formatCap.setBounds (cfg.removeFromTop (brand::space::xl));
                    formatVal.setBounds (cfg.removeFromTop (brand::space::btnH));
                }

                auto footer = getLocalBounds().removeFromBottom (56).reduced (24, 12);
                saveButton  .setBounds (footer.removeFromRight (100));
                footer.removeFromRight (brand::space::lg);
                cancelButton.setBounds (footer.removeFromRight (100));
            }

        private:
            void commit()
            {
                SessionPropertiesDialog::Fields out = initialFields;
                // out.name stays as it came in -- the folder name is authoritative.
                out.artist      = artistEditor.getText().trim();
                out.venue       = venueEditor .getText().trim();
                out.fohEngineer = fohEditor   .getText().trim();
                out.date        = dateEditor  .getText().trim();
                out.notes       = notesEditor .getText();
                if (callback) callback (out);
                dismiss (true);
            }

            void dismiss (bool saved)
            {
                // 1 = Save, 0 = Cancel, per the app-wide result-code convention.
                // This used to ignore its argument and always report 1, so Cancel
                // read as Save to anything inspecting the modal result.
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (saved ? 1 : 0);
            }

            SessionPropertiesDialog::Fields       initialFields;
            SessionPropertiesDialog::SaveCallback callback;

            juce::Label title;
            juce::Label nameLabel,   artistLabel, venueLabel, fohLabel, dateLabel, notesLabel;
            juce::Label nameValue, nameHint;
            juce::TextEditor artistEditor, venueEditor, fohEditor, dateEditor, notesEditor;
            juce::Label configHeader;
            juce::Label sampleRateCap, bitDepthCap, formatCap;
            juce::Label sampleRateVal, bitDepthVal, formatVal;
            juce::TextButton saveButton, cancelButton;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Content)
        };
    }

    void SessionPropertiesDialog::launch (Fields initial, SaveCallback onSave)
    {
        auto* content = new Content (std::move (initial), std::move (onSave));

        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Session Properties";
        opts.content.setOwned (content);
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
