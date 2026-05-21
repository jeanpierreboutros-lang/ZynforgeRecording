#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

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
        juce::int64 getPositionSamples()    const noexcept { return position   .load(); }
        double      getSampleRate()         const noexcept { return fileSampleRate; }
        juce::String getSessionName()       const          { return sessionName; }

        void start();
        void stop();
        void rewind();
        void setPositionSamples (juce::int64 s);

        // RT-safe: fills outputs[i] with samples from track i, position advances.
        void processBlock (float* const* outputs, int numOutputs, int numSamples) noexcept;

    private:
        struct Track
        {
            std::unique_ptr<juce::BufferingAudioReader> reader;
            juce::int64 length { 0 };
        };

        juce::AudioFormatManager formatManager;
        juce::TimeSliceThread    readerThread { "ZF Player Reader" };

        std::vector<Track> tracks;

        std::atomic<bool>        loaded      { false };
        std::atomic<bool>        playing     { false };
        std::atomic<int>         readerCount { 0 };
        std::atomic<juce::int64> position    { 0 };
        std::atomic<juce::int64> totalLength { 0 };

        double deviceSampleRate { 48000.0 };
        double fileSampleRate   { 48000.0 };
        int    blockSize        { 512 };
        juce::String sessionName;

        juce::AudioBuffer<float> scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionPlayer)
    };
}
