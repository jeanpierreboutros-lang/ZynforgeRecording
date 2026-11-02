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
#include "../Audio/PreflightProbes.h"
#include "BackupMirrorDialog.h"
#include "SessionRecoveryDialog.h"
#include "MarkerListDialog.h"
#include "SessionInfoDialog.h"
#include "SessionProjPath.h"
#include "RecordPosition.h"

using namespace zynforge;

namespace
{
    juce::String localCaptureFinalizationError (AudioEngine& engine)
    {
        auto& recorder = engine.getRecorder();
        juce::StringArray failures;
        if (recorder.hasPrimaryFailed()) failures.add ("primary audio write failed");
        if (recorder.hasCaptureDeviceLost()) failures.add ("capture audio device stopped during take");
        if (recorder.hasBackupFailed()) failures.add ("backup audio write failed");
        if (recorder.anyMirrorFailed()) failures.add ("mirror audio write failed");
        if (recorder.getMirrorsSkippedAtStart() > 0) failures.add ("configured mirror did not open");
        if (recorder.hasRecoveryMarkerFailed()) failures.add ("recovery marker failed");
        if (recorder.hasReportWriteFailed()) failures.add ("integrity report could not be saved");
        if (recorder.hasPunchSpliceFailed()) failures.add ("punch could not be spliced; the original take was kept and the new audio is in Session File Backups/Unspliced Punch");
        if (engine.hasStereoMixWriteFailed()) failures.add ("optional stereo mix is incomplete");
        return failures.isEmpty() ? juce::String()
            : "Recording stopped, but finalization failed: " + failures.joinIntoString (", ");
    }
}

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
    if (captureMetadataPending || engine.isCaptureFinalizing()
        || (sessionIoBusy.load() && ! engine.isRecording()))
    {
        // An autosave or layout write is over in moments and nobody chose to
        // start it, so it must not cost the engineer the press. Copies, exports
        // and a stop's finalization are not held: those can take far longer.
        if (! captureMetadataPending && ! engine.isCaptureFinalizing()
            && pendingMetadataSaves != 0 && ! metadataContinuationReserved)
        {
            recordPressHeldAtMs = juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter());
            showStatus ("RECORD held -- starting as soon as this save finishes");
            return;
        }
        showRecordRefusal ("Wait for recording finalization / session saving to finish");
        return;
    }
    recordRefusal.clear();
    // RECORD is a sequence: a press during a take (or a refused daemon state)
    // is consumed first; otherwise every pre-flight refusal runs before
    // anything is changed; then the take starts through the daemon, as a
    // selection punch, or locally (fresh / append / cursor punch).
    if (handleRecordPressDuringTake()) return;
    RecordStartPlan plan;
    if (! prepareRecordStart (plan)) return;
    if (cursorPunchNeedsAnswer (plan)) { askBeforeCursorPunch (plan); return; }
    runRecordPlan (plan);
}

// A cursor takes one click to place and can be left anywhere, so on a stopped
// transport it is not yet a decision to replace audio. A rolling punch and a
// selected range are deliberate, and a remote press is refused further on.
bool MainComponent::cursorPunchNeedsAnswer (const RecordStartPlan& plan)
{
    return plan.punchAt >= 0
        && ! cursorPunchAnswered
        && ! engine.getPlayer().isPlaying()
        && ! recordPlanHasSelection (plan)
        && ! remoteStartInFlight && ! twinStartInFlight;
}

void MainComponent::askBeforeCursorPunch (const RecordStartPlan& plan)
{
    if (cursorPunchQuestion != nullptr) return;
    const auto& player = engine.getPlayer();
    const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
    auto* aw = new juce::AlertWindow (
        "Replace audio?",
        "The EDIT cursor is at " + juce::String (plan.punchAt / sr, 1) + " s, inside the take.\n\n"
        "Replacing starts there on " + juce::String (plan.armed) + " armed track(s) and runs until you "
        "stop. It can't be undone.\n\nRecording after the take leaves everything as it is.",
        juce::MessageBoxIconType::NoIcon, this);
    // Return is the safe answer: a hurried press must never replace audio.
    aw->addButton ("Record After Take", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Replace From Cursor", 2);
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
    cursorPunchQuestion = aw;
    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([self, aw] (int answer)
    {
        std::unique_ptr<juce::AlertWindow> dispose (aw);
        if (self != nullptr) self->answerCursorPunch (answer);
    }));
}

void MainComponent::answerCursorPunch (int answer)
{
    if (answer == 0) return;
    // This is a fresh press through every refusal, because anything can have
    // moved while the question was open. A take or playback started since
    // would give the press another meaning, so it stops here.
    if (engine.isRecording() || engine.getPlayer().isPlaying())
    { showRecordRefusal ("The transport moved while RECORD was asking -- press RECORD again"); return; }
    if (answer == 1) engine.clearEditCursor();
    const juce::ScopedValueSetter<bool> answered (cursorPunchAnswered, answer == 2);
    onRecordClicked();
}

bool MainComponent::recordPlanHasSelection (const RecordStartPlan& plan)
{
    return plan.continueTake && engine.getPlayer().isRangeArmed();
}

bool MainComponent::recordPlanPunches (const RecordStartPlan& plan)
{
    return plan.punchAt >= 0 || recordPlanHasSelection (plan);
}

// A press may only start a take while it is still the engineer's intent. Past
// this, a stalled write would start a take long after they stopped waiting.
void MainComponent::releaseHeldRecordPress()
{
    static constexpr juce::uint32 kLongestHoldMs = 5000;
    if (recordPressHeldAtMs == 0 || pendingMetadataSaves != 0) return;
    const auto heldForMs = juce::Time::getMillisecondCounter() - recordPressHeldAtMs;
    recordPressHeldAtMs = 0;
    if (sessionLocked) return;
    if (heldForMs > kLongestHoldMs) showStatus ("Saving took too long -- press RECORD again");
    else                            onRecordClicked();
}

void MainComponent::runRecordPlan (RecordStartPlan& plan)
{
    const bool punch = recordPlanPunches (plan);
    // A press on this Mac can punch on purpose. A remote start cannot: nobody
    // here chose that cursor, and a punch replaces audio in this Mac's take.
    if (punch && (twinStartInFlight || remoteStartInFlight))
    {
        remotePunchRefused = true;
        showRecordRefusal (juce::String (twinStartInFlight ? "Twin" : "Remote")
                           + " RECORD refused -- this Mac would punch into its take; clear the EDIT cursor or selection and stop playback");
        return;
    }
    applyRecordAutoRouting (plan);
    if (daemonModeActive()) startDaemonTake (plan);
    else if (! startSelectionPunch (plan)) startLocalTake (plan);
    // A punch edits this Mac's own take, so it is not a take the twin joins.
    // This Mac's capture is already running before the link hears of it.
    if (! punch && engine.isRecording()) twinLink.noteLocalTakeStarted();
}

