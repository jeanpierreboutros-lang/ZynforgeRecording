// MainComponent's 10 Hz refresh callback + transport-label sync.
// Extracted from MainComponent.cpp as part of the 2026-05-24 god-
// class split so the high-traffic timer path is editable without
// scrolling past 5000 lines of menu / layout / setup code.

#include "MainComponent.h"
#include "AudioDeviceDialog.h"
#include "../Audio/VirtualSoundcheck.h"
#include "RecordPosition.h"

using namespace zynforge;

static juce::String samplesToTimecode (juce::int64 samples, double sr)
{
    if (sr <= 0.0) return "00:00";
    const auto seconds = (juce::int64) (samples / sr);
    return juce::String::formatted ("%02lld:%02lld", seconds / 60, seconds % 60);
}

void MainComponent::timerCallback()
{
    // The 10 Hz UI heartbeat (startTimerHz in MainComponentInit). Each step is
    // a named method below; order matters (strips rebuild before they are
    // synced, the status snapshot is taken once and feeds the header and the
    // show-day warnings).
    serviceCaptureDaemonLink();
    twinLink.tick();
    updateTwinStatusLabel();
    // Every way a session can change passes here within a tick.
    consoleLink.setSessionDir (engine.getActiveSessionDir(), ! engine.isTakeOnDisk());
    // Keep the macOS menu's greyed/enabled states in sync with reality
    // (Undo/Redo, Edit, Track, Export) -- they're cached until we ask for a
    // refresh, so without this they freeze in the empty launch state.
    refreshMenuStateIfChanged();
    // Coalesce any mixer change into a single undo step once it settles.
    pollMixerUndo();
    // Periodic auto-save + timestamped backup of the active session.
    serviceAutosave();
    syncFloatingPanelButtons();
    rebuildStripsIfLayoutChanged();
    updateStatusPills();
    serviceChosenDevice();
    updatePunchPill();
    updateSoundcheckState();
    updateNextCueCountdown();

    if (uiLayoutSavePending && ! sessionIoBusy.load()) requestUILayoutSave();

    cancelAnalysisForTake();
    if (! engine.isRecording()) punchWindowTakeActive = false;
    if (engine.isPunchModeOn())
        servicePunch();
    if (punchSessionActive)
        servicePunchSession();

    syncStripsWithEngine();
    updateTransportLabels();

    const bool playing = engine.isPlaying();
    surfaceAsyncReportFailure();

    const double deviceSR = [this]() -> double
    {
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
            return d->getCurrentSampleRate();
        return 48000.0;
    }();
    // Warn if the device's actual rate (e.g. the incoming Dante / wordclock)
    // doesn't match the session's intended rate -- a silent rate mismatch
    // would record everything at the wrong speed / pitch.
    checkDeviceSampleRate (deviceSR);
    // External timecode chase: drive the transport from incoming LTC / MTC.
    serviceTimecodeChase();
    serviceTrimFollowNotice();
    serviceBusRoutingNotice();

    // Auto-arm on input detect -- only meaningful while NOT recording. Arms a
    // strip after 12 ticks (1.2 s at 10 Hz) of input above -40 dBFS. Cheap;
    // skipped instantly when the flag is off.
    if (! engine.isRecording())
        engine.serviceAutoArm (12, 0.01f /* -40 dBFS */);

    updateClockAndDiskCadence (deviceSR);

    // Phase-0 status boundary: take ONE EngineStatus snapshot and feed the
    // header readouts and warnings from it. captureStatus applies the daemon's
    // receive timestamp and link-loss invalidation; reading lastStatus directly
    // would paint a dead daemon's final healthy metrics indefinitely.
    const auto status = engine.captureStatus();
    updateHeaderFromStatus (status, playing);
    updateCaptureWarnings (status);
}

void MainComponent::serviceCaptureDaemonLink()
{
    // Capture-daemon watchdog + status (Phase 1d/2); cheap when inactive. tick()
    // is state-based, so the second call the old body made per tick was redundant.
    if (useCaptureDaemon)
    {
        captureSupervisor.tick();
        serviceLostDaemon();
        const auto receivedAt = captureSupervisor.lastStatusAtMs();
        if (captureSupervisor.isAttached() && receivedAt != 0)
        {
            const auto daemonStatus = captureSupervisor.lastStatus();
            engine.setExternalCaptureStatus (daemonStatus, receivedAt);
            // A delayed status must not turn a disconnected/unknown take into
            // a healthy recording or clear a take whose state is unknown.
            if (juce::Time::currentTimeMillis() - receivedAt <= 2000)
            {
                if (daemonStatus.recording)
                {
                    if (! daemonTakeAdopted) adoptDaemonTake (daemonStatus, true);
                }
                else if (daemonStartUnansweredMs != 0)
                {
                    // A status from before the daemon reached the START proves
                    // nothing, so only time settles this one.
                    if (juce::Time::getMillisecondCounter() - daemonStartUnansweredMs >= 10000)
                        giveUpOnUnansweredStart();
                }
                else if (engine.isExternalRecording() && ! captureMetadataPending)
                    // It ended without a STOP from here. Nothing else would load it.
                    endDaemonTake (daemonStatus.captureDeviceLost
                                       ? "Capture device stopped during the take; the daemon closed it"
                                       : "", false, {});
            }
        }
        // Keep idle capture pre-roll following the GUI's device/arm layout.
        // Identical configurations are cached and cause no IPC or reinitialisation.
        if (! engine.isRecording() && captureSupervisor.isAttached()) configureCaptureDaemon();
    }
}

