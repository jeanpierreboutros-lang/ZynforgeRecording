#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace zynforge
{
    class AudioEngine;

    // Multi-machine session sync (read-only mirror).
    //
    // Use case: a FOH Mac runs the live recording. A second 'safety' Mac
    // runs a fresh ZynForge Recording instance pointed at the same audio
    // device's secondary inputs, and is told to mirror the primary's
    // session structure (strip count, names, colours, BPM, cue list,
    // transport state). The safety Mac records to its own session folder
    // independently, but its UI and naming stays locked to the primary so
    // the engineer can swap to it without losing track.
    //
    // Protocol: the primary's existing CompanionServer (port 9000) already
    // serves GET /state.json with the full mixer state. The mirror polls
    // it every 500 ms and applies non-destructive deltas (names, colours,
    // BPM, cue list) to its local engine. Audio routing and per-strip
    // gain are left alone -- each machine keeps its own monitor mix.
    //
    // This class is intentionally LIGHTWEIGHT -- it does not transfer
    // audio, does not replicate recording state, and does not become a
    // master/replica audio engine. It's a metadata mirror only.
    class SessionMirror : private juce::Timer
    {
    public:
        explicit SessionMirror (AudioEngine&);
        ~SessionMirror() override;

        // Start polling primaryHost:port for /state.json every periodMs.
        // Set to empty host or call stop() to turn the mirror off.
        void start (const juce::String& primaryHost, int port, int periodMs = 500);
        void stop();

        bool         isMirroring() const noexcept { return juce::Timer::isTimerRunning(); }
        juce::String getPrimary()  const          { return host; }
        juce::int64  getLastSyncMs() const        { return lastSyncMs; }
        juce::String getLastError()  const        { return lastError; }

    private:
        void timerCallback() override;
        void applyState (const juce::var& state);

        AudioEngine&  engine;
        juce::String  host;
        int           port { 9000 };
        juce::int64   lastSyncMs { 0 };
        juce::String  lastError;
    };
}
