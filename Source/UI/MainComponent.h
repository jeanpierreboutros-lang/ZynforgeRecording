#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <thread>

#include "../Audio/AudioEngine.h"
#include "../Audio/TrackExporter.h"
#include "../Capture/CaptureSupervisor.h"
#include "../Network/ConsoleLink.h"
#include "../Network/TwinLink.h"
#include "../Theme/ZynForgeLookAndFeel.h"
#include "AddTracksDialog.h"
#include "AlarmBanner.h"
#include "BigClockPanel.h"
#include "IconButton.h"
#include "ChannelStrip.h"
#include "EditPage.h"
#include "ExportDialog.h"
#include "ExportSongsDialog.h"
#include "../Audio/ProjectExport.h"
#include "MasterStrip.h"
#include "NewSessionDialog.h"
#include "WelcomeDialog.h"
#include "PerfDashboard.h"
#include "SetlistBar.h"
#include "MenuCommandTable.h"
#include "SessionJobRunner.h"
#include "SessionMetadataWriter.h"
#include "../Audio/SettingsFile.h"
#include "Toast.h"
#include "PlaceholderView.h"
#include "TransportBar.h"
#include "ClipEditTargets.h"

#include <memory>
#include <set>
#include <vector>

namespace zynforge { struct PreflightRow; class ChannelStrip; class CaptureDaemonLinkTests; class MainTransportRegressionTests; class PunchSessionGuardTests; class OctoberRegressionTests; class ReviewPersistenceRepairTests; class ReviewUiFormatRepairTests; class ConsoleVscTests; class NetworkStateTests; class TwinLinkHostTests; class SongExportTests; class SessionInterchangeTests; class MainWindowGuardTests; class MainWindowFixTests; class EditIntegrityTests; class PunchRuleTests; class EditDetailTests; class EditRowFixTests; class SessionSafetyTests; class SessionFixTests; struct ConcurrencyAuditAccess; class EngineFixTests; class CapturePathTests; }

