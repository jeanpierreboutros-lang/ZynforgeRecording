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
#include "../Audio/PreflightProbes.h"
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
    // A RECORD-triggered selection punch is in flight (pre-roll / recording /
    // post-roll) -- pressing RECORD again ends it: punch out if recording
    // (splices the take), then stop the transport and tear down.
    if (punchSessionActive)
    {
        if (engine.isRecording()) engine.stopRecording();
        engine.stopPlayback();
        engine.setPunchModeOn (false);
        punchSessionActive = false;
        recordButton.setButtonText ("RECORD");
        statusLabel.setText (engine.isRecording() ? "Idle" : "Punch ended",
                             juce::dontSendNotification);
        return;
    }

    // Phase 1d: a rolling daemon take stops over the wire.
    if (daemonModeActive() && captureSupervisor.isDaemonRecording())
    {
        captureSupervisor.stopRecording();
        statusLabel.setText ("Idle (daemon take closed)", juce::dontSendNotification);
        recordButton.setButtonText ("RECORD");
        return;
    }

    if (engine.isRecording())
    {
        // Two-tap "protect the take" guard -- same pattern onStopClicked uses.
        // While recording the RECORD button reads "STOP"; a stray press must
        // not end a live take. First tap arms (toast); a second tap within 2 s
        // actually stops. Shares stopArmedAtMs with onStopClicked so the two
        // stop surfaces stay consistent. Unlike onStopClicked this does NOT
        // rewind -- it keeps the take parked at its end so a subsequent RECORD
        // continues/appends (the live-recorder convention).
        const auto now = juce::Time::getMillisecondCounter();
        constexpr juce::uint32 kArmWindowMs = 2000;
        if (stopArmedAtMs == 0 || (now - stopArmedAtMs) > kArmWindowMs)
        {
            stopArmedAtMs = now;
            toast.show ("Tap STOP again to end the recording", Toast::Kind::Warning);
            statusLabel.setText ("STOP armed -- tap again within 2 s to end the take",
                                 juce::dontSendNotification);
            return;
        }
        stopArmedAtMs = 0;

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

    // CONTINUE vs FRESH take. If a session is loaded, RECORD continues INTO it
    // (punch-in) instead of spinning up a throwaway new session -- this is how
    // you "keep recording on an existing take". The freshly-recorded audio is
    // spliced into each armed track's take at the edit cursor; with no cursor
    // set it APPENDS at the end (continue where the take stopped). Audio before
    // the punch-in and after the punch-out is preserved. A fresh, no-session
    // record (or daemon mode, which can't punch) still starts a new session.
    const auto activeDir = engine.getActiveSessionDir();
    bool continueTake = activeDir.isDirectory()
                        && engine.getPlayer().isLoaded()
                        && ! daemonModeActive();
    juce::int64 punchAt = -1;
    // Where does the take continue from? The EDIT cursor if you set one (click
    // in the timeline), otherwise the PLAYHEAD (play / scrub to a spot and stop).
    // INSIDE the take -> record OVER from there (mid-take punch / splice). At the
    // start or end -> APPEND a NEW PART (Track_NN_partXX) -- the safe live model
    // that never touches the existing file (and works on multi-part takes).
    bool continueAppend = false;
    if (continueTake)
    {
        const auto cur   = engine.getEditCursorSample();
        const auto pos   = cur >= 0 ? cur : engine.getPlayer().getPositionSamples();
        const auto total = juce::jmax ((juce::int64) 0, engine.getPlayer().getTotalLengthSamples());
        if (pos > 0 && pos < total)
        {
            // Mid-take "record OVER from here" -- punch-in at the cursor/playhead.
            // Works on MULTI-PART takes too: the splice reads the whole take
            // (Track_NN + its parts) via ConcatReader and flattens it, so a take
            // built up by continue-recording can still be punched anywhere.
            punchAt = pos; continueAppend = false;
        }
        else
        {
            continueAppend = true;   // at the start / end -> append a new part
        }
    }

    const auto dir = continueTake ? activeDir : makeNewSessionDir();

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

    // Phase 1d (flagged): route the take through the capture daemon. The
    // GUI keeps its own device open for monitoring/meters/playback; the
    // daemon writes the files, so a GUI death can no longer stop the take.
    if (daemonModeActive())
    {
        juce::BigInteger arms;
        for (int i = 0; i < numTracks; ++i)
            if (recorder.getTrack (i).armed.load (std::memory_order_relaxed))
                arms.setBit (i);
        if (captureSupervisor.startRecording (dir, numTracks,
                                              (int) recorder.getCaptureFormat(), arms))
        {
            engine.setActiveSessionDir (dir);
            statusLabel.setText ("DAEMON recording " + juce::String (armed) + "/"
                                 + juce::String (numTracks) + " tracks -> " + dir.getFileName()
                                 + "  (take survives a GUI crash)",
                                 juce::dontSendNotification);
            recordButton.setButtonText ("STOP");
        }
        else
            statusLabel.setText ("Daemon record failed -- check Session > Capture daemon.",
                                 juce::dontSendNotification);
        return;
    }

    // ── Pro Tools-style SELECTION punch in/out ─────────────────────────────
    // If there's a selection (loop region) on a loaded take, RECORD does a
    // proper punch in AND out: roll the transport from a PRE-ROLL lead-in
    // (you hear the existing track), auto punch-IN at the selection start,
    // auto punch-OUT at its end, then play a POST-ROLL tail and stop. The
    // position-windowed record + splice is servicePunch(); this just rolls the
    // transport into it. No selection -> fall through to the cursor/append
    // continue below.
    {
        auto& player = engine.getPlayer();
        const bool haveSelection = continueTake && player.hasLoopRegion()
                                && player.getLoopEnd() > player.getLoopStart();
        if (haveSelection)
        {
            const double sr  = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const auto   in  = player.getLoopStart();
            const auto   out = player.getLoopEnd();
            const int    preRollSec  = juce::jmax (2, engine.getRecorder().getPreRollSeconds());
            const juce::int64 preRollS  = (juce::int64) (preRollSec * sr);
            const juce::int64 postRollS = (juce::int64) (2.0 * sr);

            for (int i = 0; i < numTracks; ++i)
                engine.setTrackPunchArmed (i, recorder.getTrack (i).armed.load (std::memory_order_relaxed));
            engine.setPunchModeOn (true);
            punchOutSample    = out;
            punchPostRollEnd  = out + postRollS;
            punchSessionActive = true;
            wasInsidePunch    = false;   // let servicePunch see the crossing
            player.setPositionSamples (juce::jmax ((juce::int64) 0, in - preRollS));
            engine.startPlayback();
            recordButton.setButtonText ("STOP");
            statusLabel.setText ("Punch-in armed -- in " + juce::String (in / sr, 1)
                                 + "s, out " + juce::String (out / sr, 1)
                                 + "s, pre-roll " + juce::String (preRollSec) + "s",
                                 juce::dontSendNotification);
            return;
        }
    }

    // Continue: append a new part (safe, no file touched), or punch-in at the
    // cursor (splice). No-op for tracks with no take (they record fresh).
    if (continueTake)
    {
        if (continueAppend)
            // Continue the timeline from the end of the existing take so the
            // clock + playhead carry on instead of restarting at 0.
            engine.armContinue (juce::jmax ((juce::int64) 0,
                                            engine.getPlayer().getTotalLengthSamples()));
        else
            engine.armPunchIn (punchAt);
    }

    if (engine.startRecording (dir))
    {
        const double sr = engine.getPlayer().getSampleRate() > 0.0
                              ? engine.getPlayer().getSampleRate() : 48000.0;
        auto msg = ! continueTake
                 ? juce::String ("Recording ") + juce::String (armed) + "/"
                       + juce::String (numTracks) + " tracks -> " + dir.getFileName()
                 : continueAppend
                 ? juce::String ("Continuing take -- recording a new part on ")
                       + juce::String (armed) + " track(s)"
                 : juce::String ("Punch-in ") + juce::String (armed)
                       + " track(s) at " + juce::String (punchAt / sr, 1) + " s";
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
    // During a continue / punch record the player is LOADED, so without this
    // guard clicking PLAY (or transportBar->onRequestPlay) would start
    // playback concurrently with the live recording. The spacebar handler
    // checks isRecording() first; match that here so PLAY is inert while
    // recording -- STOP / RECORD end the take.
    if (engine.isRecording())
    {
        statusLabel.setText ("Recording -- press STOP to end the take before playing.",
                             juce::dontSendNotification);
        return;
    }

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
                                          juce::MessageBoxIconType::WarningIcon, this);   // -> forge-mark badge (LAF)
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
                                          juce::MessageBoxIconType::QuestionIcon, this);   // -> forge-mark badge (LAF)
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
                                      juce::MessageBoxIconType::QuestionIcon,   // -> forge-mark badge (LAF)
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

    // 30-minute headroom estimate at the active configuration. Derive the real
    // bytes-per-sample from the format (was hardcoded to 3 = 24-bit, so a 32f
    // session read ~33% too optimistic and could hit disk-full mid-set). FLAC
    // uses its uncompressed size, which is conservative (it compresses).
    const int bytesPerSample =
          (fmt == zynforge::CaptureFormat::Wav16 || fmt == zynforge::CaptureFormat::Aiff16
           || fmt == zynforge::CaptureFormat::Flac16)                                       ? 2
        : (fmt == zynforge::CaptureFormat::Wav32Float || fmt == zynforge::CaptureFormat::Aiff32Float) ? 4
        : 3;   // 24-bit WAV / AIFF / FLAC
    const juce::int64 bytesPerSec = (juce::int64) (sr * juce::jmax (1, armed) * bytesPerSample);
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

    // ── Measured probes (not just configuration snapshots) ─────────────

    // CPU headroom: the real CoreAudio callback load right now.
    const float cpuPct = engine.getAudioLoadPct();
    body << "\n"
         << chk (cpuPct < 50.0f, cpuPct < 80.0f)
         << "CPU (audio callback): " << juce::String (cpuPct, 1) << " %\n";

    // Session SR vs device SR -- a mismatched virtual soundcheck plays
    // at the wrong pitch/speed.
    if (engine.getPlayer().isLoaded() && sr > 0.0)
    {
        const double fileSr = engine.getPlayer().getSampleRate();
        body << chk (std::abs (fileSr - sr) < 1.0)
             << "Session SR matches device: " << juce::String ((int) fileSr) << " Hz\n";
    }

    // Signal presence on armed channels (peaks are live at line check).
    if (armed > 0)
    {
        int withSignal = 0;
        for (int i = 0; i < total; ++i)
        {
            auto& t = engine.getRecorder().getTrack (i);
            if (t.armed.load (std::memory_order_relaxed)
                && t.peak.load (std::memory_order_relaxed) > 0.001f)   // > -60 dBFS
                ++withSignal;
        }
        body << chk (withSignal == armed, withSignal > 0)
             << "Signal on armed inputs: " << withSignal << " / " << armed << "\n";
    }

    // Measured write speed of the session volume vs what the armed
    // configuration demands (uncompressed worst case, with 1.5x margin
    // for filesystem jitter mid-show). Reuses the format-derived
    // bytesPerSample computed for the headroom estimate above.
    const double needMBps = zynforge::preflight::requiredWriteMBps (sr, juce::jmax (1, armed), bytesPerSample);
    if (hasSes || root.isDirectory())
    {
        const auto speedDir = hasSes ? sess : root;
        const double gotMBps = zynforge::preflight::measureWriteSpeedMBps (speedDir);
        body << chk (gotMBps > needMBps * 1.5, gotMBps > needMBps)
             << "Disk write speed (measured): " << juce::String (gotMBps, 0)
             << " MB/s  (need " << juce::String (needMBps, 1) << " MB/s for "
             << juce::jmax (1, armed) << " ch)\n";
    }

    // Every configured mirror drive: mounted AND actually writable.
    for (const auto& m : engine.getMirrors())
    {
        const bool w = zynforge::preflight::volumeWritable (m.root);
        body << chk (w)
             << "Mirror " << m.root.getFullPathName()
             << (w ? juce::String (": writable") : juce::String (": NOT writable / missing")) << "\n";
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