void MainComponent::syncFloatingPanelButtons()
{
    // Header "tab" buttons (DEVICE / PATCH / METERS) light up while their
    // floating panel is open and clear when it closes. The non-modal panels
    // only HIDE themselves on their close box, so reap any hidden one here
    // (delete + forget) so the button state -- and memory -- stays correct
    // whether the engineer closed it via the tab or the panel's own X.
    auto syncTab = [] (juce::Component::SafePointer<juce::DialogWindow>& dlg,
                       zynforge::IconButton& btn)
    {
        if (auto* w = dlg.getComponent())
            if (! w->isVisible() && ! w->isMinimised())   // closed via its X (not just minimised)
                { delete w; dlg = nullptr; }
        btn.setToggleState (dlg.getComponent() != nullptr, juce::dontSendNotification);
    };
    syncTab (deviceDialog, deviceButton);
    syncTab (patchDialog,  patchButton);
    syncTab (metersDialog, metersButton);
}

void MainComponent::rebuildStripsIfLayoutChanged()
{
    const int n = engine.getRecorder().getNumTracks();
    // Rebuild when the track COUNT changes OR the track SET was replaced
    // (device restart recreates TrackStates at the same count) -- either
    // way the cached TrackState& in each strip would otherwise dangle.
    if (n != lastTrackCount
        || engine.getRecorder().getTrackGeneration() != lastTrackGen)
        rebuildStrips();
}

void MainComponent::serviceChosenDevice()
{
    const auto now = juce::Time::getMillisecondCounter();
    if ((juce::int32) (now - nextDeviceCheckMs) < 0) return;
    nextDeviceCheckMs = now + 1000;

    const auto state = engine.getDeviceState();
    showDeviceState (state);

    const bool transportQuiet = ! engine.isRecording() && ! engine.isCaptureFinalizing() && ! engine.isPlaying()
                             && ! punchSessionActive && ! sessionIoBusy.load()
                             && ! zynforge::AudioDeviceDialog::isOpen() && deviceRecordQuestion == nullptr;
    if (! zynforge::devicechoice::shouldReopen (state, transportQuiet) || ! deviceReopenPacing.due (now)) return;

    juce::String error;
    if (engine.reopenChosenDevice (error))
    {
        deviceReopenPacing.reset();
        showDeviceState (engine.getDeviceState());
        showStatus ("Back on " + state.chosenNow);
    }
    else
    {
        deviceReopenPacing.failed (now);
        showStatus (state.chosenNow + " is connected but would not open: " + error);
    }
}

void MainComponent::showDeviceState (const zynforge::devicechoice::DeviceState& state)
{
    using namespace zynforge::devicechoice;
    const bool wrong = isWrongDevice (state);
    deviceLabel.setText (labelText (state), juce::dontSendNotification);
    deviceLabel.setColour (juce::Label::textColourId, wrong ? brand::alertAmber : brand::textTertiary);
    const auto line = wrongDeviceLine (state);
    deviceLabel.setTooltip (line.isNotEmpty() ? line + ". RECORD asks before it rolls."
                                              : state.active + " is the input device a take records from.");
    // Said once, when it happens. A rig left unplugged during prep stays amber
    // above, but it must not keep taking the status line away.
    if (line != deviceLineShown)
    {
        deviceLineShown = line;
        if (line.isNotEmpty()) showStatus (line);
    }
}

void MainComponent::updateStatusPills()
{
    // DANTE detection -- flag the active audio device when its name
    // contains 'Dante' / 'DVS' (Audinate's Dante Virtual Soundcard).
    {
        juce::String txt;
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
        {
            const auto deviceName = d->getName();
            if (deviceName.containsIgnoreCase ("Dante") || deviceName.containsIgnoreCase ("DVS"))
                txt = "DANTE * " + deviceName;
        }
        if (danteLabel.getText() != txt)
        { danteLabel.setText (txt, juce::dontSendNotification); resized(); }
        danteLabel.setTooltip (txt);
    }
}

// PUNCH mode must read from across the room, and a menu tick does not. The
// quieter state is the same warning about the next RECORD: it will replace
// audio, not add to it. The answer comes from the rules RECORD itself uses.
void MainComponent::updatePunchPill()
{
    auto& player = engine.getPlayer();
    juce::String text;
    if (engine.isPunchModeOn() && ! punchSessionActive)
        text = "PUNCH MODE";
    else if (! engine.isRecording() && player.isLoaded() && ! daemonModeActive())
    {
        const bool rolling = player.isPlaying();
        const auto cursor  = engine.getEditCursorSample();
        const auto pos     = manualRecordPosition (rolling, player.getPositionSamples(), cursor);
        if (player.isRangeArmed())
            text = "RECORD PUNCHES RANGE";
        // The timeline is never shorter than a take, so no take length is
        // needed here. Reading one takes the lock the audio thread only tries,
        // and a miss there is a block of silence on every output.
        else if (manualRecordPunches (rolling, cursor, pos, 0, player.getTotalLengthSamples()))
            text = rolling ? "RECORD PUNCHES HERE" : "RECORD PUNCHES AT CURSOR";
    }
    if (punchPill.getText() == text) return;
    punchPill.setText (text, juce::dontSendNotification);
    punchPill.setVisible (text.isNotEmpty());
    const bool mode = text == "PUNCH MODE";
    punchPill.setColour (juce::Label::backgroundColourId,
                         mode ? brand::alertAmber : juce::Colours::transparentBlack);
    punchPill.setColour (juce::Label::textColourId,
                         mode ? brand::onSignal (brand::alertAmber) : brand::alertAmber);
    resized();
}