class MainComponent final : public juce::Component,
                            public juce::KeyListener,
                            public juce::MenuBarModel,
                            private juce::Timer,
                            private zynforge::twin::TwinHost
{
public:
    MainComponent();
    ~MainComponent() override;

    // Test hook: when true, the ctor skips the startup timer, the deferred
    // welcome / session-recovery dialogs, and macOS menu-bar registration --
    // so a unit test can construct MainComponent headlessly (e.g. to audit
    // every menu id for collisions). Set BEFORE constructing.
    static inline bool s_testConstruct = false;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Both header rows keep their status and warnings readable down to here.
    static constexpr int kMinWidth = 1210, kMinHeight = 700;

    bool keyPressed (const juce::KeyPress&, juce::Component*) override;

    // MenuBarModel -- populates the macOS system menu bar.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected (int menuItemID, int topLevelMenuIndex) override;
    // The menu bar's dispatch table (built on first use). Public so the menu
    // tests can check every built item against it.
    const zynforge::MenuCommandTable& getMenuCommands()
    { if (menuCommands.isEmpty()) registerMenuCommands(); return menuCommands; }

private:
    zynforge::MenuCommandTable menuCommands;
    void registerMenuCommands();
    // Menu handlers too long to sit inline in the table.

    void rebuildStrips();
    void updateMixerPlaceholder();   // show/hide the MIXER empty-state overlay
    void onRecordClicked();
    // What an open question was raised about. By the time it is answered the
    // menu bar, a Finder open or a remote can have changed either.
    struct QuestionScope { juce::File session; int layout { -1 }; };
    QuestionScope questionScope();
    bool stillInScope (const QuestionScope&);
    void addTracks (const std::vector<zynforge::AddTracksDialog::Entry>&, const QuestionScope&);
    // onRecordClicked's stages, in order. A press during a take is consumed
    // first; prepareRecordStart runs EVERY refusal before anything changes;
    // then the take starts through exactly one path.
    struct RecordStartPlan
    {
        juce::File  dir;
        int         numTracks { 0 }, armed { 0 }, autoRouted { 0 };
        bool        continueTake { false }, continueAppend { false };
        juce::int64 punchAt { -1 };
        zynforge::devicechoice::RecordQuestion device;
        juce::String deviceSession;   // the session a device answer is held under
    };
    bool handleRecordPressDuringTake();
    void endPunchSession();
    bool prepareRecordStart (RecordStartPlan&);
    void planRecordPosition (RecordStartPlan&);
    bool recordPlanHasSelection (const RecordStartPlan&);
    bool recordPlanPunches (const RecordStartPlan&);
    bool cursorPunchNeedsAnswer (const RecordStartPlan&);
    void askBeforeCursorPunch (const RecordStartPlan&);
    void answerCursorPunch (int answer);
    void askBeforeDeviceRecord (const RecordStartPlan&);
    void answerDeviceRecord (int answer, const juce::String& session, const juce::String& device);
    void serviceChosenDevice();
    void showDeviceState (const zynforge::devicechoice::DeviceState&);
    void runRecordPlan (RecordStartPlan&);
    void applyRecordAutoRouting (RecordStartPlan&);
    void startDaemonTake (const RecordStartPlan&);
    bool startSelectionPunch (const RecordStartPlan&);
    void startLocalTake (const RecordStartPlan&);
    void onDeviceClicked();
    void onLoadSessionClicked();
    void completeOpenFolder (const juce::File& dir);
    void onPlayClicked();
    void startPlaybackFromCursor();
    void onStopClicked();
    // Immediate capture stop shared by the confirmed UI STOP, remote
    // transports and "Stop & Quit".  Unlike onStopClicked it has no two-tap
    // guard because its callers have already confirmed their intent.
    bool stopActiveCapture (bool stopPlaybackAndRewind, bool persistMetadata = true);
    void stopActiveCaptureAsync (bool rewind, std::function<void (bool)> completion = {});
    std::optional<bool> handleRemoteTransport (
        zynforge::AudioEngine::RemoteTransportAction, juce::String& error);
    // Twin Recorder (MainComponentTwin.cpp). The link only ever enters through
    // these, on the message thread, and a follower's take takes the same
    // RECORD path as any other remote.
    zynforge::twin::MacStatus twinStatus() override;
    bool twinStartRecord (juce::String& refusal) override;
    void twinStopRecord (std::function<void (bool, juce::String)> done) override;
    void twinDropMarker (const juce::String& name) override;
    void twinNotify (const juce::String& text) override;
    void noteTwinStopIfEnded();
    void showTwinRecorderDialog();
    void updateTwinStatusLabel();
    void onSaveSessionState();
    void onSaveSessionAs();
    void onExportAudio();      // File > Export > Export Audio...
    void chooseRawTakesFolder (std::vector<int> tracks, const zynforge::ExportOptions&);
    void onBounceStems (std::vector<int> tracks);  // render the edited clip arrangement to flat stems
    void onBounceStereoMix();  // render the edited arrangement summed to stereo
    void completeBounceStems (juce::File, const std::vector<int>& tracks);
    void completeBounceStereoMix (juce::File);
    void completeAudioImport (const juce::Array<juce::File>&);
    void exportShowHandoff();
    void exportDawProject (zynforge::projectexport::Format);
    void exportSongs();
    void startSongExport (zynforge::songexport::Choice);
    juce::String buildTimelineCsv();
    void closeSession();               // unload to the empty/Welcome state without quitting
    // Everything the window holds about a session that clearSessionState cannot
    // reach. Called beside it, on every way out of a session.
    void forgetSessionUiState();
    void confirmSessionReplacement (std::function<void()> continuation);
    void onImportAudioFiles();
    void onLockToggled();
    void onBackupClicked();
    bool applyBackupFolder (const juce::File&);
    void onVscClicked();
    void openPatchWindow();
    void openMetersWindow();
    void applyLockState();
    // Returns true if the recovery dialog was shown (and onClosed is deferred
    // to its close). Returns false if there was nothing to recover, in which
    // case onClosed is NOT called -- the caller runs its own continuation. This
    // lets the launch flow avoid auto-reopening a session under the dialog.
    bool offerSessionRecovery (std::function<void()> onClosed = {});
    juce::Array<juce::File> sessionsToRecover (const juce::File& sessionsRoot) const;
    juce::String removeRecoveredSession (const juce::File& dir);
    struct SessionPins { juce::File session, mixer, lastCapture, markers; double markerRate { 0.0 }; };
    SessionPins beforeUnansweredStart;
    void giveUpOnUnansweredStart();
    bool helperHoldsSession (const juce::File& dir) const;
    bool unprovenHelperAlive() const;
    void showStartupWelcome();
    void createSessionFromWelcome (const zynforge::WelcomeDialog::NewResult&);
    void openSessionFromWelcome (const juce::File&);
    void showKeyboardShortcuts();
    void showAboutDialog();
    void showFirstRunTutorial (std::function<void()> onFinished = {});
    void showUserGuide();
    void launchNewSessionDialog (juce::File templateFile = {});
    juce::File createSessionFolderStructure (const zynforge::NewSessionDialog::Result&);
    juce::String sessionCreateFailureMessage() const;
    // Canonical "open this session folder" sequence: pin the active dir,
    // restore setlist + UI layout, load the audio, restore the full mixer
    // state, force a strip rebuild. Returns the track count loaded. Used by
    // File > Open, the Welcome dialog, and the launch auto-reopen.
    // `reload` re-reads the session that is already open: a recovered take or a
    // restored backup changed its files. Nothing else may, because a reload
    // replaces every unsaved edit.
    int  openSessionFolder (const juce::File& dir, bool reload = false);
    bool isSessionOpen (const juce::File& dir) const;
    void reportDamagedFiles (const juce::File& dir, const std::vector<zynforge::sessionrepair::DamagedFile>& files);
    bool restoreDamagedFiles (const juce::File& dir, const std::vector<zynforge::sessionrepair::DamagedFile>& files);
    // Stands in for the dialog, which a headless test cannot answer.
    std::function<void (const std::vector<zynforge::sessionrepair::DamagedFile>&)> damagedFilesPromptForTests;
    void restoreSessionDocuments (const juce::File& dir);
    // Copies, once, the documents of a session that holds data of a feature this
    // build no longer has, before anything can overwrite them.
    void keepRemovedFeatureData (const juce::File& dir);
    void adoptRollingDaemonTake (const zynforge::EngineStatus& status, juce::int64 receivedAtMs);
    void adoptDaemonTakeSession (const zynforge::EngineStatus& status);
    void releaseHeldRecordPress();
    bool sessionFolderIsMissing() const;
    juce::String noFolderText (const juce::String& noSession, const juce::String& before) const;
    bool refuseUnloadedSession (const juce::String& before);
    void offerLeaveMissingSession (const juce::String& leaveLabel, std::function<void()> leave);
    std::function<bool (const juce::String& text)> leaveMissingSessionPromptForTests;
    void finishCloseSession (bool saved);
    void completeSaveAs (const juce::File& source, const juce::File& dest);
    void completeRelocate (const juce::File& oldDir, const juce::File& newParent);
    void completeShowHandoff (const juce::File& source, const juce::File& dest);
    bool carryMixerInto (const juce::File& copy);
    juce::String openCopiedSession (const juce::File& source, const juce::File& copy, bool carried);
    static juce::String moveStatusLine (const juce::File& oldDir, const juce::File& newDir, bool carried,
                                        bool oldCopyRemains, const juce::String& refused);
    void startHandoffCopy (const juce::File& source, const juce::File& dest);
    // When a job's save of the source fails, whether to go on from what is on
    // disk. The tests stand in for the dialog through continueFromDiskPromptForTests.
    void offerContinueFromDisk (const juce::String& action, const juce::String& carried,
                                std::function<void()> proceed, std::function<void()> cancel);
    std::function<bool (const juce::String& text)> continueFromDiskPromptForTests;
    juce::uint32 nextAutosaveRetryMs (juce::uint32 intervalMs);
    bool saveMixerToTakeFolder (const juce::File& dir);
    bool saveSessionStateTo (const juce::File& dir);
    zynforge::SessionMetadataSnapshot captureMetadataSnapshot (const juce::File& dir, bool backup = true);
    juce::String captureProjectPatchJson() const;
    juce::var captureUILayoutJson() const;
    void enqueueMetadataSave (const juce::File& dir, const juce::String& success,
                              std::function<void (bool)> completion = {}, bool automatic = false,
                              bool backup = true);
    void enqueueMetadataSnapshot (zynforge::SessionMetadataSnapshot, const juce::String& success,
                                  std::function<void (bool)> completion = {}, bool automatic = false);
    void requestUILayoutSave();
    void saveCueChanges (const juce::String& success);
    zynforge::SessionMetadataWriter metadataWriter;
    int pendingMetadataSaves = 0;
    juce::uint64 metadataRevision = 0;
    bool captureMetadataPending = false; // holds the entire media + metadata STOP operation
    bool restoringUILayout = false;
    bool metadataContinuationReserved = false;
    juce::String lastCaptureFinalizationError;
    // Periodic auto-save + timestamped backup of the active session, driven
    // from the 10 Hz timer. Interval (minutes; 0 = off) lives in appProps
    // "autosaveMinutes"; showAutosaveSettings() is the picker dialog. Each
    // interval writes the complete session state, including changes made by
    // controls that do not participate in the undo manager.
    void serviceAutosave();
    void showAutosaveSettings();
    // Kick off a track export. Resolves every source file + destination stem
    // on the MESSAGE thread (it reads TrackStates), then does the actual
    // decode / resample / encode work on `sessionJobs` and reports the result
    // on the message thread. Running the whole thing inline used to freeze the app for
    // the length of the export -- minutes for a 32-track batch, with no
    // progress and no way to cancel.
    void startExportTracksTo (const juce::File& dir,
                              const std::vector<int>& channelIndices,
                              const zynforge::ExportOptions&);
    void showStatus (const juce::String& msg);
public:
    // The cue list after a Crop or Ripple Delete. With a whole timeline the
    // cues move; a cue in the removed audio goes. On some strips only they stay.
    static std::vector<zynforge::SetlistBar::Cue> cuesAfterCut (
        const std::vector<zynforge::SetlistBar::Cue>& cues, const zynforge::timelinecut::Cut&,
        bool wholeTimeline);
    // Called from Main.cpp's systemRequestedQuit so an unsaved
    // session / running recording prompts BEFORE the app shuts down.
    void confirmAndQuit();
    // Application/document hand-off for Finder-opened .zfproj files.
    bool openSessionDocument (const juce::File& document, bool confirmBeforeReplacing = false);
private:
    // Edit menu actions.
    // Old Edit-menu stubs were renamed to edit*; declarations live in
    // the public-ish private section above (editUndo / editCutSelected
    // / editSoloSelection / editSetRangeToLoopRange / editToggleSnap /
    // editSplitAtPlayhead / editStartRange / editFinishRange).
    void removeLastCapture();
    bool deleteCaptureSession (const juce::File& target);
    static juce::String lastCapturePrompt (const juce::File& target);
    // A test process has no business in the owner's Trash.
    static juce::File testTrashFolder();
    void showBatchRenameDialog();
    // Import channel names from a console/spreadsheet CSV onto existing tracks.
    void importChannelNamesFromCsv();
    // Create a brand-new session sized + named from a channel CSV.
    void createSessionFromCsv();
    void completeSessionFromCsv (const juce::File& csv);
    juce::String notSavedWarning (bool saved) const;
    // Request channel names from a DiGiCo console, then build a session from
    // what comes back (the bidirectional console link).
    void createSessionFromConsole();
    void finishSessionFromConsole (const juce::File& sessionAtPress, bool viaLink, int stereoPairsAsked);
    // Builds the session from what the desk reported. `stereoPairsAsked` is
    // how many link states were requested; 0 means this desk or path can't
    // report them.
    void buildSessionFromConsole (const std::map<int, juce::String>& names,
                                  const std::map<int, bool>& stereoLinks,
                                  int stereoPairsAsked, const juce::String& namePrefix);
    // Link or unlink a stereo pair the way a strip's menu does: the L/R pans
    // and the rebuild come with it. False, with the reason on the status line,
    // when refused.
    bool setStereoLink (int leftIndex, bool link);
    void showBatchColourDialog();
    void deleteSelectedStrips();
    // Stop every live strip's timers before a recorder-vector shrink frees the
    // TrackStates they reference (New Session / close / smaller open / console
    // rebuild). Prevents a freed-memory read in the window before the rebuild.
    void condemnAllStrips();
    void colourSelectedStrips();
    void moveSelectedStrips (int delta);
    void clearStripSelection();
    void removeStrip (int firstTrack, int width);
    void selectAllStrips();
    int  physicalFromLogicalIdx (int logical);
    int  logicalFromPhysicalIdx (int physical);

    // Multi-select: indices are LOGICAL strip indices (stereo pairs
    // counted as one). Engineer toggles via shift/cmd-click on a strip.
    std::set<int> selectedLogical;
    // The layout and session those positions mean; see syncStripSelectionScope.
    int        selectionLayoutGen { -1 };
    juce::File selectionSession;
    void syncStripSelectionScope();
    void showSessionInfo();
    // Move the active session folder (its recorded audio included) to a new
    // location, then re-pin everything from there. Stopped-only; the copy
    // runs on a background thread so a cross-volume move can't freeze the UI.
    void relocateActiveSession();
    // OSC listen port (persisted in appProps, default 8000); the Control
    // Surfaces dialog edits it.
    int  oscListenPort() const;
    // Export a portable timeline sidecar (tracks + timecode-stamped markers
    // + cues) to a CSV an editor can use to rebuild the session elsewhere.
    void exportTimelineCsv();
    // Set the session's intended sample rate (the rate the mismatch guard
    // compares against) and try to bring the hardware along with it.
    void applySessionSampleRate (double sr);
    void writeSoundcheckReport();
    bool refuseAnalysisNow();
    void cancelAnalysisForTake();
    bool analysisResultWanted (const juce::File& scannedSession);
    void runQcAnalysis();
    void detectSongsToMarkers();
    // Launch-time crash telemetry: offer the newest report since last run.
    void scanForCrashReports();
    void showCrashReportNotice (const juce::Array<juce::File>& reports);
    bool configureCaptureDaemon();
    juce::var captureDaemonSettings();   // what the daemon takes from this window, the device aside
    bool daemonModeActive() const { return useCaptureDaemon && captureSupervisor.isAttached(); }
    void toggleCaptureDaemon();
    void connectCaptureDaemon (bool announceReattach);
    // A daemon this window lost. resolveLostDaemon looks again and says which:
    // back (attached again), gone (nothing listens, so no take is rolling),
    // unknown (something holds the port and will not prove itself). A take is
    // called over only on `gone` or by a daemon that answers.
    enum class LostDaemon { back, gone, unknown };
    LostDaemon resolveLostDaemon();
    void serviceLostDaemon();
    bool lostDaemonTakeUnknown() const;
    void reconnectCaptureDaemon();
    void adoptDaemonTake (const zynforge::EngineStatus&, bool announce);
    void leaveLostDaemon();
    void disableCaptureDaemon();
    void requestDaemonTake (const juce::File& dir, int numTracks, const juce::BigInteger& arms,
                            int armed, bool mixerInTakeFolder);
    void stopDaemonCapture (bool rewind, std::function<void (bool)> completion);
    void endDaemonTake (juce::String error, bool rewind, std::function<void (bool)> completion);
    void localTakeInterrupted();
    void saveFinalizedCaptureMetadata (bool mediaOk, juce::String mediaError,
                                       std::function<void (bool)> completion);
    void promptConsoleConnect();
    void loadConsoleStateAfterConnect (const juce::String& host);
    void wireConsoleLink();
    void saveConsoleLog();
    void writeConsoleLog (const juce::File&);
    juce::String consoleLinkSummary() const;
    bool refuseConsoleWriteDuringTake();
    void consoleToggleSoundcheck();
    void consoleCheckDeskPatch();
    void consoleCaptureGains();
    void consoleRestoreGains();
    void togglePunchMode();
    void servicePunch();        // called each timerCallback tick
    bool wasInsidePunch { false };
    bool punchWindowTakeActive { false };   // this take was opened by the PUNCH window
    // Pro Tools-style selection punch driven by the RECORD button: play from a
    // pre-roll lead-in, auto punch-in at the selection start and punch-out at
    // its end, then play a post-roll tail before stopping. servicePunch does
    // the in/out record; servicePunchSession() runs the post-roll + teardown.
    void servicePunchSession();
    bool        punchSessionActive { false };
    juce::int64 punchOutSample     { 0 };
    juce::int64 punchPostRollEnd   { 0 };
    bool playheadLockedForPunch();

    // Snap modes -- cycled by the '4' key (also surfaced in the
    // Edit menu).
    enum class SnapMode { Off = 0, Markers = 1 };
    SnapMode snapMode      { SnapMode::Off };
    bool     snapToMarkers { false };   // mirrors (snapMode == Markers) for legacy reads

    // Edit-menu plumbing.
    juce::UndoManager undoManager;
    juce::var         stripClipboard;   // JSON of cut/copied strip settings

    // Auto-save bookkeeping (see serviceAutosave / showAutosaveSettings).
    juce::uint32 lastAutosaveMs    { 0 };   // 0 = clock not started yet
    juce::uint32 recordPressHeldAtMs { 0 }; // a press on this Mac waiting out an idle save; 0 = none
    bool         recordPressHeldRolling { false };  // whether the transport rolled when it was pressed
    int          autosaveFailures  { 0 };   // in a row; spaces out the retries
    int          lastSavedUndoUnits { 0 };  // undo units at the last save -- the dirty signal
    juce::String lastMetadataSaveError;     // why the last save failed, for the STOP message
    juce::String lastMetadataSaveNotice;    // what the last good save had to set aside
    juce::var         clipClipboard;    // JSON of cut/copied audio clip(s)
    juce::File        clipClipboardSession;   // the session those clips belong to
    int               nudgeMs { 100 };  // clip-nudge step; cycled with N, persisted
    void flushMixerUndo();
    // A bulk mixer action: take `before`, make the change, then commit it.
    juce::var beginMixerUndo();
    void commitMixerUndo (const juce::String& label, const juce::var& before);
    // Clip/playlist undo: capture engine.playlistsToJson() before the edit,
    // pass it here after; records an undoable step only if clips changed.
    void pushClipUndo (const juce::String& label, const juce::var& before);
    // Crop and Ripple Delete: everything they move, as one undo step.
    struct TimelineSnapshot
    {
        zynforge::AudioEngine::TimelineState engineState;
        std::vector<zynforge::SetlistBar::Cue> cues;
    };
    TimelineSnapshot captureTimeline() const;
    void restoreTimeline (TimelineSnapshot target, const TimelineSnapshot& replaced);
    void commitTimelineCut (const juce::String& label, const TimelineSnapshot& before,
                            const zynforge::timelinecut::Cut&, const std::vector<int>& tracks);
    void finishTimelineEdit (const juce::String& label, const TimelineSnapshot& before);
    int  rippleTimeline (const juce::String& label, const std::vector<int>& tracks,
                         juce::int64 start, juce::int64 end);
    int  rippleSpans (const juce::String& label, const std::vector<zynforge::clipedit::RippleSpan>&);
    void editUndo();
    void editRedo();
    // Confirmation gate for destructive Delete-key edits in EDIT (removing a
    // recorded clip / range). Shows a warning; runs onConfirm only on "Delete".
    void confirmDeleteRecording (bool ripple, std::function<void()> onConfirm);
    // Test seam: receives what either delete prompt would run on "Delete".
    std::function<void (std::function<void()>)> confirmForTests;

    // Confirmation gate for deleting the selected channel(s) from the EDIT
    // view (right-click ▸ Delete Channel, or Delete on a selected channel).
    // Recorded files stay on disk; runs onConfirm only on "Delete".
    void confirmDeleteChannels (std::function<void()> onConfirm);
    void confirmDeleteChannels (int count, std::function<void()> onConfirm);

    // Coalesced mixer undo. The everyday strip mutations (fader, pan, mute,
    // solo, arm, monitor, rename, recolour, routing) have no dedicated undo
    // wiring, so a timer poll snapshots the whole mixer and pushes ONE undo
    // step once a change settles (~300 ms) -- so a fader drag is a single
    // step, not one per pixel. Recording is deliberately NOT undoable: while
    // armed/rolling the poll re-baselines instead of recording arm churn.
    juce::var    captureMixerSnapshot();
    void         pollMixerUndo();
    void         rebaselineMixerUndo();
    juce::var    committedMixerState;       // last state captured as an undo baseline
    juce::uint64 committedMixerFp  { 0 };   // fingerprint of committedMixerState (MixerSnapshot.h)
    int          committedMixerCount { -1 };// strip count at the baseline (structural changes re-baseline)
    juce::uint64 lastSeenMixerFp   { 0 };   // previous tick's fingerprint (settle detection)
    int          mixerChangeStableTicks { 0 };
    // Free-space readout cache: timerCallback re-queries the session volume
    // at most every 2 s (or when the session directory changes).
    juce::File   diskQueryDir;
    juce::uint32 lastDiskQueryMs { 0 };
    double       cachedFreeGB { 0.0 };
    bool         mixerUndoReady { false };  // baseline established yet?
    void editCutSelected (bool cut);
    void editPasteSelected();
    void editSoloSelection();
    // Option+R bulk record-arm toggle over the current strip selection.
    void armSelection();
    void editCropToLoopRange();
    void consolidateTracks (const std::vector<int>& targets, juce::int64 start, juce::int64 end);
    void editSetRangeToLoopRange();
    void editToggleSnap();
    void editSplitAtPlayhead();
    void editSeparateAtSelection();   // B -- isolate the range as its own clip
    void editHealSeparation();        // Cmd+H -- rejoin a clean split
    void editZoomToSelection();       // E -- zoom the EDIT view to the range
    void editConsolidateSelection();  // flatten the range to one new file
    void editClearRange();            // Delete -- clear audio inside the range
    void editDeleteSelectedClip();    // Delete -- remove the clicked clip(s)
    void editDuplicateSelectedClip(); // D -- copy the clicked clip after itself
    void editNudgeSelectedClip (int dir);   // Alt+Left/Right / numpad -- slide
    void editRippleDelete();          // Shift+Delete -- delete + close the gap
    void editCycleNudgeValue();       // N -- cycle the nudge step, persisted
    // The selected clips grown to their stereo halves and edit-group peers.
    std::vector<zynforge::EditPage::ClipRef> selectedClipTargets();
    void editClipboardCut (bool cut); // Cmd+X / Cmd+C -- clip or strip settings
    void editClipboardPaste();        // Cmd+V -- clip at playhead or strip settings
    std::vector<int> tracksToEditPhysical();   // selected strips, else all
    void editStartRange();
    void editFinishRange();

    // Pro Tools-style marker drop + naming. dropMarkerAndPromptName
    // places the marker, picks the next default name ("Marker N+1"),
    // then opens the rename dialog. Cancel keeps the default name.
    void dropMarkerAndPromptName();

    // The ONE gate for timeline mutations (clips, ranges, markers,
    // undo/redo). Refuses -- with the reason on the status
    // line -- while capture rolls (local or daemon), while a session
    // operation owns persistence (sessionIoBusy: copy/move/bounce or a
    // pending metadata save), and while LOCKed. The key handler, menu and
    // editor enablement also gate these, but the edit functions are
    // reachable from more surfaces than any one of them covers, so every
    // mutation checks here rather than repeating its own partial guard.
    // `action` completes "Stop recording before ..." (e.g. "cropping").
    bool canMutateTimeline (const juce::String& action = "editing");

    double pendingSampleRate { 48000.0 };
    // Last device sample rate we warned about (mismatch vs the session rate);
    // 0 = no outstanding warning, so we warn once per distinct device rate.
    double srMismatchWarned  { 0.0 };
    void   checkDeviceSampleRate (double deviceSampleRate);
    bool   chaseWasLive      { false };   // external-timecode chase: did we start playback?
    void   serviceTimecodeChase();
    void   serviceTrimFollowNotice();
    void   serviceBusRoutingNotice();
    void   promptStreamOutputs();
    bool   chaseParked { false };   // timecode is outside the session; chase is not following
    juce::String lastTrimFollowNotice;
    juce::String lastBusRoutingNotice;
    void updateTransportLabels();
    juce::File makeNewSessionDir() const;   // none when Local Storage's drive is missing
    juce::String missingLocalStorage() const;
    juce::File getSessionsRoot() const;
    void timerCallback() override;
    // timerCallback's steps (MainComponentTimer.cpp), in call order.
    void serviceCaptureDaemonLink();
    void syncFloatingPanelButtons();
    void rebuildStripsIfLayoutChanged();
    void updateStatusPills();
    void updatePunchPill();
    void updateSoundcheckState();
    juce::int64 takeClockSamples();
    double      takeClockRate (double deviceRate);
    double      shownTakeRate { 48000.0 };   // asked once a tick, by the big clock; a daemon take asks the helper
    void restoreOutputPatch();
    juce::String vscReachText (int moved);
    void updateNextCueCountdown();
    void syncStripsWithEngine();
    int  stripsInputs { -1 }, stripsOutputs { -1 };   // the device the strips' combos were filled for
    void surfaceAsyncReportFailure();
    void updateClockAndDiskCadence (double deviceSR);
    void updateHeaderFromStatus (const zynforge::EngineStatus& status, bool playing);
    void updateCaptureWarnings (const zynforge::EngineStatus& status);
    void updateBackupButton (const zynforge::EngineStatus& status);
    bool deviceLossLatched { false };     // as of the last status
    bool deviceLossForgotten { false };   // that loss belonged to a session that has gone
    std::unique_ptr<zynforge::VolumeProbe> backupProbe;
    juce::File backupProbeFolder;
    // The loud row: every standing alarm, and a refused RECORD until it times out.
    void refreshAlarmBanner();
    void collectTwinAlarms (const zynforge::twin::Snapshot&, std::vector<zynforge::AlarmBanner::Alarm>&);
    bool twinPeerRolling { false };   // as last heard over the link
    void showRecordRefusal (const juce::String& reason);
    // Open a modal warning if the loaded session's SR doesn't match the device.
    void warnIfSampleRateMismatch();

    // Session templates -- engineer's per-strip layout reusable across shows.
    // The menu's id range for them, 200-249. Ids past it are other commands.
    static constexpr int kMenuTemplateSlots = 50;
    juce::File  templatesDir() const;
    juce::File  templatesDirForTests;
    juce::Array<juce::File> listSessionTemplates() const;
    void  promptSaveSessionTemplate();
    void  applySessionTemplate (const juce::File& templateFile);
    juce::String sessionTemplateJson (const juce::String& name);
    void  promptDeleteSessionTemplate();
    // Default template: File ▸ New Session pre-applies it after creating
    // the session folder so the engineer starts with their preferred
    // strip count / names / colours / routings. Empty File = no default.
    juce::File  getDefaultTemplate() const;
    void        setDefaultTemplate (const juce::File& templateFile);

    // Setlist printing -- writes setlist.html into the active session
    // dir and opens it in the default browser (engineer prints from
    // there, or saves as PDF via the browser's print dialog).
    void  printSetlist();
    // Restore UI layout (view / strip width / EDIT zoom)
    // from the active session's .zfproj. Called after loadSession so
    // the engineer reopens their show with their layout intact.
    void  loadUILayoutFromActiveSession();
    // Show pre-flight checklist -- one-screen view of every show-critical
    // setting (device, SR, inputs, armed, backup, disk, cues, recording state).
    void  showPreflightChecklist();
    std::function<void()> preflightStallForTests;   // runs where a dead drive would block
    zynforge::PreflightRow deskPatchRow() const;
    zynforge::PreflightRow deskLinkRow() const;
    juce::String backupChecklistRow();

    zynforge::ZynForgeLookAndFeel laf;
    zynforge::AudioEngine         engine;

    // These objects retain callbacks into `engine`, so declaration order is
    // safety-critical: construct them after the engine and destroy them before it.
    // X32/M32 console link: soundcheck repatch + head-amp gain capture.
    zynforge::ConsoleLink consoleLink;
    // Capture-process split, Phase 1d: out-of-process recording behind a
    // flag (Session menu, id 958; appProps "useCaptureDaemon", default
    // OFF until the 64-ch hardware soak passes). When active, RECORD
    // routes through the daemon; playback/monitoring stay in-process.
    zynforge::capture::CaptureSupervisor captureSupervisor;
    bool useCaptureDaemon { false };
    static constexpr int kCaptureDaemonPort = 17890;
    static constexpr const char* kDaemonGoneMidTake =
        "CAPTURE DAEMON GONE MID-TAKE -- the take ended with it; audio up to its last header write is on disk";
    int  captureDaemonPort { kCaptureDaemonPort };
    bool punchInWaiting { false };       // a save is holding up a PUNCH-mode punch-in
    bool daemonLinkLost { false };       // reported lost, not yet resolved
    bool daemonTakeAdopted { false };    // this window knows the rolling take
    // RECORD got no reply. The take may be rolling, so the guard is up until a
    // fresh status settles it.
    juce::uint32 daemonStartUnansweredMs { 0 };
    juce::uint32 nextDaemonProbeMs { 0 }, daemonProbeDelayMs { 1000 };
    // Off by default; Session > Network & Surfaces > Twin Recorder. Destroyed
    // before everything above, so its threads never outlive what the host uses.
    zynforge::twin::TwinLink twinLink { *this };
    bool twinStartInFlight { false };   // this RECORD arrived over the twin link
    bool twinStopFollowsDaemon { false };  // a confirmed STOP waits on the daemon's reply
    bool remoteStartInFlight { false }; // this RECORD arrived from any remote surface
    bool remotePunchRefused { false };  // and it was refused because it would punch
    juce::String remoteDeviceRefusal;   // or because nobody there can answer for the device

    friend class zynforge::CaptureDaemonLinkTests;
    friend class zynforge::CapturePathTests;
    friend class zynforge::MainTransportRegressionTests;
    friend class zynforge::TwinLinkHostTests;
    friend class zynforge::PunchSessionGuardTests;
    friend class zynforge::OctoberRegressionTests;
    friend class zynforge::ReviewPersistenceRepairTests;
    friend class zynforge::ReviewUiFormatRepairTests;
    friend class zynforge::ConsoleVscTests;
    friend class zynforge::NetworkStateTests;
    friend class zynforge::SongExportTests;
    friend class zynforge::SessionInterchangeTests;
    friend class zynforge::SessionSafetyTests;
    friend class zynforge::SessionFixTests;
    friend class zynforge::MainWindowGuardTests;
    friend class zynforge::MainWindowFixTests;
    friend class zynforge::EngineFixTests;
    friend class zynforge::EditIntegrityTests;
    friend class zynforge::PunchRuleTests;
    friend class zynforge::EditDetailTests;
    friend class zynforge::EditRowFixTests;
    friend struct zynforge::ConcurrencyAuditAccess;

    // Live SafePointers to dialog windows opened by the header buttons.
    // A second click on the launching button closes the dialog instead of
    // opening another instance.
    juce::Component::SafePointer<juce::DialogWindow> deviceDialog;
    juce::Component::SafePointer<juce::DialogWindow> patchDialog;
    juce::Component::SafePointer<juce::DialogWindow> metersDialog;
    juce::Component::SafePointer<juce::DialogWindow> twinDialog;

    juce::Label  titleLabel        { {}, "ZYNFORGE  RECORDING" };
    juce::Label  statusLabel       { {}, "Idle" };
    juce::String activeCaptureWarning;
    zynforge::AlarmBanner alarmBanner;
    juce::String recordRefusal;              // shown in the banner, then clears itself
    juce::uint32 recordRefusalShownMs { 0 };
    juce::Label  nextCueLabel      { {}, "" };
    juce::Label  danteLabel        { {}, "" };
    juce::String sampleRateWarning;              // the device disagrees with the session; shown in the banner
    juce::Label  twinStatusLabel   { {}, "" };   // link state; empty while the link is off
    juce::Label  punchPill         { {}, "" };   // PUNCH mode, or what the next RECORD will replace
    juce::Label  soundcheckPill    { {}, "DESK ON SOUNDCHECK" };   // the desk is hearing card returns, not the stage
    juce::Label  sessionLabel      { {}, "No session loaded" };
    juce::Label  transportLabel    { {}, "00:00 / 00:00" };
    juce::Label  deviceLabel;        // the input a take would record from
    // Header chrome -- IconButton paints a glyph next to each label so
    // the chrome reads as icons + words instead of monospace SHOUTING.
    zynforge::IconButton deviceButton  { zynforge::icons::Glyph::Device, "DEVICE" };
    zynforge::IconButton lockButton    { zynforge::icons::Glyph::Lock,   "LOCK" };
    zynforge::IconButton backupButton  { zynforge::icons::Glyph::Disk,   "BACKUP" };
    zynforge::IconButton patchButton   { zynforge::icons::Glyph::Patch,  "PATCH" };
    zynforge::IconButton vscButton     { zynforge::icons::Glyph::Vsc,    "VSC" };
    zynforge::IconButton metersButton  { zynforge::icons::Glyph::Meters, "METERS" };
    zynforge::IconButton addChannelButton { zynforge::icons::Glyph::Plus, "CH" };
    zynforge::IconButton mixViewButton    { zynforge::icons::Glyph::Mix,  "MIXER" };
    zynforge::IconButton editViewButton   { zynforge::icons::Glyph::Edit, "EDIT" };

    enum class View { Mix, Edit };
    View currentView { View::Mix };
    void switchView (View v);

    // XS=24 strips/page, S=16, M=12 (default), L=8. Persists in appProps
    // so the engineer's preferred density survives restarts.
    enum class StripWidth { XS, S, M, L };
    StripWidth stripWidthPreset { StripWidth::M };
    void setStripWidthPreset (StripWidth);
    juce::TextButton stripXsButton { "XS" };
    juce::TextButton stripSButton  { "S"  };
    juce::TextButton stripMButton  { "M"  };
    juce::TextButton stripLButton  { "L"  };
    // Grid / table view: lay the strips out 12-per-row in 2 visible rows
    // (24 faders on one page, scroll vertically for more) instead of a
    // single horizontal row. Overrides the width preset while active.
    juce::TextButton gridButton    { "GRID" };
    bool             mixerGridView { false };

    std::atomic<bool> sessionLocked { false };
    bool explicitDocumentOpened { false };
    bool uiLayoutSavePending { false };   // a layout change arrived during a session job

    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<juce::ChangeListener> batchColourListenerHandle;

    zynforge::BigClockPanel bigClock;
    int recordDriveWaitMs { 10000 };
    zynforge::VolumeProbe::Reader recordDriveReaderForTests;
    zynforge::PerfDashboard perfDashboard;
    zynforge::SetlistBar    setlistBar;
    zynforge::Toast         toast;

    // STOP-while-recording two-tap guard. Tracks the message-counter
    // timestamp of the first arming tap; second tap within 2 s fires
    // the stop. Zero = not armed.
    juce::uint32 stopArmedAtMs { 0 };
    // A manual RECORD into an existing take is a short punch, not a new live
    // take: its next RECORD/STOP press should punch out in one action.
    bool manualPunchActive { false };
    bool cursorPunchAnswered { false };   // true only around the press that "Replace From Cursor" makes
    juce::Component::SafePointer<juce::AlertWindow> cursorPunchQuestion;
    QuestionScope cursorPunchScope;
    juce::Component::SafePointer<juce::AlertWindow> deviceRecordQuestion;
    zynforge::devicechoice::ReopenPacing deviceReopenPacing;
    juce::uint32 nextDeviceCheckMs { 0 };
    juce::String deviceLineShown;    // the device trouble already on the status line

    // Per-session cue list -- populated from <SessionName>.zfproj on
    // every session swap, persisted on every add / pick / update.
    std::vector<zynforge::SetlistBar::Cue> cues;
    int currentCueIndex { -1 };

    void loadSetlistFromActiveSession();
    bool saveSetlistToActiveSession (bool writeBackup = true) const;
    void jumpToCue (int index);
    void addCueAtTransport();
    double timelineRate();
    void updateCueAtTransport();
    void renameCurrentCue();
    int  lastExportFailures { 0 }; // tracks failed in the last exportTracksTo (partial-failure warning)
    bool songExportRunning { false };   // message thread; turns menu item 967 into its cancel

    // Session-I/O reservation: true while a session operation owns the
    // session's files -- a background job below, or pending metadata saves
    // (which count themselves in pendingMetadataSaves). Gates recording,
    // playback, timeline/strip mutations and competing transitions.
    std::atomic<bool> sessionIoBusy { false };

    // Every unbounded session-file job (track export, stem/mix bounce,
    // import, Save As copy, relocation, show handoff) runs on
    // this ONE owned worker, never on the message thread and never detached.
    // It releases sessionIoBusy on every exit path before the job's
    // completion runs, and the destructor cancels + joins it while the engine
    // (declared above) is still alive. Declared after sessionIoBusy, which it
    // references.
    zynforge::SessionJobRunner sessionJobs { sessionIoBusy };
    // QC and song detection: owned and joined like a session
    // job, but they only read, so they hold no session reservation. RECORD must
    // never wait for one; the 10 Hz timer stops it when a take starts.
    std::atomic<bool> analysisBusy { false };
    zynforge::SessionJobRunner analysisJobs { analysisBusy };
    void promptCueName (const juce::String& title,
                        const juce::String& initial,
                        std::function<void (const juce::String&)> onAccept);
    std::unique_ptr<zynforge::TransportBar>  transportBar;

    juce::Viewport  stripsViewport;
    juce::Component stripsContainer;
    zynforge::PlaceholderView mixerPlaceholder;   // MIXER empty-state overlay
    std::unique_ptr<zynforge::EditPage>     editPage;
    std::unique_ptr<zynforge::MasterStrip>  masterStrip;

    juce::TooltipWindow tooltipWindow { this, 500 };

    std::vector<std::unique_ptr<zynforge::ChannelStrip>> strips;
    int  lastTrackCount { -1 };
    int  lastTrackGen   { -1 };        // recorder track-set generation last seen (rebuild on change)
    bool asyncReportFailureShown { false };
    // macOS keeps the native menu's enabled/greyed states cached until
    // menuItemsChanged() is called. We poll a cheap signature of every bit of
    // state that drives a menu item's enablement and refresh only when it
    // changes -- otherwise Undo/Redo/Edit/Export stayed frozen in the empty
    // launch state forever (everything greyed even after a session loaded).
    juce::String lastMenuStateSig;
    void refreshMenuStateIfChanged();
    bool rebuildingStrips { false };   // re-entrancy guard for the resized() stale-strip rebuild

    // Timer-tick counter for cheap "every N frames" gates (e.g. the
    // free-space pre-flight refresh, which queries the volume only
    // every ~2 s rather than at the full timer rate).
    int  diskTick { 0 };

    // Tracks the recording state the header was last laid out for, so the
    // perf dashboard can grow/shrink (resized()) only when REC toggles.
    bool dashWasRecording { false };

    // Snap Separate/split cuts to the nearest audio zero crossing (±5 ms) so
    // hard cuts are click-free. Toggled from the Edit menu.
    bool zeroCrossSnap { true };

    // SMART status cache. `diskutil info` is a blocking subprocess call
    // (50-200 ms typical, unbounded on an unresponsive volume), so it runs on a
    // BACKGROUND thread and publishes here; the timer only ever reads these
    // atomics. Running it inline on the message thread froze the whole UI
    // mid-take whenever the drive was slow to answer. -1 = unset,
    // 0 = Verified, 1 = Failing, 2 = Unknown.
    std::atomic<int>  smartPrimaryStatus { -1 };
    std::atomic<int>  smartBackupStatus  { -1 };
    // At most one SMART probe in flight -- the 30 s poll must not stack up
    // background queries when a drive is answering slowly.
    std::atomic<bool> smartQueryInFlight { false };
    void pollSmartStatusAsync();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
