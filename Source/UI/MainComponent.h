#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/TrackExporter.h"
#include "../Network/SessionMirror.h"
#include "../Theme/ZynForgeLookAndFeel.h"
#include "AutomationToolbar.h"
#include "BigClockPanel.h"
#include "IconButton.h"
#include "ChannelStrip.h"
#include "EditPage.h"
#include "ExportDialog.h"
#include "MasterStrip.h"
#include "NewSessionDialog.h"
#include "WelcomeDialog.h"
#include "PerfDashboard.h"
#include "SetlistBar.h"
#include "PeakTally.h"
#include "TempoBar.h"
#include "TimelineStrip.h"
#include "Toast.h"
#include "PlaceholderView.h"
#include "VcaPanel.h"
#include "TransportBar.h"

#include <memory>
#include <set>
#include <vector>

namespace zynforge { class ChannelStrip; }

class MainComponent final : public juce::Component,
                            public juce::KeyListener,
                            public juce::MenuBarModel,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool keyPressed (const juce::KeyPress&, juce::Component*) override;

    // MenuBarModel -- populates the macOS system menu bar.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

private:
    void rebuildStrips();
    void updateMixerPlaceholder();   // show/hide the MIXER empty-state overlay
    void onRecordClicked();
    void onDeviceClicked();
    void onLoadSessionClicked();
    void onPlayClicked();
    void onStopClicked();
    void onFileMenuClicked();
    void onSaveSessionState();
    void onSaveSessionAs();
    void onExportAllTracks();
    void onBounceStems();      // render the edited clip arrangement to flat stems
    void onBounceStereoMix();  // render the edited arrangement summed to stereo
    void onExportIndividualTrack (int channelIndex);
    void onImportAudioFiles();
    void onLockToggled();
    void onBackupClicked();
    void onVscClicked();
    void applyLockState();
    void offerSessionRecovery();
    void showStartupWelcome();
    void showKeyboardShortcuts();
    void showAboutDialog();
    void showFirstRunTutorial();
    void showUserGuide();
    void launchNewSessionDialog();
    juce::File createSessionFolderStructure (const zynforge::NewSessionDialog::Result&);
    // Canonical "open this session folder" sequence: pin the active dir,
    // restore setlist + UI layout, load the audio, restore the full mixer
    // state, force a strip rebuild. Returns the track count loaded. Used by
    // File > Open, the Welcome dialog, and the launch auto-reopen.
    int  openSessionFolder (const juce::File& dir);
    bool saveSessionStateTo (const juce::File& dir);
    int  exportTracksTo (const juce::File& dir,
                         const std::vector<int>& channelIndices,
                         const zynforge::ExportOptions&);
    void showStatus (const juce::String& msg);
public:
    // Called from Main.cpp's systemRequestedQuit so an unsaved
    // session / running recording prompts BEFORE the app shuts down.
    void confirmAndQuit();
