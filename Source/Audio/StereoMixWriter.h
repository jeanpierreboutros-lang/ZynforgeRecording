#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "CheckedFileStream.h"

#include <atomic>
#include <memory>

namespace zynforge
{
    // StereoMix.wav's writer. The audio callback hands it each block and never
    // waits: no lock and no signal to the writer thread, which polls instead.
    // (JUCE's ThreadedWriter wakes its thread on every block, and that takes a
    // mutex in the callback.) The writer thread does everything else: the
    // file writes, a stretch of silence where the device was away, and the
    // flag that says a write failed (JUCE's drops a writer's result).
    class StereoMixWriter final : private juce::TimeSliceClient
    {
    public:
        // `failed` outlives this writer (it is the engine's), and is raised on
        // any write the file did not take, a full queue included. `fileFailed`
        // is the stream's own: a short close shows only once the writer is gone.
        StereoMixWriter (std::unique_ptr<juce::AudioFormatWriter> innerIn, juce::TimeSliceThread& threadIn,
                         int bufferFrames, std::atomic<bool>& failedIn, CheckedFileStream::Failed fileFailedIn,
                         int flushIntervalFrames)
            : writer (std::move (innerIn)), thread (threadIn), failed (failedIn),
              fileFailed (std::move (fileFailedIn)), fifo (bufferFrames),
              buffer ((int) writer->getNumChannels(), bufferFrames),
              silence ((int) writer->getNumChannels(), kSilenceFrames),
              flushInterval (flushIntervalFrames)
        {
            // Only `silence` is cleared: the queue's buffer is read only where
            // the callback wrote, and clearing it would leave its flag to race.
            silence.clear();
            thread.addTimeSliceClient (this);
        }

        // Writes what is queued, then a lead-in still owed, and closes the file.
        ~StereoMixWriter() override
        {
            thread.removeTimeSliceClient (this);   // waits out a slice in progress
            while (writePending()) {}
            writer.reset();
            if (fileFailed != nullptr && fileFailed->load()) failed.store (true, std::memory_order_relaxed);
        }

        // Audio thread. False when the queue is full: those frames are lost.
        bool write (const float* const* channels, int numSamples) noexcept
        {
            if (numSamples <= 0) return true;
            int start1, size1, start2, size2;
            fifo.prepareToWrite (numSamples, start1, size1, start2, size2);
            if (size1 + size2 < numSamples) return false;
            for (int c = 0; c < buffer.getNumChannels(); ++c)
            {
                buffer.copyFrom (c, start1, channels[c], size1);
                if (size2 > 0) buffer.copyFrom (c, start2, channels[c] + size1, size2);
            }
            fifo.finishedWrite (size1 + size2);
            queued += numSamples;
            return true;
        }

        // Audio thread, before the block that follows a stretch the device
        // did not deliver: that many frames of silence go in at this point.
        void addGap (juce::int64 frames) noexcept
        {
            if (frames <= 0) return;
            // One at a time; the writer thread takes it within milliseconds,
            // and a take is held open far longer than that between two.
            if (gapAt.load (std::memory_order_acquire) >= 0)
            {
                failed.store (true, std::memory_order_relaxed);
                return;
            }
            gapFrames.store (frames, std::memory_order_relaxed);
            gapAt.store (queued, std::memory_order_release);
        }

    private:
        static constexpr int kSilenceFrames = 4096;

        int useTimeSlice() override { return writePending() ? 0 : 5; }

        // Writer thread. True while it wrote something.
        bool writePending()
        {
            // Ready first, then the gap: a block queued after a gap is only
            // seen once the gap that comes before it is.
            const int ready = fifo.getNumReady();
            const auto at = gapAt.load (std::memory_order_acquire);
            if (at >= 0 && at == consumed)
            {
                for (auto left = gapFrames.load (std::memory_order_relaxed); left > 0;)
                {
                    const int count = (int) juce::jmin (left, (juce::int64) kSilenceFrames);
                    writeFrom (silence, 0, count);
                    left -= count;
                }
                gapAt.store (-1, std::memory_order_release);
                return true;
            }
            int wanted = ready;
            if (at >= 0) wanted = (int) juce::jmin ((juce::int64) ready, at - consumed);
            if (wanted <= 0) return false;
            int start1, size1, start2, size2;
            fifo.prepareToRead (wanted, start1, size1, start2, size2);
            writeFrom (buffer, start1, size1);
            writeFrom (buffer, start2, size2);
            fifo.finishedRead (size1 + size2);
            consumed += size1 + size2;
            return size1 + size2 > 0;
        }

        void writeFrom (const juce::AudioBuffer<float>& source, int start, int count)
        {
            if (count <= 0) return;
            if (! writer->writeFromAudioSampleBuffer (source, start, count))
                failed.store (true, std::memory_order_relaxed);
            untilFlush -= count;
            if (flushInterval > 0 && untilFlush <= 0)
            {
                untilFlush = flushInterval;
                writer->flush();
            }
        }

        std::unique_ptr<juce::AudioFormatWriter> writer;
        juce::TimeSliceThread& thread;
        std::atomic<bool>& failed;
        CheckedFileStream::Failed fileFailed;
        juce::AbstractFifo fifo;
        juce::AudioBuffer<float> buffer, silence;
        juce::int64 queued { 0 };      // audio thread: frames queued so far
        juce::int64 consumed { 0 };    // writer thread: of those, frames written
        std::atomic<juce::int64> gapAt { -1 }, gapFrames { 0 };
        const int flushInterval;
        int untilFlush { 0 };   // the first write puts a length in the header at once

        JUCE_DECLARE_NON_COPYABLE (StereoMixWriter)
    };
}
