#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace zynforge
{
    // Bounded UI-only overview of a growing take. The whole-take history is
    // downsampled as it grows, while a ring retains the original recorder bins
    // at the live edge. At 48 kHz the ring covers about 5.8 minutes, enough for
    // the five-minute recording viewport even after a very long take.
    class LivePeakHistory
    {
    public:
        static constexpr size_t maxPoints = 1 << 15;
        static constexpr size_t recentPoints = 1 << 16;

        explicit LivePeakHistory (int inputBinSamples) : binSamples (inputBinSamples) {}

        void reset (juce::int64 prefillBins = 0)
        {
            totalBins = juce::jmax ((juce::int64) 0, prefillBins);
            binsPerPoint = 1;
            pendingPeak = 0.0f;
            // A three-hour continuation must not allocate millions of silent
            // columns per track before the first new sample arrives.
            while (totalBins / binsPerPoint > (juce::int64) maxPoints / 2)
                binsPerPoint *= 2;
            points.assign ((size_t) (totalBins / binsPerPoint), 0.0f);
            pendingBins = totalBins % binsPerPoint;
            recent.clear();
            recent.reserve (recentPoints);
            recentNext = 0;
        }

        void append (float peak)
        {
            ++totalBins;
            const float p = juce::jlimit (0.0f, 1.0f, peak);
            if (recent.size() < recentPoints)
                recent.push_back (p);
            else
                recent[recentNext] = p;
            recentNext = (recentNext + 1) % recentPoints;

            pendingPeak = juce::jmax (pendingPeak, p);
            if (++pendingBins < binsPerPoint) return;

            points.push_back (pendingPeak);
            pendingPeak = 0.0f;
            pendingBins = 0;
            if (points.size() < maxPoints) return;

            const size_t half = points.size() / 2;
            for (size_t i = 0; i < half; ++i)
                points[i] = juce::jmax (points[2 * i], points[2 * i + 1]);
            points.resize (half);
            binsPerPoint *= 2;
        }

        size_t size() const noexcept { return points.size() + (pendingBins > 0 ? 1 : 0); }
        bool empty() const noexcept { return size() == 0; }
        float at (size_t i) const noexcept
        { return i < points.size() ? points[i] : pendingPeak; }
        juce::int64 binCount() const noexcept { return totalBins; }
        juce::int64 spanSamples() const noexcept { return totalBins * binSamples; }
        juce::int64 getBinsPerPoint() const noexcept { return binsPerPoint; }

        // Map a visible pixel's time range to the best available peaks. The
        // recent ring preserves the raw 256-sample bins; older audio uses the
        // bounded overview. A range crossing the seam combines both parts.
        float maxPeakInBinRange (juce::int64 begin, juce::int64 end) const noexcept
        {
            begin = juce::jlimit ((juce::int64) 0, totalBins, begin);
            end   = juce::jlimit (begin, totalBins, end);
            if (begin == end) return 0.0f;

            const auto recentBegin = totalBins - (juce::int64) recent.size();
            float peak = 0.0f;
            if (begin < recentBegin)
            {
                const auto first = begin / binsPerPoint;
                const auto last = juce::jmin ((juce::int64) size() - 1,
                                             (juce::jmin (end, recentBegin) - 1) / binsPerPoint);
                for (auto i = first; i <= last; ++i)
                    peak = juce::jmax (peak, at ((size_t) i));
            }
            if (end > recentBegin)
            {
                const auto oldest = recent.size() == recentPoints ? recentNext : 0;
                for (auto bin = juce::jmax (begin, recentBegin); bin < end; ++bin)
                {
                    const auto offset = (size_t) (bin - recentBegin);
                    peak = juce::jmax (peak, recent[(oldest + offset) % recentPoints]);
                }
            }
            return peak;
        }

    private:
        std::vector<float> points;
        std::vector<float> recent;
        size_t recentNext { 0 };
        juce::int64 totalBins { 0 }, binsPerPoint { 1 }, pendingBins { 0 };
        float pendingPeak { 0.0f };
        const int binSamples;
    };
}