bool MainComponent::handleRecordPressDuringTake()
{
    // A RECORD-triggered selection punch is in flight (pre-roll / recording /
    // post-roll) -- pressing RECORD again ends it: punch out if recording
    // (splices the take), then stop the transport and tear down.
    if (punchSessionActive)
    {
        const bool wasRecording = engine.isRecording();
        if (wasRecording) stopActiveCaptureAsync (false);
        engine.stopPlayback();
        engine.setPunchModeOn (false);
        punchSessionActive = false;
        if (! wasRecording) statusLabel.setText ("Punch ended", juce::dontSendNotification);
        return true;
    }

    if (manualPunchActive && engine.isRecording())
    {
        stopArmedAtMs = 0;
        stopActiveCaptureAsync (false);
        return true;
    }
    if (! engine.isRecording()) manualPunchActive = false;

    // The engineer selected crash-surviving daemon capture.  If that daemon
    // dies or disconnects, never silently fall back to the in-process recorder
    // on the next RECORD press -- that would look protected while it is not.
    if (useCaptureDaemon && ! captureSupervisor.isAttached())
    {
        // The press asks for the daemon back. If that leaves a take to look
        // after, or no daemon, it has done all it can do.
        reconnectCaptureDaemon();
        if (engine.isRecording() || captureMetadataPending || ! captureSupervisor.isAttached())
            return true;
    }

    // StereoMix is produced by the in-process audio callback. The capture
    // daemon intentionally owns the device exclusively and cannot produce
    // that auxiliary file yet, so fail before arming rather than silently
    // completing a take with the requested deliverable missing.
    if (daemonModeActive() && engine.getRecordStereoMix())
    {
        showRecordRefusal ("Stereo stream-mix recording is unavailable in capture-daemon mode; disable it or use in-process capture");
        return true;
    }

    // Phase 1d: a rolling daemon take stops over the wire.
    if (daemonModeActive() && captureSupervisor.isDaemonRecording())
    {
        onStopClicked();
        return true;
    }

    if (engine.isRecording())
    {
        // Two-tap "protect the take" guard -- same pattern onStopClicked uses.
        // A stray press must not end a live take. First tap arms (toast); a
        // second tap within 2 s actually stops. Shares stopArmedAtMs with
        // onStopClicked so the two stop surfaces stay consistent. Unlike
        // onStopClicked this does NOT
        // rewind -- it keeps the take parked at its end so a subsequent RECORD
        // continues/appends (the live-recorder convention).
        const auto now = juce::Time::getMillisecondCounter();
        constexpr juce::uint32 kArmWindowMs = 2000;
        if (stopArmedAtMs == 0 || (now - stopArmedAtMs) > kArmWindowMs)
        {
            stopArmedAtMs = now;
            toast.showNow ("Tap STOP again to end the recording", Toast::Kind::Warning);
            statusLabel.setText ("STOP armed -- tap again within 2 s to end the take",
                                 juce::dontSendNotification);
            return true;
        }
        stopArmedAtMs = 0;

        stopActiveCaptureAsync (false);
        noteTwinStopIfEnded();
        return true;
    }
    return false;
}

bool MainComponent::prepareRecordStart (RecordStartPlan& plan)
{
    // Pre-flight: surface why recording wouldn't capture anything BEFORE
    // we open writers + flip the engine into recording state.
    auto& recorder = engine.getRecorder();
    const int numTracks = recorder.getNumTracks();
    if (numTracks == 0)
    {
        showRecordRefusal ("Add a channel with +CH before recording.");
        return false;
    }

    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    if (device == nullptr)
    {
        showRecordRefusal ("No audio device open -- pick one with DEVICE.");
        return false;
    }
    const int numInputs = device->getActiveInputChannels().countNumberOfSetBits();
    if (numInputs == 0)
    {
        showRecordRefusal ("Device has 0 active inputs -- open DEVICE and enable inputs.");
        return false;
    }

    // Count armed tracks. The routing auto-fix for armed strips whose input
    // lies outside the device runs only after every refusal below has passed
    // (applyRecordAutoRouting) -- it used to rewrite and persist routing even
    // when the press was then refused for the clock, stereo mix or disk space.
    int armed = 0;
    for (int i = 0; i < numTracks; ++i)
        if (recorder.getTrack (i).armed.load (std::memory_order_relaxed)) ++armed;
    if (armed == 0)
    {
        showRecordRefusal ("Arm at least one track (R button) before recording.");
        return false;
    }

    if (! daemonModeActive() && engine.getRecordStereoMix())
    {
        bool hasStreamSource = false;
        for (int i = 0; i < numTracks; ++i)
            hasStreamSource = hasStreamSource
                           || recorder.getTrack (i).streamSend.load (std::memory_order_relaxed);
        if (! hasStreamSource)
        {
            showRecordRefusal ("StereoMix is enabled but no channel is sent to STREAM; enable a STREAM send before recording");
            return false;
        }
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
            showRecordRefusal ("Recording blocked -- device " + khz (devSR)
                               + " != session " + khz (sessionSR)
                               + ". Match the clock first.");
            srMismatchWarned = 0.0;          // re-arm so the dialog pops again
            checkDeviceSampleRate (devSR);   // show the explanatory warning
            return false;                    // <-- do NOT start recording
        }
    }

    // Everything from here on stats the session's drive on this thread. A drive
    // that is only asleep answers within this; a dead mount must not take the
    // window with it. Copies are no concern here: they open after go-live.
    const auto activeSession = engine.getActiveSessionDir();
    if (! zynforge::VolumeProbe::answersWithin (activeSession != juce::File() ? activeSession : getSessionsRoot(),
                                                recordDriveWaitMs, recordDriveReaderForTests != nullptr
                                                                       ? recordDriveReaderForTests
                                                                       : zynforge::VolumeProbe::readVolume))
    {
        showRecordRefusal ("The session's drive is not answering -- check it before recording");
        return false;
    }

    planRecordPosition (plan);
    plan.dir = plan.continueTake ? engine.getActiveSessionDir() : makeNewSessionDir();
    plan.numTracks = numTracks;
    plan.armed = armed;

    // Disk-space pre-flight -- abort if the drive can't hold at least
    // 30 minutes at current SR × armed-track count × bit depth (assume
    // 24-bit if unknown). Surface the actual free GB so the engineer
    // knows what to clear.
    {
        const juce::int64 wantBytes = recordHeadroomBytes (device->getCurrentSampleRate(), plan.armed,
                                                           engine.getRecorder().getCaptureFormat());
        const auto        volume    = plan.dir.getParentDirectory();
        const juce::int64 freeBytes = volume.getBytesFreeOnVolume();
        if (zynforge::preflight::freeSpaceKnown (freeBytes, volume.getVolumeTotalSize()) && freeBytes < wantBytes)
        {
            const double freeGB = freeBytes / 1.0e9;
            showRecordRefusal ("Drive has only " + juce::String (freeGB, 1)
                               + " GB free -- < 30 min headroom. Clear space before recording.");
            return false;
        }
    }

    return true;
}

