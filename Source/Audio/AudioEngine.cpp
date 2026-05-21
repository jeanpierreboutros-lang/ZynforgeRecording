#include "AudioEngine.h"

namespace zynforge
{
    AudioEngine::AudioEngine()
    {
        // Open with up to 32 inputs / 2 outputs by default — adjust later from UI.
        auto err = deviceManager.initialise (/*numInputs*/ 32, /*numOutputs*/ 2,
                                             /*savedState*/ nullptr,
                                             /*selectDefault*/ true);
        if (err.isNotEmpty())
            DBG ("AudioDeviceManager init: " << err);

        deviceManager.addAudioCallback (this);
    }

    AudioEngine::~AudioEngine()
    {
        deviceManager.removeAudioCallback (this);
    }

    void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
    {
        const auto sr        = device->getCurrentSampleRate();
        const auto blockSize = device->getCurrentBufferSizeSamples();
        const auto inputs    = device->getActiveInputChannels().countNumberOfSetBits();

        recorder.prepare (sr, blockSize, juce::jmax (1, inputs));
        player  .prepare (sr, blockSize);
    }

    void AudioEngine::audioDeviceStopped()
    {
        recorder.release();
        player  .release();
    }

    void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                        float* const* outputs, int numOutputs,
                                                        int numSamples,
                                                        const juce::AudioIODeviceCallbackContext&)
    {
        // Always clear outputs first; player fills them if active.
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        recorder.processBlock (inputs, numInputs, numSamples);
        player  .processBlock (outputs, numOutputs, numSamples);
    }
}
