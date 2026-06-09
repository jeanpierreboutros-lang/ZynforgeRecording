#include "MainComponent.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "AddTracksDialog.h"
#include "AudioDeviceDialog.h"
#include "Meterbridge.h"
#include "NewSessionDialog.h"
#include "PatchPage.h"
#include "EditToolsBar.h"
#include "../Audio/MidiClockOut.h"
#include "../Audio/NoiseAnalyzer.h"
#include "NoiseReportDialog.h"
#include "SessionRecoveryDialog.h"
#include "MarkerListDialog.h"
#include "ClickSettingsDialog.h"
#include "../Audio/SpectralClassifier.h"
#include "SessionPropertiesDialog.h"
#include "SessionProjPath.h"

using namespace zynforge;

void MainComponent::updateMixerPlaceholder()
{
    // Only in MIX view (stripsViewport is the view's visibility proxy) and
    // only when there are no channels. The CTA reuses the Add-tracks flow.
    const bool inMix = stripsViewport.isVisible();
    const bool empty = engine.getRecorder().getNumTracks() == 0;
    const auto want = (inMix && empty) ? PlaceholderView::State::Empty
                                       : PlaceholderView::State::Hidden;
    if (mixerPlaceholder.getState() == want) return;

    if (want == PlaceholderView::State::Empty)
        mixerPlaceholder.showEmpty ("No channels yet",
            "Add tracks or open a session to start mixing.",
            "Add tracks",
            [this] { if (addChannelButton.onClick) addChannelButton.onClick(); });
    else
        mixerPlaceholder.clear();
}

