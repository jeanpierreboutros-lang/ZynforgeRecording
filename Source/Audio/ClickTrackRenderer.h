#pragma once

#include "ClickEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <vector>

namespace zynforge::clickrender
{
    struct TempoChange
    {
        juce::int64 samplePos { 0 };
        float bpm { 120.0f };
    };

    struct Settings
    {
        double sampleRate { 48000.0 };
        juce::int64 totalSamples { 0 };
        float initialBpm { 120.0f };
        int beatsPerBar { 4 };
        ClickEngine::VoicePreset voice1 { 1000.0, 60.0 };
        ClickEngine::VoicePreset voice2 { 1000.0, 60.0 };
        ClickEngine::Subdivision subdivision1 { ClickEngine::Subdivision::Quarter };
        ClickEngine::Subdivision subdivision2 { ClickEngine::Subdivision::Quarter };
        float gain1 { 1.0f };
        float gain2 { 1.0f };
        std::vector<TempoChange> tempoMap;
    };

    enum class Result { succeeded, cancelled, failed };

    // Writes to a unique sibling first and atomically installs it only after
    // the WAV writer has closed successfully. The previous click therefore
    // survives disk-full, cancellation, and encode/open failures.
    Result render (const juce::File& destination,
                   const Settings& settings,
                   const std::atomic<bool>* cancel = nullptr);
}
