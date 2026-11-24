#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "ClipModel.h"
#include "PlaybackReader.h"

#include <algorithm>
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
    // Disk I/O happens on a background TimeSliceThread via PlaybackReader;
    // the audio thread only copies samples from already-buffered RAM.
    //
    // Every track enters a new position together. A loop start or a clip
    // coming up is cued ahead of the jump, so it is in RAM when the playhead
    // gets there. A jump nobody could see coming (a locate, PLAY from a marker)
    // holds the playhead on the target, silent, until every track has it. It
    // never rolls on with some tracks missing: that is what used to drop the
    // start of each pass, track by track.
    //
    // Stereo files: a natively-recorded stereo pair is ONE interleaved
    // 2-channel Track_NN.wav. loadSession expands it into TWO logical
    // tracks that SHARE one reader -- the L track reads file channel 0,
    // the R track channel 1 -- so each physical channel still gets its
    // own output stream and the index→output mapping is unchanged.
    // Legacy two-mono-file pairs load as two independent mono tracks and
    // keep working untouched.
    class EngineFix20261116Tests;

    class SessionPlayer
    {
        friend class EngineFix20261116Tests;   // holds tracksLock as a render would
    public:
        SessionPlayer();
        ~SessionPlayer();

        void prepare (double sampleRate, int blockSize);
        void release();

        // Off-thread: scan dir for its takes (TakeNames.h), build readers.
        // Stops playback first. Returns number of tracks loaded.
        int  loadSession (const juce::File& sessionDir);
        struct PreparedLoad;
        using Prepared = std::shared_ptr<PreparedLoad>;
        std::vector<Clip> snapshotExplicitReaderRequests() const;
        Prepared prepareSessionLoad (const juce::File&, double playbackRate, int playbackBlock,
                                     const std::vector<Clip>& explicitReaders = {});
        static bool preparedLoadComplete (const Prepared&);
        Prepared installPreparedSession (Prepared, bool preserveClipState);
        void finishPreparedClipPublication();
        void unload();

        bool isLoaded()       const noexcept { return loaded .load (std::memory_order_acquire); }
        bool isPlaying()      const noexcept { return playing.load (std::memory_order_acquire); }
        int  getNumTracks()   const noexcept { return (int) readerCount.load(); }
        juce::int64 getTotalLengthSamples() const noexcept { return totalLength.load(); }
        // Under infoLock, never the render's tracksLock: a .zfproj save reads
        // every track's length, and each wait there made a block's try-lock
        // fail (a dropped block, or a jump hold) during playback.
        juce::int64 getTrackLengthSamples (int trackIdx) const
        {
            const juce::ScopedLock sl (infoLock);
            if (trackIdx < 0 || trackIdx >= (int) trackLengths.size()) return 0;
            return trackLengths[(size_t) trackIdx];
        }
        // A strip whose take is on disk but would not open: a part missing (a
        // silent lead-in still owed), no main file, a file that will not read.
        // It plays silence, and its clips must not be reseeded from length 0.
        bool trackFailedToOpen (int trackIdx) const
        {
            const juce::ScopedLock sl (infoLock);
            return std::find (unopenedTracks.begin(), unopenedTracks.end(), trackIdx) != unopenedTracks.end();
        }
        // Strips the last load left silent because two takes claim them, one
        // line each naming the files ("Track 5 has two take files, ...").
        juce::StringArray loadClashes() const
        {
            const juce::ScopedLock sl (infoLock);
            return clashText;
        }
        juce::int64 getPositionSamples()    const noexcept { return position   .load(); }
        double      getSampleRate()         const noexcept { return fileSampleRate; }
        juce::String getSessionName()       const          { return sessionName; }
        juce::File   getSessionDir()        const          { return sessionDir; }

        void start();
        void stop();
        // True when start() would actually play something: a session is
        // loaded and the playhead has audio ahead of it, or loop-play will
        // wrap it back into the session.
        bool hasAudioAhead() const noexcept;
        void rewind();
        void setPositionSamples (juce::int64 s);

        // Loop region. When both ends are valid (start < end), playback
        // wraps from end → start each pass. clearLoopRegion() disables.
        // The region is also the EDIT selection and RECORD's punch range, and
        // only the punch can destroy a take, so a region arms a punch only
        // when it came from the engineer's hand this run (selectRange). One
        // that merely exists, such as a range restored from a saved session,
        // goes through setLoopRegion: it loops and edits, and RECORD ignores it.
        void setLoopRegion   (juce::int64 startSample, juce::int64 endSample) noexcept;
        void selectRange     (juce::int64 startSample, juce::int64 endSample) noexcept;
        void clearLoopRegion()                                                noexcept;
        bool hasLoopRegion() const noexcept;
        bool isRangeArmed() const noexcept;
        // One selection, one punch: the range stays for Loop and edits, and a
        // later RECORD appends until the engineer selects again.
        void disarmRange() noexcept { rangeArmed.store (false, std::memory_order_release); }
        juce::int64 getLoopStart() const noexcept { return loopStart.load(std::memory_order_relaxed); }
        juce::int64 getLoopEnd()   const noexcept { return loopEnd  .load(std::memory_order_relaxed); }

        // Loop-PLAY toggle, independent of the loop REGION. The region (set by
        // the Selector / , / .) is also the edit selection, so merely having a
        // region shouldn't force playback to loop. With loop-play on (the
        // transport ⟲ button) playback wraps within the region, or within the
        // whole session when there is none. That case makes no region: an
        // implicit one would read as a selection to everything else.
        void setLoopEnabled (bool on) noexcept
        {
            if (on && isSeekLocked()) return;
            loopEnabled.store (on, std::memory_order_relaxed);
        }
        bool isLoopEnabled() const noexcept    { return loopEnabled.load (std::memory_order_relaxed); }

        // While a punch take captures, the playhead is the punch clock: the
        // capture window and the splice point are player positions, so a seek
        // or a loop wrap mid-punch records the wrong span and the splice
        // overwrites original audio past the punch-out. The engine holds this
        // lock for the whole punch take; setPositionSamples and turning loop
        // on are ignored meanwhile, whichever key, cue, click or remote asked.
        void setSeekLocked (bool on) noexcept { seekLocked.store (on, std::memory_order_release); }
        bool isSeekLocked() const noexcept    { return seekLocked.load (std::memory_order_acquire); }

        // RT-safe: fills outputs[i] with samples from track i, position advances.
        void processBlock (float* const* outputs, int numOutputs, int numSamples) noexcept;

        // Per-track clip list -- when a track has been given an explicit
        // clip list (even an EMPTY one), processBlock honours it: it
        // renders only the audio inside clips, silence elsewhere, and an
        // empty list means the whole track is silent (e.g. every clip
        // deleted). A track the engine has NEVER set clips for falls back
        // to the legacy 'play the whole file' path -- so untouched tracks
        // still play. The setter builds the new lists off the audio thread and
        // publishes them with one atomic store; the render never waits on it
        // (see ClipSet).
        void setTrackClips (int trackIdx, std::vector<Clip> clips);
        void clearAllClips();

        // How long a load waits for each take's first read before it moves on.
        void setPrimeTimeoutForTests (int milliseconds) noexcept { primeTimeoutMs = milliseconds; }

    private:
        // Where playback wraps right now. A punch take is never looped: its
        // timeline is the punch clock, so an already-enabled loop stays inert
        // until the seek lock lifts.
        struct LoopSpan
        {
            juce::int64 begin { 0 }, end { 0 };
            bool active() const noexcept { return end > begin; }
        };
        LoopSpan activeLoopSpan (juce::int64 total) const noexcept;

        void renderBlock (float* const*, int, int) noexcept;
        // Bounded handshake used by loadSession/unload after clearing
        // `playing`: waits until two full audio callbacks have completed
        // (via callbackGeneration) so no in-flight block is still reading
        // `tracks`, then returns. Ceiling-capped for a stalled device.
        void waitForCallbackDrain() noexcept;

        struct Track
        {
            // shared_ptr because the two halves of an interleaved stereo
            // file share ONE reader (L reads channel 0, R reads channel 1).
            std::shared_ptr<PlaybackReader> reader;
            juce::int64 length { 0 };
            // Which file channel this logical track reads: false for the L
            // half of a stereo file, true for the R half and for a mono file.
            bool        useRight { true };
        };

        juce::AudioFormatManager formatManager;
        juce::TimeSliceThread    readerThread { "ZF Player Reader" };

        std::vector<Track> tracks;
        // Reader lifetime guard. The audio thread only try-locks (and emits
        // silence on contention), while load/unload take the lock before
        // replacing readers. This makes teardown safe even with a stopped
        // device where the old generation-counter drain cannot advance.
        mutable juce::CriticalSection tracksLock;
        // What the message thread asks about the tracks, under a lock the
        // audio thread never takes. Written with `tracks`, under tracksLock
        // then this one (never the other way round).
        mutable juce::CriticalSection infoLock;
        std::vector<juce::int64> trackLengths;     // tracks[i].length
        std::vector<int>   unopenedTracks;         // see trackFailedToOpen
        juce::StringArray  clashText;              // see loadClashes

        // The clip lists, as one immutable set the render reads without a
        // lock. An edit builds the next set on its own thread, publishes it
        // with one atomic store, and frees a replaced set later, on an editing
        // thread, once the render is not on it (clipsInUse). A clip edit while
        // the show plays therefore never costs a block, and nothing is freed
        // on the audio thread. The render reads a set only while it holds
        // tracksLock, so an edit holding tracksLock knows no set is in use.
        struct ClipSource
        {
            // A clip naming its own file reads this reader, resolved from
            // extraReaders when its list was published; nullptr means the
            // media is missing, and it must never fall back to the own take.
            PlaybackReader* reader { nullptr };
            bool ownTake { true };   // no audioFile: the track's own reader
        };
        struct TrackClips
        {
            std::vector<Clip>       clips;
            std::vector<ClipSource> sources;   // parallel to clips
        };
        struct ClipSet
        {
            // By track. nullptr: never given a list, the track plays its whole
            // file. An empty list is authoritative: the track is silent.
            std::vector<std::shared_ptr<const TrackClips>> tracks;
            // Readers dropped from extraReaders when this set was replaced. Only
            // this set and older ones point at them, so they go with it.
            std::vector<std::unique_ptr<PlaybackReader>> droppedReaders;

            const TrackClips* listFor (int track) const noexcept
            {
                return track >= 0 && track < (int) tracks.size() ? tracks[(size_t) track].get() : nullptr;
            }
        };
        using ReaderMap = std::map<juce::String, std::unique_ptr<PlaybackReader>>;
        using FreedSets = std::vector<std::unique_ptr<ClipSet>>;
        static std::shared_ptr<const TrackClips> resolveClips (std::vector<Clip>, const ReaderMap&);
        static bool resolvedAgainst (const TrackClips&, const ReaderMap&);
        static PlaybackReader* readerFor (const TrackClips&, size_t clip, const Track&) noexcept;
        // clipsLock held. Publish `lists`; the set they replace takes
        // `dropped` with it. Sets the render has left go to `freed`, which the
        // caller destroys after its locks are released.
        void publishClips (std::vector<std::shared_ptr<const TrackClips>> lists,
                           std::vector<std::unique_ptr<PlaybackReader>> dropped, FreedSets& freed);
        void reclaimClipSets (FreedSets& freed);
        // Audio thread: the published set, marked in use; nullptr if an edit
        // kept replacing it (the block then treats every list as unreadable).
        const ClipSet* enterClipSet() noexcept;

        std::unique_ptr<ClipSet>     liveClipSet;       // the published set's owner
        FreedSets                    retiredClipSets;   // replaced, oldest first
        std::atomic<const ClipSet*>  publishedClips { nullptr };
        std::atomic<const ClipSet*>  clipsInUse     { nullptr };   // the set the render is reading
        // Serialises every edit of the clip lists, extraReaders and `tracks`.
        // The audio thread never takes it, so an edit may take its time here.
        mutable juce::CriticalSection  clipsLock;

        // Lock-free mirror of which tracks have a list, for a block that got no
        // clip set: a flagged track has an explicit arrangement and renders
        // SILENCE for that block rather than leak its underlying whole file.
        // Indices at/above the array size are treated as non-authoritative
        // (legacy whole-file). 512 comfortably covers the 256-strip ceiling.
        static constexpr int kMaxAuthoritativeTracks = 512;
        std::array<std::atomic<char>, kMaxAuthoritativeTracks> authoritativeFlags {};

        // Ask for [position, position + span) on every track. True once all of
        // it is in RAM. Audio thread, with the set it entered.
        bool prepareJump (const ClipSet&, juce::int64 position, juce::int64 span, int numTracks) noexcept;
        void cueUpcoming (const ClipSet&, juce::int64 blockEnd, juce::int64 horizon, juce::int64 wrapTarget,
                          int numTracks) noexcept;

        // Per-clip readers for clips that reference a file OTHER than their
        // track's own Track_NN.wav (cross-track paste). Keyed by absolute
        // path. Built on the message thread in setTrackClips (file open is
        // slow), changed only under clipsLock; the audio thread reaches them
        // through the clip set's resolved pointers, never by name. A clip
        // with an empty audioFile uses its track's default reader, so
        // existing sessions are completely unaffected.
        ReaderMap extraReaders;

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
        std::atomic<bool>        rangeArmed  { false };
        std::atomic<bool>        seekLocked  { false };

        double deviceSampleRate { 48000.0 };
        double fileSampleRate   { 48000.0 };
        int    blockSize        { 512 };
        int    primeTimeoutMs   { 1000 };
        juce::String sessionName;
        juce::File   sessionDir;

        juce::AudioBuffer<float> scratch;

        // Audio thread only. Where the next block continues from (-1: the
        // last block did not render), the jump being held and for how long,
        // and a block counter that thins out the cue pass.
        juce::int64 nextRenderPosition { -1 };
        juce::int64 heldJump           { -1 };
        juce::int64 heldJumpSamples    { 0 };
        juce::uint32 cuePassCounter    { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionPlayer)
    };
}
