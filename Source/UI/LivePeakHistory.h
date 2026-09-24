#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace zynforge
{
    // Bounded UI-only overview of a growing take. Incoming peaks each cover
    // one recorder bin. After downsampling, aggregate future bins at the new
    // resolution too; otherwise a long take repeatedly doubles its apparent
    // duration and eventually overflows the old int sample counter.
    class LivePeakHistory
    {
    public:
        static constexpr size_t maxPoints = 1 << 15;

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
        }

        void append (float peak)
        {
            ++totalBins;
            pendingPeak = juce::jmax (pendingPeak, juce::jlimit (0.0f, 1.0f, peak));
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
        juce::int64 spanSamples() const noexcept { return totalBins * binSamples; }
        juce::int64 getBinsPerPoint() const noexcept { return binsPerPoint; }

    private:
        std::vector<float> points;
        juce::int64 totalBins { 0 }, binsPerPoint { 1 }, pendingBins { 0 };
        float pendingPeak { 0.0f };
        const int binSamples;
    };
}