// Both states are read every tick rather than set where they change: a session
// open, an undo and a console reply can each change them.
void MainComponent::updateSoundcheckState()
{
    const bool outputsMoved = zynforge::vsc::hasSavedPatch (engine);
    if (vscButton.getToggleState() != outputsMoved)
    {
        vscButton.setToggleState (outputsMoved, juce::dontSendNotification);
        vscButton.setButtonText (outputsMoved ? "VSC ON" : "VSC");
    }

    const bool deskOnCard = consoleLink.isOnSoundcheckPatch();
    if (soundcheckPill.isVisible() != deskOnCard)
    {
        soundcheckPill.setVisible (deskOnCard);
        resized();
    }
}

void MainComponent::updateNextCueCountdown()
{
    // LCD countdown to next cue -- only when a setlist exists AND the
    // player is rolling. Pre-show or scrub state shows blank so the
    // chip doesn't lie about a still session.
    {
        juce::String txt;
        if (engine.getPlayer().isPlaying() && ! cues.empty())
        {
            const auto pos = engine.getPlayer().getPositionSamples();
            const auto sr  = engine.getPlayer().getSampleRate();
            const zynforge::SetlistBar::Cue* next = nullptr;
            for (const auto& c : cues)
                if (c.samplePos > pos) { next = &c; break; }
            if (next != nullptr && sr > 0.0)
            {
                const double secs = (double) (next->samplePos - pos) / sr;
                const int totalSec = juce::jmax (0, (int) secs);
                const int mins  = totalSec / 60;
                const int secsR = totalSec % 60;
                txt = "Next: " + next->name + " in " + juce::String (mins)
                    + juce::String::formatted (":%02d", secsR);
            }
        }
        if (nextCueLabel.getText() != txt)
            nextCueLabel.setText (txt, juce::dontSendNotification);
    }

    // The first cue brings the setlist row in; deleting the last takes it away.
    if (cues.empty() != setlistBar.isCollapsed()) resized();
}

void MainComponent::syncStripsWithEngine()
{
    // Keep each strip's input/output combos in sync with engine state --
    // the PATCH page can mutate routing behind the strip's back. Also
    // refresh name + colour so changes made from the EDIT view show up. A
    // device change moves neither the strip count nor the generation, and what
    // a combo reads depends on the channels this device has.
    const int inputs  = engine.getCurrentDeviceInputCount();
    const int outputs = engine.getCurrentDeviceOutputCount();
    const bool deviceChanged = inputs != stripsInputs || outputs != stripsOutputs;
    stripsInputs  = inputs;
    stripsOutputs = outputs;
    for (size_t k = 0; k < strips.size(); ++k)
    {
        auto& s = strips[k];
        if (s == nullptr) continue;
        if (deviceChanged)
        {
            s->setAvailableInputs  (inputs);
            s->setAvailableOutputs (outputs);
        }
        s->refreshRoutingSelection();
        s->refreshAppearance();
    }
}

void MainComponent::surfaceAsyncReportFailure()
{
    auto& recorder = engine.getRecorder();
    if (recorder.isRecording()) asyncReportFailureShown = false;
    else if (recorder.hasAsyncReportFailure() && ! asyncReportFailureShown)
    {
        asyncReportFailureShown = true;
        showStatus ("Integrity report hashing or final write failed; check session.report.json and the take files");
    }
}

void MainComponent::updateClockAndDiskCadence (double deviceSR)
{
    auto& recorder = engine.getRecorder();
    auto& player   = engine.getPlayer();
    auto& markers  = engine.getMarkers();
    BigClockPanel::Mode m = BigClockPanel::Mode::Idle;
    juce::int64 elapsed = 0;
    double      timerSR = deviceSR;

    if (engine.isRecording())
    {
        m = BigClockPanel::Mode::Recording;
        // Timeline position (continues from the take end on a continue), not the
        // new file's raw length, so the clock carries on instead of restarting.
        elapsed = takeClockSamples();
        timerSR = takeClockRate (deviceSR);
        shownTakeRate = timerSR;

        // Refresh the idle minutes-remaining estimate every 48 ticks (4.8 s at
        // the 10 Hz timer). A take reads its own volume probes instead.
        if ((++diskTick % 48) == 0)
            engine.refreshDiskMinutesRemaining();
        // Disk keep-up every 12 ticks (1.2 s): three consecutive smoothed
        // samples below ~85 % of the expected bytes/sec (a few seconds,
        // plus EMA lag) flip the "disk struggling" flag so the status bar
        // warns before missed samples start landing.
        // The rate is only wanted for a take in this process, and with no take
        // here it would stat a backup path that may be why a take is struggling.
        if ((diskTick % 12) == 0)
            recorder.updateDiskHealth (recorder.isRecording()
                                           ? recorder.estimateBytesPerSecondForArmedTracks() : 0);
        // Slow SMART status poll -- every 720 ticks (72 s at 10 Hz), and
        // only when recording. Dispatched to a background thread: `diskutil
        // info` blocks for 50-200 ms normally and can hang outright on an
        // unresponsive volume, which froze the UI mid-take when it ran here.
        // Failing status flips a hard warning.
        if ((diskTick % 720) == 0)
            pollSmartStatusAsync();
    }
    else if (engine.isPlaying())
    {
        m = BigClockPanel::Mode::Playing;
        elapsed = player.getPositionSamples();
        timerSR = player.getSampleRate();
    }

    bigClock.setMode (m);
    bigClock.setElapsed (elapsed, timerSR);
    bigClock.setMarkers (markers.getCount());

    bool anyArmed = false;
    for (int i = 0, count = recorder.getNumTracks(); i < count; ++i)
    {
        if (recorder.getTrack (i).armed.load (std::memory_order_relaxed))
        {
            anyArmed = true;
            break;
        }
    }
    bigClock.setArmedReady (anyArmed && ! engine.isRecording() && ! engine.isPlaying());
}

