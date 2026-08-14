#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>

#include "TrackState.h"

namespace zynforge
{
    enum class CaptureFormat
    {
        Wav16,        // 16-bit PCM WAV
        Wav24,        // 24-bit PCM WAV (default)
        Wav32Float,   // 32-bit IEEE float WAV -- clip-proof at file level
        Aiff16,       // 16-bit PCM AIFF
        Aiff24,       // 24-bit PCM AIFF
        Aiff32Float,  // 32-bit IEEE float AIFF
        Flac16,       // 16-bit FLAC
        Flac24        // 24-bit FLAC -- ~50% disk size vs WAV
    };

    // Lock-free per-channel ring buffer + background disk writer.
    // The audio thread only ever pushes; a dedicated TimeSliceClient
    // drains and writes the configured format per track.
    //
    // Pre-roll: a separate per-channel ring of N seconds is always
    // pushed (when N > 0) so RECORD can dump audio that happened
    // BEFORE the button was pressed.
    class MultitrackRecorder final : private juce::TimeSliceClient
    {
    public:
        MultitrackRecorder();
        ~MultitrackRecorder() override;

        // Called by AudioEngine when the device opens / sample rate changes.
        void prepare (double sampleRateHz, int maxBlockSize, int numInputs);
        void release();

        // Real-time safe: pushes input samples and updates meters.
        // Called from the audio thread.
        void processBlock (const float* const* inputs, int numChannels, int numSamples) noexcept;

        bool startRecording (const juce::File& sessionDir);
        void stopRecording();
        bool isRecording() const noexcept { return recording.load (std::memory_order_acquire); }

        // ── Punch-in (overdub a region of an existing take) ────────────────
        // Arm the NEXT startRecording as a punch-in at `punchInSample` on the
        // session's existing take. The real-time capture path is unchanged: we
        // still record a clean fresh Track_NN file from 0. The punch is an
        // OFFLINE splice on stop -- each armed track's existing primary/backup/
        // mirror copy is moved aside to a sidecar before the writer truncates,
        // then on stop spliced back as base[0,punchIn) + newTake + base[after],
        // via a temp file + atomic swap, so a failure can't corrupt the take.
        // Cleared automatically after each stopRecording(). The CALLER must
        // ensure the existing take is single-file (not RF64/ceiling-split) --
        // a multi-part base isn't spliced and would be silently lost.
        void armPunchIn (juce::int64 punchInSample) noexcept
        {
            punchInActive = true;
            punchInPos    = juce::jmax ((juce::int64) 0, punchInSample);
            // The fresh punch file records from sample 0, but on the TIMELINE
            // the take resumes at the punch point: anchor the clock / playhead /
            // live-capture waveform there so the new audio builds AT the punch
            // position (with the existing take under it), not from 0. Cleared in
            // stopRecording like the rest of the punch state.
            recordBaseSamples = punchInPos;
        }
        bool isPunchInArmed() const noexcept { return punchInActive; }

        // ── Continue recording (append a new part) ─────────────────────────
        // Arm the NEXT startRecording to CONTINUE the existing take rather than
        // overwrite it: each armed track that already has a take records a fresh
        // Track_NN_partXX continuation file, leaving the existing file(s)
        // untouched -- the take is the sequence of parts (SessionPlayer stitches
        // them on playback). This is the safe "keep recording on my file" model:
        // nothing existing is modified, only a new finalised part is added.
        // Auto-cleared after each stopRecording(). An armed track with no take
        // yet just records Track_NN normally (part 1).
        // baseSamples is the existing take's length: the new part records into
        // its own file from 0, but the take's TIMELINE position continues from
        // baseSamples, so the clock / playhead / ruler show "carrying on from
        // where it stopped" instead of restarting at 0.
        void armContinue (juce::int64 baseSamples) noexcept
        {
            continueAsPart    = true;
            recordBaseSamples = juce::jmax ((juce::int64) 0, baseSamples);
        }
        bool isContinueArmed() const noexcept { return continueAsPart; }

        // Recording position ON THE TIMELINE: the take base (0 for a normal
        // take, the existing length for a continue) + samples captured so far.
        // Use this for the playhead / clock; getSamplesSinceStart() is the new
        // FILE's length (integrity / write accounting).
        juce::int64 getRecordTimelineSamples() const noexcept
        { return recordBaseSamples + getSamplesSinceStart(); }
        juce::int64 getRecordBaseSamples() const noexcept { return recordBaseSamples; }

