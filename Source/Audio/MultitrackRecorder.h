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

        // Optional second copy of every track to a backup folder.
        // Pass an empty File to disable. Only effective for the next session.
        void setBackupDirectory (const juce::File& dir);
        juce::File getBackupDirectory() const               { return backupDir; }
        bool       isBackupActive() const noexcept          { return backupActive.load (std::memory_order_relaxed); }
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

        double sampleRate { 48000.0 };
        int    blockSize  { 512 };

        std::vector<std::unique_ptr<TrackState>>     tracks;
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
        std::atomic<bool> backupActive { false };
        std::atomic<bool> backupFailed { false };

        std::vector<float> scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackRecorder)
    };
}