void MainComponent::updateHeaderFromStatus (const zynforge::EngineStatus& status, bool playing)
{
    // captureStatus applies the daemon's receive timestamp and link-loss
    // invalidation. Reading lastStatus directly here would bypass both and
    // paint a dead daemon's final healthy metrics indefinitely.
    const bool rec = status.recording;
    // Capture locks timeline mutations, but navigation must remain available
    // to inspect a rolling take or a pending metadata save. Media jobs still
    // own the entire editor; metadata persistence only locks its mutations.
    // JUCE delivers mouse events to a disabled component, so a job needs the
    // read-only lock as well: it is what makes the rows click-through.
    if (editPage != nullptr)
    {
        editPage->setReadOnlyWhileRecording (rec || engine.isRecording() || sessionIoBusy.load()
                                             || captureMetadataPending || sessionLocked);
        const bool enableEditor = (! sessionIoBusy.load() || pendingMetadataSaves != 0 || captureMetadataPending) && ! sessionLocked;
        if (editPage->isEnabled() != enableEditor)
            editPage->setEnabled (enableEditor);
    }

    // Free space on the volume the take ACTUALLY lands on. This used to
    // hardcode ~/Music/Zynforge Sessions, so an engineer recording to an
    // external drive (the `sessionsRoot` override, or any session opened from
    // elsewhere) watched the free-space figure for the wrong disk all show.
    // The query is a stat + statfs on the session volume -- the drive taking
    // the heaviest writes during a show, often a slow USB/exFAT or network
    // volume where either call can stall. Free space at GB resolution does
    // not need 10 Hz: re-query at most every 2 s, or at once when the session
    // directory changes.
    if (engine.isTakeOnDisk())
    {
        cachedFreeGB = status.primaryFreeBytes >= 0 ? bytesToGB (status.primaryFreeBytes) : -1.0;
        lastDiskQueryMs = 0;
    }
    else
    {
        const auto active = engine.getActiveSessionDir();
        const auto nowMs  = juce::Time::getMillisecondCounter();
        if (active != diskQueryDir || nowMs - lastDiskQueryMs >= 2000 || lastDiskQueryMs == 0)
        {
            diskQueryDir    = active;
            lastDiskQueryMs = juce::jmax ((juce::uint32) 1, nowMs);
            const auto sessRoot = active.isDirectory() ? active : getSessionsRoot();
            const auto bytesFree = sessRoot.exists() ? sessRoot.getBytesFreeOnVolume()
                                                      : juce::File ("/").getBytesFreeOnVolume();
            cachedFreeGB = bytesToGB (bytesFree);
        }
    }
    const double freeGB = cachedFreeGB;

    // This estimate groups copies by physical volume and selects the first
    // volume to fill. Dividing one drive's free space by the aggregate write
    // rate makes independent backup/mirror drives look needlessly scarce.
    const double remainingSec = (double) status.minutesRemaining * 60.0;

    bigClock.setDiskInfo (freeGB,
                          status.lastWriteMs,
                          status.missedSamples,
                          remainingSec,
                          status.primaryDiskStale ? BigClockPanel::DiskReading::PrimaryStale
                          : status.copyDiskStale  ? BigClockPanel::DiskReading::CopyStale
                                                  : BigClockPanel::DiskReading::Fresh);
    perfDashboard.setLoudness (status.integratedLufs,
                               status.momentaryLufs,
                               status.truePeakDb);
    perfDashboard.setMetrics (status.audioLoadPct,
                              (float) status.diskMBPerSec,
                              status.ringFillPct,
                              status.missedSamples);
    perfDashboard.setDeviceInfo (status.sampleRate,
                                 status.blockSize);
    perfDashboard.setRecording (rec);
    updateBackupButton (status);
    for (auto& strip : strips) strip->setTakeRolling (rec);
    // Drives per-strip meter refresh: full rate while recording or
    // playing, throttled to ~8 Hz when stopped (see TrackState.h).
    gTransportActive.store (rec || playing, std::memory_order_relaxed);
    // Re-lay-out the header once when REC toggles so the dashboard resizes.
    if (rec != dashWasRecording)
    {
        dashWasRecording = rec;
        resized();
    }
}