        // True if the session's take for `trackIndex` was auto-split into
        // multiple parts (Track_NN_partXX) -- punch-in must refuse these.
        static bool takeIsMultiPart (const juce::File& sessionDir, int trackIndex);

        // Next continuation file for a take: Track_NN.<ext> is part 1, then
        // Track_NN_part02, _part03 ... Returns the path to write next + its part
        // number (1 when no take exists yet -> a normal first recording).
        static std::pair<juce::File, int> nextContinuationPart (
            const juce::File& audioDir, const juce::String& trackName, const juce::String& ext);

        // Capture format & pre-roll are settings -- only changeable while not recording.
        void setCaptureFormat (CaptureFormat f) noexcept     { if (! isRecording()) captureFormat = f; }
        CaptureFormat getCaptureFormat() const noexcept       { return captureFormat; }

        // Optional second format for the backup writer. When set to a
        // different value than the primary, recording produces TWO files
        // per channel -- e.g., 24-bit WAV to the main drive AND 24-bit
        // FLAC to the backup drive. Default mirrors the primary.
        void setBackupCaptureFormat (CaptureFormat f) noexcept { if (! isRecording()) backupCaptureFormat = f; }
        CaptureFormat getBackupCaptureFormat() const noexcept  { return backupCaptureFormat; }

        void setPreRollSeconds (int seconds);
        int  getPreRollSeconds() const noexcept               { return preRollSeconds; }

        // Decouples strip count from device input count -- engineer picks
        // how many tracks they want, routing chooses which device input
        // each strip captures. No-op while recording.
        void addTrack();
        void removeLastTrack();
        void removeTrackAt (int index);
        void setTrackCount (int n);

        int          getNumTracks() const noexcept { return (int) tracks.size(); }
        // Bumped whenever the track SET changes (added / removed / cleared
        // + recreated on a device restart). UI watches this to rebuild
        // strips even when the COUNT is unchanged but the underlying
        // TrackState objects were replaced -- otherwise a cached
        // TrackState& dangles and crashes on the next paint/timer.
        int          getTrackGeneration() const noexcept { return trackGen.load (std::memory_order_relaxed); }
        // Bump the generation without changing the track COUNT -- e.g. a stereo
        // link/unlink changes how every view collapses the pair, so the MIXER /
        // EDIT rebuild + the meterbridge (which both watch this counter) must
        // re-run even though the count is unchanged.
        void         bumpTrackGeneration() noexcept { trackGen.fetch_add (1, std::memory_order_relaxed); }
        // NOT bounds-checked in release -- this is on the audio thread's hot
        // path (once per strip per block), so every caller must range-check
        // against getNumTracks() first. The debug assert catches the callers
        // that don't: an out-of-range index here is an unchecked
        // `*tracks[i]` past the end of the vector, which is how a stale
        // player-derived track count crashed the stereo-mix bounce.
        TrackState&  getTrack (int i) noexcept
        {
            jassert (i >= 0 && i < (int) tracks.size());
            return *tracks[(std::size_t) i];
        }

        // Health + position counters -- all RT-safe to read.
        juce::int64 getSamplesSinceStart() const noexcept { return samplesSinceStart.load(std::memory_order_relaxed); }
        juce::int64 getMissedSamples()     const noexcept { return missedSamples    .load(std::memory_order_relaxed); }
        int         getLastWriteMs()       const noexcept { return lastWriteMs      .load(std::memory_order_relaxed); }

        // Apple-Silicon scheduling: when the engine learns about a new
        // audio device, it forwards the device's AudioWorkgroup here so
        // every writer worker can join it -- the macOS scheduler then
        // co-schedules them with the CoreAudio IO thread (no priority
        // inversion, no jitter under load). Pass an empty/disengaged
        // workgroup to revert to the default scheduler.
        void setAudioWorkgroup (juce::AudioWorkgroup);

        // Live telemetry for the CPU/disk dashboard:
        //   - bytesPerSec: rolling 1 s window of bytes pushed to disk
        //     (across primary + backup writers, all channels).
        //   - ringFillPct: 0..100, max over channels of FIFO occupancy.
        // Both are updated by drainOnce() on the writer thread(s); the UI
        // polls them via these accessors.
        float getDiskBytesPerSec() const noexcept;
        float getRingFillPct()     const noexcept;
        juce::File  getActiveSessionDir()  const          { return activeSessionDir; }