void MainComponent::planRecordPosition (RecordStartPlan& plan)
{
    // CONTINUE vs FRESH take. If a session is loaded, RECORD continues INTO it
    // (punch-in) instead of spinning up a throwaway new session -- this is how
    // you "keep recording on an existing take". The freshly-recorded audio is
    // spliced into each armed track's take at the edit cursor; with no cursor
    // set it APPENDS at the end (continue where the take stopped). Audio before
    // the punch-in and after the punch-out is preserved. A fresh, no-session
    // record (or daemon mode, which can't punch) still starts a new session.
    const auto activeDir = engine.getActiveSessionDir();
    const auto activeAudioDir = activeDir.getChildFile ("Audio Files");
    const auto takeScanDir = activeAudioDir.isDirectory() ? activeAudioDir : activeDir;
    const bool existingTakeMedia = takeScanDir.isDirectory()
        && ! takeScanDir.findChildFiles (juce::File::findFiles, false,
                                         "Track_*.wav;Track_*.flac;Track_*.aif;Track_*.aiff").isEmpty();
    auto& player = engine.getPlayer();
    // Continuing joins these strips to that session's tracks, so it needs that
    // session's mixer. One pinned but not loaded gets a new folder instead.
    plan.continueTake = activeDir.isDirectory()
                        && engine.mixerBelongsTo (activeDir)
                        && (player.isLoaded() || existingTakeMedia)
                        && ! daemonModeActive();
    if (! plan.continueTake) return;

    // While playback rolls, a manual punch uses its CURRENT playhead. The EDIT
    // cursor may still hold the place where playback began; using it here
    // silently turns an intended punch into an append/new part. When stopped,
    // the explicitly placed EDIT cursor remains the insertion point. Anything
    // else APPENDS a NEW PART (Track_NN_partXX), which never touches the
    // existing file. Punches work on MULTI-PART takes too: the splice reads the
    // whole take (Track_NN + its parts) via ConcatReader and flattens it.
    const bool rolling = player.isPlaying();
    const auto cursor  = engine.getEditCursorSample();
    const auto pos     = manualRecordPosition (rolling, player.getPositionSamples(), cursor);
    if (manualRecordPunches (rolling, cursor, pos, engine.getCaptureLengthSamples(),
                             player.getTotalLengthSamples()))
    {
        plan.punchAt = pos;
        plan.continueAppend = false;
    }
    else
        plan.continueAppend = true;
}

void MainComponent::applyRecordAutoRouting (RecordStartPlan& plan)
{
    auto& recorder = engine.getRecorder();
    const int numTracks = plan.numTracks;
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    const int numInputs = device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
    // Auto-fix any armed strip whose inputRouting points outside the device's
    // actual input count -- fall back to a sequential wrap so the engineer
    // doesn't have to open PATCH to start recording.
    int autoRouted = 0;
    for (int i = 0; i < numTracks; ++i)
    {
        auto& t = recorder.getTrack (i);
        const bool isArmed = t.armed.load (std::memory_order_relaxed);
        // What the strip hears on this device: a saved patch past a smaller
        // device's inputs already wraps live and must not be overwritten here.
        const int dev = engine.getEffectiveInputRouting (i);
        if (isArmed && (dev < 0 || dev >= numInputs))
        {
            const int target = numInputs > 0 ? (i % numInputs) : 0;
            engine.setTrackInputRouting (i, target);
            ++autoRouted;
        }
    }
    plan.autoRouted = autoRouted;
}

// The daemon outlives this process. The take's folder gets this mixer before it
// rolls, because a relaunch has nothing else to load the take's strips from.
// plan.dir is this mixer's session or a new folder (makeNewSessionDir), never
// another session's.
bool MainComponent::saveMixerToTakeFolder (const juce::File& dir)
{
    if (! dir.isDirectory() && dir.createDirectory().failed()) return false;
    auto snapshot = captureMetadataSnapshot (dir, false);
    snapshot.mixerOwner = dir;
    return zynforge::writeSessionMetadata (snapshot).ok;
}

// Phase 1d (flagged): route the take through the capture daemon. The GUI keeps
// its own device open for monitoring/meters/playback; the daemon writes the
// files, so a GUI death can no longer stop the take.
void MainComponent::startDaemonTake (const RecordStartPlan& plan)
{
    auto& recorder = engine.getRecorder();
    if (! configureCaptureDaemon())
    { showRecordRefusal ("Daemon recording refused: could not apply the selected device, routing and backup configuration"); return; }
    const bool mixerInTakeFolder = saveMixerToTakeFolder (plan.dir);
    juce::BigInteger arms;
    for (int i = 0; i < plan.numTracks; ++i)
        if (recorder.getTrack (i).armed.load (std::memory_order_relaxed))
            arms.setBit (i);
    requestDaemonTake (plan.dir, plan.numTracks, arms, plan.armed, mixerInTakeFolder);
}

void MainComponent::requestDaemonTake (const juce::File& dir, int numTracks, const juce::BigInteger& arms,
                                       int armed, bool mixerInTakeFolder)
{
    const double rate = captureSupervisor.lastStatus().sampleRate > 0.0
                            ? captureSupervisor.lastStatus().sampleRate : pendingSampleRate;
    zynforge::bwf::TimeAnchor timecode;
    juce::String refusal;
    bool unanswered = false;
    // Before asking: once the daemon rolls, this thread stays off its volume.
    auto& markers = engine.getMarkers();
    const auto markerSession = markers.getSessionDir();
    const auto markerRate    = markers.getSampleRate();
    markers.setContext (dir, rate);
    if (captureSupervisor.startRecording (dir, numTracks, (int) engine.getRecorder().getCaptureFormat(), arms,
                                          engine.getTimecodeAnchor (timecode) ? timecode.toJson() : juce::var(),
                                          &refusal, &unanswered))
    {
        engine.setExternalCaptureStatus (captureSupervisor.lastStatus(),
                                         captureSupervisor.lastStatusAtMs());
        lastCaptureFinalizationError.clear();
        engine.beginExternalTake (dir, rate);
        engine.claimMixerFor (dir);
        engine.noteLastCapture (dir);
        engine.snapshotCaptureInputGains();
        daemonTakeAdopted = true;
        statusLabel.setText ("DAEMON recording " + juce::String (armed) + "/"
                             + juce::String (numTracks) + " tracks -> " + dir.getFileName()
                             + (mixerInTakeFolder ? "  (take survives a GUI crash)"
                                                  : "  (mixer not saved to the take folder)"),
                             juce::dontSendNotification);
    }
    else if (unanswered)
    {
        // Not a refusal. The window must not call this failed and move on while
        // the daemon rolls into a session it does not know about.
        lastCaptureFinalizationError.clear();
        engine.beginExternalTake (dir, rate);
        engine.claimMixerFor (dir);
        engine.noteLastCapture (dir);
        engine.snapshotCaptureInputGains();
        daemonStartUnansweredMs = juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter());
        showStatus ("Daemon has not answered RECORD yet -- the take may be rolling");
    }
    else
    {
        markers.setContext (markerSession, markerRate);
        showRecordRefusal (refusal.isNotEmpty() ? "Daemon refused RECORD: " + refusal
                                                : juce::String ("Daemon record failed -- check Session > Capture daemon."));
    }
}

