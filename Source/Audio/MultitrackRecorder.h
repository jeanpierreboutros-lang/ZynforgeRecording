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
    // Lock-free per-channel ring buffer + background disk writer.
    // The audio thread only ever pushes; a dedicated TimeSliceClient
    // drains and writes WAV files.
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

        int          getNumTracks() const noexcept { return (int) tracks.size(); }
        TrackState&  getTrack (int i) noexcept     { return *tracks[(std::size_t) i]; }

        // Health + position counters — all RT-safe to read.
        juce::int64 getSamplesSinceStart() const noexcept { return samplesSinceStart.load(std::memory_order_relaxed); }
        juce::int64 getMissedSamples()     const noexcept { return missedSamples    .load(std::memory_order_relaxed); }
        int         getLastWriteMs()       const noexcept { return lastWriteMs      .load(std::memory_order_relaxed); }
        juce::File  getActiveSessionDir()  const          { return activeSessionDir; }

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
        };

        void drainOnce();
        void closeWriters();

        double sampleRate { 48000.0 };
        int    blockSize  { 512 };

        std::vector<std::unique_ptr<TrackState>>  tracks;
        std::vector<std::unique_ptr<ChannelFifo>> fifos;
        std::vector<WriterChannel>                writers;

        juce::AudioFormatManager formatManager;
        juce::TimeSliceThread    writerThread { "ZF Recorder Writer" };

        std::atomic<bool>        recording          { false };
        std::atomic<bool>        writersReady       { false };
        std::atomic<juce::int64> samplesSinceStart  { 0 };
        std::atomic<juce::int64> missedSamples      { 0 };
        std::atomic<int>         lastWriteMs        { 0 };
        juce::int64              samplesSinceFlush  { 0 };

        juce::File activeSessionDir;
        std::vector<float> scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackRecorder)
    };
}