        // Free-space pre-flight + live "minutes remaining" estimate.
        //
        // estimateBytesPerSecondForArmedTracks computes the worst-case
        // per-second disk-write rate for the current arm + format
        // configuration, summed across primary + (when active) backup.
        // Engineers see this before they hit record and can decide
        // whether to add storage or change format.
        //
        // estimateMinutesRemaining queries the free space on the
        // active session's volume (and the backup root's volume when
        // backup is active), divides by the projected rate, and
        // returns whichever drive runs out first. Returns 0 if no
        // session is active or no tracks are armed; returns INT_MAX
        // when free space is effectively unbounded.
        juce::int64 estimateBytesPerSecondForArmedTracks() const noexcept;
        int         estimateMinutesRemaining (const juce::File& primaryVolume,
                                              const juce::File& backupVolume) const noexcept;

        // Disk-keep-up health, as a proxy for the SMART data we don't
        // (currently) read. Returns true if actual bytes/sec has been
        // < ~85 % of expected (estimateBytesPerSecondForArmedTracks)
        // for sustained windows -- means the disk is failing to drain
        // the ring as fast as audio arrives, missed samples are
        // imminent. UI surfaces this as "⚠ DISK STRUGGLING".
        bool isDiskStruggling() const noexcept
        {
            return diskStrugglingFlag.load (std::memory_order_relaxed);
        }
        // Update the disk-struggling flag from the latest sampled
        // throughput. Called from the UI's timer; cheap (one atomic
        // compare + EMA). Pass the current expected bytes/sec so the
        // recorder doesn't have to re-derive the arm + format setup.
        void updateDiskHealth (juce::int64 expectedBytesPerSec) noexcept;

        // SMART status query. macOS-only at present; shells out to
        // `diskutil info <mountPoint>` and parses the SMART Status
        // line. Slow (~50-200 ms blocking subprocess call) -- the
        // host calls this from a low-frequency timer (every 30 s),
        // not the audio path. Returns:
        //   Verified  -- drive's SMART controller reports healthy
        //   Failing   -- imminent hardware failure predicted
        //   Unknown   -- internal disk that doesn't report SMART
        //                (some NVMe + APFS containers), network mount,
        //                or query failed
        enum class SmartStatus { Verified, Failing, Unknown };
        static SmartStatus querySmartStatus (const juce::File& mountPoint);

        // Optional second copy of every track to a backup folder.
        // Pass an empty File to disable. Only effective for the next session.
        void setBackupDirectory (const juce::File& dir);
        juce::File getBackupDirectory() const               { return backupDir; }
        bool       isBackupActive() const noexcept          { return backupActive.load (std::memory_order_relaxed); }

        // ── N-way mirror destinations ──────────────────────────────
        // Beyond primary + backup, the engineer can register any
        // number of extra mirror destinations -- each gets a full
        // parallel copy of every armed track, in its own configurable
        // format. Useful for broadcast / archival workflows that want
        // primary (fast SSD) + backup (different drive) + off-board
        // mirror (NAS / second machine) running concurrently.
        struct MirrorConfig
        {
            juce::File    root;
            CaptureFormat format { CaptureFormat::Wav24 };
        };
        // Returns false (and changes nothing) when a take is rolling -- the
        // caller MUST NOT persist a config the recorder just refused, or the
        // settings file and the live recorder disagree for the rest of the show.
        bool setMirrors (const std::vector<MirrorConfig>& configs);
        std::vector<MirrorConfig> getMirrors() const               { return mirrorConfigs; }

        // Why a mirror root is unusable, or an empty string when it's safe.
        //
        // A mirror writes to `root/<sessionName>/Audio Files/Track_NN.<ext>`,
        // which is the SAME shape the primary and backup use. So a root that
        // happens to be the session's parent resolves to the primary take's own
        // files -- two writers, one path, for the length of the show. The mirror
        // picker opens at ~/Music and the sessions root is ~/Music/Zynforge
        // Sessions, so "mirror into my sessions folder" is one click away and
        // destroys the take it was meant to protect. Same for two rows on one
        // drive, and for a root pointed at the backup destination.
        //
        // Checked at record start (colliding mirrors are skipped, never opened)
        // AND in the mirror picker, so the engineer hears about it at the desk
        // rather than at load-out.
        static juce::String mirrorRootRejection (const juce::File& root,
                                                 const juce::File& sessionDir,
                                                 const juce::File& backupRoot,
                                                 const std::vector<juce::File>& alreadyAccepted);