void MainComponent::onRecordClicked()
{
    if (engine.isRecording())
    {
        engine.stopRecording();
        statusLabel.setText ("Idle", juce::dontSendNotification);
        recordButton.setButtonText ("RECORD");
        return;
    }

    // Pre-flight: surface why recording wouldn't capture anything BEFORE
    // we open writers + flip the engine into recording state.
    auto& recorder = engine.getRecorder();
    const int numTracks = recorder.getNumTracks();
    if (numTracks == 0)
    {
        statusLabel.setText ("Add a channel with +CH before recording.", juce::dontSendNotification);
        return;
    }

    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    if (device == nullptr)
    {
        statusLabel.setText ("No audio device open -- pick one with DEVICE.", juce::dontSendNotification);
        return;
    }
    const int numInputs = device->getActiveInputChannels().countNumberOfSetBits();
    if (numInputs == 0)
    {
        statusLabel.setText ("Device has 0 active inputs -- open DEVICE and enable inputs.",
                             juce::dontSendNotification);
        return;
    }

    // Count armed tracks AND auto-fix any strip whose inputRouting points
    // outside the device's actual input count -- fall back to a sequential
    // wrap so engineer doesn't have to open PATCH to start recording.
    int armed = 0;
    int autoRouted = 0;
    for (int i = 0; i < numTracks; ++i)
    {
        auto& t = recorder.getTrack (i);
        if (t.armed.load (std::memory_order_relaxed)) ++armed;
        const int dev = t.inputRouting.load (std::memory_order_relaxed);
        if (dev < 0 || dev >= numInputs)
        {
            const int target = numInputs > 0 ? (i % numInputs) : 0;
            engine.setTrackInputRouting (i, target);
            ++autoRouted;
        }
    }
    if (armed == 0)
    {
        statusLabel.setText ("Arm at least one track (R button) before recording.",
                             juce::dontSendNotification);
        return;
    }

    // HARD GUARD: never start a take while the device clock disagrees with
    // the session rate. A silent mismatch captures every track at the wrong
    // speed + pitch -- unrecoverable -- so we refuse to arm the writers and
    // re-surface the warning dialog. This is the app's top data-integrity
    // protection; it cannot be bypassed from the record button.
    {
        const double sessionSR = (engine.getPlayer().isLoaded()
                                  && engine.getPlayer().getSampleRate() > 0.0)
                                     ? engine.getPlayer().getSampleRate()
                                     : pendingSampleRate;
        const double devSR = device->getCurrentSampleRate();
        if (devSR > 0.0 && std::abs (devSR - sessionSR) > 1.0)
        {
            const auto khz = [] (double sr)
            {
                const double k = sr / 1000.0;
                return ((k == std::floor (k)) ? juce::String ((int) k)
                                              : juce::String (k, 1)) + " kHz";
            };
            statusLabel.setText ("Recording blocked -- device " + khz (devSR)
                                 + " != session " + khz (sessionSR)
                                 + ". Match the clock first.",
                                 juce::dontSendNotification);
            srMismatchWarned = 0.0;          // re-arm so the dialog pops again
            checkDeviceSampleRate (devSR);   // show the explanatory warning
            return;                          // <-- do NOT start recording
        }
    }

    const auto dir = makeNewSessionDir();

    // Disk-space pre-flight -- abort if the drive can't hold at least
    // 30 minutes at current SR × armed-track count × bit depth (assume
    // 24-bit if unknown). Surface the actual free GB so the engineer
    // knows what to clear.
    {
        const double sr = device->getCurrentSampleRate();
        const int    bytesPerSample = 3;   // 24-bit ≈ worst-case PCM
        const juce::int64 bytesPerSec    = (juce::int64) (sr * armed * bytesPerSample);
        const juce::int64 wantBytes      = bytesPerSec * 60 * 30;  // 30 min headroom
        const juce::int64 freeBytes      = dir.getParentDirectory().getBytesFreeOnVolume();
        if (freeBytes > 0 && freeBytes < wantBytes)
        {
            const double freeGB = freeBytes / 1.0e9;
            statusLabel.setText ("Drive has only "
                                 + juce::String (freeGB, 1)
                                 + " GB free -- < 30 min headroom. Clear space before recording.",
                                 juce::dontSendNotification);
            return;
        }
    }

    if (engine.startRecording (dir))
    {
        auto msg = juce::String ("Recording ") + juce::String (armed) + "/"
                 + juce::String (numTracks) + " tracks -> " + dir.getFileName();
        if (autoRouted > 0)
            msg << " (auto-routed " << autoRouted << ")";
        // Free-space pre-flight: warn loudly if the engineer is about to
        // start a take with very little room on either the primary or
        // backup volume. Pre-show beats mid-show discovery every time.
        const int mins = engine.getEstimatedMinutesRemaining();
        if (mins > 0 && mins < 30)
            msg << "  --  ! DISK ~" << mins << " min remaining";
        else if (mins > 0)
            msg << "  --  ~" << mins << " min remaining";
        statusLabel.setText (msg, juce::dontSendNotification);
        recordButton.setButtonText ("STOP");
    }
    else
    {
        statusLabel.setText ("Failed to start recording -- could not open writer files.",
                             juce::dontSendNotification);
    }
}

void MainComponent::onPlayClicked()
{
    auto& player = engine.getPlayer();
    if (! player.isLoaded())
    {
        statusLabel.setText ("Load a session first", juce::dontSendNotification);
        return;
    }

    if (player.isPlaying())
    {
        engine.stopPlayback();
        playButton.setButtonText ("PLAY");
        statusLabel.setText ("Paused", juce::dontSendNotification);
    }
    else
    {
        // Pro Tools-style: if the engineer has placed the edit cursor,
        // playback starts there instead of from wherever the player
        // happened to be paused. Without a cursor (-1) the player
        // resumes from its current position as before.
        const auto cursor = engine.getEditCursorSample();
        if (cursor >= 0)
            player.setPositionSamples (cursor);

        engine.startPlayback();
        playButton.setButtonText ("PAUSE");
        statusLabel.setText ("Playing -> " + player.getSessionName(), juce::dontSendNotification);
    }
}

