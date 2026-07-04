#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "ClipModel.h"

#include <array>
#include <atomic>
#include <map>
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
    //
    // Stereo files: a natively-recorded stereo pair is ONE interleaved
    // 2-channel Track_NN.wav. loadSession expands it into TWO logical
    // tracks that SHARE one reader -- the L track reads file channel 0,
    // the R track channel 1 -- so each physical channel still gets its
    // own output stream and the index→output mapping is unchanged.
    // Legacy two-mono-file pairs load as two independent mono tracks and
    // keep working untouched.
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

        // Loop-PLAY toggle, independent of the loop REGION. The region (set by
        // the Selector / , / .) is also the edit selection, so merely having a
        // region shouldn't force playback to loop. Playback wraps within the
        // region only when loop-play is enabled (the transport ⟲ button).
        void setLoopEnabled (bool on) noexcept { loopEnabled.store (on, std::memory_order_relaxed); }
        bool isLoopEnabled() const noexcept    { return loopEnabled.load (std::memory_order_relaxed); }

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
        // Bounded handshake used by loadSession/unload after clearing
        // `playing`: waits until two full audio callbacks have completed
        // (via callbackGeneration) so no in-flight block is still reading
        // `tracks`, then returns. Ceiling-capped for a stalled device.
        void waitForCallbackDrain() noexcept;

        struct Track
        {
            // shared_ptr because the two halves of an interleaved stereo
            // file share ONE reader (L reads channel 0, R reads channel 1).
            std::shared_ptr<juce::BufferingAudioReader> reader;
            juce::int64 length { 0 };
            // Which file channel this logical track reads. For a mono file
            // both are true (the single channel). For a stereo file the L
            // track is (true,false) and the R track is (false,true).
            bool        useLeft  { true };
            bool        useRight { true };
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

        // Lock-free mirror of clipsAuthoritative, readable by the audio thread
        // WITHOUT clipsLock. When the processBlock try-lock is contended we
        // can't read the (resizable) clipsAuthoritative vector safely, so we
        // consult this instead: a track flagged here has an explicit clip
        // arrangement and must render SILENCE for the block rather than leak
        // its underlying whole file. Indices at/above the array size are
        // treated as non-authoritative (legacy whole-file). 512 comfortably
        // covers the 256-strip ceiling.
        static constexpr int kMaxAuthoritativeTracks = 512;
        std::array<std::atomic<char>, kMaxAuthoritativeTracks> authoritativeFlags {};

        // Per-clip readers for clips that reference a file OTHER than their
        // track's own Track_NN.wav (cross-track paste). Keyed by absolute
        // path. Built on the message thread in setTrackClips (file open is
        // slow); the audio thread only looks them up, under clipsLock. A
        // clip with an empty audioFile uses its track's default reader, so
        // existing sessions are completely unaffected.
        std::map<juce::String, std::unique_ptr<juce::BufferingAudioReader>> extraReaders;

        std::atomic<bool>        loaded      { false };
        std::atomic<bool>        playing     { false };
        // Incremented once at the top of every processBlock (even when not
        // playing). loadSession/unload snapshot it after clearing `playing`
        // and wait for it to advance by >=2 before freeing readers, so no
        // in-flight callback is still reading `tracks`. Bounded by a ceiling
        // so a stopped device can't hang the caller.
        std::atomic<juce::uint32> callbackGeneration { 0 };
        std::atomic<int>         readerCount { 0 };
        std::atomic<juce::int64> position    { 0 };
        std::atomic<juce::int64> totalLength { 0 };
        std::atomic<juce::int64> loopStart   { -1 };
        std::atomic<juce::int64> loopEnd     { -1 };
        std::atomic<bool>        loopEnabled { false };

        double deviceSampleRate { 48000.0 };
        double fileSampleRate   { 48000.0 };
        int    blockSize        { 512 };
        juce::String sessionName;
        juce::File   sessionDir;

        juce::AudioBuffer<float> scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionPlayer)
    };
}
