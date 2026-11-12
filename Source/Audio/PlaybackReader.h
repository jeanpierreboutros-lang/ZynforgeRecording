#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>

namespace zynforge
{
    // The session player's disk reader. Audio lives in RAM blocks a background
    // thread fills, the audio thread only copies, and a read of anything not
    // in RAM comes back as silence at once.
    //
    // juce::BufferingAudioReader works the same way but keeps one window ahead
    // of the last read, and learns of a new position only when a read has
    // already missed. Every jump therefore lost audio, up to its 100 ms idle
    // poll, by a different amount on each track. This reader also holds:
    //   - a CUE: where the player will read soon (the loop start, the next
    //     clip), so that block is in RAM before the jump;
    //   - two read positions, for one reader serving two places at once, as
    //     overlapping clips cut from one file do.
    // isBuffered lets the player hold a jump until every track has the target.
    //
    // read, cue, prepareAt and isBuffered are real-time safe: atomics and a
    // memcpy, no lock, no allocation. Only the background thread touches the
    // source reader. A block is pinned while copied from (`readers`), and the
    // background thread blanks `start` before it looks at the pin, so one side
    // always sees the other and a block is never refilled under a copy.
    class PlaybackReader final : public juce::AudioFormatReader,
                                 private juce::TimeSliceClient
    {
    public:
        PlaybackReader (juce::AudioFormatReader* sourceReader,
                        juce::TimeSliceThread& backgroundThread,
                        int samplesToBuffer)
            : juce::AudioFormatReader (nullptr, sourceReader->getFormatName()),
              source (sourceReader), thread (backgroundThread),
              playBlocks (1 + samplesToBuffer / kBlockSamples)
        {
            sampleRate            = source->sampleRate;
            lengthInSamples       = source->lengthInSamples;
            numChannels           = source->numChannels;
            metadataValues        = source->metadataValues;
            bitsPerSample         = 32;
            usesFloatingPointData = true;

            const int poolSize = playBlocks + kCueBlocks;
            pool.reserve ((size_t) poolSize);
            for (int i = 0; i < poolSize; ++i)
            {
                pool.push_back (std::make_unique<Block>());
                pool.back()->audio.setSize ((int) numChannels, kBlockSamples, false, false, true);
            }
            thread.addTimeSliceClient (this);
        }

        ~PlaybackReader() override { thread.removeTimeSliceClient (this); }

        // How long read may wait for the background thread. 0 = never wait,
        // which is what the audio thread needs. Set before a reader is shared.
        void setReadTimeout (int milliseconds) noexcept { timeoutMs = milliseconds; }

        // The player will read here soon. Keeps that block in RAM until a read
        // reaches it or the next cue, or until it goes unrefreshed for
        // kCueExpiry reads (the player cues again every few blocks while the
        // jump is still ahead). A cue held for ever took two of a small pool's
        // blocks from the read-ahead of every later crossfade.
        void cue (juce::int64 position) noexcept
        {
            if (position >= 0 && position < lengthInSamples)
            {
                cueTick.store (readTick.load (std::memory_order_relaxed), std::memory_order_relaxed);
                cuePosition.store (position, std::memory_order_relaxed);
            }
        }

        // Fill from here as a read here would, without reading.
        void prepareAt (juce::int64 position) noexcept { noteRead (position); }

        // True when all of [position, position + numSamples) is in RAM. Past
        // the end of the file there is nothing to wait for.
        bool isBuffered (juce::int64 position, juce::int64 numSamples) const noexcept
        {
            if (position >= lengthInSamples) return true;
            const auto end = juce::jmin (lengthInSamples, position + numSamples);
            for (auto start = blockStart (position); start < end; start += kBlockSamples)
                if (! isLoaded (start)) return false;
            return true;
        }

        bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                          juce::int64 startSampleInFile, int numSamples) override
        {
            const auto began = timeoutMs != 0 ? juce::Time::getMillisecondCounter() : 0u;
            clearSamplesBeyondAvailableLength (destChannels, numDestChannels, startOffsetInDestBuffer,
                                               startSampleInFile, numSamples, lengthInSamples);
            if (numSamples <= 0) return true;

            noteRead (startSampleInFile);
            bool allRead = true;
            while (numSamples > 0)
            {
                const int copied = copyFromBlock (destChannels, numDestChannels, startOffsetInDestBuffer,
                                                  startSampleInFile, numSamples, allRead);
                if (copied > 0)
                {
                    startOffsetInDestBuffer += copied;
                    startSampleInFile       += copied;
                    numSamples              -= copied;
                    continue;
                }
                if (timeoutMs >= 0 && (timeoutMs == 0
                        || juce::Time::getMillisecondCounter() - began >= (juce::uint32) timeoutMs))
                {
                    for (int ch = 0; ch < numDestChannels; ++ch)
                        if (auto* dest = reinterpret_cast<float*> (destChannels[ch]))
                            juce::FloatVectorOperations::clear (dest + startOffsetInDestBuffer, numSamples);
                    return false;
                }
                juce::Thread::yield();
            }
            return allRead;
        }