        // Mirrors dropped at the last record start (collision or unreachable
        // drive). A configured mirror that never opens produces no Mirror entry,
        // so anyMirrorFailed() structurally cannot see it -- this is how the
        // banner reports "you have fewer copies than you configured".
        int getMirrorsSkippedAtStart() const noexcept
        {
            return mirrorsSkippedAtStart.load (std::memory_order_relaxed);
        }
        // Any mirror failed during the active take? Cleared at start.
        bool anyMirrorFailed() const noexcept;
        bool       hasBackupFailed() const noexcept         { return backupFailed.load (std::memory_order_relaxed); }

    private:
        // Inherited but no longer used directly -- drain logic moves into
        // per-shard ShardClients (see below). Kept as a stub returning a
        // long delay so the base class doesn't compile-error.
        int useTimeSlice() override;

        // One drain worker per writer-thread shard. Each owns a range of
        // channel indices [firstChannel, lastChannel) and drains only
        // those FIFOs on its own juce::TimeSliceThread. This is the
        // change that lets disk I/O scale linearly with track count at
        // 64 / 128 / 256 channels instead of being serialised through a
        // single writer thread.
        struct ShardClient final : public juce::TimeSliceClient
        {
            MultitrackRecorder* owner          { nullptr };
            int                 firstChannel   { 0 };
            int                 lastChannel    { 0 };

            // Per-shard throughput accumulator + per-shard publish atomic
            // so the global getDiskBytesPerSec() can sum without ever
            // taking a lock.
            std::atomic<float>  shardBytesPerSec      { 0.0f };
            juce::int64         throughputAccumBytes  { 0 };
            double              throughputWindowMs    { 0.0 };
            std::atomic<int>    shardRingFillPct      { 0 };

            // Serialises drainShard() for THIS shard. The FIFO is single-
            // reader, but the shard's own TimeSliceThread and a fan-out
            // drainOnce() (stopRecording's final flush, or the test harness's
            // drainPendingForTests) can both try to drain it -- two readers
            // corrupt the FIFO + the part-file/byte bookkeeping. Per-shard
            // (not global) so different shards still drain in parallel.
            // Never taken on the audio thread (drainShard is consumer-side).
            juce::CriticalSection drainLock;

            // Reused planar staging buffers for the stereo-pair write path:
            // a 2-channel writer needs its two source channels as contiguous
            // arrays (writeFromFloatArrays is planar, not interleaved), but
            // the L + R FIFOs each hand back a possibly-wrapped two-region
            // ScopedRead. We copy both into these before the single 2-ch
            // write. Owned by the shard so a steady take never reallocates.
            std::vector<float> stageL, stageR;

            // Reused all-zero buffer for the overflow silence-pad path (see
            // drainShard). Grows once to the chunk cap then stays all zeros --
            // never written into, so it needs no per-use clearing.
            std::vector<float> silence;

            int useTimeSlice() override;
        };

        struct ChannelFifo
        {
            juce::AbstractFifo fifo { 1 };
            std::vector<float> data;

            // Samples the audio thread had to DROP because this channel's FIFO
            // was full (disk fell behind). Bumped on the audio thread (push),
            // consumed on the drain thread which compensates by writing that
            // many silence samples so every track's file advances by the same
            // total sample count -- otherwise per-channel drop counts diverge
            // and the take gains permanent inter-track sync drift.
            std::atomic<juce::int64> droppedSamples { 0 };

            void resize (int size)
            {
                data.assign ((std::size_t) size, 0.0f);
                fifo.setTotalSize (size);
                fifo.reset();
                droppedSamples.store (0, std::memory_order_relaxed);
            }
        };

        struct WriterChannel
        {
            std::unique_ptr<juce::AudioFormatWriter> writer;
            std::unique_ptr<juce::AudioFormatWriter> backupWriter;