// What the header says about the backup is read fresh, never remembered from
// the dialog that picked the folder. A take's writers know how it is going.
// Between takes a probe thread watches the folder, so a dead mount never reaches
// this tick.
void MainComponent::updateBackupButton (const zynforge::EngineStatus& status)
{
    constexpr int kProbePeriodMs = 2000, kProbeStaleMs = 6000;
    const auto folder = engine.getRecorder().getBackupDirectory();
    const bool configured = folder.getFullPathName().isNotEmpty();
    if (folder != backupProbeFolder)
    {
        backupProbeFolder = folder;
        backupProbe.reset (configured ? new VolumeProbe (folder, VolumeProbe::readFolder, kProbePeriodMs) : nullptr);
    }

    bool ok = false;
    if (status.recording) ok = status.backupActive;
    else if (backupProbe != nullptr)
    {
        const auto reading = backupProbe->latest();
        if (reading.atMs <= 0.0) return;   // no answer yet, so nothing new to say
        ok = reading.totalBytes > 0 && juce::Time::getMillisecondCounterHiRes() - reading.atMs < kProbeStaleMs;
    }
    const juce::String text (! configured ? "BACKUP" : ok && ! status.backupFailed ? "BACKUP OK" : "BACKUP !");
    if (backupButton.getButtonText() != text) backupButton.setButtonText (text);
}

void MainComponent::updateCaptureWarnings (const zynforge::EngineStatus& status)
{
    constexpr int kRecordTimeNoticeMinutes = 30, kRecordTimeDangerMinutes = 10;
    const bool rec = status.recording;
    // The recorder holds this until the next take, but it belongs to the
    // session it happened in.
    if (rec || ! status.captureDeviceLost) deviceLossForgotten = false;
    deviceLossLatched = status.captureDeviceLost;
    const bool deviceLost = status.captureDeviceLost && ! deviceLossForgotten;
    // Live show-day warnings -- only while recording, refreshed at the
    // timer cadence. Both flags clear automatically when the condition
    // recovers (e.g. swap drives + the new primary write succeeds, or
    // disk catches back up). Status bar is the engineer's first read.
    if (rec || deviceLost || (useCaptureDaemon && engine.isRecording()))
    {
        const bool primFail   = status.primaryFailed;
        const bool backupFail = status.backupFailed;
        const bool mirrorFail = status.mirrorFailed;
        const bool recoveryFail = status.recoveryMarkerFailed;
        const bool mixFail = ! useCaptureDaemon && engine.hasStereoMixWriteFailed();
        const bool diskTrouble = status.diskStruggling;
        const bool samplesMissed = status.missedSamples > 0;
        // A daemon closing its files answers no status until it is done.
        const bool daemonUnavailable = status.source == "daemon-unavailable" && ! captureSupervisor.isStopInFlight();
        const bool smartBad = (smartPrimaryStatus == (int) MultitrackRecorder::SmartStatus::Failing)
                           || (smartBackupStatus  == (int) MultitrackRecorder::SmartStatus::Failing);
        // A mirror that never OPENED (drive absent, or a root that would have
        // collided with the take) creates no Mirror entry, so anyMirrorFailed()
        // -- which walks the entries -- structurally cannot see it. Without this
        // the engineer runs the whole show believing they have a copy they don't.
        const int mirrorsSkipped = status.mirrorsSkipped;
        const auto inputAlarms = channelalarm::bannerText (status.channelAlarms);
        const bool primaryNotAnswering = status.primaryDiskStale && status.source != "daemon-unavailable";
        // RECORD only asks for 30 minutes of room, so a long show can get here.
        const int minutesLeft = rec ? status.minutesRemaining : 0;   // 0 = no estimate
        const bool timeRunningOut = minutesLeft > 0 && minutesLeft <= kRecordTimeDangerMinutes;
        const bool timeShort      = minutesLeft > kRecordTimeDangerMinutes && minutesLeft <= kRecordTimeNoticeMinutes;
        const bool deviceStalled = status.deviceStalled || status.deviceStallMs > 0;
        // Only a desk that was answering. One that never did may simply not
        // answer probes, and a notice that is always up teaches nothing.
        const bool consoleSilent = consoleLink.isConnected()
                                && consoleLink.getHealth() == zynforge::ConsoleLink::Health::Silent;
        if (primaryNotAnswering || status.copyDiskStale || daemonUnavailable || deviceLost || deviceStalled || primFail || backupFail || mirrorFail || recoveryFail || mixFail
            || diskTrouble || samplesMissed || smartBad || mirrorsSkipped > 0 || inputAlarms.isNotEmpty()
            || ! status.copyProblems.isEmpty() || timeRunningOut || timeShort || consoleSilent || status.preRollNotice.isNotEmpty())
        {
            // Most serious first. Two spaces end each alarm: that is how the
            // banner tells them apart, and a '!' is what makes one a danger.
            juce::String warn;
            if (deviceLost) warn << "!! CAPTURE DEVICE STOPPED -- take ended; inspect audio and reconnect  ";
            if (status.deviceStalled) warn << "!! NO AUDIO FROM THE DEVICE -- the take is open but nothing is being captured; check the interface  ";
            else if (deviceStalled)   warn << "! DEVICE STOPPED FOR " << juce::String (status.deviceStallMs / 1000.0, 1)
                                           << " s -- that time is not in the take  ";
            if (daemonUnavailable) warn << "!! DAEMON STATUS UNAVAILABLE -- take may still be rolling; reconnect and verify  ";
            if (primFail)    warn << "! PRIMARY WRITE FAILED -- recording on backup/mirror  ";
            if (primaryNotAnswering) warn << "! PRIMARY DRIVE NOT ANSWERING -- free space unknown  ";
            if (samplesMissed) warn << "! " << status.missedSamples << " SAMPLES MISSED -- the take has gaps  ";
            if (timeRunningOut) warn << "! " << recordTimeText (minutesLeft * 60.0) << " OF RECORD TIME LEFT -- a drive is nearly full  ";
            if (timeShort) warn << recordTimeText (minutesLeft * 60.0) << " of record time left  ";
            warn << inputAlarms;
            if (smartBad)    warn << "! SMART FAILING -- replace this drive  ";
            if (backupFail)  warn << "! BACKUP WRITE FAILED -- primary recording continues  ";
            if (mirrorFail)  warn << "! MIRROR WRITE FAILED -- check mirror drive  ";
            if (recoveryFail) warn << "! CRASH-RECOVERY MARKER NOT WRITING -- protect power  ";
            if (mixFail)      warn << "! STEREO MIX NOT WRITING -- multitracks continue  ";
            if (mirrorsSkipped > 0)
                warn << "! " << mirrorsSkipped << " MIRROR"
                     << (mirrorsSkipped == 1 ? "" : "S") << " NOT WRITING -- drive missing "
                                                            "or folder unusable  ";
            if (status.copyDiskStale) warn << "! A COPY'S DRIVE IS NOT ANSWERING -- its free space is unknown  ";
            for (const auto& problem : status.copyProblems) warn << "! " << problem << "  ";
            if (diskTrouble) warn << "! DISK STRUGGLING -- missed samples imminent  ";
            if (consoleSilent) warn << "CONSOLE NOT ANSWERING -- scene markers have stopped  ";
            warn << status.preRollNotice;
            activeCaptureWarning = warn.trim();
            statusLabel.setText (activeCaptureWarning, juce::dontSendNotification);
        }
        else if (activeCaptureWarning.isNotEmpty())
        {
            if (statusLabel.getText() == activeCaptureWarning)
                statusLabel.setText (rec ? "Recording" : "Idle", juce::dontSendNotification);
            activeCaptureWarning.clear();
        }
    }
    else if (activeCaptureWarning.isNotEmpty())
    {
        if (statusLabel.getText() == activeCaptureWarning)
            statusLabel.setText ("Idle", juce::dontSendNotification);
        activeCaptureWarning.clear();
    }
    refreshAlarmBanner();
}

