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
        TrackState&  getTrack (int i) noexcept     { return *tracks[(std::size_t) i]; }

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
        void setMirrors (const std::vector<MirrorConfig>& configs);
        std::vector<MirrorConfig> getMirrors() const               { return mirrorConfigs; }
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

            int useTimeSlice() override;
        };

        struct ChannelFifo
        {
            juce::AbstractFifo fifo { 1 };
            std::vector<float> data;

            void resize (int size)
            {
                data.assign ((std::size_t) size, 0.0f);
                fifo.setTotalSize (size);
                fifo.reset();
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
        };

        // Per-channel rolling history used for pre-roll. Audio thread is the
        // only writer; UI thread reads only via dumpPreRollToWriters() before
        // setting recording=true, so no lock is needed.
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
                                                   int bits) noexcept;

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
        juce::int64              samplesSinceFlush  { 0 };

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

        std::vector<float> scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackRecorder)
    };
}
