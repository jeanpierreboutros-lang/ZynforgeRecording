#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>

// CoreAudio can skip a whole IO cycle when the process misses its deadline
// ("IOWorkLoop: skipping cycle due to overload"). That cycle's input never
// reaches the callback, so nothing the recorder counts of its own losses moves.
// In the 2026-11-23 show it happened about every 73 s and the report said 0
// missed. The trace it leaves is the next buffer's host time: it is late by
// the frames that were lost.
//
// Audio thread only: no allocation, no lock. Frames are counted from stamp to
// stamp, because a split IO cycle is not stamped piece by piece: JUCE's
// CoreAudio device stamps only its first chunk, and JUCE's
// CallbackMaxSizeEnforcer (a driver whose buffer is bigger than the size
// asked for) hands every piece the same stamp. A stamp equal to the last one
// is that same cycle, so it counts as no stamp.
namespace zynforge
{
    class CycleGapDetector
    {
    public:
        // Frames lost before this block, 0 when none. `hostNs` may be null.
        juce::int64 onBlock (const std::uint64_t* hostNs, int frames, double sampleRate) noexcept
        {
            if (frames <= 0 || sampleRate <= 0.0)
            {
                reset();
                return 0;
            }
            juce::int64 lost = 0;
            if (hostNs != nullptr && (std::int64_t) *hostNs != lastNs)
            {
                const auto now = (std::int64_t) *hostNs;
                if (lastNs >= 0 && framesSinceStamp > 0)
                {
                    const double expected = (double) framesSinceStamp * 1.0e9 / sampleRate;
                    const double lateNs   = (double) (now - lastNs) - expected;
                    // Half a cycle late is not jitter: CoreAudio stamps are
                    // smooth, and a lost cycle is a whole cycle late.
                    if (lateNs > 0.5 * expected)
                        lost = (juce::int64) std::llround (lateNs * sampleRate / 1.0e9);
                }
                lastNs = now;
                framesSinceStamp = 0;
            }
            framesSinceStamp += frames;
            return lost;
        }

        // A device start, stop or stand-in: the next stamp starts afresh.
        void reset() noexcept
        {
            lastNs = -1;
            framesSinceStamp = 0;
        }

    private:
        std::int64_t lastNs { -1 };
        juce::int64  framesSinceStamp { 0 };
    };
}