            // Auto-split state. WAV RIFF caps file size at 4 GiB
            // (32-bit unsigned chunk header); AIFF caps at 2 GiB
            // (signed 32-bit). When a writer approaches its format's
            // limit the drain loop closes the current file and opens
            // Track_NN_partNN with the same parameters so a long
            // multi-hour show keeps capturing past the limit instead
            // of silently producing a malformed header.
            juce::int64  bytesWrittenPrimary  { 0 };  // resets at each roll
            juce::int64  bytesWrittenBackup   { 0 };
            juce::int64  totalSamplesPrimary  { 0 };  // never resets (sum across parts)
            juce::int64  totalSamplesBackup   { 0 };
            // Filenames of every part written, in order. Reported in
            // session.report.json so the mix engineer can confirm at a
            // glance which files belong to which track when a long take
            // produced multiple parts.
            juce::StringArray partFilesPrimary;
            juce::StringArray partFilesBackup;
            int          partNumberPrimary    { 1 };
            int          partNumberBackup     { 1 };

            // N-way mirror writers. The primary + backup pair stays
            // intact (engineers know that workflow); these are ADDITIONAL
            // mirror destinations on top, for crews who need three+
            // simultaneous copies (e.g. live broadcast houses running
            // an on-board SSD + a backup HDD + an off-board NAS).
            // Each mirror is independent: own file path, own bytes
            // counter, own auto-split state, own failure flag.
            struct Mirror
            {
                std::unique_ptr<juce::AudioFormatWriter> writer;
                juce::File   baseFile;
                juce::String ext;
                int          container         { 0 };
                int          bitDepth          { 24 };
                int          bytesPerSample    { 3 };
                juce::int64  bytesWritten      { 0 };   // resets at each roll
                juce::int64  totalSamples      { 0 };   // never resets
                int          partNumber        { 1 };
                juce::StringArray partFiles;
                // Drain thread sets this to true if the mirror writer
                // returns false from writeFromFloatArrays (disk full,
                // path disappeared). No atomic needed -- only the
                // drain thread for THIS channel ever touches it during
                // a take; the report reads it on the message thread
                // after closeWriters has serialised. Mirror with
                // failed=true is skipped on subsequent writes.
                bool         failed            { false };
            };
            std::vector<Mirror> mirrors;
            juce::File   primaryBaseFile;     // "Audio Files/Track_01" (no extension)
            juce::File   backupBaseFile;
            juce::String primaryExt;          // ".wav" etc
            juce::String backupExt;
            int          primaryBitDepth      { 24 };
            int          backupBitDepth       { 24 };
            int          primaryContainer     { 0 };   // Container enum value
            int          backupContainer      { 0 };
            int          bytesPerSamplePrimary { 3 };
            int          bytesPerSampleBackup  { 3 };

            // Interleaved-stereo capture: a stereo pair (isStereo on the L
            // track) records ONE 2-channel file named for the L slot --
            // Track_NN.wav with two channels -- instead of two mono files.
            // The R partner's WriterChannel is left empty (no writer); its
            // FIFO is drained here, by the L channel's drain step, which
            // reads both FIFOs and does one 2-ch write. All destinations
            // (primary / backup / every mirror) carry the same channel
            // count, so one field governs the whole WriterChannel. 1 = mono
            // (the default, every non-stereo track), 2 = interleaved stereo.
            int          numChannels { 1 };

            // Does this channel PARTICIPATE in the take (a writer was opened
            // for it at record start)? The drain loop keys on this, NOT on
            // `writer != nullptr`. Otherwise a primary write-failure (which
            // nulls `writer`) is indistinguishable from a deliberately-empty
            // stereo-R slot, and the whole channel -- backup, mirrors, FIFO
            // drain -- is skipped for the rest of the take, silently freezing
            // the backup the failover promise says keeps rolling. Empty R
            // slots + unarmed/bus channels stay false and are skipped.
            bool         active { false };
        };

        // Per-channel rolling history used for pre-roll. The audio thread is
        // the only writer (push, continuously -- NOT gated on recording); the
        // message thread reads once via dumpPreRollToWriters() at record start.
        // No lock: push publishes its data writes with a release store on
        // totalWritten that readHistory pairs with an acquire load, and the
        // ring is over-allocated by kPreRollSafetySec seconds BEYOND the
        // requested pre-roll so an in-flight push during the (sub-millisecond)
        // read can never wrap around and overwrite the window being copied.
        struct PreRollBuffer
        {
            std::vector<float>       data;
            std::atomic<juce::int64> totalWritten { 0 };

