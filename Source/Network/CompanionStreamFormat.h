#pragma once

#include <juce_core/juce_core.h>

#include <limits>

namespace zynforge::companionstream
{
    // A classic RIFF/WAV stream has 32-bit chunk sizes. Browser audio clients
    // still expect a finite placeholder for a live response, so cap the
    // desired duration to the largest complete PCM frame whose enclosing RIFF
    // size (data + 36) is representable too.
    inline juce::uint32 placeholderDataBytes (int sampleRate, int channels,
                                               int bitsPerSample,
                                               int desiredHours = 24) noexcept
    {
        const auto safeRate = (juce::uint64) juce::jmax (1, sampleRate);
        const auto safeChannels = (juce::uint64) juce::jmax (1, channels);
        const auto bytesPerSample = (juce::uint64) juce::jmax (1, bitsPerSample / 8);
        const auto blockAlign = safeChannels * bytesPerSample;
        const auto seconds = (juce::uint64) juce::jmax (1, desiredHours) * 60u * 60u;
        const auto desired = safeRate * blockAlign * seconds;
        constexpr auto riffOverhead = (juce::uint64) 36u;
        constexpr auto maxData = (juce::uint64) std::numeric_limits<juce::uint32>::max()
                               - riffOverhead;
        const auto bounded = juce::jmin (desired, maxData);
        return (juce::uint32) (bounded - (bounded % blockAlign));
    }
}
