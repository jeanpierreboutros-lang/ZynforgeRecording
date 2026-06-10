#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace zynforge
{
    // A narrow, serialisable snapshot of everything a status consumer (the
    // UI readouts, the companion server, and -- in the phased capture-split
    // plan -- a future headless capture daemon over IPC) needs to display
    // during a take. The point of Phase 0 (see decisions.md): consumers read
    // ONE struct rather than reaching into recorder / engine internals, and
    // the struct round-trips through JSON so the same boundary works across a
    // process split.
    //
    // Pure data: no engine dependency, so it's portable. `AudioEngine::
    // captureStatus()` fills it from the engine's atomics at one chokepoint.

    struct TrackStatus
    {
        juce::String  name;
        float         peak       { 0.0f };
        float         rms        { 0.0f };
        bool          armed      { false };
        bool          muted      { false };
        bool          soloed     { false };
        bool          monitor    { false };
        bool          stereoLeft { false };   // L of a collapsed stereo pair
        juce::uint32  colourARGB { 0 };        // already resolved (no 0 = "use default")

        juce::var toJson() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("name",   name);
            o->setProperty ("peak",   peak);
            o->setProperty ("rms",    rms);
            o->setProperty ("armed",  armed);
            o->setProperty ("muted",  muted);
            o->setProperty ("soloed", soloed);
            o->setProperty ("monitor", monitor);
            o->setProperty ("stereo", stereoLeft);
            // "#RRGGBB" for the web client (low 24 bits of ARGB). Core-only --
            // no juce::Colour dependency so this header stays portable.
            o->setProperty ("colour", "#" + juce::String::toHexString (
                                          (int) (colourARGB & 0x00ffffffu)).paddedLeft ('0', 6));
            return juce::var (o);
        }

        static TrackStatus fromJson (const juce::var& v)
        {
            TrackStatus t;
            t.name    = v.getProperty ("name", "").toString();
            t.peak    = (float) (double) v.getProperty ("peak", 0.0);
            t.rms     = (float) (double) v.getProperty ("rms", 0.0);
            t.armed   = (bool) v.getProperty ("armed", false);
            t.muted   = (bool) v.getProperty ("muted", false);
            t.soloed  = (bool) v.getProperty ("soloed", false);
            t.monitor = (bool) v.getProperty ("monitor", false);
            t.stereoLeft = (bool) v.getProperty ("stereo", false);
            const auto c = v.getProperty ("colour", "").toString();
            if (c.startsWith ("#") && c.length() >= 7)
                t.colourARGB = (juce::uint32) (0xff000000u
                    | (juce::uint32) c.substring (1).getHexValue32());
            return t;
        }
    };

    struct EngineStatus
    {
        bool          recording   { false };
        bool          playing     { false };
        juce::int64   positionSamples { 0 };
        juce::int64   elapsedSamples  { 0 };   // samples since record start
        double        sampleRate  { 0.0 };
        int           blockSize   { 0 };
        float         audioLoadPct { 0.0f };
        double        diskMBPerSec { 0.0 };
        int           minutesRemaining { 0 };
        juce::int64   missedSamples { 0 };
        int           numTracks   { 0 };
        int           armedTracks { 0 };
        bool          backupActive { false };
        int           captureFormat { 0 };     // CaptureFormat as int
        std::vector<TrackStatus> tracks;

        juce::var toJson() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("recording", recording);
            o->setProperty ("playing",   playing);
            o->setProperty ("positionSamples", positionSamples);
            o->setProperty ("elapsedSamples",  elapsedSamples);
            o->setProperty ("sampleRate", sampleRate);
            o->setProperty ("blockSize",  blockSize);
            o->setProperty ("audioLoadPct", audioLoadPct);
            o->setProperty ("diskMBPerSec", diskMBPerSec);
            o->setProperty ("minutesRemaining", minutesRemaining);
            o->setProperty ("missedSamples", missedSamples);
            o->setProperty ("numTracks",   numTracks);
            o->setProperty ("armedTracks", armedTracks);
            o->setProperty ("backupActive", backupActive);
            o->setProperty ("captureFormat", captureFormat);
            juce::Array<juce::var> arr;
            for (const auto& t : tracks) arr.add (t.toJson());
            o->setProperty ("tracks", arr);
            return juce::var (o);
        }

        static EngineStatus fromJson (const juce::var& v)
        {
            EngineStatus s;
            s.recording   = (bool) v.getProperty ("recording", false);
            s.playing     = (bool) v.getProperty ("playing", false);
            s.positionSamples = (juce::int64) v.getProperty ("positionSamples", 0);
            s.elapsedSamples  = (juce::int64) v.getProperty ("elapsedSamples", 0);
            s.sampleRate  = (double) v.getProperty ("sampleRate", 0.0);
            s.blockSize   = (int) v.getProperty ("blockSize", 0);
            s.audioLoadPct = (float) (double) v.getProperty ("audioLoadPct", 0.0);
            s.diskMBPerSec = (double) v.getProperty ("diskMBPerSec", 0.0);
            s.minutesRemaining = (int) v.getProperty ("minutesRemaining", 0);
            s.missedSamples = (juce::int64) v.getProperty ("missedSamples", 0);
            s.numTracks   = (int) v.getProperty ("numTracks", 0);
            s.armedTracks = (int) v.getProperty ("armedTracks", 0);
            s.backupActive = (bool) v.getProperty ("backupActive", false);
            s.captureFormat = (int) v.getProperty ("captureFormat", 0);
            if (auto* a = v.getProperty ("tracks", {}).getArray())
                for (const auto& tv : *a)
                    s.tracks.push_back (TrackStatus::fromJson (tv));
            return s;
        }
    };
}
