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
    const int n = engine.getRecorder().getNumTracks();
    if (n != lastTrackCount)
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

    if (engine.isRecording())
    {
        m = BigClockPanel::Mode::Recording;
        elapsed = recorder.getSamplesSinceStart();
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

    perfDashboard.setMetrics (engine.getAudioLoadPct(),
                              engine.getDiskMBPerSec(),
                              engine.getRingFillPct(),
                              recorder.getMissedSamples());
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