bool MainComponent::startSelectionPunch (const RecordStartPlan& plan)
{
    auto& recorder = engine.getRecorder();
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
        if (recordPlanHasSelection (plan))
        {
            const double sr  = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const auto   in  = player.getLoopStart();
            const auto   out = player.getLoopEnd();
            const int    preRollSec  = juce::jmax (2, engine.getRecorder().getPreRollSeconds());
            const juce::int64 preRollS  = (juce::int64) (preRollSec * sr);
            const juce::int64 postRollS = (juce::int64) (2.0 * sr);

            engine.stopPlayback();
            player.setPositionSamples (juce::jmax ((juce::int64) 0, in - preRollS));
            engine.setPunchModeOn (true);
            punchOutSample    = out;
            punchPostRollEnd  = out + postRollS;
            // Open the take before transport rolls. The audio callback admits
            // only input samples in [in,out), including a sub-block range, so
            // message-thread timer jitter cannot miss or extend the punch.
            engine.setCaptureWindow (in, out);
            engine.armPunchIn (in);
            if (! engine.startRecording (plan.dir))
            {
                engine.clearCaptureWindow();
                recorder.cancelPunchIn();
                engine.setPunchModeOn (false);
                showRecordRefusal (recorder.getStartError().isNotEmpty() ? recorder.getStartError()
                    : "Selection punch refused -- check take and backup/mirror copies");
                return true;
            }
            // One selection, one punch. Left armed, the same range would take
            // the next RECORD too, say the one meant to continue the show.
            player.disarmRange();
            lastCaptureFinalizationError.clear();
            punchSessionActive = true;
            wasInsidePunch    = false;
            engine.startPlayback();
            statusLabel.setText ("Punch-in armed -- in " + juce::String (in / sr, 1)
                                 + "s, out " + juce::String (out / sr, 1)
                                 + "s, pre-roll " + juce::String (preRollSec) + "s",
                                 juce::dontSendNotification);
            return true;
        }
    }

    return false;
}

void MainComponent::startLocalTake (const RecordStartPlan& plan)
{
    auto& recorder = engine.getRecorder();
    // Continue: append a new part (safe, no file touched), or punch-in at the
    // cursor (splice). No-op for tracks with no take (they record fresh).
    if (plan.continueTake)
    {
        if (plan.continueAppend)
            // Continue the timeline from the end of the existing take so the
            // clock + playhead carry on instead of restarting at 0.
            engine.armContinue (engine.getCaptureLengthSamples());
        else
            engine.armPunchIn (plan.punchAt);
    }

    if (engine.startRecording (plan.dir))
    {
        lastCaptureFinalizationError.clear();
        manualPunchActive = plan.continueTake && ! plan.continueAppend;
        const double sr = engine.getPlayer().getSampleRate() > 0.0
                              ? engine.getPlayer().getSampleRate() : 48000.0;
        auto msg = ! plan.continueTake
                 ? juce::String ("Recording ") + juce::String (plan.armed) + "/"
                       + juce::String (plan.numTracks) + " tracks -> " + plan.dir.getFileName()
                 : plan.continueAppend
                 ? juce::String ("Continuing take -- recording a new part on ")
                       + juce::String (plan.armed) + " track(s)"
                       + (engine.getPlayer().hasLoopRegion() ? " (select the range again to punch it)" : "")
                 : juce::String ("Punch-in ") + juce::String (plan.armed)
                       + " track(s) at " + juce::String (plan.punchAt / sr, 1) + " s";
        if (plan.autoRouted > 0)
            msg << " (auto-routed " << plan.autoRouted << ")";
        // Free-space pre-flight: warn loudly if the engineer is about to
        // start a take with very little room on either the primary or
        // backup volume. Pre-show beats mid-show discovery every time.
        const int mins = engine.getEstimatedMinutesRemaining();
        if (mins > 0 && mins < 30)
            msg << "  --  ! DISK ~" << mins << " min remaining";
        else if (mins > 0)
            msg << "  --  ~" << mins << " min remaining";
        statusLabel.setText (msg, juce::dontSendNotification);
    }
    else
    {
        showRecordRefusal (recorder.getStartError().isNotEmpty() ? recorder.getStartError()
                           : plan.continueTake && ! plan.continueAppend
                           ? "Punch refused -- could not safely open the existing take or writer files"
                           : "Failed to start recording -- could not open writer files.");
    }
}

void MainComponent::onPlayClicked()
{
    if (captureMetadataPending || pendingMetadataSaves != 0)
    { showStatus ("Wait for session finalization / saving before playing"); return; }
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
        statusLabel.setText ("Playing -> " + player.getSessionName(), juce::dontSendNotification);
    }
}