void MainComponent::refreshAlarmBanner()
{
    constexpr juce::uint32 kRefusalShownMs = 8000;
    if (recordRefusal.isNotEmpty()
        && (engine.isRecording()
            || juce::Time::getMillisecondCounter() - recordRefusalShownMs > kRefusalShownMs))
        recordRefusal.clear();

    // Red is for a take in trouble, so it still means something when it shows.
    // A refused press and the quiet "no signal yet" notice, which has no '!',
    // are amber. A failed finalization stays up after STOP, until the next take
    // or another session. The banner covers everything else in this row, so
    // whatever would be hidden under it goes in here.
    using Alarm = AlarmBanner::Alarm;
    using Level = AlarmBanner::Level;
    std::vector<Alarm> alarms;
    for (const auto& alarm : AlarmBanner::splitAlarms (activeCaptureWarning))
        alarms.push_back ({ alarm, alarm.startsWithChar ('!') ? Level::Danger : Level::Notice });
    if (lastCaptureFinalizationError.isNotEmpty())
        alarms.push_back ({ lastCaptureFinalizationError, Level::Danger });
    // Nothing can roll at the wrong rate from idle: RECORD refuses. A take that
    // is already rolling is the one in trouble.
    if (sampleRateWarning.isNotEmpty())
        alarms.push_back ({ sampleRateWarning, engine.isRecording() ? Level::Danger : Level::Notice });
    collectTwinAlarms (twinLink.snapshot(), alarms);
    if (recordRefusal.isNotEmpty()) alarms.push_back ({ recordRefusal, Level::Notice });
    alarmBanner.show (std::move (alarms), juce::Time::getMillisecondCounter());
}

// While a take rolls on either Mac, a link in trouble means the second copy may
// not be there, and that raises the banner on its own. The rest only comes along
// on a banner already up: the header pill says it well enough until something
// covers the pill. A link that dropped reports no peer, so the peer's state is
// the last one heard.
void MainComponent::collectTwinAlarms (const zynforge::twin::Snapshot& link,
                                       std::vector<zynforge::AlarmBanner::Alarm>& alarms)
{
    using Level = AlarmBanner::Level;
    if (link.state == zynforge::twin::LinkState::Off) twinPeerRolling = false;
    else if (link.peer.has_value()) twinPeerRolling = link.peer->recording;
    const bool rolling = engine.isRecording() || twinPeerRolling;

    juce::StringArray trouble, quiet;
    if (link.level == 2)
        trouble.add ("! " + link.headline + (link.detail.isNotEmpty() ? " -- " + link.detail : juce::String()));
    else if (link.level == 1 && link.warnings.isEmpty())
        quiet.add (link.headline);
    for (int i = 0; i < link.warnings.size(); ++i)
    {
        if (i < link.setupWarnings) quiet.add ("TWIN: " + link.warnings[i]);
        else                        trouble.add ("! TWIN: " + link.warnings[i]);
    }

    if (rolling)
        for (const auto& alarm : trouble) alarms.push_back ({ alarm, Level::Danger });
    if (alarms.empty()) return;
    if (! rolling)
        for (const auto& alarm : trouble) alarms.push_back ({ alarm, Level::Notice });
    for (const auto& alarm : quiet) alarms.push_back ({ alarm, Level::Notice });
}

void MainComponent::showRecordRefusal (const juce::String& reason)
{
    recordRefusal = reason;
    recordRefusalShownMs = juce::Time::getMillisecondCounter();
    refreshAlarmBanner();
    alarmBanner.showPageHolding (reason, recordRefusalShownMs);
    // The banner is the one place for this. Only a reason too long for one page
    // also goes to the status line: a press never goes unanswered.
    if (! alarmBanner.getShownText().contains (reason)) showStatus (reason);
}