void MainComponent::onStopClicked()
{
    // STOP-while-recording is a take-killing action. A fat-fingered
    // keystroke or stray touch must not end a live recording. First
    // tap arms (flash + toast); a second tap within 2 s fires for
    // real. Any other state -- playback, idle -- stops immediately
    // because there's nothing irreversible to protect.
    auto& player = engine.getPlayer();

    if (engine.isRecording())
    {
        const auto now = juce::Time::getMillisecondCounter();
        constexpr juce::uint32 kArmWindowMs = 2000;

        if (stopArmedAtMs == 0 || (now - stopArmedAtMs) > kArmWindowMs)
        {
            // First tap -- arm. Don't actually stop yet.
            stopArmedAtMs = now;
            toast.show ("Tap STOP again to end the recording", Toast::Kind::Warning);
            statusLabel.setText ("STOP armed -- tap again within 2 s to stop recording",
                                 juce::dontSendNotification);
            return;
        }
        // Second tap within window -- disarm and fall through to stop.
        stopArmedAtMs = 0;
    }
    else
    {
        stopArmedAtMs = 0;
    }

    if (engine.isRecording()) engine.stopRecording();
    engine.stopPlayback();
    player.rewind();
    playButton.setButtonText ("PLAY");
    recordButton.setButtonText ("RECORD");
    statusLabel.setText (player.isLoaded() ? "Stopped" : "Idle", juce::dontSendNotification);
}

void MainComponent::onFormatClicked()
{
    if (engine.isRecording()) return;

    auto& recorder = engine.getRecorder();
    using F = zynforge::CaptureFormat;
    const auto cur = recorder.getCaptureFormat();
    const auto next = cur == F::Wav24      ? F::Wav32Float
                    : cur == F::Wav32Float ? F::Flac24
                                            : F::Wav24;
    recorder.setCaptureFormat (next);
    refreshFormatButton();
}

void MainComponent::onPreRollClicked()
{
    if (engine.isRecording()) return;

    auto& recorder = engine.getRecorder();
    const int cur = recorder.getPreRollSeconds();
    const int next = cur == 0  ? 5
                   : cur == 5  ? 10
                   : cur == 10 ? 30
                               : 0;
    recorder.setPreRollSeconds (next);
    refreshPreRollButton();
}

void MainComponent::refreshFormatButton()
{
    using F = zynforge::CaptureFormat;
    juce::String label;
    switch (engine.getRecorder().getCaptureFormat())
    {
        case F::Wav16:       label = "WAV 16";   break;
        case F::Wav24:       label = "WAV 24";   break;
        case F::Wav32Float:  label = "WAV 32F";  break;
        case F::Aiff16:      label = "AIFF 16";  break;
        case F::Aiff24:      label = "AIFF 24";  break;
        case F::Aiff32Float: label = "AIFF 32F"; break;
        case F::Flac16:      label = "FLAC 16";  break;
        case F::Flac24:      label = "FLAC 24";  break;
    }
    formatButton.setButtonText (label);
}

void MainComponent::refreshPreRollButton()
{
    const int s = engine.getRecorder().getPreRollSeconds();
    preRollButton.setButtonText ("PRE " + juce::String (s) + "s");
}

void MainComponent::onLockToggled()
{
    sessionLocked = ! sessionLocked;
    applyLockState();
}

void MainComponent::applyLockState()
{
    const bool e = ! sessionLocked;

    recordButton .setEnabled (e);
    deviceButton .setEnabled (e);
    formatButton .setEnabled (e && ! engine.isRecording());
    preRollButton.setEnabled (e && ! engine.isRecording());
    loadButton   .setEnabled (e);
    playButton   .setEnabled (e);
    stopButton   .setEnabled (e);
    backupButton .setEnabled (e);
    patchButton  .setEnabled (e);
    metersButton .setEnabled (e);
    oscButton    .setEnabled (e);
    addChannelButton.setEnabled (e && ! engine.isRecording());

    for (auto& s : strips) if (s != nullptr) s->setEnabled (e);

    lockButton.setButtonText (sessionLocked ? "UNLOCK" : "LOCK");
    showStatus (sessionLocked ? "LOCKED -- click UNLOCK to resume control"
                              : engine.isRecording()  ? "Recording"
                              : engine.isPlaying()    ? "Playing"
                                                      : "Idle");
}