void MainComponent::onStopClicked()
{
    if (captureMetadataPending)
    { showStatus ("Recording stopped; finalization pending"); return; }
    if (punchSessionActive)
    {
        onRecordClicked(); // a deliberate selection punch ends in one press
        return;
    }
    // A normal live take gets a two-tap STOP guard against accidental cuts.
    // A deliberate manual punch is different: one press punches out at the
    // chosen point, preserving the rest of the original take.
    if (engine.isRecording() && ! manualPunchActive)
    {
        const auto now = juce::Time::getMillisecondCounter();
        constexpr juce::uint32 kArmWindowMs = 2000;

        if (stopArmedAtMs == 0 || (now - stopArmedAtMs) > kArmWindowMs)
        {
            // First tap -- arm. Don't actually stop yet.
            stopArmedAtMs = now;
            toast.showNow ("Tap STOP again to end the recording", Toast::Kind::Warning);
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

    stopActiveCaptureAsync (true);
    noteTwinStopIfEnded();
}

bool MainComponent::stopActiveCapture (bool stopPlaybackAndRewind, bool persistMetadata)
{
    auto& recorder = engine.getRecorder();
    const bool localRecording = recorder.isRecording();
    const bool resumeManualPlayback = localRecording && manualPunchActive
        && ! stopPlaybackAndRewind && engine.getPlayer().isPlaying();
    const auto resumeAt = engine.getPlayer().getPositionSamples();

    bool sessionStateSaved = true;
    juce::String finalizationError;
    if (localRecording)
    {
        engine.stopRecording();
        manualPunchActive = false;
        finalizationError = localCaptureFinalizationError (engine);
        if (finalizationError.isNotEmpty()) sessionStateSaved = false;
        const auto session = engine.getActiveSessionDir();
        if (! session.isDirectory() || (persistMetadata && ! saveSessionStateTo (session)))
            sessionStateSaved = false;
    }

    if (stopPlaybackAndRewind)
    {
        engine.stopPlayback();
        engine.getPlayer().rewind();
    }
    statusLabel.setText (stopPlaybackAndRewind && engine.getPlayer().isLoaded()
                            ? "Stopped" : "Idle",
                         juce::dontSendNotification);
    if (! sessionStateSaved)
    {
        showStatus (finalizationError.isNotEmpty()
            ? finalizationError
            : recorder.hasPunchSpliceFailed()
                ? "Punch could not be spliced into the take -- original audio was retained; check disk and session files"
            : recorder.hasReportWriteFailed()
                ? "Recording stopped, but the integrity report could not be saved -- check disk permissions / free space"
            : lastMetadataSaveError.isNotEmpty()
                ? "Recording stopped, but session state could not be saved: " + lastMetadataSaveError
                : "Recording stopped, but session state could not be saved -- check disk permissions / free space");
        return false;
    }
    if (resumeManualPlayback)
    {
        engine.getPlayer().setPositionSamples (resumeAt);
        engine.startPlayback();
    }
    return true;
}

void MainComponent::saveFinalizedCaptureMetadata (bool mediaOk, juce::String mediaError,
                                                  std::function<void (bool)> completion)
{
    juce::Component::SafePointer<MainComponent> self (this);
    captureMetadataPending = true;
    lastCaptureFinalizationError.clear();
    enqueueMetadataSave (engine.getActiveSessionDir(), {},
        [self, mediaOk, mediaError, completion] (bool saved)
        {
            if (self == nullptr) return;
            self->captureMetadataPending = false;
            if (self->pendingMetadataSaves == 0)
            {
                self->sessionIoBusy.store (false);
                self->engine.setSessionTransitionActive (false);
            }
            const bool ok = mediaOk && saved;
            self->lastCaptureFinalizationError = ok ? juce::String()
                : ! mediaOk ? mediaError
                : "Recording stopped, but session metadata could not be saved"
                    + (self->lastMetadataSaveError.isNotEmpty() ? ": " + self->lastMetadataSaveError
                                                                : juce::String());
            self->showStatus (ok
                ? self->lastSavedUndoUnits < 0
                    ? "Recording stopped; captured state saved, newer changes remain unsaved"
                    : "Recording stopped and session saved"
                : self->lastCaptureFinalizationError);
            if (completion) completion (ok);
        });
    if (captureMetadataPending) showStatus ("Recording stopped; finalization pending");
}

// STOP for a take the daemon owns. "Took it down" needs the daemon's word, or
// proof that no daemon exists: anything less leaves the take called rolling.
void MainComponent::stopDaemonCapture (bool rewind, std::function<void (bool)> completion)
{
    const auto link = captureSupervisor.isAttached() ? LostDaemon::back : resolveLostDaemon();
    if (link == LostDaemon::gone)
    {
        endDaemonTake (kDaemonGoneMidTake, rewind, std::move (completion));
        return;
    }
    if (link == LostDaemon::unknown)
    {
        lastCaptureFinalizationError = "Daemon did not confirm STOP; take may still be recording";
        showStatus (lastCaptureFinalizationError);
        if (completion) completion (false);
        return;
    }
    // Pending until the daemon answers, however long its disks take. A status
    // that says the take ended meanwhile must not start a second follow-up.
    captureMetadataPending = true;
    showStatus ("Stopping -- the capture daemon is closing its files");
    juce::Component::SafePointer<MainComponent> self (this);
    captureSupervisor.stopRecordingAsync ([self, rewind, completion]
        (zynforge::capture::CaptureSupervisor::StopResult result)
    {
        if (self == nullptr) return;
        self->captureMetadataPending = false;
        if (! result.clean && ! result.stopped)
        {
            self->lastCaptureFinalizationError = "Daemon did not confirm STOP; take may still be recording";
            self->showStatus (self->lastCaptureFinalizationError);
            if (completion) completion (false);
            return;
        }
        self->endDaemonTake (result.clean ? juce::String()
                                          : result.error.isNotEmpty() ? result.error
                                          : juce::String ("Recording stopped, but daemon finalisation failed"),
                             rewind, completion);
    });
}

void MainComponent::endDaemonTake (juce::String error, bool rewind, std::function<void (bool)> completion)
{
    const auto recordEnd = engine.getRecordTimelineSamples();
    engine.setExternalRecording (false);
    daemonTakeAdopted = false;
    daemonStartUnansweredMs = 0;
    const auto session = engine.getActiveSessionDir();
    if (! session.isDirectory())
    {
        // A removable volume can vanish while the daemon holds its files open.
        // The audio stop may have worked, but nowhere is left to save to.
        if (error.isEmpty())
            error = "Recording stopped, but session state could not be saved -- check disk permissions / free space";
        saveFinalizedCaptureMetadata (false, error, std::move (completion));
        return;
    }

    // Reading a long take back belongs on a worker, exactly as for a local STOP.
    juce::Component::SafePointer<MainComponent> self (this);
    const bool previousBusy = sessionIoBusy.load();
    captureMetadataPending = true;
    sessionIoBusy.store (true);
    const bool accepted = engine.finishExternalTakeAsync (session, recordEnd,
        [self, error, rewind, completion] (bool loaded)
        {
            if (self == nullptr) return;
            if (rewind)
            {
                self->engine.stopPlayback();
                self->engine.getPlayer().rewind();
            }
            const auto why = error.isNotEmpty() || loaded ? error
                : juce::String ("Recording stopped, but media could not be reopened");
            self->saveFinalizedCaptureMetadata (why.isEmpty(), why, completion);
        });
    if (! accepted)
    {
        captureMetadataPending = false;
        sessionIoBusy.store (previousBusy);
        lastCaptureFinalizationError = "Capture finalization was not accepted; check the recording state";
        showStatus (lastCaptureFinalizationError);
        if (completion) completion (false);
    }
}

// No STOP press ran, so nothing else resets the transport, and the session
// has never been saved if this was its first take.
void MainComponent::localTakeInterrupted()
{
    stopArmedAtMs = 0;
    manualPunchActive = false;
    if (punchSessionActive)
    {
        engine.setPunchModeOn (false);
        punchSessionActive = false;
    }
    saveFinalizedCaptureMetadata (false, "Capture device stopped during the take; check the audio and the device", {});
}

void MainComponent::stopActiveCaptureAsync (bool rewind, std::function<void (bool)> completion)
{
    if (captureMetadataPending)
    {
        showStatus ("Recording stopped; finalization pending");
        if (completion) completion (false);
        return;
    }
    juce::Component::SafePointer<MainComponent> self (this);

    if (engine.getRecorder().isRecording())
    {
        // Media finalization owns the recorder/player until its UI completion.
        // Keep session mutations guarded before capture publishes recording=false,
        // but let the engine own its worker lease and reader lifetimes.
        const bool resumeManualPlayback = manualPunchActive && ! rewind && engine.getPlayer().isPlaying();
        const auto resumeAt = engine.getPlayer().getPositionSamples();
        const bool previousBusy = sessionIoBusy.load();
        captureMetadataPending = true;
        sessionIoBusy.store (true);
        const bool accepted = engine.stopRecordingAsync (
            [self, rewind, resumeManualPlayback, resumeAt, completion] (bool mediaOk)
            {
                if (self == nullptr) return;
                auto mediaError = localCaptureFinalizationError (self->engine);
                mediaOk = mediaOk && mediaError.isEmpty();
                if (! mediaOk && mediaError.isEmpty())
                    mediaError = "Recording stopped, but media could not be finalized or reopened";
                if (rewind)
                {
                    self->engine.stopPlayback();
                    self->engine.getPlayer().rewind();
                }
                else if (resumeManualPlayback && mediaOk)
                {
                    self->engine.getPlayer().setPositionSamples (resumeAt);
                    self->engine.startPlayback();
                }
                self->saveFinalizedCaptureMetadata (mediaOk, std::move (mediaError), completion);
            });
        if (! accepted)
        {
            captureMetadataPending = false;
            sessionIoBusy.store (previousBusy);
            lastCaptureFinalizationError = "Capture finalization was not accepted; check the recording state";
            showStatus (lastCaptureFinalizationError);
            // No callback is promised by a rejected engine request. Preserve
            // the stopped/failed outcome without running another STOP.
            if (completion) completion (false);
            return;
        }
        manualPunchActive = false;
        showStatus ("Recording stopped; finalization pending");
        return;
    }

    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    {
        stopDaemonCapture (rewind, std::move (completion));
        return;
    }
    const bool mediaOk = stopActiveCapture (rewind, false);
    // The banner already stands for a failed take; an idle STOP repeats nothing.
    if (completion) completion (mediaOk && lastCaptureFinalizationError.isEmpty());
}

std::optional<bool> MainComponent::handleRemoteTransport (
    zynforge::AudioEngine::RemoteTransportAction action, juce::String& error)
{
    if (sessionLocked)
    {
        error = "session is locked; unlock on the host";
        return false;
    }
    using A = zynforge::AudioEngine::RemoteTransportAction;
    if (action == A::TogglePlay || action == A::StartPlay || action == A::StopPlay)
    {
        if (captureMetadataPending || pendingMetadataSaves != 0)
        { error = "session finalization or saving is pending"; return false; }
        // Rolling into the selected range in PUNCH mode opens a punch, and no
        // remote may start one.
        if (action != A::StopPlay && engine.isPunchModeOn() && ! engine.isPlaying())
        {
            error = "PUNCH mode is on; start playback on the host";
            showStatus ("Remote PLAY refused -- PUNCH mode is on");
            return false;
        }
        return std::nullopt;
    }

    const bool recording = engine.isRecording();
    if (action == A::StopAll || action == A::StopRecord
        || (action == A::ToggleRecord && recording))
    {
        if (! recording)
        {
            if (captureMetadataPending)
            { error = "recording stopped; finalization pending"; return false; }
            // A failed take stays reported, but it never holds up stopping
            // the playback that is rolling now.
            if (action == A::StopAll) onStopClicked();
            if (lastCaptureFinalizationError.isNotEmpty())
            { error = lastCaptureFinalizationError; return false; }
            return true;
        }
        // Every remote surface uses the same two-tap guard as the local STOP.
        // A first tap is not a successful stop and must not be ACKed as one.
        onStopClicked();
        if (engine.isRecording())
        {
            error = captureMetadataPending ? "recording stopping; finalization pending"
                  : stopArmedAtMs != 0     ? "STOP armed; send STOP again within 2 seconds"
                  : "capture stop or session finalization failed; check the host";
            return false;
        }
        if (captureMetadataPending)
        { error = "recording stopped; finalization pending"; return false; }
        if (lastCaptureFinalizationError.isNotEmpty())
        { error = lastCaptureFinalizationError; return false; }
        return true;
    }

    if (action == A::StartRecord || action == A::ToggleRecord)
    {
        if (recording) return true;
        if (useCaptureDaemon && ! captureSupervisor.isAttached())
        {
            error = "capture daemon is unavailable";
            showRecordRefusal ("Remote RECORD refused -- capture daemon is unavailable");
            return false;
        }
        // During a punch's post-roll a RECORD press would end the punch.
        if (punchSessionActive)
        {
            error = "a punch is in progress on the host";
            return false;
        }

        const juce::ScopedValueSetter<bool> remoteStart (remoteStartInFlight, true);
        remotePunchRefused = false;
        onRecordClicked();
        if (engine.isRecording()) return true;
        error = recordPressHeldAtMs != 0 ? "a session save is finishing; RECORD starts when it ends"
              : remotePunchRefused ? "RECORD would punch into the take; clear the cursor or selection on the host"
                                   : "recording could not start; check the host pre-flight message";
        return false;
    }

    return std::nullopt;
}

void MainComponent::onLockToggled()
{
    sessionLocked = ! sessionLocked;
    applyLockState();
}

void MainComponent::applyLockState()
{
    engine.setControlsLocked (sessionLocked.load());
    const bool e = ! sessionLocked;

    deviceButton .setEnabled (e);
    backupButton .setEnabled (e);
    vscButton    .setEnabled (e);
    patchButton  .setEnabled (e);
    metersButton .setEnabled (e);
    addChannelButton.setEnabled (e && ! engine.isRecording());
    setlistBar.setEnabled (e);
    if (transportBar != nullptr) transportBar->setEnabled (e);
    if (masterStrip != nullptr) masterStrip->setEnabled (e);

    for (auto& s : strips) if (s != nullptr) s->setEnabled (e);
    if (editPage != nullptr)
    {
        editPage->setEnabled (e && (! sessionIoBusy.load() || pendingMetadataSaves != 0 || captureMetadataPending));
        // Rows and ruler take the mouse by hand, so disabling them is not enough.
        editPage->setReadOnlyWhileRecording (sessionLocked || engine.isRecording()
                                             || pendingMetadataSaves != 0 || captureMetadataPending);
        if (auto* tools = editPage->getEditToolsBar()) tools->setEnabled (e);
    }
    // Floating panels that change the mix or the patch lock with the rest.
    if (patchDialog != nullptr) patchDialog->setEnabled (e);
    if (deviceDialog != nullptr) deviceDialog->setEnabled (e);
    if (twinDialog != nullptr) twinDialog->setEnabled (e);

    lockButton.setButtonText (sessionLocked ? "UNLOCK" : "LOCK");
    lockButton.setToggleState (sessionLocked, juce::dontSendNotification);
    showStatus (sessionLocked ? "LOCKED -- click UNLOCK to resume control"
                              : engine.isRecording()  ? "Recording"
                              : engine.isPlaying()    ? "Playing"
                                                      : "Idle");
}

// The header tab and the menu open the same one window. Only a tracked window
// is reached by LOCK and by the reaper that frees it after its close box.
void MainComponent::openPatchWindow()
{
    if (auto* w = patchDialog.getComponent()) { w->toFront (true); return; }
    patchDialog = zynforge::PatchPage::launch (engine);
    if (patchDialog != nullptr) patchDialog->setEnabled (! sessionLocked);
}

void MainComponent::openMetersWindow()
{
    if (auto* w = metersDialog.getComponent()) { w->toFront (true); return; }
    metersDialog = zynforge::Meterbridge::launch (engine);
}

void MainComponent::forgetSessionUiState()
{
    lastCaptureFinalizationError.clear();
    recordRefusal.clear();
    cues.clear();
    currentCueIndex = -1;
    setlistBar.setCues (cues, currentCueIndex);
    wasInsidePunch = false;
    analysisJobs.cancel();
}

void MainComponent::onVscClicked()
{
    // This repatches and saves every output, which setTrackLinkedRouting
    // refuses during a take for the same reason.
    if (engine.isRecording() || captureMetadataPending)
    { showStatus ("Virtual Soundcheck waits until the take has stopped"); return; }

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
    if (engine.isRecording() || sessionIoBusy.load())
    {
        showStatus (engine.isRecording() ? "Stop recording to change the backup folder"
                                         : "Wait for the session operation to finish");
        return;
    }

    juce::Component::SafePointer<MainComponent> self (this);
    zynforge::BackupMirrorDialog::launch (engine, {
        [self] (const juce::String& refusal)
        {
            if (self == nullptr) return;
            self->backupButton.setButtonText ("BACKUP !");
            self->showStatus (refusal);
        },
        [self] (const juce::File& dir) { return self != nullptr && self->applyBackupFolder (dir); } });
}

bool MainComponent::applyBackupFolder (const juce::File& dir)
{
    // The dialog can close after a take started behind it.
    if (! engine.setBackupDirectory (dir))
    {
        showStatus ("Backup folder not changed -- a take is rolling or finalizing");
        return false;
    }
    backupButton.setButtonText ("BACKUP OK");
    showStatus ("Backup folder -> " + dir.getFileName());
    return true;
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
        // The live-warning strings ("! SAMPLE-RATE MISMATCH", "! PRIMARY WRITE
        // FAILED", ...) all start with '!', and several hard failures used
        // wording none of the old substrings matched -- "Recording blocked ...",
        // "Export INCOMPLETE", "PARTIAL capture NOT saved" all rendered as
        // neutral grey Info, which is exactly backwards for the messages the
        // engineer most needs to catch mid-show.
        const bool bang     = msg.trimStart().startsWithChar ('!');
        const bool hardFail = bang
                           || msg.containsIgnoreCase ("error")
                           || msg.containsIgnoreCase ("disk full")
                           || msg.containsIgnoreCase ("out of space")
                           || msg.containsIgnoreCase ("device lost")
                           || msg.containsIgnoreCase ("lost the audio")
                           || msg.containsIgnoreCase ("could not")
                           || msg.containsIgnoreCase ("couldn't")
                           || msg.containsIgnoreCase ("unable to")
                           || msg.containsIgnoreCase ("blocked")
                           || msg.containsIgnoreCase ("failed")
                           || msg.containsIgnoreCase ("incomplete")
                           || msg.containsIgnoreCase ("not saved");
        const bool warn     = msg.containsIgnoreCase ("can't")
                           || msg.containsIgnoreCase ("fail")
                           || msg.containsIgnoreCase ("mismatch")
                           || msg.containsIgnoreCase ("timed out")
                           || msg.containsIgnoreCase ("partial")
                           || msg.containsIgnoreCase ("stop record");
        const auto kind = hardFail ? Toast::Kind::Error
                        : warn     ? Toast::Kind::Warning
                                   : Toast::Kind::Info;
        toast.show (msg, kind);
    }
}



void MainComponent::removeLastCapture()
{
    if (sessionIoBusy.load()) { showStatus ("Wait for the session file operation to finish"); return; }
    if (engine.isRecording()) { showStatus ("Stop recording before removing a capture"); return; }
    // The recorded capture, never a guess from folder dates: an older session
    // that was only opened looks newest, and a capture can live anywhere.
    const auto target = engine.getLastCapture();
    if (target == juce::File())
    {
        showStatus ("No recorded capture to remove -- it has been removed or moved, or nothing was recorded");
        return;
    }
    if (! sessionJobs.reserve()) { showStatus ("Wait for the session operation to finish"); return; }
    juce::Component::SafePointer<MainComponent> self (this);
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::NoIcon)
            .withTitle ("Remove last capture?")
            .withMessage (lastCapturePrompt (target))
            .withButton ("Move to Trash")
            .withButton ("Cancel"),
        [self, target] (int result)
    {
        if (self == nullptr) return;
        self->sessionJobs.release();
        if (result == 1) self->deleteCaptureSession (target);
    });
}