// Query SMART health for the session + backup volumes on a BACKGROUND thread.
// MultitrackRecorder::querySmartStatus shells out to `diskutil info`, which
// blocks for 50-200 ms on a healthy drive and can stall far longer on a network
// mount or a spun-down disk. Running it inline from timerCallback froze the
// message thread -- and therefore the whole UI -- in the middle of a take.
// Only ONE probe is ever in flight; the results land in atomics the timer reads.
void MainComponent::pollSmartStatusAsync()
{
    if (smartQueryInFlight.exchange (true, std::memory_order_acq_rel))
        return;   // a previous probe is still waiting on a slow drive

    const auto sessionDir = engine.getActiveSessionDir();
    const auto backupDir  = engine.getRecorder().getBackupDirectory();

    juce::Component::SafePointer<MainComponent> self (this);
    juce::Thread::launch ([self, sessionDir, backupDir]
    {
        const int prim = sessionDir.isDirectory()
            ? (int) MultitrackRecorder::querySmartStatus (sessionDir) : -1;
        const int back = backupDir.isDirectory()
            ? (int) MultitrackRecorder::querySmartStatus (backupDir)  : -1;

        // Publish on the message thread through the SafePointer so a quit
        // mid-probe can't write into a freed MainComponent.
        juce::MessageManager::callAsync ([self, prim, back]
        {
            if (self == nullptr) return;
            if (prim >= 0) self->smartPrimaryStatus.store (prim, std::memory_order_relaxed);
            if (back >= 0) self->smartBackupStatus .store (back, std::memory_order_relaxed);
            self->smartQueryInFlight.store (false, std::memory_order_release);
        });
    });
}

// Timeline position (continues from the take end on a continue), not the new
// file's raw length, so the clock carries on instead of restarting.
juce::int64 MainComponent::takeClockSamples()
{
    auto& player = engine.getPlayer();
    return punchSessionActive && player.isPlaying() ? player.getPositionSamples()
                                                    : engine.getRecordTimelineSamples();
}

double MainComponent::takeClockRate (double deviceRate)
{
    if (useCaptureDaemon && captureSupervisor.isDaemonRecording())
    {
        const auto remote = engine.captureStatus();
        if (remote.source == "daemon" && remote.sampleRate > 0.0)
            return remote.sampleRate;
    }
    return deviceRate;
}

void MainComponent::updateTransportLabels()
{
    auto& player = engine.getPlayer();
    // The session, not the player: a new session has no audio until its first
    // take has stopped.
    const bool sessionOpen = engine.hasSessionFolder();
    sessionLabel.setText (sessionOpen || player.isLoaded()
                              ? "Session: " + (sessionOpen ? engine.getActiveSessionDir().getFileName() : player.getSessionName())
                                    + " (" + juce::String (engine.getRecorder().getNumTracks()) + " tr)"
                              : juce::String ("No session loaded"),
                          juce::dontSendNotification);

    // The player stands still through a plain take, so this clock follows the
    // take instead, or it would sit frozen beside the big one.
    if (engine.isRecording())
    {
        transportLabel.setText (samplesToTimecode (takeClockSamples(), shownTakeRate), juce::dontSendNotification);
        return;
    }
    const auto sr  = player.getSampleRate();
    const auto pos = samplesToTimecode (player.getPositionSamples(), sr);
    const auto tot = samplesToTimecode (player.getTotalLengthSamples(), sr);
    transportLabel.setText (pos + " / " + tot, juce::dontSendNotification);
}

// Compare the device's live sample rate against the session's intended rate
// and warn once per distinct mismatch. A silent mismatch (e.g. the incoming
// Dante clock running 44.1k while the session was created at 48k) would
// capture every track at the wrong speed/pitch, so this is a data-integrity
// guard. We don't pop a modal mid-take (a status-line warning still shows);
// the dialog fires while stopped, which is when the engineer can fix it.
void MainComponent::checkDeviceSampleRate (double deviceSampleRate)
{
    auto setWarning = [this] (const juce::String& warning) { sampleRateWarning = warning; };
    if (deviceSampleRate <= 0.0) { setWarning ({}); return; }

    // With no session there is no rate to disagree with: 48 kHz here is only a
    // launch default, so the next session starts from what the device runs at.
    if (! engine.hasSessionFolder() && ! engine.getPlayer().isLoaded() && ! engine.isRecording())
    {
        pendingSampleRate = deviceSampleRate;
        engine.setSessionSampleRate (deviceSampleRate);
        srMismatchWarned = 0.0;
        setWarning ({});
        return;
    }

    // The session's authoritative rate: once a take is loaded that's the
    // recorded files' rate; otherwise it's the rate the session was created
    // at. Either way the comparison catches ANY divergence -- 44.1 / 48 /
    // 88.2 / 96 / 176.4 / 192 -- since it's a numeric difference, not a
    // fixed list.
    const double sessionSR = (engine.getPlayer().isLoaded()
                              && engine.getPlayer().getSampleRate() > 0.0)
                                 ? engine.getPlayer().getSampleRate()
                                 : pendingSampleRate;

    // Keep the engine's record-guard rate in lockstep with what we display,
    // so AudioEngine::startRecording refuses a mismatched take from ANY caller.
    engine.setSessionSampleRate (sessionSR);

    const auto fmt = [] (double sr)
    {
        const double k = sr / 1000.0;
        return ((k == std::floor (k)) ? juce::String ((int) k) : juce::String (k, 1)) + " kHz";
    };

    if (std::abs (deviceSampleRate - sessionSR) <= 1.0)
    {
        setWarning ({});   // matched
        srMismatchWarned = 0.0;                                 // re-arm the dialog
        return;
    }

    // Stands in the alarm banner while mismatched.
    // ASCII only -- sample-rate mismatch must stay legible in every UI face.
    // (it rendered as mojibake); the banner's colour carries the alarm.
    setWarning ("! SAMPLE-RATE MISMATCH  device " + fmt (deviceSampleRate)
                + "  vs  session " + fmt (sessionSR) + "  - recording blocked");

    // One-shot modal, once per distinct device rate, and never mid-take.
    if (std::abs (deviceSampleRate - srMismatchWarned) < 1.0 || engine.isRecording())
        return;
    srMismatchWarned = deviceSampleRate;

    auto* aw = new juce::AlertWindow ("Sample-Rate Mismatch",
        "The audio device is running at " + fmt (deviceSampleRate)
        + ", but this session is at " + fmt (sessionSR) + ".\n\n"
        "Recording or playing now would be at the wrong speed and pitch. "
        "Match the device clock (or the incoming Dante / wordclock) to "
        + fmt (sessionSR) + ", or start a new session at "
        + fmt (deviceSampleRate) + ".",
        juce::MessageBoxIconType::WarningIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE-default navy
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw] (int) { std::unique_ptr<juce::AlertWindow> dispose (aw); }), false);
}