            void allocate (int samples) noexcept
            {
                data.assign ((std::size_t) samples, 0.0f);
                totalWritten.store (0, std::memory_order_release);
            }

            void push (const float* src, int n) noexcept;

            // Reads up to samplesWanted of the most recent history into dest.
            // Returns how many were actually available.
            int readHistory (float* dest, int samplesWanted) const noexcept;
        };

        void drainOnce();
        void drainShard (ShardClient&);
        // Rewrite the header of every open writer in [first, last) so a crash
        // mid-take leaves a self-describing file (and promotes WAV to RF64
        // once it crosses 4 GiB). Called periodically from drainShard.
        void flushOpenWriters (std::size_t first, std::size_t last);
        std::atomic<double> lastWriterFlushMs { 0.0 };
        void rebuildShards();
        void closeWriters();
        void allocatePreRollBuffers();
        void dumpPreRollToWriters();

        // Open a writer at the given path with the given format.
        // containerCode: 0 = WAV, 1 = AIFF, 2 = FLAC. Used both at
        // startRecording and by the drain loop's auto-split path so a
        // long take that crosses the WAV 4 GiB / AIFF 2 GiB chunk
        // limit gets a fresh Track_NN_partXX.<ext> file instead of a
        // corrupt-header tail. Returns nullptr on failure (e.g. disk
        // full, permissions).
        juce::AudioFormatWriter* openWriterAtPath (const juce::File& target,
                                                   int containerCode,
                                                   int bits,
                                                   int numChannels = 1) noexcept;

    public:
        // Safe byte-count ceiling per container. WAV / FLAC: 3.9 GiB.
        // AIFF: 1.9 GiB. (The actual format limits are 4 GiB unsigned
        // for RIFF and 2 GiB signed for AIFF; we cut some margin so
        // header rewrites + tail flushes don't push over the line.)
        // containerCode: 0 = WAV, 1 = AIFF, 2 = FLAC.
        static juce::int64 maxBytesForContainer (int containerCode) noexcept;

        // Test hook: when > 0, overrides maxBytesForContainer for all
        // formats so a unit test can force the auto-split path without
        // writing actual gigabytes. 0 = use real limits (production).
        static void setAutoSplitThresholdBytesForTests (juce::int64 bytes) noexcept;

        // Test hook: force a writer-thread drain. Production code lets
        // the JUCE TimeSliceThread schedule this; tests feed the IO
        // callback synchronously and need a way to flush the ring
        // between blocks so the auto-split path doesn't roll one
        // gigantic accumulated write.
        void drainPendingForTests() { drainOnce(); }

        // Test hook: rewrite every open writer's header right now (the same
        // thing the periodic in-take flush does), so a test can verify that a
        // crashed-but-flushed file is already readable before stopRecording.
        void flushOpenWritersForTests() { flushOpenWriters (0, writers.size()); }

        // Report-write generation, keyed by SESSION.
        //
        // stopRecording writes session.report.json synchronously, then a
        // BACKGROUND thread hashes the audio (GBs) and rewrites it with the
        // SHA-256s. Two takes stopped close together put two hash threads on
        // the SAME file, and the slower one wins -- so the integrity manifest
        // could end up describing the PREVIOUS take. Worse, a punch splice
        // REPLACES the take while an earlier take's hash thread may still be
        // reading it, so that thread's hashes are of a file being overwritten.
        //
        // Each stop claims a generation for its session; a background write is
        // dropped once a newer stop has claimed one. Keyed by session PATH and
        // process-wide, because the racing stops are frequently different
        // recorder instances writing one session -- a per-recorder counter (the
        // first version of this fix) doesn't serialise them at all.
        //
        // Returns a shared token so the background thread can capture it BY
        // VALUE: that thread deliberately never captures `this`, so the token
        // has to be able to outlive the recorder.
        static std::shared_ptr<std::atomic<juce::int64>> claimReportGeneration (
            const juce::File& sessionDir, juce::int64& outGeneration);

    private:

        double sampleRate { 48000.0 };
        int    blockSize  { 512 };

        std::vector<std::unique_ptr<TrackState>>     tracks;
        std::atomic<int>                             trackGen { 0 };   // bumped on any track-set change
        std::vector<std::unique_ptr<ChannelFifo>>    fifos;
        std::vector<WriterChannel>                   writers;
        std::vector<std::unique_ptr<PreRollBuffer>>  preRoll;

