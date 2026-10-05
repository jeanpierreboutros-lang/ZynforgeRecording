#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace zynforge::testaudio
{
struct ReadEvidence
{
    int successfulReads = 0;
    int failedReads = 0;
};

// A real AudioFormatReader: metadata is valid, the first read succeeds, and
// every later read reports an I/O failure. No timers, real disks, or races.
class FailingReader final : public juce::AudioFormatReader
{
public:
    explicit FailingReader (ReadEvidence& e, juce::InputStream* input = nullptr)
        : AudioFormatReader (input, "audit read failure"), evidence (e)
    {
        sampleRate = 48000.0;
        bitsPerSample = 32;
        numChannels = 1;
        usesFloatingPointData = true;
        lengthInSamples = 65536;
    }

    bool readSamples (int* const* destination, int channels, int offset,
                      juce::int64, int samples) override
    {
        if (evidence.successfulReads > 0)
        {
            ++evidence.failedReads;
            return false;
        }
        ++evidence.successfulReads;
        for (int channel = 0; channel < channels; ++channel)
            if (destination[channel] != nullptr)
                juce::FloatVectorOperations::fill (
                    reinterpret_cast<float*> (destination[channel]) + offset,
                    0.125f, samples);
        return true;
    }

private:
    ReadEvidence& evidence;
};

class FailingFormat final : public juce::AudioFormat
{
public:
    explicit FailingFormat (ReadEvidence& e)
        : AudioFormat ("audit read failure", ".zffault"), evidence (e) {}
    juce::Array<int> getPossibleSampleRates() override { return { 48000 }; }
    juce::Array<int> getPossibleBitDepths() override { return { 32 }; }
    bool canDoStereo() override { return false; }
    bool canDoMono() override { return true; }
    juce::AudioFormatReader* createReaderFor (juce::InputStream* input, bool) override
    { return new FailingReader (evidence, input); }
    juce::AudioFormatWriter* createWriterFor (juce::OutputStream*, double, unsigned int,
                                             int, const juce::StringPairArray&, int) override
    { return nullptr; }

private:
    ReadEvidence& evidence;
};
}
