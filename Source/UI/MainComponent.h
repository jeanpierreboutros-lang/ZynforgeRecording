#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/TrackExporter.h"
#include "../Theme/ZynForgeLookAndFeel.h"
#include "BigClockPanel.h"
#include "ChannelStrip.h"
#include "EditPage.h"
#include "ExportDialog.h"
#include "MasterStrip.h"
#include "NewSessionDialog.h"
#include "PerfDashboard.h"
#include "PhaseMeter.h"
#include "TimelineStrip.h"
#include "TransportBar.h"

#include <memory>
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
    void soloSelection();
    void setRangeToLoopRange();
    void toggleSnap();
    void splitSeparate();
    void startRange();
    void finishRange();
    void removeLastCapture();
    void showBatchRenameDialog();
    void showBatchColourDialog();
    void showSessionProperties();

    bool snapToMarkers { false };

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

    zynforge::ZynForgeLookAndFeel laf;
    zynforge::AudioEngine         engine;

    juce::Label  titleLabel        { {}, "ZYNFORGE  RECORDING" };
    juce::Label  statusLabel       { {}, "Idle" };
    juce::Label  sessionLabel      { {}, "No session loaded" };
    juce::Label  transportLabel    { {}, "00:00 / 00:00" };
    juce::TextButton recordButton  { "RECORD" };
    juce::TextButton deviceButton  { "AUDIO DEVICE" };
    juce::TextButton loadButton    { "FILE" };
    juce::TextButton playButton    { "PLAY" };
    juce::TextButton stopButton    { "STOP" };
    juce::TextButton formatButton  { "WAV 24" };
    juce::TextButton preRollButton { "PRE 0s" };
    juce::TextButton lockButton    { "LOCK" };
    juce::TextButton backupButton  { "BACKUP" };
    juce::TextButton patchButton   { "PATCH" };
    juce::TextButton vscButton     { "VSC" };
    juce::TextButton metersButton  { "METERS" };
    juce::TextButton oscButton     { "OSC" };
    juce::TextButton addChannelButton { "+ CH" };
    juce::TextButton mixViewButton    { "MIXER" };
    juce::TextButton editViewButton   { "EDIT" };

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
    std::unique_ptr<zynforge::PhaseMeter>    phaseMeter;
    std::unique_ptr<zynforge::TimelineStrip> timeline;
    std::unique_ptr<zynforge::TransportBar>  transportBar;

    juce::Viewport  stripsViewport;
    juce::Component stripsContainer;
    std::unique_ptr<zynforge::EditPage>     editPage;
    std::unique_ptr<zynforge::MasterStrip>  masterStrip;

    juce::TooltipWindow tooltipWindow { this, 500 };

    std::vector<std::unique_ptr<zynforge::ChannelStrip>> strips;
    int  lastTrackCount { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
