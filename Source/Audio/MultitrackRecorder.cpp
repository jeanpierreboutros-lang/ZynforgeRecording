#include "MultitrackRecorder.h"

namespace zynforge
{
    static constexpr int kFifoSeconds = 4;   // per-channel ring buffer length

    MultitrackRecorder::MultitrackRecorder()
    {
        formatManager.registerBasicFormats();
        writerThread.startThread();
    }

    MultitrackRecorder::~MultitrackRecorder()
    {
        stopRecording();
        writerThread.removeTimeSliceClient (this);
        writerThread.stopThread (2000);
    }

    void MultitrackRecorder::prepare (double sr, int maxBlock, int numInputs)
    {
        stopRecording();
        writerThread.removeTimeSliceClient (this);

        sampleRate = sr;
        blockSize  = maxBlock;

        const int fifoSize = juce::nextPowerOfTwo ((int) (sr * kFifoSeconds));
        scratch.assign ((std::size_t) maxBlock, 0.0f);

        tracks.clear();
        fifos.clear();
        for (int i = 0; i < numInputs; ++i)
        {
            auto t = std::make_unique<TrackState>();
            t->name = "In " + juce::String (i + 1);
            tracks.push_back (std::move (t));

            auto f = std::make_unique<ChannelFifo>();
            f->resize (fifoSize);
            fifos.push_back (std::move (f));
        }

        writerThread.addTimeSliceClient (this);
    }

    void MultitrackRecorder::release()
    {
        stopRecording();
        writerThread.removeTimeSliceClient (this);
        tracks.clear();
        fifos.clear();
    }

    void MultitrackRecorder::processBlock (const float* const* inputs,
                                           int numChannels,
                                           int numSamples) noexcept
    {
        const int n = juce::jmin (numChannels, (int) fifos.size());

        for (int ch = 0; ch < n; ++ch)
        {
            const float* src = inputs[ch];
            if (src == nullptr) continue;

            // Meter (peak + simple block RMS)
            float peak = 0.0f, sumSq = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                const float s = src[i];
                const float a = std::abs (s);
                if (a > peak) peak = a;
                sumSq += s * s;
            }
            const float rms = std::sqrt (sumSq / (float) juce::jmax (1, numSamples));
            auto& t = *tracks[(std::size_t) ch];
            // peak-hold style: replace if louder, else decay
            const float prevPeak = t.peak.load (std::memory_order_relaxed);
            t.peak.store (juce::jmax (peak, prevPeak * 0.92f), std::memory_order_relaxed);
            t.rms .store (rms,  std::memory_order_relaxed);
            if (peak >= 0.999f) t.clipped.store (true, std::memory_order_relaxed);

            // Push to ring buffer when recording
            if (recording.load (std::memory_order_acquire) && t.armed.load (std::memory_order_relaxed))
            {
                auto& cf = *fifos[(std::size_t) ch];
                const auto scope = cf.fifo.write (numSamples);

                if (scope.blockSize1 > 0)
                    std::memcpy (cf.data.data() + scope.startIndex1,
                                 src,
                                 (std::size_t) scope.blockSize1 * sizeof (float));
                if (scope.blockSize2 > 0)
                    std::memcpy (cf.data.data() + scope.startIndex2,
                                 src + scope.blockSize1,
                                 (std::size_t) scope.blockSize2 * sizeof (float));
            }
        }
    }

    bool MultitrackRecorder::startRecording (const juce::File& sessionDir)
    {
        if (recording.load()) return false;
        if (tracks.empty())   return false;

        sessionDir.createDirectory();

        writers.clear();
        writers.reserve (tracks.size());

        juce::WavAudioFormat wav;
        const int bitDepth = 24;

        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            WriterChannel w;
            const auto name = juce::String::formatted ("Track_%02d.wav", (int) i + 1);
            auto file = sessionDir.getChildFile (name);
            file.deleteFile();

            if (auto* out = file.createOutputStream().release())
            {
                std::unique_ptr<juce::AudioFormatWriter> aw (
                    wav.createWriterFor (out, sampleRate, 1, bitDepth, {}, 0));
                if (aw == nullptr) { delete out; }
                w.writer = std::move (aw);
            }
            writers.push_back (std::move (w));

            fifos[i]->fifo.reset();
        }

        writersReady.store (true,  std::memory_order_release);
        recording   .store (true,  std::memory_order_release);
        return true;
    }

    void MultitrackRecorder::stopRecording()
    {
        if (! recording.exchange (false, std::memory_order_acq_rel))
            return;

        // Drain remaining samples before closing.
        for (int i = 0; i < 8; ++i) drainOnce();

        closeWriters();
    }

    void MultitrackRecorder::closeWriters()
    {
        writersReady.store (false, std::memory_order_release);
        writers.clear();
    }

    int MultitrackRecorder::useTimeSlice()
    {
        drainOnce();
        return 5; // ms
    }

    void MultitrackRecorder::drainOnce()
    {
        if (! writersReady.load (std::memory_order_acquire)) return;

        for (std::size_t i = 0; i < fifos.size() && i < writers.size(); ++i)
        {
            auto& cf = *fifos[i];
            auto& w  = writers[i];
            if (w.writer == nullptr) continue;

            const int available = cf.fifo.getNumReady();
            if (available <= 0) continue;

            const auto scope = cf.fifo.read (available);

            if (scope.blockSize1 > 0)
            {
                const float* ptr = cf.data.data() + scope.startIndex1;
                const float* const channels[] = { ptr };
                w.writer->writeFromFloatArrays (channels, 1, scope.blockSize1);
            }
            if (scope.blockSize2 > 0)
            {
                const float* ptr = cf.data.data() + scope.startIndex2;
                const float* const channels[] = { ptr };
                w.writer->writeFromFloatArrays (channels, 1, scope.blockSize2);
            }
        }
    }
}
