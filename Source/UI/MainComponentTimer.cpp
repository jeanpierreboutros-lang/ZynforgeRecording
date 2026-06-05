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
        elapsed = recorder.getSamplesSinceStart();

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
        // Slow SMART status poll -- diskutil info is 50-200 ms
        // blocking, so we run it every ~30 s (24 Hz * 30 = 720 ticks)
        // and only when recording. Failing status flips a hard warning.
        if ((diskTick % 720) == 0)
        {
            const auto sd = engine.getActiveSessionDir();
            if (sd.isDirectory())
                smartPrimaryStatus = (int) MultitrackRecorder::querySmartStatus (sd);
            const auto bd = engine.getRecorder().getBackupDirectory();
            if (bd.isDirectory())
                smartBackupStatus = (int) MultitrackRecorder::querySmartStatus (bd);
        }
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

    const bool rec = engine.isRecording();
    formatButton .setEnabled (! rec);
    preRollButton.setEnabled (! rec);

    const auto sessRoot = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                              .getChildFile ("Zynforge Sessions");
    const auto bytesFree = sessRoot.exists() ? sessRoot.getBytesFreeOnVolume()
                                              : juce::File ("/").getBytesFreeOnVolume();
    const double freeGB = (double) bytesFree / (1024.0 * 1024.0 * 1024.0);

    const int    bitDepth     = 24;
    const int    bytesPerSamp = bitDepth / 8;
    const int    channels     = juce::jmax (1, recorder.getNumTracks());
    const double bytesPerSec  = deviceSR * bytesPerSamp * channels;
    const double remainingSec = bytesPerSec > 0 ? (double) bytesFree / bytesPerSec : 0.0;

    bigClock.setDiskInfo (freeGB,
                          recorder.getLastWriteMs(),
                          recorder.getMissedSamples(),
                          remainingSec);
    bigClock.setLoudness (engine.getIntegratedLufs(),
                          engine.getMomentaryLufs(),
                          engine.getTruePeakDb());

    perfDashboard.setMetrics (engine.getAudioLoadPct(),
                              engine.getDiskMBPerSec(),
                              engine.getRingFillPct(),
                              recorder.getMissedSamples());
    perfDashboard.setDeviceInfo (engine.getDeviceSampleRate(),
                                 engine.getDeviceBlockSize());
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
        if (primFail || diskTrouble || smartBad)
        {
            juce::String warn;
            if (smartBad)    warn << "⚠ SMART FAILING -- replace this drive  ";
            if (primFail)    warn << "⚠ PRIMARY WRITE FAILED -- recording on backup/mirror  ";
            if (diskTrouble) warn << "⚠ DISK STRUGGLING -- missed samples imminent";
            statusLabel.setText (warn.trim(), juce::dontSendNotification);
        }
    }
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