    private:
        static constexpr int kBlockSamples = 32768;
        // Two blocks around a cue: the second covers a target near the end of
        // the first, and the time the read window needs to catch up.
        static constexpr int kCueBlocks = 2;
        static constexpr juce::int64 kEmpty = -1;
        // A new position only notices an idle reader when the thread next
        // calls it, so this is the longest a jump waits before disk work
        // starts. 10 ms per reader is a few thousand no-op calls a second.
        static constexpr int kIdleCheckMs = 10;
        // Reads a position may go unused before its blocks are released. After
        // a plain jump the old position must not keep holding RAM.
        static constexpr juce::uint32 kReadPositionExpiry = 64;
        // Reads a cue may go unrefreshed: a loop turned off, a locate elsewhere.
        static constexpr juce::uint32 kCueExpiry = 4 * kReadPositionExpiry;

        struct Block
        {
            std::atomic<juce::int64> start { kEmpty };
            std::atomic<int>  readers { 0 };
            std::atomic<bool> clean   { true };
            juce::AudioBuffer<float> audio;
        };

        static juce::int64 blockStart (juce::int64 position) noexcept
        {
            return (position / kBlockSamples) * kBlockSamples;
        }

        bool isLoaded (juce::int64 start) const noexcept
        {
            for (const auto& block : pool)
                if (block->start.load (std::memory_order_acquire) == start) return true;
            return false;
        }

        void noteRead (juce::int64 position) noexcept
        {
            const auto tick = readTick.fetch_add (1, std::memory_order_relaxed) + 1;
            // A read inside the cue's blocks is the jump it was for. The read
            // window keeps those blocks from here, so the cue lets them go.
            auto cued = cuePosition.load (std::memory_order_relaxed);
            if (cued >= 0 && position >= blockStart (cued)
                && position < blockStart (cued) + (juce::int64) kCueBlocks * kBlockSamples)
                cuePosition.compare_exchange_strong (cued, -1, std::memory_order_relaxed);
            const auto near = [position] (juce::int64 at)
            {
                return at >= 0 && std::abs (position - at) <= (juce::int64) kBlockSamples;
            };
            const bool nearFirst  = near (readPositions[0].load (std::memory_order_relaxed));
            const bool nearSecond = near (readPositions[1].load (std::memory_order_relaxed));
            const bool firstOlder = readTicks[0].load (std::memory_order_relaxed)
                                 <= readTicks[1].load (std::memory_order_relaxed);
            const int slot = nearFirst ? 0 : nearSecond ? 1 : firstOlder ? 0 : 1;
            // Two clips of one file less than a block apart share a slot, and
            // the later one, read last, is what it holds. The earlier one's
            // place is kept beside it, or its block can be the next victim
            // in the middle of their crossfade. A reader moving forward never
            // reads behind its slot, so this costs plain playback nothing.
            if ((nearFirst || nearSecond) && position < readPositions[slot].load (std::memory_order_relaxed))
            {
                readLows[slot].store (position, std::memory_order_relaxed);
                lowTicks[slot].store (tick, std::memory_order_relaxed);
            }
            else if (! (nearFirst || nearSecond))
            {
                readLows[slot].store (-1, std::memory_order_relaxed);
            }
            readPositions[slot].store (position, std::memory_order_relaxed);
            readTicks[slot].store (tick, std::memory_order_relaxed);
        }

        int copyFromBlock (int* const* destChannels, int numDestChannels, int destOffset,
                           juce::int64 position, int numSamples, bool& allRead) noexcept
        {
            const auto start = blockStart (position);
            for (auto& block : pool)
            {
                if (block->start.load (std::memory_order_acquire) != start) continue;
                block->readers.fetch_add (1, std::memory_order_seq_cst);
                int copied = 0;
                if (block->start.load (std::memory_order_seq_cst) == start)
                {
                    const int offset = (int) (position - start);
                    copied = juce::jmin (numSamples, kBlockSamples - offset);
                    for (int ch = 0; ch < numDestChannels; ++ch)
                        if (auto* dest = reinterpret_cast<float*> (destChannels[ch]))
                        {
                            if (ch < (int) numChannels)
                                juce::FloatVectorOperations::copy (dest + destOffset,
                                                                   block->audio.getReadPointer (ch, offset), copied);
                            else
                                juce::FloatVectorOperations::clear (dest + destOffset, copied);
                        }
                    allRead = allRead && block->clean.load (std::memory_order_relaxed);
                }
                block->readers.fetch_sub (1, std::memory_order_seq_cst);
                if (copied > 0) return copied;
            }
            return 0;
        }