juce::File MainComponent::testTrashFolder()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("zynforge-test-trash");
}

juce::String MainComponent::lastCapturePrompt (const juce::File& target)
{
    return "Move the whole session \"" + target.getFileName() + "\" to the Trash?\n\n"
         + target.getFullPathName() + "\n\nThis is the session the last take was recorded into.";
}

bool MainComponent::deleteCaptureSession (const juce::File& target)
{
    if (engine.isRecording() || sessionIoBusy.load())
    { showStatus ("Stop recording and finish session operations before removing a capture"); return false; }
    const bool deletingActive = target == engine.getActiveSessionDir();
    const auto moveToTrash = [&target]
    {
        if (! zynforge::isolatedSettings.load()) return target.moveToTrash();
        const auto slot = testTrashFolder().getChildFile (juce::Uuid().toString());
        return slot.createDirectory().wasOk() && target.moveFileTo (slot.getChildFile (target.getFileName()));
    };
    if (! zynforge::MultitrackRecorder::removeSessionAfterCancellingReports (target, moveToTrash))
    { showStatus ("Couldn't move that folder to the Trash; nothing was removed"); return false; }
    // Forget it, so a second press finds nothing rather than an older session.
    engine.forgetLastCapture();
    if (deletingActive)
    {
        condemnAllStrips();
        engine.clearSessionState();
        forgetSessionUiState();
        engine.setStripCount (0);
        lastTrackCount = -1;
        undoManager.clearUndoHistory();
        updateTransportLabels(); explicitDocumentOpened = false; showStartupWelcome();
    }
    showStatus ("Moved to the Trash: " + target.getFileName());
    return true;
}