        juce::AudioFormatManager formatManager;

        // Writer-thread pool. We start min(8, max(2, hardware/2)) worker
        // threads -- one TimeSliceThread per shard -- and each one drains
        // a contiguous slice of channels. rebuildShards() reshapes the
        // assignment when the track count changes.
        std::vector<std::unique_ptr<juce::TimeSliceThread>> writerThreads;
        std::vector<std::unique_ptr<ShardClient>>           shards;

        // Current audio device workgroup + a generation counter so the
        // shard threads know when to (re-)join.
        juce::CriticalSection workgroupLock;
        juce::AudioWorkgroup  currentWorkgroup;
        std::atomic<int>      workgroupGeneration { 0 };
        void joinAudioWorkgroupOnCurrentThread (juce::WorkgroupToken&);

        std::atomic<bool>        recording          { false };
        std::atomic<bool>        writersReady       { false };
        std::atomic<juce::int64> samplesSinceStart  { 0 };
        std::atomic<juce::int64> missedSamples      { 0 };
        std::atomic<int>         lastWriteMs        { 0 };
        // Read-modify-written on the drain thread and reset on the message
        // thread at record start; atomic so those accesses never tear.
        std::atomic<juce::int64> samplesSinceFlush  { 0 };

        // (Disk throughput + ring-fill accumulators now live per-shard in
        // ShardClient. The public getters in this class sum / max across
        // shards each time the UI polls them -- see the .cpp.)

        CaptureFormat captureFormat        { CaptureFormat::Wav24 };
        CaptureFormat backupCaptureFormat  { CaptureFormat::Wav24 };
        int           preRollSeconds { 0 };  // 0 = disabled

        juce::File activeSessionDir;
        juce::File backupDir;
        std::atomic<bool> backupActive  { false };
        std::atomic<bool> backupFailed  { false };

        // Primary-writer failure. Drain thread sets true when a
        // writeFromFloatArrays returns false (disk full, path gone).
        // The audio thread keeps capturing into the ring buffer; the
        // engineer just loses the primary file's tail. UI surfaces
        // this so the operator can act (swap drives, etc.) -- audio
        // continues to land on backup + any mirrors.
        std::atomic<bool> primaryFailed { false };

        // Disk-struggling flag + 2-second EMA of the keep-up ratio.
        // Updated by updateDiskHealth() from the UI timer; the audio
        // thread + writer threads don't read these.
        std::atomic<bool>  diskStrugglingFlag { false };
        float              diskKeepUpEma      { 1.0f };  // 0..1, 1.0 = caught up
        int                diskStrugglingStreak { 0 };   // consecutive low samples
    public:
        bool hasPrimaryFailed() const noexcept
        {
            return primaryFailed.load (std::memory_order_relaxed);
        }
    private:

        // Extra mirror destinations beyond primary + backup. Editable
        // only while not recording; the active take captures whatever
        // was configured at startRecording.
        std::vector<MirrorConfig> mirrorConfigs;
        // The subset of mirrorConfigs that actually opened for the live take:
        // collisions and unreachable drives are filtered out at record start.
        // WriterChannel::mirrors is built from THIS list, so anything indexing
        // a mirror by position (the session report) must index this one -- not
        // the configured list, which can be longer.
        std::vector<MirrorConfig> activeMirrors;
        std::atomic<int>          mirrorsSkippedAtStart { 0 };

        std::vector<float> scratch;

        // Punch-in state for the active take (set by armPunchIn, cleared in
        // stopRecording). punchInPos is the timeline sample the freshly-recorded
        // audio is spliced in at.
        bool        punchInActive { false };
        juce::int64 punchInPos    { 0 };
        bool        continueAsPart    { false };   // append a new part instead of overwriting
        juce::int64 recordBaseSamples { 0 };       // timeline offset for a continue take

        // Sidecar path that holds an existing take while a punch records over
        // it: "Audio Files/Track_01.wav" -> "Audio Files/Track_01.punchbase.wav".
        static juce::File punchSidecar (const juce::File& take)
        {
            return take.getParentDirectory().getChildFile (
                take.getFileNameWithoutExtension() + ".punchbase" + take.getFileExtension());
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackRecorder)
    };
}
