#include "AudioEngine.h"

namespace zynforge
{
    bool AudioEngine::startRecording (const juce::File& sessionDir)
    {
        if (! recorder.startRecording (sessionDir)) return false;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            markers.setContext (sessionDir, device->getCurrentSampleRate());
        else
            markers.setContext (sessionDir, 48000.0);
        return true;
    }

    int AudioEngine::loadSession (const juce::File& sessionDir)
    {
        const auto n = player.loadSession (sessionDir);
        if (n > 0)
            markers.setContext (sessionDir, player.getSampleRate());
        return n;
    }

    void AudioEngine::setPhasePair (int leftCh1Based, int rightCh1Based) noexcept
    {
        phaseLeft .store (juce::jmax (0, leftCh1Based  - 1), std::memory_order_relaxed);
        phaseRight.store (juce::jmax (0, rightCh1Based - 1), std::memory_order_relaxed);
    }

    int AudioEngine::dropMarkerAtCurrentPosition()
    {
        if (! markers.hasContext()) return -1;

        juce::int64 pos = 0;
        if (recorder.isRecording())
            pos = recorder.getSamplesSinceStart();
        else if (player.isLoaded())
            pos = player.getPositionSamples();
        else
            return -1;

        markers.drop (pos);
        return markers.getCount();
    }

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

        // Apply persisted per-channel colour + name overrides.
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            if (stripColours.hasColour (i))
                recorder.getTrack (i).colourARGB.store (
                    stripColours.getColour (i).getARGB(),
                    std::memory_order_relaxed);

            if (stripNames.hasName (i))
                recorder.getTrack (i).name = stripNames.getName (i);
        }
    }

    juce::File AudioEngine::getActiveSessionDir() const
    {
        if (recorder.isRecording())
            return recorder.getActiveSessionDir();
        if (player.isLoaded())
            return player.getSessionDir();
        return {};
    }

    void AudioEngine::setTrackName (int channelIndex, const juce::String& name)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        if (name.isEmpty())
        {
            stripNames.clearName (channelIndex);
            recorder.getTrack (channelIndex).name =
                "In " + juce::String (channelIndex + 1);
        }
        else
        {
            stripNames.setName (channelIndex, name);
            recorder.getTrack (channelIndex).name = name;
        }
    }

    void AudioEngine::setTrackColour (int channelIndex, juce::Colour c)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        if (c.getAlpha() == 0)
        {
            stripColours.clearColour (channelIndex);
            recorder.getTrack (channelIndex).colourARGB.store (0, std::memory_order_relaxed);
        }
        else
        {
            stripColours.setColour (channelIndex, c);
            recorder.getTrack (channelIndex).colourARGB.store (
                c.getARGB(), std::memory_order_relaxed);
        }
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
        // Always clear outputs first; player + monitor sum into them.
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        recorder.processBlock (inputs, numInputs, numSamples);
        player  .processBlock (outputs, numOutputs, numSamples);

        // Mute / solo: when any channel is soloed, only soloed channels pass.
        // Otherwise muted channels are silenced. Applies to both VSC playback
        // outputs (channel N played out N) and to the monitor sum below.
        bool anySolo = false;
        const int trackCount = recorder.getNumTracks();
        for (int i = 0; i < trackCount; ++i)
            if (recorder.getTrack (i).soloed.load (std::memory_order_relaxed))
            { anySolo = true; break; }

        auto channelAudible = [&] (int i) -> bool
        {
            auto& t = recorder.getTrack (i);
            if (anySolo) return t.soloed.load (std::memory_order_relaxed);
            return ! t.muted.load (std::memory_order_relaxed);
        };

        // Apply mute/solo to playback outputs (VSC pass-through routing).
        for (int i = 0; i < trackCount && i < numOutputs; ++i)
            if (! channelAudible (i) && outputs[i] != nullptr)
                juce::FloatVectorOperations::clear (outputs[i], numSamples);

        // Phase correlation between the selected pair (smoothed).
        {
            const int li = phaseLeft .load (std::memory_order_relaxed);
            const int ri = phaseRight.load (std::memory_order_relaxed);
            if (li < numInputs && ri < numInputs
                && inputs[li] != nullptr && inputs[ri] != nullptr)
            {
                const float* L = inputs[li];
                const float* R = inputs[ri];
                float lr = 0.0f, ll = 0.0f, rr = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    lr += L[i] * R[i];
                    ll += L[i] * L[i];
                    rr += R[i] * R[i];
                }
                const float denom = std::sqrt (ll * rr) + 1.0e-12f;
                const float instantaneous = juce::jlimit (-1.0f, 1.0f, lr / denom);
                const float prev = phaseCorrelation.load (std::memory_order_relaxed);
                phaseCorrelation.store (prev * 0.85f + instantaneous * 0.15f,
                                        std::memory_order_relaxed);
            }
        }

        // Sum monitored inputs into the stereo monitor bus (outputs 0 + 1).
        // Mute/solo gate applies to monitoring the same as to playback.
        const int monL = 0, monR = 1;
        for (int ch = 0; ch < trackCount && ch < numInputs; ++ch)
        {
            auto& t = recorder.getTrack (ch);
            if (! t.monitor.load (std::memory_order_relaxed)) continue;
            if (! channelAudible (ch)) continue;

            const float* src = inputs[ch];
            if (src == nullptr) continue;
            if (monL < numOutputs && outputs[monL] != nullptr)
                juce::FloatVectorOperations::add (outputs[monL], src, numSamples);
            if (monR < numOutputs && outputs[monR] != nullptr)
                juce::FloatVectorOperations::add (outputs[monR], src, numSamples);
        }
    }
}
