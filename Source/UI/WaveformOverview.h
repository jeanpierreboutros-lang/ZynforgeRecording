#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace zynforge
{
    struct WaveformOverview
    {
        std::vector<float> level;   // one height per screen column, 0..1
    };

    // At a multi-hour zoom, one screen pixel represents seconds of audio. A
    // single transient then fills the whole pixel if we draw only its maximum
    // (the solid blocks of a five-hour take), and a median body with the
    // maximum drawn behind it read as two waveforms on top of each other
    // (the three-hour take of 2026-11-04). Draw ONE shape instead: the
    // quadratic mean of 16 shorter peak windows per column. A sustained
    // passage keeps its level, a dense one stays near its peaks, and a lone
    // transient lifts its column a little instead of filling it.
    template <typename ReadPeak>
    WaveformOverview buildWaveformOverview (int width, double start, double end,
                                            ReadPeak readPeak)
    {
        WaveformOverview result;
        if (width <= 0 || end <= start) return result;
        result.level.resize ((size_t) width);

        constexpr int slices = 16;
        const double secondsPerPixel = (end - start) / (double) width;
        for (int x = 0; x < width; ++x)
        {
            const double pixelStart = start + (double) x * secondsPerPixel;
            float sumSquares = 0.0f;
            for (int i = 0; i < slices; ++i)
            {
                const double a = pixelStart + (double) i * secondsPerPixel / slices;
                const double b = pixelStart + (double) (i + 1) * secondsPerPixel / slices;
                const float p = std::clamp (readPeak (a, b), 0.0f, 1.0f);
                sumSquares += p * p;
            }
            result.level[(size_t) x] = std::sqrt (sumSquares / (float) slices);
        }

        // Soften one-column steps.
        const auto unsmoothed = result.level;
        for (int x = 0; x < width; ++x)
        {
            const auto left  = unsmoothed[(size_t) (x > 0 ? x - 1 : x)];
            const auto right = unsmoothed[(size_t) (x + 1 < width ? x + 1 : x)];
            result.level[(size_t) x] = (left + 2.0f * unsmoothed[(size_t) x] + right) * 0.25f;
        }
        return result;
    }
}
