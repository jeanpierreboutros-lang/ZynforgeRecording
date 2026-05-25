#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "ClipModel.h"

#include <atomic>
#include <memory>
#include <vector>

namespace zynforge
{
    // Multitrack playback for virtual soundcheck.
    //
    // Loads Track_NN.wav files from a session directory and routes each
    // track to the matching output channel (Track 1 → out 1, etc.).
    // Disk I/O happens on a background TimeSliceThread via
    // juce::BufferingAudioReader; the audio thread only copies samples
    // from already-buffered RAM.
    class SessionPlayer
    {
    public:
        SessionPlayer();
        ~SessionPlayer();

        void prepare (double sampleRate, int blockSize);
        void release();

        // Off-thread: scan dir for Track_*.wav, build readers. Stops
        // playback first. Returns number of tracks loaded.
        int  loadSession (const juce::File& sessionDir);
        void unload();

        bool isLoaded()       const noexcept { return loaded .load (std::memory_order_acquire); }
        bool isPlaying()      const noexcept { return playing.load (std::memory_order_acquire); }
        int  getNumTracks()   const noexcept { return (int) readerCount.load(); }
        juce::int64 getTotalLengthSamples() const noexcept { return totalLength.load(); }
        juce::int64 getTrackLengthSamples (int trackIdx) const noexcept
        {
            if (trackIdx < 0 || trackIdx >= (int) tracks.size()) return 0;
            return tracks[(size_t) trackIdx].length;
        }
        juce::int64 getPositionSamples()    const noexcept { return position   .load(); }
        double      getSampleRate()         const noexcept { return fileSampleRate; }
        juce::String getSessionName()       const          { return sessionName; }
        juce::File   getSessionDir()        const          { return sessionDir; }

        void start();
        void stop();
        void rewind();
        void setPositionSamples (juce::int64 s);

        // Loop region. When both ends are valid (start < end), playback
        // wraps from end → start each pass. clearLoopRegion() disables.
        void setLoopRegion   (juce::int64 startSample, juce::int64 endSample) noexcept;
        void clearLoopRegion()                                                noexcept;
        bool hasLoopRegion() const noexcept;
        juce::int64 getLoopStart() const noexcept { return loopStart.load(std::memory_order_relaxed); }
        juce::int64 getLoopEnd()   const noexcept { return loopEnd  .load(std::memory_order_relaxed); }

        // RT-safe: fills outputs[i] with samples from track i, position advances.
        void processBlock (float* const* outputs, int numOutputs, int numSamples) noexcept;

        // Per-track clip list -- when a track has been given an explicit
        // clip list (even an EMPTY one), processBlock honours it: it
        // renders only the audio inside clips, silence elsewhere, and an
        // empty list means the whole track is silent (e.g. every clip
        // deleted). A track the engine has NEVER set clips for falls back
        // to the legacy 'play the whole file' path -- so untouched tracks
        // still play. Setter is UI-thread; the audio thread reads under a
        // lock the setter holds only briefly while swapping the vector.
        void setTrackClips (int trackIdx, std::vector<Clip> clips);
        void clearAllClips();

    private:
        struct Track
        {
            std::unique_ptr<juce::BufferingAudioReader> reader;
            juce::int64 length { 0 };
        };

        juce::AudioFormatManager formatManager;
        juce::TimeSliceThread    readerThread { "ZF Player Reader" };

        std::vector<Track> tracks;

        // Active clip lists, keyed by track index. Audio thread reads
        // under clipsLock -- UI thread holds it just long enough to
        // std::move a new vector in.
        std::vector<std::vector<Clip>> activeClips;
        // Parallel to activeClips: 1 once the engine has explicitly set a
        // clip list for that track. Disambiguates "empty list = silence"
        // (authoritative) from "no list = play whole file" (never set).
        std::vector<char>              clipsAuthoritative;
        mutable juce::CriticalSection  clipsLock;

        std::atomic<bool>        loaded      { false };
        std::atomic<bool>        playing     { false };
        std::atomic<int>         readerCount { 0 };
        std::atomic<juce::int64> position    { 0 };
        std::atomic<juce::int64> totalLength { 0 };
        std::atomic<juce::int64> loopStart   { -1 };
        std::atomic<juce::int64> loopEnd     { -1 };

        double deviceSampleRate { 48000.0 };
        double fileSampleRate   { 48000.0 };
        int    blockSize        { 512 };
        juce::String sessionName;
        juce::File   sessionDir;

        juce::AudioBuffer<float> scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionPlayer)
    };
}