void MainComponent::confirmAndQuit()
{
    // File > Quit refuses under LOCK; the close box and Cmd+Q come straight
    // here, and Return on the dialog below would end a locked take.
    if (sessionLocked)
    { showStatus ("LOCKED -- click UNLOCK before quitting"); return; }
    if (pendingMetadataSaves != 0 || captureMetadataPending)
    { showStatus ("Session saving is still finishing -- quit again after completion"); return; }
    if (sessionIoBusy.load())
    {
        sessionJobs.cancel();
        showStatus ("Cancelling session file operation -- quit again when it finishes");
        return;
    }
    // The dialog has to tell the truth about a lost daemon, so look first.
    if (lostDaemonTakeUnknown() && resolveLostDaemon() == LostDaemon::gone)
    {
        endDaemonTake (kDaemonGoneMidTake, false, {});
        return;
    }
    const bool recording = engine.isRecording() || captureSupervisor.isDaemonRecording();
    const auto activeDir = engine.getActiveSessionDir();
    const bool hasActiveSession = engine.hasSessionFolder() && engine.mixerBelongsTo (activeDir);

    // Recording in progress is its own conversation -- two buttons,
    // primary "Stop & Quit" / Cancel. No Save question because the
    // engineer is mid-take and saving doesn't make sense yet.
    // Explicit addButton return values -- MessageBoxOptions' index-based
    // result mapping is unreliable across platforms (see the three-button
    // dialog below), which made the bare "Quit" button do nothing.
    if (lostDaemonTakeUnknown())
    {
        constexpr int kQuit = 1, kCancel = 2;
        auto* aw = new juce::AlertWindow ("Capture daemon is not answering",
                                          "A take may still be rolling in the capture daemon, and it cannot be "
                                          "stopped from here. Quitting does not stop it.",
                                          juce::MessageBoxIconType::WarningIcon, this);
        aw->setLookAndFeel (&laf);
        aw->addButton ("Quit, Leave It Rolling", kQuit);
        aw->addButton ("Cancel", kCancel, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<MainComponent> self (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([self, aw] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != kQuit || self == nullptr) return;
            self->leaveLostDaemon();
            if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
        }), false);
        return;
    }
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
        juce::Component::SafePointer<MainComponent> self (this);
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create ([self, aw, activeDir] (int result)
            {
                std::unique_ptr<juce::AlertWindow> dispose (aw);
                if (result == kCancel || self == nullptr) return; // stay in app
                self->stopActiveCaptureAsync (false, [self] (bool ok)
                {
                    if (! ok || self == nullptr) return;
                    if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
                });
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
        juce::Component::SafePointer<MainComponent> self (this);
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create ([aw, self] (int result)
            {
                std::unique_ptr<juce::AlertWindow> dispose (aw);
                if (result == kCancel) return;       // stay in app
                // A remote RECORD may have started behind this dialog; that
                // take gets the proper Stop & Quit, not a bare teardown.
                if (self != nullptr
                    && (self->engine.isRecording() || self->captureSupervisor.isDaemonRecording()))
                {
                    self->confirmAndQuit();
                    return;
                }
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

    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([self, aw, activeDir] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);

            if (result == kCancel || self == nullptr)
                return;                              // abort the quit, stay in app

            if (self->pendingMetadataSaves != 0 || self->engine.isRecording())
            { self->showStatus ("Quit cancelled -- recording or saving began while the dialog was open"); return; }
            if (result == kSave)
            {
                self->metadataContinuationReserved = true;
                self->setEnabled (false);
                self->enqueueMetadataSave (activeDir, {}, [self] (bool ok)
                {
                    if (self == nullptr) return;
                    self->metadataContinuationReserved = false;
                    self->setEnabled (true);
                    if (ok)
                        if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
                });
                return;
            }
            if (result == kDontSave)
                if (auto* app = juce::JUCEApplication::getInstance()) app->quit();
        }),
        false);
}

