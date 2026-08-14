// MainComponent's 10 Hz refresh callback + transport-label sync.
// Extracted from MainComponent.cpp as part of the 2026-05-24 god-
// class split so the high-traffic timer path is editable without
// scrolling past 5000 lines of menu / layout / setup code.

#include "MainComponent.h"

using namespace zynforge;

static juce::String samplesToTimecode (juce::int64 samples, double sr)
{
    if (sr <= 0.0) return "00:00";
    const auto seconds = (juce::int64) (samples / sr);
    return juce::String::formatted ("%02lld:%02lld", seconds / 60, seconds % 60);
}

void MainComponent::timerCallback()
{
    // Keep the macOS menu's greyed/enabled states in sync with reality
    // (Undo/Redo, Edit, Track, Export) -- they're cached until we ask for a
    // refresh, so without this they freeze in the empty launch state.
    refreshMenuStateIfChanged();

    // Coalesce any mixer change (fader / pan / mute / solo / rename / colour /
    // routing) into a single undo step once it settles -- so Cmd+Z undoes the
    // everyday operations, not just clip + automation edits. Skips recording.
    pollMixerUndo();

    // Periodic auto-save + timestamped backup of the active session (interval
    // from appProps "autosaveMinutes"; only writes when something changed).
    serviceAutosave();

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

    const int n = engine.getRecorder().getNumTracks();
    // Rebuild when the track COUNT changes OR the track SET was replaced
    // (device restart recreates TrackStates at the same count) -- either
    // way the cached TrackState& in each strip would otherwise dangle.
    if (n != lastTrackCount
        || engine.getRecorder().getTrackGeneration() != lastTrackGen)
        rebuildStrips();

    // MIDI clock status pill -- visible reassurance that the engineer's
    // outboard sync is alive. Empty string when disabled hides it.
    {
        const auto& clock = engine.getMidiClockOut();
        const auto label = clock.isEnabled()
            ? juce::String ("MIDI * ") + clock.getOutputDeviceName()
            : juce::String();
        if (midiStatusLabel.getText() != label)
            midiStatusLabel.setText (label, juce::dontSendNotification);
    }

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
            danteLabel.setText (txt, juce::dontSendNotification);
    }

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
                txt = juce::String::formatted ("Next: %s in %d:%02d",
                                                next->name.toRawUTF8(), mins, secsR);
            }
        }
        if (nextCueLabel.getText() != txt)
            nextCueLabel.setText (txt, juce::dontSendNotification);
    }

    if (cueRamp.active) updateCueRamp();

    if (engine.isPunchModeOn() && engine.getPlayer().hasLoopRegion())
        servicePunch();
    if (punchSessionActive)
        servicePunchSession();

    // Keep each strip's input/output combos in sync with engine state --
    // the PATCH page can mutate routing behind the strip's back. Also
    // refresh name + colour so changes made from the EDIT view show up,
    // and push the per-track automation LED state (WRITE-armed +
    // Safe-locked) so the strip header reads true.
    const bool autoWriting = engine.isAutomationWriting();
    for (size_t k = 0; k < strips.size(); ++k)
    {
        auto& s = strips[k];
        if (s == nullptr) continue;
        s->refreshRoutingSelection();
        s->refreshAppearance();
        const int trackIdx = s->getStripIndex();
        const bool safe = engine.isTrackAutomationSafe (trackIdx);
        s->setAutomationLed (autoWriting && ! safe, safe);
    }

    updateTransportLabels();

    const bool playing = engine.isPlaying();
    if (! playing && playButton.getButtonText() == "PAUSE")
        playButton.setButtonText ("PLAY");

    auto& recorder = engine.getRecorder();
    auto& player   = engine.getPlayer();
    auto& markers  = engine.getMarkers();

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

    BigClockPanel::Mode m = BigClockPanel::Mode::Idle;
    juce::int64 elapsed = 0;
    double      timerSR = deviceSR;

    // Auto-arm on input detect -- only meaningful while NOT recording.
    // ~0.5 s sustained input above -40 dBFS arms the strip. Cheap;
    // skipped instantly when the flag is off.
    if (! engine.isRecording())
        engine.serviceAutoArm (12 /* ~0.5 s @ 24 Hz */, 0.01f /* -40 dBFS */);

    if (engine.isRecording())
    {
        m = BigClockPanel::Mode::Recording;
        // Timeline position (continues from the take end on a continue), not the
        // new file's raw length, so the clock carries on instead of restarting.
        elapsed = recorder.getRecordTimelineSamples();

        // Refresh the free-space estimate every ~2 s of timer ticks
        // (the timer runs at 24 Hz, so divide by 48). Querying the
        // volume's free bytes is one syscall per drive; cheap.
        if ((++diskTick % 48) == 0)
            engine.refreshDiskMinutesRemaining();
        // Update disk-keep-up at the same cadence -- if actual bytes/
        // sec falls below ~85 % of expected for ~6 s, we flip the
        // "disk struggling" flag and the status bar warns the engineer
        // before missed samples start landing.
        if ((diskTick % 12) == 0)
            engine.getRecorder().updateDiskHealth (
                engine.getRecorder().estimateBytesPerSecondForArmedTracks());
        // Slow SMART status poll -- every ~30 s (24 Hz * 30 = 720 ticks) and
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

    // Phase-0 status boundary: take ONE EngineStatus snapshot and feed the
    // header readouts from it, instead of pulling a dozen individual engine
    // getters here. When capture moves to a daemon (Phase 1) this becomes the
    // single point that swaps its source from the local engine to IPC.
    // Capture-daemon watchdog (Phase 1d/2): cheap when inactive.
    if (useCaptureDaemon) captureSupervisor.tick();

    const auto status = engine.captureStatus();
    const bool rec = status.recording;
    formatButton .setEnabled (! rec);
    preRollButton.setEnabled (! rec);

    // Free space on the volume the take ACTUALLY lands on. This used to
    // hardcode ~/Music/Zynforge Sessions, so an engineer recording to an
    // external drive (the `sessionsRoot` override, or any session opened from
    // elsewhere) watched the free-space figure for the wrong disk all show.
    const auto sessRoot = [this]
    {
        const auto active = engine.getActiveSessionDir();
        if (active.isDirectory()) return active;
        return getSessionsRoot();
    }();
    const auto bytesFree = sessRoot.exists() ? sessRoot.getBytesFreeOnVolume()
                                              : juce::File ("/").getBytesFreeOnVolume();
    const double freeGB = (double) bytesFree / (1024.0 * 1024.0 * 1024.0);

    // Ask the recorder for the real write rate: it knows the capture format's
    // bit depth and counts backup + every mirror destination. The old inline
    // maths assumed 24-bit and ALL tracks (not just armed), so the "remaining"
    // readout was wrong in both directions depending on the rig.
    double bytesPerSec = (double) recorder.estimateBytesPerSecondForArmedTracks();
    if (bytesPerSec <= 0.0)
    {
        // Nothing armed yet -- show a pre-show estimate at the current format
        // across every strip so the number isn't just blank/infinite.
        const auto fmt = recorder.getCaptureFormat();
        const int bytesPerSamp =
              (fmt == zynforge::CaptureFormat::Wav16 || fmt == zynforge::CaptureFormat::Aiff16
               || fmt == zynforge::CaptureFormat::Flac16)                                            ? 2
            : (fmt == zynforge::CaptureFormat::Wav32Float || fmt == zynforge::CaptureFormat::Aiff32Float) ? 4
            : 3;
        bytesPerSec = deviceSR * bytesPerSamp * juce::jmax (1, status.numTracks);
    }
    const double remainingSec = bytesPerSec > 0.0 ? (double) bytesFree / bytesPerSec : 0.0;

    bigClock.setDiskInfo (freeGB,
                          status.lastWriteMs,
                          status.missedSamples,
                          remainingSec);
    bigClock.setLoudness (status.integratedLufs,
                          status.momentaryLufs,
                          status.truePeakDb);

    perfDashboard.setMetrics (status.audioLoadPct,
                              (float) status.diskMBPerSec,
                              status.ringFillPct,
                              status.missedSamples);
    perfDashboard.setDeviceInfo (status.sampleRate,
                                 status.blockSize);
    perfDashboard.setRecording (rec);
    // Drives per-strip meter/spectrum refresh: full rate while recording or
    // playing, throttled to ~8 Hz when stopped (see TrackState.h).
    gTransportActive.store (rec || playing, std::memory_order_relaxed);
    // Re-lay-out the header once when REC toggles so the dashboard resizes.
    if (rec != dashWasRecording)
    {
        dashWasRecording = rec;
        resized();
    }

    // Live show-day warnings -- only while recording, refreshed at the
    // timer cadence. Both flags clear automatically when the condition
    // recovers (e.g. swap drives + the new primary write succeeds, or
    // disk catches back up). Status bar is the engineer's first read.
    if (rec)
    {
        const bool primFail   = recorder.hasPrimaryFailed();
        const bool diskTrouble = recorder.isDiskStruggling();
        const bool smartBad = (smartPrimaryStatus == (int) MultitrackRecorder::SmartStatus::Failing)
                           || (smartBackupStatus  == (int) MultitrackRecorder::SmartStatus::Failing);
        // A mirror that never OPENED (drive absent, or a root that would have
        // collided with the take) creates no Mirror entry, so anyMirrorFailed()
        // -- which walks the entries -- structurally cannot see it. Without this
        // the engineer runs the whole show believing they have a copy they don't.
        const int mirrorsSkipped = recorder.getMirrorsSkippedAtStart();
        if (primFail || diskTrouble || smartBad || mirrorsSkipped > 0)
        {
            juce::String warn;
            if (smartBad)    warn << "! SMART FAILING -- replace this drive  ";
            if (primFail)    warn << "! PRIMARY WRITE FAILED -- recording on backup/mirror  ";
            if (mirrorsSkipped > 0)
                warn << "! " << mirrorsSkipped << " MIRROR"
                     << (mirrorsSkipped == 1 ? "" : "S") << " NOT WRITING -- drive missing "
                                                            "or folder unusable  ";
            if (diskTrouble) warn << "! DISK STRUGGLING -- missed samples imminent";
            statusLabel.setText (warn.trim(), juce::dontSendNotification);
        }
    }
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
    if (! sessionDir.isDirectory() && ! backupDir.isDirectory())
    {
        smartQueryInFlight.store (false, std::memory_order_release);
        return;
    }

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

void MainComponent::updateTransportLabels()
{
    auto& player = engine.getPlayer();
    const auto sr  = player.getSampleRate();
    const auto pos = samplesToTimecode (player.getPositionSamples(), sr);
    const auto tot = samplesToTimecode (player.getTotalLengthSamples(), sr);
    transportLabel.setText (pos + " / " + tot, juce::dontSendNotification);

    if (player.isLoaded())
    {
        const auto name = player.getSessionName();
        const auto tracks = juce::String (player.getNumTracks());
        sessionLabel.setText ("Session: " + name + " (" + tracks + " tr)",
                              juce::dontSendNotification);
    }
    else
    {
        sessionLabel.setText ("No session loaded", juce::dontSendNotification);
    }
}

// Compare the device's live sample rate against the session's intended rate
// and warn once per distinct mismatch. A silent mismatch (e.g. the incoming
// Dante clock running 44.1k while the session was created at 48k) would
// capture every track at the wrong speed/pitch, so this is a data-integrity
// guard. We don't pop a modal mid-take (a status-line warning still shows);
// the dialog fires while stopped, which is when the engineer can fix it.
void MainComponent::checkDeviceSampleRate (double deviceSampleRate)
{
    if (deviceSampleRate <= 0.0) { srWarnLabel.setText ({}, juce::dontSendNotification); return; }

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
        srWarnLabel.setText ({}, juce::dontSendNotification);   // matched
        srMismatchWarned = 0.0;                                 // re-arm the dialog
        return;
    }

    // Persistent always-visible banner (record-red) while mismatched.
    // ASCII only -- the bundled Inter font has no warning-triangle glyph
    // (it rendered as mojibake); the red colour carries the alarm.
    srWarnLabel.setText ("! SAMPLE-RATE MISMATCH  device " + fmt (deviceSampleRate)
                         + "  vs  session " + fmt (sessionSR) + "  - recording blocked",
                         juce::dontSendNotification);

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
        return;
    }

    if (engine.getChaseMode() == zynforge::AudioEngine::ChaseMode::Off)
    {
        chaseWasLive = false;
        return;
    }

    const bool live = engine.isChaseLive();
    if (live)
    {
        const double sr = engine.getPlayer().getSampleRate() > 0.0
                            ? engine.getPlayer().getSampleRate() : pendingSampleRate;
        const juce::int64 target = engine.getChaseTargetSamples();

        if (! engine.isPlaying())
            engine.startPlayback();

        const juce::int64 cur   = engine.getPlayer().getPositionSamples();
        const juce::int64 slack = (juce::int64) (0.08 * sr);   // 80 ms lock window
        if (std::llabs ((long long) (cur - target)) > slack)
            engine.getPlayer().setPositionSamples (target);
    }
    else if (chaseWasLive && engine.isPlaying() && ! engine.isKeepRollingOnSyncLoss())
    {
        // Timecode master stopped -> park the transport (unless the engineer
        // chose to keep rolling through a sync dropout).
        engine.stopPlayback();
    }
    chaseWasLive = live;
}
