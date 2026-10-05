#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace zynforge
{
    // AudioFormatReaderSource discards read failures. Offline conversion must
    // distinguish a decoder/I/O failure from normal zero-padding past EOF.
    class CheckedReaderSource final : public juce::AudioSource
    {
    public:
        explicit CheckedReaderSource (juce::AudioFormatReader& source) : reader (source) {}

        void prepareToPlay (int, double) override { position = 0; failed = false; }
        void releaseResources() override {}

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            info.clearActiveBufferRegion();
            if (failed || info.buffer == nullptr || position >= reader.lengthInSamples) return;
            const int count = static_cast<int> (juce::jmin (
                static_cast<juce::int64> (info.numSamples), reader.lengthInSamples - position));
            if (count <= 0) return;
            if (! reader.read (info.buffer, info.startSample, count, position, true, true))
            {
                failed = true;
                info.clearActiveBufferRegion();
                return;
            }
            position += count;
        }

        bool hasFailed() const noexcept { return failed; }

    private:
        juce::AudioFormatReader& reader;
        juce::int64 position = 0;
        bool failed = false;
    };
}