static juce::String checklistFormatName (zynforge::CaptureFormat format)
{
    using F = zynforge::CaptureFormat;
    return format == F::Wav24       ? "WAV/24"
         : format == F::Wav16       ? "WAV/16"
         : format == F::Wav32Float  ? "WAV/32f"
         : format == F::Aiff24      ? "AIFF/24"
         : format == F::Aiff16      ? "AIFF/16"
         : format == F::Aiff32Float ? "AIFF/32f"
         : format == F::Flac24      ? "FLAC/24"
         : format == F::Flac16      ? "FLAC/16"
                                    : "?";
}

juce::String MainComponent::backupChecklistRow()
{
    const auto backupDir = engine.getRecorder().getBackupDirectory();
    const bool backupConfigured = backupDir.getFullPathName().isNotEmpty();
    const bool backupWriterActive = captureSupervisor.isDaemonRecording()
                                      ? captureSupervisor.lastStatus().backupActive
                                      : engine.getRecorder().isBackupActive();
    const bool backupWritable = ! engine.isRecording() && backupConfigured
                                  && zynforge::preflight::volumeWritable (backupDir);
    using BackupState = zynforge::preflight::RedundancyReadiness;
    const auto backupState = zynforge::preflight::redundancyReadiness (
        engine.isRecording(), backupConfigured, backupWritable, backupWriterActive);
    const auto format = checklistFormatName (engine.getRecorder().getBackupCaptureFormat());
    juce::String row;
    row << (backupState == BackupState::Active
              || backupState == BackupState::ConfiguredWritable ? "[OK]  " : "[X]  ")
        << "Backup:      ";
    if (backupState == BackupState::NotConfigured) row << "not configured";
    else if (backupState == BackupState::ConfiguredWritable)
        row << backupDir.getFullPathName() << " (configured + writable, " << format << ")";
    else if (backupState == BackupState::ConfiguredUnavailable)
        row << backupDir.getFullPathName() << " (NOT writable / missing, " << format << ")";
    else if (backupState == BackupState::Active) row << "writer active (" << format << ")";
    else row << "configured but NOT WRITING (" << format << ")";
    return row;
}

void MainComponent::showPreflightChecklist()
{
    // Every check below goes to the drives, from this thread.
    if (engine.isTakeOnDisk())
    {
        showStatus ("Pre-flight reads the drives -- run it before the take or after STOP");
        return;
    }
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
    const auto root   = zynforge::preflight::storageRoot (sess, getSessionsRoot());
    const double freeGB = (root.exists() ? root.getBytesFreeOnVolume() : 0) / 1.0e9;
    const auto fmt    = engine.getRecorder().getCaptureFormat();
    const auto fmtStr = checklistFormatName (fmt);

    // Headroom at the active configuration, by the rule RECORD refuses on, so
    // this list can never call a disk fine that RECORD then turns away.
    const auto wantBytes = recordHeadroomBytes (sr, juce::jmax (1, armed), fmt);
    const double minHeadroom = wantBytes > 0 ? (freeGB * 1.0e9 / (double) wantBytes) * 30.0 : 0.0;

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
         << (cues.empty() ? "[i]  Setlist cues: none (optional)" : chk (true) + "Setlist cues: " + juce::String ((int) cues.size()))
         << "\n"
         << "\n"
         << chk (preflightDiskOk (freeGB, wantBytes, armed), freeGB > 1.0)
         << "Disk free:    " << juce::String (freeGB, 1) << " GB ("
         << juce::String (juce::roundToInt (minHeadroom)) << " min headroom at " << fmtStr << ")\n"
         << chk (! engine.isRecording())
         << "Status:       " << (engine.isRecording() ? juce::String ("RECORDING ROLLING")
                                                       : engine.getPlayer().isPlaying()
                                                           ? juce::String ("Playing back")
                                                           : juce::String ("Idle"))
         << "\n";

    body << backupChecklistRow();
    body << "\n\n";

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

    // Asked on the probe's own thread: a stalled driver costs this checklist
    // the wait below, and then reads as "not available".
    constexpr int kClockReadWaitMs = 250;
    body << zynforge::clocksource::preflightRow (engine.readClockSource (kClockReadWaitMs)) << "\n";

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
    // for filesystem jitter mid-show).
    const double needMBps = zynforge::preflight::requiredWriteMBps (sr, juce::jmax (1, armed),
                                                                    captureBytesPerSample (fmt));
    if (engine.isRecording())
    {
        // NEVER probe mid-take. measureWriteSpeedMBps writes 16 MB
        // synchronously, on the message thread, into the volume the take is
        // streaming to -- it steals write bandwidth from the capture (a real
        // dropout risk) and freezes the UI for the duration. Pre-flight is a
        // pre-show tool; while rolling we report the requirement only.
        body << "[!]  Disk write speed: not measured while recording (the probe "
                "would write 16 MB to the take's volume)  --  need "
             << juce::String (needMBps, 1) << " MB/s for "
             << juce::jmax (1, armed) << " ch\n";
    }
    else if (hasSes || root.isDirectory())
    {
        const auto speedDir = hasSes ? sess : root;
        const double gotMBps = zynforge::preflight::measureWriteSpeedMBps (speedDir);
        body << chk (gotMBps > needMBps * 1.5, gotMBps > needMBps)
             << "Disk write speed (measured): " << juce::String (juce::roundToInt (gotMBps))
             << " MB/s  (need " << juce::String (needMBps, 1) << " MB/s for "
             << juce::jmax (1, armed) << " ch)\n";
    }

    // Every configured mirror drive: mounted AND actually writable.
    for (const auto& m : engine.getMirrors())
    {
        if (engine.isRecording())
            body << "[!]  Mirror " << m.root.getFullPathName()
                 << ": write probe skipped while recording\n";
        else
        {
            const bool w = zynforge::preflight::volumeWritable (m.root);
            body << chk (w)
                 << "Mirror " << m.root.getFullPathName()
                 << (w ? juce::String (": writable") : juce::String (": NOT writable / missing")) << "\n";
        }
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
