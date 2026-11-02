#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/PreflightProbes.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    // Show-day drive picker: the backup folder, then any extra mirror
    // destinations. The backup and each mirror write a full parallel copy of
    // every armed track in their own format to their own drive root.
    //
    // Persistence + audio-thread wiring already lives in AudioEngine. This
    // dialog is a thin editor over setBackupFormat, the host's backup folder
    // path and getMirrors() / setMirrors(), plus two things it must own:
    //
    //   1. ROOT VALIDATION. A mirror writes to root/<sessionName>/Audio
    //      Files/ -- the same shape the primary and backup use. A root that
    //      is the session's parent therefore resolves to the take's OWN
    //      files, and with the default WAV 24-bit mirror format matching the
    //      default capture format, the same filenames too. Since the picker
    //      opens at ~/Music and sessions live in ~/Music/Zynforge Sessions,
    //      "mirror into my sessions folder" was one click from putting two
    //      writers on every take file for the length of the show. The rule
    //      lives in MultitrackRecorder::mirrorRootRejection so the recorder
    //      enforces the same thing at record start.
    //
    //   2. A RECORDING GUARD. The recorder refuses a mirror change mid-take;
    //      this dialog is where the engineer finds that out, at the desk.
    class BackupMirrorDialog
    {
    public:
        // What the header knows about the backup folder. Message thread.
        struct Host
        {
            std::function<void (const juce::String&)> backupRefused;      // a picked folder can't be used
            std::function<bool (const juce::File&)>   applyBackupFolder;  // false when a take owns the recorder
        };

        // Open the modal. Shows the backup and the mirrors as editable rows
        // and writes them back on Apply. Cancel discards changes.
        static juce::DialogWindow* launch (AudioEngine& engine, Host host);

        // Why this folder cannot be the backup; empty when it can. Creates a
        // folder that does not exist yet, as the chooser's New Folder would.
        static juce::String backupFolderRefusal (const juce::File& folder, const juce::File& activeSession)
        {
            if ((! folder.isDirectory() && ! folder.createDirectory().wasOk())
                || ! preflight::volumeWritable (folder))
                return "Backup folder is not writable";
            const auto rejection = MultitrackRecorder::mirrorRootRejection (folder, activeSession, {}, {});
            return rejection.isNotEmpty() ? "Backup folder rejected: " + rejection : juce::String();
        }

    private:
        BackupMirrorDialog() = delete;
    };

    class DialogAuditTests;

    namespace backupdlg
    {
        // Persistence keeps 16 slots (AudioEngine::setMirrors); refuse to add
        // past that rather than silently dropping the tail on save.
        inline constexpr int kMaxMirrors = 16;

        // One row per destination: path label + format combo + remove button.
        // The backup row has no remove button, and may refuse a picked folder
        // through acceptPick. The owning Content rebuilds the mirror rows on
        // every mutation so the list always reflects the working state.
        class DriveRow final : public juce::Component
        {
        public:
            DriveRow (int idx,
                      MultitrackRecorder::MirrorConfig& config,
                      std::function<void(int)> onRemove,
                      std::function<void()>    onChanged,
                      std::function<juce::String (const juce::File&, int)> validateRoot,
                      std::function<bool (const juce::File&)> acceptPick = {})
                : index (idx), cfg (config), removeCb (std::move (onRemove)),
                  changedCb (std::move (onChanged)), validateCb (std::move (validateRoot)),
                  acceptCb (std::move (acceptPick))
            {
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
                addChildComponent (removeBtn);
                removeBtn.setVisible ((bool) removeCb);

                refreshPathDisplay();
            }

            // Greyed while a take is rolling -- the recorder refuses the change,
            // so the controls must not look live.
            void setRowEnabled (bool on)
            {
                chooseBtn.setEnabled (on);
                formatBox.setEnabled (on);
                removeBtn.setEnabled (on);
            }

            // A row whose root the recorder would refuse is shown in the record
            // colour with the reason, so it's obvious BEFORE Apply which row is
            // the problem -- not just that "something" is wrong.
            void refreshPathDisplay()
            {
                const auto reason = validateCb ? validateCb (cfg.root, index) : juce::String();
                const bool blank  = (cfg.root == juce::File());

                pathLabel.setText (blank ? "(no folder chosen)"
                                         : cfg.root.getFullPathName()
                                           + (reason.isNotEmpty() ? "   --  " + reason : juce::String()),
                                   juce::dontSendNotification);
                pathLabel.setColour (juce::Label::textColourId,
                                     reason.isNotEmpty() && ! blank ? brand::accentRecord
                                     : blank                        ? brand::textTertiary
                                                                    : brand::textPrimary);
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
                    acceptCb ? "Pick a backup folder (recordings are copied here)" : "Pick a mirror drive root",
                    cfg.root.isDirectory()
                        ? cfg.root
                        : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                    "");
                const auto flags = juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories;
                // SafePointer: the picker outlives this click, and the row list
                // is rebuilt (destroying rows) on every mutation.
                juce::Component::SafePointer<DriveRow> safeSelf (this);
                chooser->launchAsync (flags, [safeSelf] (const juce::FileChooser& fc)
                {
                    if (safeSelf == nullptr) return;
                    const auto picked = fc.getResult();
                    if (picked.getFullPathName().isEmpty()) return;
                    if (safeSelf->acceptCb && ! safeSelf->acceptCb (picked)) return;
                    safeSelf->cfg.root = picked;
                    safeSelf->refreshPathDisplay();
                    if (safeSelf->changedCb) safeSelf->changedCb();
                });
            }

            int                                  index;
            MultitrackRecorder::MirrorConfig&    cfg;
            std::function<void(int)>             removeCb;
            std::function<void()>                changedCb;
            std::function<juce::String (const juce::File&, int)> validateCb;
            std::function<bool (const juce::File&)> acceptCb;
            juce::Label                          pathLabel;
            juce::TextButton                     chooseBtn;
            juce::ComboBox                       formatBox;
            juce::TextButton                     removeBtn;
            std::unique_ptr<juce::FileChooser>   chooser;
        };

        class Content final : public juce::Component, private juce::Timer
        {
        public:
            Content (AudioEngine& eng, BackupMirrorDialog::Host hostHooks)
                : engine (eng), host (std::move (hostHooks))
            {
                working = engine.getMirrors();
                backup.root   = engine.getBackupDirectory();
                backup.format = engine.getRecorder().getBackupCaptureFormat();

                titleHint.setText ("The backup and each mirror write a full parallel copy of every armed "
                                   "track to their own drive, in their own format.",
                                   juce::dontSendNotification);
                titleHint.setColour (juce::Label::textColourId, brand::textTertiary);
                titleHint.setFont   (brand::type::caption());
                titleHint.setJustificationType (juce::Justification::topLeft);
                titleHint.setMinimumHorizontalScale (1.0f);
                addAndMakeVisible (titleHint);

                backupRow = std::make_unique<DriveRow> (
                    -1, backup, std::function<void(int)>(),
                    [this] { refreshAllRows(); },
                    [this] (const juce::File& f, int)
                    { return MultitrackRecorder::mirrorRootRejection (f, engine.getActiveSessionDir(), {}, {}); },
                    [this] (const juce::File& f) { return acceptBackup (f); });
                addAndMakeVisible (*backupRow);

                for (auto* caption : { &backupCaption, &mirrorCaption })
                {
                    caption->setFont (brand::type::captionBold());
                    caption->setColour (juce::Label::textColourId, brand::textTertiary);
                    addAndMakeVisible (*caption);
                }
                backupCaption.setText ("Backup", juce::dontSendNotification);
                mirrorCaption.setText ("Mirror drives", juce::dontSendNotification);

                // The row list scrolls. It used to lay rows out straight down a
                // fixed 460px window, so past ~6 mirrors the rows, the Add button
                // and eventually the footer all collapsed to zero-height rects --
                // you could neither add another nor remove the ones you had.
                rowHolder.setInterceptsMouseClicks (false, true);
                rowView.setViewedComponent (&rowHolder, false);
                rowView.setScrollBarsShown (true, false);
                rowView.setColour (juce::ScrollBar::thumbColourId, brand::edge);
                addAndMakeVisible (rowView);

                addBtn.setButtonText ("+ Add mirror drive...");
                dialog::stylePrimary (addBtn);
                addBtn.onClick = [this] { addBlankRow(); };
                addAndMakeVisible (addBtn);

                status.setFont (brand::type::caption());
                status.setColour (juce::Label::textColourId, brand::accentRecord);
                status.setJustificationType (juce::Justification::centredLeft);
                addAndMakeVisible (status);

                applyBtn.setButtonText ("Apply");
                dialog::stylePrimary (applyBtn);
                applyBtn.onClick = [this] { applyAndClose(); };
                addAndMakeVisible (applyBtn);

                cancelBtn.setButtonText ("Cancel");
                dialog::styleSecondary (cancelBtn);
                cancelBtn.onClick = [this] { close (false); };
                addAndMakeVisible (cancelBtn);

                rebuildRows();
                setSize (760, 540);

                // The recorder refuses a backup or mirror change mid-take. This dialog can
                // already be open when RECORD is pressed, so enablement has to be
                // re-derived from live state rather than computed once here.
                lastRecording = engine.isRecording();
                applyRecordingState();
                startTimerHz (4);
            }

            ~Content() override { stopTimer(); }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "BACKUP & MIRROR DRIVES");
            }

            void resized() override
            {
                auto body = dialog::bodyBounds (*this).reduced (brand::space::md,
                                                                 brand::space::sm);
                titleHint.setBounds (body.removeFromTop (40));
                body.removeFromTop (brand::space::sm);
                backupCaption.setBounds (body.removeFromTop (18));
                backupRow->setBounds (body.removeFromTop (kRowH).withTrimmedRight (12));   // the mirror rows' scrollbar
                body.removeFromTop (brand::space::md);
                mirrorCaption.setBounds (body.removeFromTop (18));

                addBtn.setBounds (body.removeFromBottom (brand::space::btnH)
                                      .withSizeKeepingCentre (220, brand::space::btnH));
                body.removeFromBottom (brand::space::sm);
                rowView.setBounds (body);
                layoutRows();

                auto footer = dialog::footerBounds (*this);
                applyBtn .setBounds (footer.removeFromRight (dialog::btnPrimary)
                                            .reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::sm);
                cancelBtn.setBounds (footer.removeFromRight (dialog::btnSecond)
                                            .reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::md);
                status.setBounds (footer);
            }

        private:
            friend class zynforge::DialogAuditTests;
            static constexpr int kRowH = 36;

            void timerCallback() override
            {
                const bool rec = engine.isRecording();
                if (rec == lastRecording) return;      // change-gated: idle costs nothing
                lastRecording = rec;
                applyRecordingState();
            }

            void applyRecordingState()
            {
                const bool rec = lastRecording;
                addBtn  .setEnabled (! rec);
                applyBtn.setEnabled (! rec);
                backupRow->setRowEnabled (! rec);
                for (auto& r : rows) r->setRowEnabled (! rec);
                if (rec)
                    status.setText ("Recording -- these drives are locked until the take ends.",
                                    juce::dontSendNotification);
                else if (status.getText().startsWith ("Recording"))
                    status.setText ({}, juce::dontSendNotification);
            }

            // Would the recorder refuse this root? `selfIdx` is excluded from the
            // duplicate check so a row never conflicts with itself.
            juce::String rejectionFor (const juce::File& root, int selfIdx) const
            {
                if (root == juce::File()) return {};      // blank rows are just dropped
                std::vector<juce::File> others;
                for (int i = 0; i < (int) working.size(); ++i)
                    if (i != selfIdx && working[(size_t) i].root != juce::File())
                        others.push_back (working[(size_t) i].root);
                return MultitrackRecorder::mirrorRootRejection (
                           root, engine.getActiveSessionDir(), backup.root, others);
            }

            // The refusals a backup folder has always had, at the moment of
            // picking it. An offline drive already saved here is left alone.
            bool acceptBackup (const juce::File& folder)
            {
                const auto refusal = BackupMirrorDialog::backupFolderRefusal (folder, engine.getActiveSessionDir());
                if (refusal.isEmpty()) return true;
                status.setText (refusal, juce::dontSendNotification);
                if (host.backupRefused) host.backupRefused (refusal);
                return false;
            }

            void layoutRows()
            {
                const int w = juce::jmax (0, rowView.getWidth() - 12);   // leave the scrollbar
                rowHolder.setSize (w, (int) rows.size() * (kRowH + brand::space::xs));
                int y = 0;
                for (auto& row : rows)
                {
                    row->setBounds (0, y, w, kRowH);
                    y += kRowH + brand::space::xs;
                }
            }

            void rebuildRows()
            {
                rows.clear();
                for (size_t i = 0; i < working.size(); ++i)
                {
                    auto row = std::make_unique<DriveRow> (
                        (int) i, working[i],
                        [this] (int idx) { removeRow (idx); },
                        [this] { refreshAllRows(); },
                        [this] (const juce::File& f, int selfIdx) { return rejectionFor (f, selfIdx); });
                    row->setRowEnabled (! lastRecording);
                    rowHolder.addAndMakeVisible (*row);
                    rows.push_back (std::move (row));
                }
                layoutRows();
            }

            // One row's change can make ANOTHER row valid or invalid (duplicates),
            // so re-run every row's display, not just the one that moved.
            void refreshAllRows()
            {
                backupRow->refreshPathDisplay();
                for (auto& r : rows) r->refreshPathDisplay();
            }

            void addBlankRow()
            {
                if ((int) working.size() >= kMaxMirrors)
                {
                    status.setText ("Maximum of " + juce::String (kMaxMirrors) + " mirror drives.",
                                    juce::dontSendNotification);
                    return;
                }
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
                rowView.setViewPositionProportionately (0.0, 1.0);   // show the new row
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
                if (engine.isRecording())
                {
                    status.setText ("Recording -- stop the take before changing these drives.",
                                    juce::dontSendNotification);
                    return;
                }

                // Keep every row the engineer actually pointed at a folder --
                // even one whose drive isn't mounted RIGHT NOW. The old filter
                // silently DROPPED an unmounted mirror, so a show recorded with
                // no redundancy while the engineer believed the mirror was armed.
                // The recorder re-creates the dir at record start when the drive
                // returns; only genuinely blank rows (no folder ever picked) go.
                //
                // A root the recorder would REFUSE is a different case entirely:
                // it can't be written to safely at all, so refuse it here rather
                // than let the recorder silently skip it on the night.
                std::vector<MultitrackRecorder::MirrorConfig> cleaned;
                for (int i = 0; i < (int) working.size(); ++i)
                {
                    const auto& m = working[(size_t) i];
                    if (m.root == juce::File()) continue;     // never picked a folder -> drop

                    const auto reason = rejectionFor (m.root, i);
                    if (reason.isNotEmpty())
                    {
                        status.setText ("Mirror " + juce::String (i + 1) + ": " + reason,
                                        juce::dontSendNotification);
                        refreshAllRows();
                        return;                                // don't close on a bad row
                    }
                    cleaned.push_back (m);                     // keep even if not mounted now
                }

                // Each of these lost a race with RECORD if it says no.
                if (backup.root != engine.getBackupDirectory()
                    && (! host.applyBackupFolder || ! host.applyBackupFolder (backup.root)))
                {
                    status.setText ("Recording started -- the backup folder was not changed.",
                                    juce::dontSendNotification);
                    return;
                }
                if (! engine.setBackupFormat (backup.format) || ! engine.setMirrors (cleaned))
                {
                    status.setText ("Recording started -- the drives were not all changed.",
                                    juce::dontSendNotification);
                    return;
                }
                close (true);
            }

            void close (bool applied)
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (applied ? 1 : 0);
            }

            AudioEngine&                                     engine;
            BackupMirrorDialog::Host                         host;
            MultitrackRecorder::MirrorConfig                 backup;
            std::unique_ptr<DriveRow>                        backupRow;
            std::vector<MultitrackRecorder::MirrorConfig>    working;
            std::vector<std::unique_ptr<DriveRow>>           rows;
            juce::Viewport                                   rowView;
            juce::Component                                  rowHolder;
            juce::Label                                      titleHint, backupCaption, mirrorCaption;
            juce::Label                                      status;
            juce::TextButton                                 addBtn;
            juce::TextButton                                 applyBtn;
            juce::TextButton                                 cancelBtn;
            bool                                             lastRecording { false };
        };
    }

    inline juce::DialogWindow* BackupMirrorDialog::launch (AudioEngine& engine, Host host)
    {
        auto content = std::make_unique<backupdlg::Content> (engine, std::move (host));
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle             = "Backup & Mirror Drives";
        opts.dialogBackgroundColour  = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar       = false;
        opts.resizable               = false;
        opts.content.setOwned (content.release());
        return opts.launchAsync();
    }
}
