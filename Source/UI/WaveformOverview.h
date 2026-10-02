#pragma once

#include <algorithm>
#include <array>
#include <vector>

namespace zynforge
{
    struct WaveformOverview
    {
        std::vector<float> body;
        std::vector<float> peaks;
    };

    // At a multi-hour zoom, one screen pixel represents seconds of audio. A
    // single transient then fills the whole pixel if we draw only its maximum.
    // Use the median of shorter peak windows for the sustained body, and keep
    // the true per-pixel maximum separately so brief peaks remain visible.
    template <typename ReadPeak>
    WaveformOverview buildWaveformOverview (int width, double start, double end,
                                            ReadPeak readPeak)
    {
        WaveformOverview result;
        if (width <= 0 || end <= start) return result;
        result.body.resize ((size_t) width);
        result.peaks.resize ((size_t) width);

        constexpr int slices = 16;
        const double secondsPerPixel = (end - start) / (double) width;
        for (int x = 0; x < width; ++x)
        {
            std::array<float, slices> levels {};
            const double pixelStart = start + (double) x * secondsPerPixel;
            for (int i = 0; i < slices; ++i)
            {
                const double a = pixelStart + (double) i * secondsPerPixel / slices;
                const double b = pixelStart + (double) (i + 1) * secondsPerPixel / slices;
                levels[(size_t) i] = std::clamp (readPeak (a, b), 0.0f, 1.0f);
            }
            result.peaks[(size_t) x] = *std::max_element (levels.begin(), levels.end());
            std::nth_element (levels.begin(), levels.begin() + slices / 2, levels.end());
            result.body[(size_t) x] = levels[slices / 2];
        }

        // Soften one-column steps in the body without altering peak markers.
        const auto unsmoothed = result.body;
        for (int x = 0; x < width; ++x)
        {
            const auto left  = unsmoothed[(size_t) (x > 0 ? x - 1 : x)];
            const auto right = unsmoothed[(size_t) (x + 1 < width ? x + 1 : x)];
            result.body[(size_t) x] = (left + 2.0f * unsmoothed[(size_t) x] + right) * 0.25f;
        }
        return result;
    }
}