private:
    void applySessionSettings();

    // Edit menu actions.
    // Old Edit-menu stubs were renamed to edit*; declarations live in
    // the public-ish private section above (editUndo / editCutSelected
    // / editSoloSelection / editSetRangeToLoopRange / editToggleSnap /
    // editSplitAtPlayhead / editStartRange / editFinishRange).
    void removeLastCapture();
    void showBatchRenameDialog();
    void showBatchColourDialog();
    void showSelectionMenu();
    void deleteSelectedStrips();
    void colourSelectedStrips();
    void moveSelectedStrips (int delta);
    void clearStripSelection();
    void selectAllStrips();
    int  physicalFromLogicalIdx (int logical);
    int  logicalFromPhysicalIdx (int physical);

    // Multi-select: indices are LOGICAL strip indices (stereo pairs
    // counted as one). Engineer toggles via shift/cmd-click on a strip.
    std::set<int> selectedLogical;
    void showSessionProperties();
    void runSpectralAutoName();
    void writeSoundcheckReport();
    void runNoiseAnalysis();
    void promptMirrorHost();
    zynforge::SessionMirror sessionMirror { engine };
    void togglePunchMode();
    void servicePunch();        // called each timerCallback tick
    bool wasInsidePunch { false };

    // Snap modes -- cycled by the '4' key (also surfaced in the
    // Edit menu). Off / Markers / Bars; Bars uses the engine's
    // tempo map + time-sig to align edits to bar boundaries. The
    // bool name is kept for source compatibility with code that
    // checks "is snap on at all"; new code should branch on
    // snapMode for the actual snap behaviour.
    enum class SnapMode { Off = 0, Markers = 1 };
    SnapMode snapMode      { SnapMode::Off };
    bool     snapToMarkers { false };   // mirrors (snapMode == Markers) for legacy reads

    // Edit-menu plumbing.
    juce::UndoManager undoManager;
    juce::var         stripClipboard;   // JSON of cut/copied strip settings
    juce::var         clipClipboard;    // JSON of cut/copied audio clip(s)
    int               nudgeMs { 100 };  // clip-nudge step; cycled with N, persisted
    void recordUndoSnapshot (const juce::String& label);
    // Clip/playlist undo: capture engine.playlistsToJson() before the edit,
    // pass it here after; records an undoable step only if clips changed.
    void pushClipUndo (const juce::String& label, const juce::var& before);
    void restoreUndoSnapshot (const juce::var& snapshot);
    void editUndo();
    void editRedo();
    // Confirmation gate for destructive Delete-key edits in EDIT (removing a
    // recorded clip / range). Shows a warning; runs onConfirm only on "Delete".
    void confirmDeleteRecording (bool ripple, std::function<void()> onConfirm);

    // Confirmation gate for deleting the selected channel(s) from the EDIT
    // view (right-click ▸ Delete Channel, or Delete on a selected channel).
    // Recorded files stay on disk; runs onConfirm only on "Delete".
    void confirmDeleteChannels (std::function<void()> onConfirm);

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
    juce::String committedMixerSig;         // JSON of committedMixerState (fast compare)
    juce::String lastSeenMixerSig;          // previous tick's state (settle detection)
    int          mixerChangeStableTicks { 0 };
    bool         mixerUndoReady { false };  // baseline established yet?
    void editCutSelected (bool cut);
    void editPasteSelected();
    void editSoloSelection();
    // Option+R bulk record-arm toggle over the current strip selection.
    void armSelection();
    void editCropToLoopRange();
    void editSetRangeToLoopRange();
    void editToggleSnap();
    void editSplitAtPlayhead();
    void editSeparateAtSelection();   // B -- isolate the range as its own clip
    void editHealSeparation();        // Cmd+H -- rejoin a clean split
    void editZoomToSelection();       // E -- zoom the EDIT view to the range
    void editStripSilence();          // separate clips around silence
    void editConsolidateSelection();  // flatten the range to one new file
    void editClearRange();            // Delete -- clear audio inside the range
    void editDeleteSelectedClip();    // Delete -- remove the clicked clip(s)
    void editDuplicateSelectedClip(); // D -- copy the clicked clip after itself
    void editNudgeSelectedClip (int dir);   // Alt+Left/Right / numpad -- slide
    void editRippleDelete();          // Shift+Delete -- delete + close the gap
    void editCycleNudgeValue();       // N -- cycle the nudge step, persisted
    void editClipboardCut (bool cut); // Cmd+X / Cmd+C -- clip or strip settings
    void editClipboardPaste();        // Cmd+V -- clip at playhead or strip settings
    std::vector<int> tracksToEditPhysical();   // selected strips, else all
    void editStartRange();
    void editFinishRange();

    // Pro Tools-style marker drop + naming. dropMarkerAndPromptName
    // places the marker, picks the next default name ("Marker N+1"),
    // then opens the rename dialog. Cancel keeps the default name.
    // showMarkersDialog opens the standalone Memory Locations table.
    void dropMarkerAndPromptName();
    void showMarkersDialog();

    // EditPage calls this to bracket an automation edit. The lambda
    // performs the mutation; the helper snapshots automationData
    // before and after, pushes a single AutomationSnapshotAction so
    // Cmd+Z reverts cleanly.
    void runAutomationEdit (const juce::String& label,
                            std::function<void()> mutate);

    // Drag-coalesce variant. beginAutomationTransaction snapshots
    // the engine's current automation lanes; endAutomationTransaction
    // snapshots again and pushes ONE AutomationSnapshotAction so a
    // multi-tick mouseDrag becomes a single undo step.
    void beginAutomationTransaction();
    void endAutomationTransaction (const juce::String& label);

    int    pendingContainer  { 0 };       // 0 = WAV, 1 = AIFF, 2 = FLAC
    int    pendingBitDepth   { 24 };      // 16 / 24 / 32 (32 = float)
    double pendingSampleRate { 48000.0 };
    void onFormatClicked();
    void onPreRollClicked();
    void refreshFormatButton();
    void refreshPreRollButton();
    void updateTransportLabels();
    juce::File makeNewSessionDir() const;
    juce::File getSessionsRoot() const;
    void timerCallback() override;
    // Open a modal warning if the loaded session's SR doesn't match the device.
    void warnIfSampleRateMismatch();

    // Session templates -- engineer's per-strip layout reusable across shows.
    juce::File  templatesDir() const;
    juce::Array<juce::File> listSessionTemplates() const;
    void  promptSaveSessionTemplate();
    void  applySessionTemplate (const juce::File& templateFile);
    void  promptDeleteSessionTemplate();
    // Default template: File ▸ New Session pre-applies it after creating
    // the session folder so the engineer starts with their preferred
    // strip count / names / colours / routings. Empty File = no default.
    juce::File  getDefaultTemplate() const;
    void        setDefaultTemplate (const juce::File& templateFile);

    // Push UI layout (view / strip width / VCA panel / EDIT zoom) into
    // the active session's .zfproj. No-op when no session is active.
    // Called on every layout change so reopening the show restores the
    // exact workspace the engineer left.
    void  saveUILayoutToActiveSession();

    // Setlist printing -- writes setlist.html into the active session
    // dir and opens it in the default browser (engineer prints from
    // there, or saves as PDF via the browser's print dialog).
    void  printSetlist();
    // Restore UI layout (view / strip width / VCA panel / EDIT zoom)
    // from the active session's .zfproj. Called after loadSession so
    // the engineer reopens their show with their layout intact.
    void  loadUILayoutFromActiveSession();
    // Show pre-flight checklist -- one-screen view of every show-critical
    // setting (device, SR, inputs, armed, backup, disk, cues, recording state).
    void  showPreflightChecklist();

    zynforge::ZynForgeLookAndFeel laf;
    zynforge::AudioEngine         engine;

    // Live SafePointers to dialog windows opened by the header buttons.
    // A second click on the launching button closes the dialog instead of
    // opening another instance.
    juce::Component::SafePointer<juce::DialogWindow> deviceDialog;
    juce::Component::SafePointer<juce::DialogWindow> patchDialog;
    juce::Component::SafePointer<juce::DialogWindow> metersDialog;

    juce::Label  titleLabel        { {}, "ZYNFORGE  RECORDING" };
    juce::Label  statusLabel       { {}, "Idle" };
    juce::Label  midiStatusLabel   { {}, "" };
    juce::Label  nextCueLabel      { {}, "" };
    juce::Label  danteLabel        { {}, "" };
    juce::Label  sessionLabel      { {}, "No session loaded" };
    juce::Label  transportLabel    { {}, "00:00 / 00:00" };
    // Header chrome -- IconButton paints a glyph next to each label so
    // the chrome reads as icons + words instead of monospace SHOUTING.
    zynforge::IconButton recordButton  { zynforge::icons::Glyph::Record, "RECORD" };
    zynforge::IconButton deviceButton  { zynforge::icons::Glyph::Device, "DEVICE" };
    zynforge::IconButton loadButton    { zynforge::icons::Glyph::File,   "FILE" };
    juce::TextButton     playButton    { "PLAY" };
    juce::TextButton     stopButton    { "STOP" };
    zynforge::IconButton formatButton  { zynforge::icons::Glyph::Disk,   "WAV 24" };
    juce::TextButton     preRollButton { "PRE 0s" };
    zynforge::IconButton lockButton    { zynforge::icons::Glyph::Lock,   "LOCK" };
    zynforge::IconButton backupButton  { zynforge::icons::Glyph::Disk,   "BACKUP" };
    zynforge::IconButton patchButton   { zynforge::icons::Glyph::Patch,  "PATCH" };
    zynforge::IconButton vscButton     { zynforge::icons::Glyph::Vsc,    "VSC" };
    zynforge::IconButton metersButton  { zynforge::icons::Glyph::Meters, "METERS" };
    zynforge::IconButton oscButton     { zynforge::icons::Glyph::Osc,    "OSC" };
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

    bool sessionLocked { false };

    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<juce::ChangeListener> batchColourListenerHandle;

    zynforge::BigClockPanel bigClock;
    zynforge::PerfDashboard perfDashboard;
    zynforge::SetlistBar    setlistBar;
    zynforge::TempoBar      tempoBar;
    zynforge::Toast         toast;
    std::unique_ptr<zynforge::PeakTally> peakTally;

    // STOP-while-recording two-tap guard. Tracks the message-counter
    // timestamp of the first arming tap; second tap within 2 s fires
    // the stop. Zero = not armed.
    juce::uint32 stopArmedAtMs { 0 };
    // Held during a multi-tick automation drag so endAutomationTransaction
    // can compare against the pre-drag state.
    juce::var pendingAutomationBefore;
    bool      automationTransactionOpen { false };
    zynforge::AutomationToolbar* automationToolbar { nullptr };   // owned by editPage

    // Per-session cue list -- populated from <SessionName>.zfproj on
    // every session swap, persisted on every add / pick / update.
    std::vector<zynforge::SetlistBar::Cue> cues;
    int currentCueIndex { -1 };

    // Cue-transition ramp state. Active while a Fade-mode cue is in
    // flight; updated by the existing 24 Hz timerCallback. Each strip
    // gets its own (start, target) pair and we lerp gain + pan toward
    // the target proportionally to elapsed beats.
    struct RampState
    {
        bool        active { false };
        double      startMs { 0.0 };
        double      durationMs { 0.0 };
        std::vector<std::pair<float, float>> gainStartTarget; // per-track
        std::vector<std::pair<float, float>> panStartTarget;  // per-track
    };
    RampState cueRamp;
    void startCueRampTo (const zynforge::SetlistBar::Cue&);
    void updateCueRamp();

    void loadSetlistFromActiveSession();
    void saveSetlistToActiveSession() const;
    void jumpToCue (int index);
    void addCueAtTransport();
    void updateCueAtTransport();
    void renameCurrentCue();
    // Build / refresh the metronome track. Walks the current tempo
    // (and tempoMap, if any) to lay down a click WAV in the session's
    // Audio Files/ folder. Tracks the file path so a re-press just
    // overwrites it in place rather than piling up tracks.
    void generateOrRefreshClickTrack();
    int  clickTrackIndex { -1 };   // -1 = not yet created in this session
    void promptCueName (const juce::String& title,
                        const juce::String& initial,
                        std::function<void (const juce::String&)> onAccept);
    std::unique_ptr<zynforge::TimelineStrip> timeline;
    std::unique_ptr<zynforge::TransportBar>  transportBar;

    juce::Viewport  stripsViewport;
    juce::Component stripsContainer;
    zynforge::PlaceholderView mixerPlaceholder;   // MIXER empty-state overlay
    std::unique_ptr<zynforge::EditPage>     editPage;
    std::unique_ptr<zynforge::MasterStrip>  masterStrip;
    std::unique_ptr<zynforge::VcaPanel>     vcaPanel;
    zynforge::IconButton vcaToggleButton  { zynforge::icons::Glyph::Mix, "VCA" };
    bool                 showVcaPanel { false };

    juce::TooltipWindow tooltipWindow { this, 500 };

    std::vector<std::unique_ptr<zynforge::ChannelStrip>> strips;
    int  lastTrackCount { -1 };
    int  lastTrackGen   { -1 };        // recorder track-set generation last seen (rebuild on change)
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

    // SMART status cache. Polled in a slow timer because `diskutil
    // info` is a 50-200 ms blocking subprocess call; we don't want it
    // running every 24 Hz frame. -1 = unset, 0 = Verified, 1 = Failing,
    // 2 = Unknown. Mirrors record their own slot per index.
    int  smartPrimaryStatus { -1 };
    int  smartBackupStatus  { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