void MainComponent::onVscClicked()
{
    // Virtual Soundcheck: copy each strip's input routing to its output
    // routing, so playback feeds the same hardware channel the live
    // input would have used. The desk sees the recorded tracks on the
    // same snake / network channels it'd see the live mics on.
    auto& rec = engine.getRecorder();
    const int n = rec.getNumTracks();
    int repatched = 0;
    for (int i = 0; i < n; ++i)
    {
        const int inDev = rec.getTrack (i).inputRouting.load (std::memory_order_relaxed);
        // -2 means 'identity default' -- resolve to i so the output ends
        // up explicitly set to the strip's identity input.
        const int target = (inDev == -2) ? i : inDev;
        engine.setTrackOutputRouting (i, target);
        ++repatched;
    }
    showStatus (repatched > 0
                ? "Virtual Soundcheck -- " + juce::String (repatched)
                  + " strip(s) repatched: outputs now match inputs"
                : juce::String ("No strips to repatch"));
}

void MainComponent::onBackupClicked()
{
    if (engine.isRecording())
    {
        showStatus ("Stop recording to change the backup folder");
        return;
    }

    chooser = std::make_unique<juce::FileChooser> (
        "Pick a backup folder (recordings are mirrored here)",
        engine.getBackupDirectory().exists() ? engine.getBackupDirectory()
                                              : getSessionsRoot(),
        "");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        auto dir = fc.getResult();
        if (dir.getFullPathName().isEmpty()) return;
        engine.setBackupDirectory (dir);
        backupButton.setButtonText ("BACKUP OK");
        showStatus ("Backup folder -> " + dir.getFileName());
    });
}


void MainComponent::onFileMenuClicked()
{
    const auto activeDir   = engine.getActiveSessionDir();
    const bool hasActive   = activeDir.isDirectory();

    juce::PopupMenu menu;
    menu.addItem (1, "Open Session...");
    menu.addItem (4, "Import Audio Files...",   ! engine.isRecording());
    menu.addSeparator();
    menu.addItem (2, "Save Session State",      hasActive);
    menu.addItem (3, "Save Session As...",   hasActive);
    menu.addSeparator();

    juce::PopupMenu exportMenu;
    exportMenu.addItem (10, "Export All Tracks...", hasActive);

    juce::PopupMenu individualMenu;
    const int trackCount = engine.getRecorder().getNumTracks();
    for (int i = 0; i < trackCount; ++i)
    {
        const auto& t   = engine.getRecorder().getTrack (i);
        const auto name = t.name.isNotEmpty() ? t.name
                                              : juce::String (i + 1);
        individualMenu.addItem (100 + i, name, hasActive);
    }
    exportMenu.addSubMenu ("Export Individual Track", individualMenu, hasActive && trackCount > 0);
    menu.addSubMenu ("Export", exportMenu);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&loadButton),
                        [this] (int chosen)
    {
        if (chosen == 0) return;
        if (chosen == 1)           onLoadSessionClicked();
        else if (chosen == 2)      onSaveSessionState();
        else if (chosen == 3)      onSaveSessionAs();
        else if (chosen == 4)      onImportAudioFiles();
        else if (chosen == 10)     onExportAllTracks();
        else if (chosen >= 100)    onExportIndividualTrack (chosen - 100);
    });
}

