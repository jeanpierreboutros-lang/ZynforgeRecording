#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    // Show-day mirror-drives picker. Lets the engineer add / remove
    // extra mirror destinations on top of the existing primary +
    // backup pair. Each mirror writes a full parallel copy of every
    // armed track in its own format to its own drive root.
    //
    // Persistence + audio-thread wiring already lives in AudioEngine::
    // setMirrors. This dialog is purely a thin editor over the
    // engine's getMirrors() / setMirrors() pair.
    class MirrorDrivesDialog
    {
    public:
        // Open the modal. Reads engine.getMirrors(), shows them as
        // editable rows, writes back via engine.setMirrors() on
        // Apply. Cancel discards changes.
        static juce::DialogWindow* launch (AudioEngine& engine);

    private:
        MirrorDrivesDialog() = delete;
    };

    namespace mirrordlg
    {
        // One row per mirror destination: path label + format combo
        // + remove button. The owning Content rebuilds these on every
        // mutation so the row list always reflects the working state.
        class MirrorRow final : public juce::Component
        {
        public:
            MirrorRow (int idx,
                       MultitrackRecorder::MirrorConfig& config,
                       std::function<void(int)> onRemove,
                       std::function<void()>    onChanged)
                : index (idx), cfg (config), removeCb (std::move (onRemove)),
                  changedCb (std::move (onChanged))
            {
                pathLabel.setText (config.root.getFullPathName(), juce::dontSendNotification);
                pathLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                pathLabel.setColour (juce::Label::backgroundColourId, brand::bgDeep);
                pathLabel.setFont   (brand::type::uiBody());
                pathLabel.setBorderSize ({ 4, 8, 4, 8 });
                addAndMakeVisible (pathLabel);

                chooseBtn.setButtonText ("Choose...");
                dialog::styleSecondary (chooseBtn);
                chooseBtn.onClick = [this] { pickFolder(); };
                addAndMakeVisible (chooseBtn);

                dialog::styleCombo (formatBox);
                formatBox.addItem ("WAV 16-bit",       (int) CaptureFormat::Wav16       + 1);
                formatBox.addItem ("WAV 24-bit",       (int) CaptureFormat::Wav24       + 1);
                formatBox.addItem ("WAV 32-bit float", (int) CaptureFormat::Wav32Float  + 1);
                formatBox.addItem ("AIFF 16-bit",      (int) CaptureFormat::Aiff16      + 1);
                formatBox.addItem ("AIFF 24-bit",      (int) CaptureFormat::Aiff24      + 1);
                formatBox.addItem ("FLAC 16-bit",      (int) CaptureFormat::Flac16      + 1);
                formatBox.addItem ("FLAC 24-bit",      (int) CaptureFormat::Flac24      + 1);
                formatBox.setSelectedId ((int) config.format + 1, juce::dontSendNotification);
                formatBox.onChange = [this]
                {
                    cfg.format = (CaptureFormat) (formatBox.getSelectedId() - 1);
                    if (changedCb) changedCb();
                };
                addAndMakeVisible (formatBox);

                removeBtn.setButtonText ("x");
                dialog::styleSecondary (removeBtn);
                removeBtn.setColour (juce::TextButton::buttonColourId,
                                     brand::accentRecord.withAlpha (zynforge::brand::alpha::dimmed));
                removeBtn.onClick = [this] { if (removeCb) removeCb (index); };
                addAndMakeVisible (removeBtn);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (0, 2);
                removeBtn.setBounds (r.removeFromRight (30));
                r.removeFromRight (brand::space::sm);
                formatBox .setBounds (r.removeFromRight (170));
                r.removeFromRight (brand::space::sm);
                chooseBtn .setBounds (r.removeFromRight (90));
                r.removeFromRight (brand::space::sm);
                pathLabel .setBounds (r);
            }

        private:
            void pickFolder()
            {
                chooser = std::make_unique<juce::FileChooser> (
                    "Pick a mirror drive root",
                    cfg.root.isDirectory()
                        ? cfg.root
                        : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                    "");
                const auto flags = juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories;
                chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
                {
                    const auto picked = fc.getResult();
                    if (picked.getFullPathName().isEmpty()) return;
                    cfg.root = picked;
                    pathLabel.setText (picked.getFullPathName(), juce::dontSendNotification);
                    if (changedCb) changedCb();
                });
            }

            int                                  index;
            MultitrackRecorder::MirrorConfig&    cfg;
            std::function<void(int)>             removeCb;
            std::function<void()>                changedCb;
            juce::Label                          pathLabel;
            juce::TextButton                     chooseBtn;
            juce::ComboBox                       formatBox;
            juce::TextButton                     removeBtn;
            std::unique_ptr<juce::FileChooser>   chooser;
        };

        class Content final : public juce::Component
        {
        public:
            explicit Content (AudioEngine& eng)
                : engine (eng)
            {
                working = engine.getMirrors();

                titleHint.setText ("Each mirror writes a full parallel copy of every armed track to "
                                   "its own drive. Primary + backup keep running independently.",
                                   juce::dontSendNotification);
                titleHint.setColour (juce::Label::textColourId, brand::textTertiary);
                titleHint.setFont   (brand::type::caption());
                titleHint.setJustificationType (juce::Justification::topLeft);
                titleHint.setMinimumHorizontalScale (1.0f);
                addAndMakeVisible (titleHint);

                addBtn.setButtonText ("+ Add mirror drive...");
                dialog::stylePrimary (addBtn);
                addBtn.onClick = [this] { addBlankRow(); };
                addAndMakeVisible (addBtn);

                applyBtn.setButtonText ("Apply");
                dialog::stylePrimary (applyBtn);
                applyBtn.onClick = [this] { applyAndClose(); };
                addAndMakeVisible (applyBtn);

                cancelBtn.setButtonText ("Cancel");
                dialog::styleSecondary (cancelBtn);
                cancelBtn.onClick = [this] { close (false); };
                addAndMakeVisible (cancelBtn);

                rebuildRows();
                setSize (760, 460);
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "MIRROR DRIVES");
            }

            void resized() override
            {
                auto body = dialog::bodyBounds (*this).reduced (brand::space::md,
                                                                 brand::space::sm);
                titleHint.setBounds (body.removeFromTop (40));
                body.removeFromTop (brand::space::sm);

                // Row list.
                const int rowH = 36;
                for (auto& row : rows)
                {
                    row->setBounds (body.removeFromTop (rowH));
                    body.removeFromTop (brand::space::xs);
                }
                body.removeFromTop (brand::space::sm);
                addBtn.setBounds (body.removeFromTop (brand::space::btnH).withSizeKeepingCentre (220, brand::space::btnH));

                auto footer = dialog::footerBounds (*this);
                applyBtn .setBounds (footer.removeFromRight (dialog::btnPrimary)
                                            .reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::sm);
                cancelBtn.setBounds (footer.removeFromRight (dialog::btnSecond)
                                            .reduced (0, brand::space::xs));
            }

        private:
            void rebuildRows()
            {
                rows.clear();
                for (size_t i = 0; i < working.size(); ++i)
                {
                    auto row = std::make_unique<MirrorRow> (
                        (int) i, working[i],
                        [this] (int idx) { removeRow (idx); },
                        [this] { /* working updated in-place via row callbacks */ });
                    addAndMakeVisible (*row);
                    rows.push_back (std::move (row));
                }
                resized();
            }

            void addBlankRow()
            {
                MultitrackRecorder::MirrorConfig c;
                // Genuinely BLANK root (not ~/Music). Defaulting to ~/Music made
                // an un-configured row pass the applyAndClose filter (~/Music
                // always exists), so a forgotten row armed a mirror that strewed
                // Track_NN files into the Music folder. The picker still opens at
                // ~/Music via its own default.
                c.root = juce::File();
                c.format = CaptureFormat::Wav24;
                working.push_back (c);
                rebuildRows();
            }

            void removeRow (int idx)
            {
                if (idx >= 0 && idx < (int) working.size())
                {
                    working.erase (working.begin() + idx);
                    rebuildRows();
                }
            }

            void applyAndClose()
            {
                // Keep every row the engineer actually pointed at a folder --
                // even one whose drive isn't mounted RIGHT NOW. The old filter
                // silently DROPPED an unmounted mirror, so a show recorded with
                // no redundancy while the engineer believed the mirror was armed.
                // The recorder re-creates the dir at record start when the drive
                // returns; only genuinely blank rows (no folder ever picked) go.
                std::vector<MultitrackRecorder::MirrorConfig> cleaned;
                for (const auto& m : working)
                {
                    if (m.root == juce::File()) continue;     // never picked a folder -> drop
                    cleaned.push_back (m);                    // keep even if not mounted now
                }
                engine.setMirrors (cleaned);
                close (true);
            }

            void close (bool applied)
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (applied ? 1 : 0);
            }

            AudioEngine&                                     engine;
            std::vector<MultitrackRecorder::MirrorConfig>    working;
            std::vector<std::unique_ptr<MirrorRow>>          rows;
            juce::Label                                      titleHint;
            juce::TextButton                                 addBtn;
            juce::TextButton                                 applyBtn;
            juce::TextButton                                 cancelBtn;
        };
    }

    inline juce::DialogWindow* MirrorDrivesDialog::launch (AudioEngine& engine)
    {
        auto content = std::make_unique<mirrordlg::Content> (engine);
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle             = "Mirror drives";
        opts.dialogBackgroundColour  = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar       = false;
        opts.resizable               = false;
        opts.content.setOwned (content.release());
        return opts.launchAsync();
    }
}
