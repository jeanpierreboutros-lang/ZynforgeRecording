#include "AddTracksDialog.h"
#include "../Theme/DialogChrome.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <memory>

namespace zynforge
{
    namespace
    {
        // Single row of the dialog: "Create [N] new [Mono ▼] Audio Track   Name: [____] [+]".
        // Width laid out by the parent — height is fixed.
        class TrackRow final : public juce::Component
        {
        public:
            std::function<void()> onAddRow;
            std::function<void(TrackRow*)> onRemoveRow;
            bool isFirstRow { true };

            TrackRow()
            {
                createLabel.setText ("Create", juce::dontSendNotification);
                createLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                createLabel.setFont (brand::type::uiBody());
                addAndMakeVisible (createLabel);

                countEditor.setText ("1", juce::dontSendNotification);
                countEditor.setInputRestrictions (3, "0123456789");
                countEditor.setJustification (juce::Justification::centred);
                countEditor.setFont (brand::type::captionBold());
                countEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff000000));
                countEditor.setColour (juce::TextEditor::textColourId,       brand::accentStatus);
                countEditor.setColour (juce::TextEditor::outlineColourId,    brand::edge);
                countEditor.setColour (juce::TextEditor::focusedOutlineColourId, brand::accentStatus);
                addAndMakeVisible (countEditor);

                newLabel.setText ("new", juce::dontSendNotification);
                newLabel.setColour (juce::Label::textColourId, brand::textSecondary);
                newLabel.setFont (brand::type::uiBody());
                addAndMakeVisible (newLabel);

                auto styleCombo = [] (juce::ComboBox& c)
                {
                    c.setColour (juce::ComboBox::backgroundColourId, brand::bgElevated);
                    c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
                    c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
                    c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
                };
                modeCombo.addItem ("Mono",   1);
                modeCombo.addItem ("Stereo", 2);
                modeCombo.setSelectedItemIndex (0, juce::dontSendNotification);
                styleCombo (modeCombo);
                addAndMakeVisible (modeCombo);

                // Track type — Audio (records input + plays back) vs
                // Bus (no input, sums sends, routes to outputs).
                typeCombo.addItem ("Audio Track", 1);
                typeCombo.addItem ("Bus Track",   2);
                typeCombo.setSelectedItemIndex (0, juce::dontSendNotification);
                styleCombo (typeCombo);
                addAndMakeVisible (typeCombo);

                nameLabel.setText ("Name:", juce::dontSendNotification);
                nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                nameLabel.setFont (brand::type::uiBody());
                addAndMakeVisible (nameLabel);