void MainComponent::showStatus (const juce::String& msg)
{
    statusLabel.setText (msg, juce::dontSendNotification);
    // Also surface the message as a non-modal toast so the engineer
    // catches feedback even when their eyes are on the strips, not the
    // footer. Pick the Kind from the message tone, by severity:
    //   Error   (red)   -- hard failures the engineer MUST act on
    //                      (disk full / out of space, device lost,
    //                      could-not-write, generic "error").
    //   Warning (amber) -- recoverable / confirm-again prompts.
    //   Info    (grey)  -- neutral feedback.
    if (msg.isNotEmpty())
    {
        const bool hardFail = msg.containsIgnoreCase ("error")
                           || msg.containsIgnoreCase ("disk full")
                           || msg.containsIgnoreCase ("out of space")
                           || msg.containsIgnoreCase ("device lost")
                           || msg.containsIgnoreCase ("lost the audio")
                           || msg.containsIgnoreCase ("could not")
                           || msg.containsIgnoreCase ("unable to");
        const bool warn     = msg.containsIgnoreCase ("can't")
                           || msg.containsIgnoreCase ("fail")
                           || msg.containsIgnoreCase ("stop record");
        const auto kind = hardFail ? Toast::Kind::Error
                        : warn     ? Toast::Kind::Warning
                                   : Toast::Kind::Info;
        toast.show (msg, kind);
    }
}



void MainComponent::removeLastCapture()
{
    const auto root = getSessionsRoot();
    if (! root.isDirectory()) { showStatus ("No sessions to remove"); return; }

    auto dirs = root.findChildFiles (juce::File::findDirectories, false, "Session_*");
    if (dirs.isEmpty()) { showStatus ("No sessions to remove"); return; }

    // Find the most-recently-modified session folder.
    dirs.sort();   // alphabetical; our naming is ISO-style so this is chronological
    const auto target = dirs.getLast();

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::NoIcon)
            .withTitle ("Remove last capture?")
            .withMessage ("Permanently delete\n\n" + target.getFullPathName() + "\n\nThis cannot be undone.")
            .withButton ("Delete")
            .withButton ("Cancel"),
        [this, target] (int result)
    {
        if (result != 1) return;   // first button ("Delete") = commandID 1
        if (target.deleteRecursively())
            showStatus ("Removed: " + target.getFileName());
        else
            showStatus ("Couldn't remove that folder");
    });
}

void MainComponent::applySessionSettings()
{
    if (engine.isRecording()) { showStatus ("Stop recording before applying settings"); return; }

    using F = zynforge::CaptureFormat;
    F fmt;
    if (pendingContainer == 0)
        fmt = pendingBitDepth == 16 ? F::Wav16  : pendingBitDepth == 24 ? F::Wav24  : F::Wav32Float;
    else if (pendingContainer == 1)
        fmt = pendingBitDepth == 16 ? F::Aiff16 : pendingBitDepth == 24 ? F::Aiff24 : F::Aiff32Float;
    else
        fmt = pendingBitDepth == 16 ? F::Flac16 : F::Flac24;   // FLAC has no 32-float

    engine.getRecorder().setCaptureFormat (fmt);
    refreshFormatButton();

    auto setup = engine.getDeviceManager().getAudioDeviceSetup();
    setup.sampleRate = pendingSampleRate;
    const auto err = engine.getDeviceManager().setAudioDeviceSetup (setup, true);
    if (err.isEmpty())
        showStatus ("Session settings applied (" + formatButton.getButtonText()
                     + " @ " + juce::String (pendingSampleRate / 1000.0, 1) + " kHz)");
    else
        showStatus ("Sample rate change failed: " + err);
}

