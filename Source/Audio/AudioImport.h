#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <vector>

namespace zynforge::audioimport
{
    struct ImportedTrack
    {
        int trackIndex { 0 };
        bool stereo { false };
        juce::String name;
    };

    struct Result
    {
        std::vector<ImportedTrack> tracks;
        int failed { 0 };
        int converted { 0 };
        bool cancelled { false };
    };

    // Decode the selected files and write session-rate, 24-bit WAV media.
    // This function touches no AudioEngine/UI state and is safe to run on the
    // owned session-I/O worker. Partial outputs are removed on every failure or
    // cancellation so a later session load cannot mistake them for a take.
    Result importFiles (const juce::Array<juce::File>& sources,
                        const juce::File& audioFilesDir,
                        int firstTrack,
                        double targetSampleRate,
                        const std::atomic<bool>* cancel = nullptr);
}
