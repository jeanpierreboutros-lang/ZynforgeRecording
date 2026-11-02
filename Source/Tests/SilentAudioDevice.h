#pragma once

#include "../Audio/AudioEngine.h"

namespace zynforge::testaudio
{
inline constexpr int kSilentBlock = 256;

// Gives pre-flight a device to read. Nothing here reaches CoreAudio,
// so no mic-consent prompt and no real interface is touched.
class SilentDevice final : public juce::AudioIODevice
{
public:
    SilentDevice (double rateIn, int inputsIn)
        : juce::AudioIODevice ("Silent", "Silent"), rate (rateIn), inputs (inputsIn) {}

    juce::StringArray getOutputChannelNames() override { return { "1", "2" }; }
    juce::StringArray getInputChannelNames() override
    {
        juce::StringArray names;
        for (int i = 0; i < inputs; ++i) names.add (juce::String (i + 1));
        return names;
    }
    juce::Array<double> getAvailableSampleRates() override { return { rate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kSilentBlock }; }
    int getDefaultBufferSize() override { return kSilentBlock; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override
    { opened = true; return {}; }
    void close() override { opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback*) override { started = true; }
    void stop() override { started = false; }
    bool isPlaying() override { return started; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kSilentBlock; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 24; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
    juce::BigInteger getActiveOutputChannels() const override { return 3; }
    juce::BigInteger getActiveInputChannels() const override
    {
        juce::BigInteger bits;
        bits.setRange (0, inputs, true);
        return bits;
    }

private:
    double rate;
    int inputs;
    bool opened { false }, started { false };
};

class SilentDeviceType final : public juce::AudioIODeviceType
{
public:
    SilentDeviceType (double rateIn, int inputsIn)
        : juce::AudioIODeviceType ("Silent"), rate (rateIn), inputs (inputsIn) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override { return { "Silent" }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override
    { return new SilentDevice (rate, inputs); }

private:
    double rate;
    int inputs;
};

inline void useSilentDevice (AudioEngine& engine, double rate = 48000.0, int inputs = 8)
{
    auto& manager = engine.getDeviceManager();
    manager.addAudioDeviceType (std::make_unique<SilentDeviceType> (rate, inputs));
    manager.setCurrentAudioDeviceType ("Silent", true);
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = "Silent";
    setup.inputDeviceName = "Silent";
    setup.sampleRate = rate;
    setup.bufferSize = kSilentBlock;
    manager.setAudioDeviceSetup (setup, false);
    engine.noteChosenDevice();
}
}