// Say so when Trim-Follow is holding a channel back. Shown when the set
// changes, so a desk reading on a channel with no reference is reported as it
// arrives; the Session menu item keeps the count on show.
void MainComponent::serviceTrimFollowNotice()
{
    const auto notice = engine.isTrimFollowEnabled() ? engine.getTrimFollowNotice() : juce::String();
    if (notice == lastTrimFollowNotice) return;
    lastTrimFollowNotice = notice;
    if (notice.isNotEmpty()) showStatus (notice);
}

// A bus held off its outputs is a routing change the engineer did not ask for,
// so say so as it happens. The master strip keeps showing it.
void MainComponent::serviceBusRoutingNotice()
{
    const auto notice = engine.getBusRoutingNotice();
    if (notice == lastBusRoutingNotice) return;
    lastBusRoutingNotice = notice;
    if (notice.isNotEmpty()) showStatus (notice);
}

// Drive the transport from an external timecode master (LTC decoded off an
// audio input, or MTC over MIDI). When the chase source is live we follow it:
// start playback, and resync the playhead whenever it drifts more than ~80 ms
// from the incoming timecode (otherwise free-run so it doesn't stutter). When
// the master parks/stops, we stop the playback we started.
void MainComponent::serviceTimecodeChase()
{
    // Never chase during a live take. startPlayback is already guarded, but the
    // playhead re-seek below (setPositionSamples every tick) would still yank
    // the transport mid-record -- summing the old take into the monitor and
    // desyncing capture. House timecode often runs all night, so a chase left
    // enabled must stay inert while recording.
    if (engine.isRecording())
    {
        chaseWasLive = false;
        chaseParked  = false;
        return;
    }

    if (engine.getChaseMode() == zynforge::AudioEngine::ChaseMode::Off)
    {
        chaseWasLive = false;
        chaseParked  = false;
        return;
    }

    // LOCK holds the transport against every remote, chase included: it neither
    // starts, locates nor stops anything until the session is unlocked. A
    // session job owns the player, so chase waits for that a tick at a time.
    if (engine.areControlsLocked())
    {
        if (! chaseParked)
        {
            chaseParked = true;
            showStatus ("Chase is holding: session is locked");
        }
        return;
    }
    if (sessionIoBusy.load()) return;

    const bool live = engine.isChaseLive();
    const auto parkReason = live ? engine.getChaseParkReason() : juce::String();
    if (parkReason.isNotEmpty())
    {
        // Leave the playhead alone: a locate would clamp to the session end,
        // PLAY would refuse, and every manual locate would be undone a tenth
        // of a second later. Stop what chase started, once, and say why.
        if (! chaseParked)
        {
            chaseParked = true;
            if (chaseWasLive && engine.isPlaying()) engine.stopPlayback();
            showStatus (parkReason);
        }
    }
    else if (live)
    {
        chaseParked = false;
        const double sr = engine.getPlayer().getSampleRate() > 0.0
                            ? engine.getPlayer().getSampleRate() : pendingSampleRate;
        const juce::int64 target = engine.getChaseTargetSamples();

        // Locate BEFORE starting: startPlayback refuses with nothing ahead of
        // the playhead (STOP parks it at the take end).
        const juce::int64 cur   = engine.getPlayer().getPositionSamples();
        const juce::int64 slack = (juce::int64) (0.08 * sr);   // 80 ms lock window
        if (std::llabs ((long long) (cur - target)) > slack)
            engine.getPlayer().setPositionSamples (target);

        if (! engine.isPlaying())
            engine.startPlayback();
    }
    else if (chaseWasLive && engine.isPlaying() && ! engine.isKeepRollingOnSyncLoss())
    {
        // Timecode master stopped -> park the transport (unless the engineer
        // chose to keep rolling through a sync dropout).
        engine.stopPlayback();
    }
    chaseWasLive = live;
}