        int useTimeSlice() override { return fillNextBlock() ? 1 : kIdleCheckMs; }

        // Load one wanted block that is not in RAM yet. True when there may
        // be more to do.
        bool fillNextBlock()
        {
            std::array<juce::int64, 32> wanted;
            const int limit = juce::jmin ((int) pool.size(), (int) wanted.size());
            int count = 0;
            const auto want = [&] (juce::int64 start)
            {
                if (start >= lengthInSamples || count >= limit) return;
                for (int i = 0; i < count; ++i)
                    if (wanted[(size_t) i] == start) return;
                wanted[(size_t) count++] = start;
            };

            // A fixed order: both read positions alternate on overlapping
            // clips, and swapping them per read would refill blocks for ever.
            const auto tick  = readTick.load (std::memory_order_relaxed);
            const auto ticks = std::array<juce::uint32, 2> { readTicks[0].load (std::memory_order_relaxed),
                                                             readTicks[1].load (std::memory_order_relaxed) };
            const int  newest = ticks[0] >= ticks[1] ? 0 : 1;
            std::array<juce::int64, 2> reads { -1, -1 }, lows { -1, -1 };
            for (int i = 0; i < 2; ++i)
            {
                if (i == newest || tick - ticks[(size_t) i] <= kReadPositionExpiry)
                    reads[(size_t) i] = readPositions[i].load (std::memory_order_relaxed);
                if (reads[(size_t) i] >= 0 && tick - lowTicks[i].load (std::memory_order_relaxed) <= kReadPositionExpiry)
                    lows[(size_t) i] = readLows[i].load (std::memory_order_relaxed);
            }
            auto cued = cuePosition.load (std::memory_order_relaxed);
            if (cued >= 0 && tick - cueTick.load (std::memory_order_relaxed) > kCueExpiry) cued = -1;
            for (int step = 0; step < playBlocks; ++step)
            {
                const auto ahead = (juce::int64) step * kBlockSamples;
                for (const auto at : reads)
                    if (at >= 0) want (blockStart (at) + ahead);
                // The earlier clip of a shared slot: its block and the next.
                if (step < 2)
                    for (const auto low : lows)
                        if (low >= 0) want (blockStart (low) + ahead);
                if (step < kCueBlocks && cued >= 0) want (blockStart (cued) + ahead);
            }

            juce::int64 missing = kEmpty;
            for (int i = 0; i < count && missing == kEmpty; ++i)
                if (! isLoaded (wanted[(size_t) i])) missing = wanted[(size_t) i];
            if (missing == kEmpty) return false;

            Block* victim = nullptr;
            for (auto& block : pool)
            {
                const auto held = block->start.load (std::memory_order_relaxed);
                bool keep = false;
                for (int i = 0; i < count && ! keep && held != kEmpty; ++i)
                    keep = wanted[(size_t) i] == held;
                if (! keep) { victim = block.get(); break; }
            }
            if (victim == nullptr) return false;

            victim->start.store (kEmpty, std::memory_order_seq_cst);
            if (victim->readers.load (std::memory_order_seq_cst) != 0)
                return true;   // being copied from: it is free next time

            const auto numSamples = (int) juce::jmin ((juce::int64) kBlockSamples, lengthInSamples - missing);
            victim->clean.store (source->read (&victim->audio, 0, numSamples, missing, true, true),
                                 std::memory_order_relaxed);
            victim->start.store (missing, std::memory_order_seq_cst);
            return true;
        }

        std::unique_ptr<juce::AudioFormatReader> source;
        juce::TimeSliceThread& thread;
        const int playBlocks;
        int timeoutMs { 0 };
        std::vector<std::unique_ptr<Block>> pool;

        // Two read positions, for one reader serving two places at once.
        std::atomic<juce::int64>  readPositions[2] { { 0 }, { -1 } };
        std::atomic<juce::uint32> readTicks[2]     { { 0 }, { 0 } };
        // An earlier position merged into each slot, and when it was last read.
        std::atomic<juce::int64>  readLows[2]      { { -1 }, { -1 } };
        std::atomic<juce::uint32> lowTicks[2]      { { 0 }, { 0 } };
        std::atomic<juce::uint32> readTick    { 0 };
        std::atomic<juce::int64>  cuePosition { -1 };
        std::atomic<juce::uint32> cueTick     { 0 };   // readTick when it was cued

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackReader)
    };
}
