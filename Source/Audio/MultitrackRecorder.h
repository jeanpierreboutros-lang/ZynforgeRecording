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
        Wav32Float,   // 32-bit IEEE float WAV — clip-proof at file level
        Aiff16,       // 16-bit PCM AIFF
        Aiff24,       // 24-bit PCM AIFF
        Aiff32Float,  // 32-bit IEEE float AIFF
        Flac16,       // 16-bit FLAC
        Flac24        // 24-bit FLAC — ~50% disk size vs WAV
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

        // Capture format & pre-roll are settings — only changeable while not recording.
        void setCaptureFormat (CaptureFormat f) noexcept     { if (! isRecording()) captureFormat = f; }
        CaptureFormat getCaptureFormat() const noexcept       { return captureFormat; }

        void setPreRollSeconds (int seconds);
        int  getPreRollSeconds() const noexcept               { return preRollSeconds; }

        int          getNumTracks() const noexcept { return (int) tracks.size(); }
        TrackState&  getTrack (int i) noexcept     { return *tracks[(std::size_t) i]; }

        // Health + position counters — all RT-safe to read.
        juce::int64 getSamplesSinceStart() const noexcept { return samplesSinceStart.load(std::memory_order_relaxed); }
        juce::int64 getMissedSamples()     const noexcept { return missedSamples    .load(std::memory_order_relaxed); }
        int         getLastWriteMs()       const noexcept { return lastWriteMs      .load(std::memory_order_relaxed); }
        juce::File  getActiveSessionDir()  const          { return activeSessionDir; }

        // Optional second copy of every track to a backup folder.
        // Pass an empty File to disable. Only effective for the next session.
        void setBackupDirectory (const juce::File& dir);
        juce::File getBackupDirectory() const               { return backupDir; }
        bool       isBackupActive() const noexcept          { return backupActive.load (std::memory_order_relaxed); }
        bool       hasBackupFailed() const noexcept         { return backupFailed.load (std::memory_order_relaxed); }

    private:
        int useTimeSlice() override;

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
        juce::TimeSliceThread    writerThread { "ZF Recorder Writer" };

        std::atomic<bool>        recording          { false };
        std::atomic<bool>        writersReady       { false };
        std::atomic<juce::int64> samplesSinceStart  { 0 };
        std::atomic<juce::int64> missedSamples      { 0 };
        std::atomic<int>         lastWriteMs        { 0 };
        juce::int64              samplesSinceFlush  { 0 };

        CaptureFormat captureFormat  { CaptureFormat::Wav24 };
        int           preRollSeconds { 0 };  // 0 = disabled

        juce::File activeSessionDir;
        juce::File backupDir;
        std::atomic<bool> backupActive { false };
        std::atomic<bool> backupFailed { false };

        std::vector<float> scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackRecorder)
    };
}