                nameEditor.setText ("Audio", juce::dontSendNotification);
                nameEditor.setFont (brand::type::uiBody());
                nameEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff000000));
                nameEditor.setColour (juce::TextEditor::textColourId,       brand::textPrimary);
                nameEditor.setColour (juce::TextEditor::outlineColourId,    brand::edge);
                addAndMakeVisible (nameEditor);

                plusButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                plusButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
                plusButton.setButtonText ("+");
                plusButton.setTooltip ("Add another track row.");
                plusButton.onClick = [this] { if (onAddRow) onAddRow(); };
                addAndMakeVisible (plusButton);

                minusButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                minusButton.setColour (juce::TextButton::textColourOffId, brand::textMuted);
                minusButton.setButtonText ("-");
                minusButton.setTooltip ("Remove this row.");
                minusButton.onClick = [this] { if (onRemoveRow) onRemoveRow (this); };
                addAndMakeVisible (minusButton);
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat().reduced (2.0f);
                g.setGradientFill (brand::verticalGradient (brand::bgStrip, r, 0.05f, 0.08f));
                g.fillRoundedRectangle (r, brand::radius::md);
                g.setColour (brand::edge);
                g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (brand::space::md, brand::space::xs);

                // ── Left block (count + mode) ────────────────────────────
                createLabel.setBounds (r.removeFromLeft (54));
                r.removeFromLeft (brand::space::sm);
                countEditor.setBounds (r.removeFromLeft (48).reduced (0, 2));
                r.removeFromLeft (brand::space::sm);
                newLabel   .setBounds (r.removeFromLeft (28));
                r.removeFromLeft (brand::space::sm);
                modeCombo  .setBounds (r.removeFromLeft (110).reduced (0, 2));
                r.removeFromLeft (brand::space::sm);
                typeCombo  .setBounds (r.removeFromLeft (130).reduced (0, 2));

                // ── Right block (+/-) ────────────────────────────────────
                plusButton .setBounds (r.removeFromRight (28).reduced (0, 2));
                r.removeFromRight (brand::space::xs);
                if (isFirstRow)
                {
                    minusButton.setVisible (false);
                    minusButton.setBounds ({});
                }
                else
                {
                    minusButton.setVisible (true);
                    minusButton.setBounds (r.removeFromRight (28).reduced (0, 2));
                    r.removeFromRight (brand::space::xs);
                }

                // ── Name block (fixed width, not stretched) ──────────────
                r.removeFromRight (brand::space::md);
                nameEditor .setBounds (r.removeFromRight (200).reduced (0, 2));
                r.removeFromRight (brand::space::xs);
                nameLabel  .setBounds (r.removeFromRight (44));
                // Any leftover space sits between modeCombo and nameLabel
                // as breathing room.
            }

            int  count()  const { return juce::jlimit (1, 256, countEditor.getText().getIntValue()); }
            bool stereo() const { return modeCombo.getSelectedItemIndex() == 1; }
            bool isBus()  const { return typeCombo.getSelectedItemIndex() == 1; }
            juce::String baseName() const { return nameEditor.getText().trim(); }

        private:
            juce::Label      createLabel;
            juce::TextEditor countEditor;
            juce::Label      newLabel;
            juce::ComboBox   modeCombo;
            juce::ComboBox   typeCombo;
            juce::Label      nameLabel;
            juce::TextEditor nameEditor;
            juce::TextButton plusButton;
            juce::TextButton minusButton;
        };

        class DialogContent final : public juce::Component
        {
        public:
            explicit DialogContent (AddTracksDialog::ResultCallback cb) : callback (std::move (cb))
            {
                // Title is painted by DialogChrome — keep the label
                // hidden so we don't double up.
                title.setVisible (false);

                dialog::stylePrimary (createButton);
                createButton.setButtonText ("Create");
                createButton.onClick = [this] { commit(); };
                addAndMakeVisible (createButton);

                dialog::styleSecondary (cancelButton);
                cancelButton.setButtonText ("Cancel");
                cancelButton.onClick = [this] { dismiss ({}); };
                addAndMakeVisible (cancelButton);

                addRow();   // start with one row
                setSize (680, 150);
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "NEW TRACKS");
            }

            void resized() override
            {
                title.setBounds (0, 0, getWidth(), 44);

                auto rows = getLocalBounds()
                                .withTrimmedTop (50)
                                .withTrimmedBottom (62)
                                .reduced (12, 0);

                const int rowH = 44;
                for (auto* r : rowComponents)
                {
                    r->setBounds (rows.removeFromTop (rowH));
                    rows.removeFromTop (brand::space::xs);
                }

                auto footer = getLocalBounds().removeFromBottom (56).reduced (12);
                createButton.setBounds (footer.removeFromRight (110).reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::sm);
                cancelButton.setBounds (footer.removeFromRight (100).reduced (0, brand::space::xs));
            }

        private:
            void addRow()
            {
                auto r = std::make_unique<TrackRow>();
                r->isFirstRow = rowComponents.empty();
                r->onAddRow = [this] { addRow(); };
                r->onRemoveRow = [this] (TrackRow* who)
                {
                    for (auto it = rowComponents.begin(); it != rowComponents.end(); ++it)
                        if (*it == who)
                        {
                            removeChildComponent (who);
                            auto* deleted = *it;
                            rowComponents.erase (it);
                            delete deleted;
                            // Re-flag the first row in case it was removed.
                            for (size_t i = 0; i < rowComponents.size(); ++i)
                                rowComponents[i]->isFirstRow = (i == 0);
                            resizeFor (rowComponents.size());
                            return;
                        }
                };
                addAndMakeVisible (*r);
                rowComponents.push_back (r.release());
                resizeFor (rowComponents.size());
            }

            void resizeFor (size_t numRows)
            {
                const int rowH = 44;
                const int gap  = brand::space::xs;
                const int h    = 50 + (int) numRows * rowH + ((int) numRows - 1) * gap + 62;
                setSize (getWidth(), juce::jmax (150, h));
                resized();
            }

            void commit()
            {
                std::vector<AddTracksDialog::Entry> entries;
                for (auto* r : rowComponents)
                {
                    AddTracksDialog::Entry e;
                    e.count    = r->count();
                    e.stereo   = r->stereo();
                    e.isBus    = r->isBus();
                    e.baseName = r->baseName();
                    entries.push_back (std::move (e));
                }
                dismiss (entries);
            }

            void dismiss (const std::vector<AddTracksDialog::Entry>& entries)
            {
                if (callback) callback (entries);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (entries.empty() ? 0 : 1);
            }

            juce::Label title;
            std::vector<TrackRow*> rowComponents;
            juce::TextButton createButton, cancelButton;
            AddTracksDialog::ResultCallback callback;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DialogContent)
        };
    }

    void AddTracksDialog::launch (ResultCallback cb)
    {
        auto content = std::make_unique<DialogContent> (std::move (cb));

        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "New Tracks";
        opts.content.setOwned (content.release());
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