void MainComponent::confirmAndQuit()
{
    const bool recording = engine.isRecording();
    const auto activeDir = engine.getActiveSessionDir();
    const bool hasActiveSession = activeDir.isDirectory();

    // Recording in progress is its own conversation -- two buttons,
    // primary "Stop & Quit" / Cancel. No Save question because the
    // engineer is mid-take and saving doesn't make sense yet.
    // Explicit addButton return values -- MessageBoxOptions' index-based
    // result mapping is unreliable across platforms (see the three-button
    // dialog below), which made the bare "Quit" button do nothing.
    if (recording)
    {
        constexpr int kStopQuit = 1, kCancel = 2;
        auto* aw = new juce::AlertWindow ("Recording is still rolling",
                                          "A recording is in progress.\n"
                                          "Stop the recording cleanly and quit?",
                                          juce::MessageBoxIconType::NoIcon, this);
        aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
        aw->addButton ("Stop & Quit", kStopQuit, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel",      kCancel,   juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create ([this, aw] (int result)
            {
                std::unique_ptr<juce::AlertWindow> dispose (aw);
                if (result == kCancel) return;       // stay in app
                engine.stopRecording();
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->quit();
            }),
            false);
        return;
    }

    // No active session -- nothing to save. Two buttons.
    if (! hasActiveSession)
    {
        constexpr int kQuit = 1, kCancel = 2;
        auto* aw = new juce::AlertWindow ("Quit Zynforge Recording?",
                                          "No active session. Any unsaved app state will be lost.",
                                          juce::MessageBoxIconType::NoIcon, this);
        aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
        aw->addButton ("Quit",   kQuit,   juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", kCancel, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create ([aw] (int result)
            {
                std::unique_ptr<juce::AlertWindow> dispose (aw);
                if (result == kCancel) return;       // stay in app
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->quit();
            }),
            false);
        return;
    }

    // Pro Tools-style three-button save-on-quit dialog. Button order
    // matches the Pro Tools convention the engineer expects:
    //   [Don't Save]  [Cancel]  [Save]
    //
    // Using AlertWindow::addButton with explicit return values is
    // critical here. MessageBoxOptions' index-based return mapping
    // proved unreliable -- in earlier builds Cancel was triggering
    // 'Don't Save' behaviour and vice versa because juce::AlertWindow
    // doesn't number index-added buttons the way the docs suggest on
    // every platform. Explicit IDs leave no room for confusion.
    constexpr int kDontSave = 1;
    constexpr int kCancel   = 2;
    constexpr int kSave     = 3;

    auto* aw = new juce::AlertWindow ("Save changes to \"" + activeDir.getFileName()
                                       + "\" before closing?",
                                      "Your session state, take history, cues and "
                                      "markers will be written to the session folder.",
                                      juce::MessageBoxIconType::NoIcon,
                                      this);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
    aw->addButton ("Don't Save", kDontSave);
    aw->addButton ("Cancel",     kCancel, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->addButton ("Save",       kSave,   juce::KeyPress (juce::KeyPress::returnKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, activeDir] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);

            if (result == kCancel)
                return;                              // abort the quit, stay in app

            if (result == kSave)
                saveSessionStateTo (activeDir);      // silent write; no picker

            // kDontSave (or any unexpected value) -- fall through to quit.
            if (auto* app = juce::JUCEApplication::getInstance())
                app->quit();
        }),
        false);
}

