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
#include "PerfDashboard.h"
#include "SetlistBar.h"
#include "TempoBar.h"
#include "TimelineStrip.h"
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

    // MenuBarModel — populates the macOS system menu bar.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

private:
    void rebuildStrips();
    void onRecordClicked();
    void onDeviceClicked();
    void onLoadSessionClicked();
    void onPlayClicked();
    void onStopClicked();
    void onFileMenuClicked();
    void onSaveSessionState();
    void onSaveSessionAs();
    void onExportAllTracks();
    void onExportIndividualTrack (int channelIndex);
    void onImportAudioFiles();
    void onLockToggled();
    void onBackupClicked();
    void onVscClicked();
    void applyLockState();
    void offerSessionRecovery();
    void showStartupWelcome();
    void launchNewSessionDialog();
    juce::File createSessionFolderStructure (const zynforge::NewSessionDialog::Result&);
    bool saveSessionStateTo (const juce::File& dir);
    int  exportTracksTo (const juce::File& dir,
                         const std::vector<int>& channelIndices,
                         const zynforge::ExportOptions&);
    void showStatus (const juce::String& msg);
    void confirmAndQuit();
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
    int  physicalFromLogicalIdx (int logical);

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

    bool snapToMarkers { false };

    // Edit-menu plumbing.
    juce::UndoManager undoManager;
    juce::var         stripClipboard;   // JSON of cut/copied strip settings
    void recordUndoSnapshot (const juce::String& label);
    void restoreUndoSnapshot (const juce::var& snapshot);
    void editUndo();
    void editRedo();
    void editCutSelected (bool cut);
    void editPasteSelected();
    void editSoloSelection();
    void editCropToLoopRange();
    void editSetRangeToLoopRange();
    void editToggleSnap();
    void editSplitAtPlayhead();
    void editStartRange();
    void editFinishRange();

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

    // Session templates — engineer's per-strip layout reusable across shows.
    juce::File  templatesDir() const;
    juce::Array<juce::File> listSessionTemplates() const;
    void  promptSaveSessionTemplate();
    void  applySessionTemplate (const juce::File& templateFile);
    void  promptDeleteSessionTemplate();

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
    juce::Label  sessionLabel      { {}, "No session loaded" };
    juce::Label  transportLabel    { {}, "00:00 / 00:00" };
    // Header chrome — IconButton paints a glyph next to each label so
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
    zynforge::AutomationToolbar automationToolbar;

    // Per-session cue list — populated from <SessionName>.zfproj on
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
    std::unique_ptr<zynforge::EditPage>     editPage;
    std::unique_ptr<zynforge::MasterStrip>  masterStrip;
    std::unique_ptr<zynforge::VcaPanel>     vcaPanel;
    zynforge::IconButton vcaToggleButton  { zynforge::icons::Glyph::Mix, "VCA" };
    bool                 showVcaPanel { false };

    juce::TooltipWindow tooltipWindow { this, 500 };

    std::vector<std::unique_ptr<zynforge::ChannelStrip>> strips;
    int  lastTrackCount { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