void MainComponent::showPreflightChecklist()
{
    // Build a one-screen status report -- green checks for what's OK,
    // amber warnings for risks, red items for blockers. Engineer
    // reads this before downbeat to verify the rig.
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    const int numInputs  = device != nullptr
        ? device->getActiveInputChannels().countNumberOfSetBits()  : 0;
    const int numOutputs = device != nullptr
        ? device->getActiveOutputChannels().countNumberOfSetBits() : 0;
    const double sr      = device != nullptr ? device->getCurrentSampleRate()      : 0.0;
    const int blockSize  = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;

    const int total = engine.getRecorder().getNumTracks();
    int armed = 0;
    for (int i = 0; i < total; ++i)
        if (engine.getRecorder().getTrack (i).armed.load (std::memory_order_relaxed)) ++armed;

    const auto sess   = engine.getActiveSessionDir();
    const bool hasSes = sess.isDirectory();
    const auto root   = sess.getParentDirectory();
    const double freeGB = (root.exists() ? root.getBytesFreeOnVolume() : 0) / 1.0e9;
    const auto fmt    = engine.getRecorder().getCaptureFormat();
    const juce::String fmtStr =
          fmt == zynforge::CaptureFormat::Wav24      ? "WAV/24"
        : fmt == zynforge::CaptureFormat::Wav16      ? "WAV/16"
        : fmt == zynforge::CaptureFormat::Wav32Float ? "WAV/32f"
        : fmt == zynforge::CaptureFormat::Aiff24     ? "AIFF/24"
        : fmt == zynforge::CaptureFormat::Aiff16     ? "AIFF/16"
        : fmt == zynforge::CaptureFormat::Aiff32Float? "AIFF/32f"
        : fmt == zynforge::CaptureFormat::Flac24     ? "FLAC/24"
        : fmt == zynforge::CaptureFormat::Flac16     ? "FLAC/16"
                                                      : "?";

    // 30-minute headroom estimate at the active configuration.
    const juce::int64 bytesPerSec = (juce::int64) (sr * juce::jmax (1, armed) * 3);
    const double minHeadroom = bytesPerSec > 0 ? (freeGB * 1.0e9 / (double) bytesPerSec) / 60.0
                                                : 0.0;

    auto chk = [] (bool ok, bool warn = false) -> juce::String
    {
        return ok ? juce::String ("[OK]  ")    // ✓
              : warn ? juce::String ("[!]  ") // ⚠
                      : juce::String ("[X]  ");// ✗
    };

    juce::String body;
    body << chk (device != nullptr)
         << "Audio device: " << (device != nullptr ? device->getName() : juce::String ("(none)"))
         << "\n"
         << chk (sr > 0.0)
         << "Sample rate:  " << juce::String ((int) sr) << " Hz, buffer " << blockSize << " samples\n"
         << chk (numInputs > 0)
         << "Inputs:       " << numInputs << " active\n"
         << chk (numOutputs > 0)
         << "Outputs:      " << numOutputs << " active\n"
         << "\n"
         << chk (total > 0) << "Channel strips: " << total << "\n"
         << chk (armed > 0) << "Armed for record: " << armed << " / " << total << "\n"
         << chk (hasSes) << "Active session: "
                          << (hasSes ? sess.getFileName() : juce::String ("(none)")) << "\n"
         << chk (! cues.empty(), cues.empty())
                          << "Setlist cues: " << (int) cues.size() << "\n"
         << chk (engine.getRecorder().isBackupActive())
                          << "Backup writer: "
                          << (engine.getRecorder().isBackupActive() ? juce::String ("active")
                                                                     : juce::String ("not configured"))
                          << "\n"
         << "\n"
         << chk (freeGB > 5.0, freeGB > 1.0)
         << "Disk free:    " << juce::String (freeGB, 1) << " GB ("
         << juce::String (minHeadroom, 0) << " min headroom at " << fmtStr << ")\n"
         << chk (! engine.isRecording())
         << "Status:       " << (engine.isRecording() ? juce::String ("RECORDING ROLLING")
                                                       : engine.getPlayer().isPlaying()
                                                           ? juce::String ("Playing back")
                                                           : juce::String ("Idle"))
         << "\n";

    if (engine.getMidiClockOut().isEnabled())
        body << "[OK]  MIDI clock master: " << engine.getMidiClockOut().getOutputDeviceName() << "\n";

    if (device != nullptr)
    {
        const auto dn = device->getName();
        if (dn.containsIgnoreCase ("Dante") || dn.containsIgnoreCase ("DVS"))
            body << "[OK]  Dante network active (" << dn << ")\n";
    }

    auto* aw = new juce::AlertWindow ("Pre-flight checklist",
                                      body, juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw] (int) { std::unique_ptr<juce::AlertWindow> dispose (aw); }));
}

void MainComponent::onDeviceClicked()
{
    if (auto* w = deviceDialog.getComponent())
    {
        delete w;                      // second click closes the non-modal window
        deviceDialog = nullptr;
        return;
    }
    deviceDialog = zynforge::AudioDeviceDialog::launch (engine);
}


