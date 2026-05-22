#include "AudioEngine.h"
#include "OscRemote.h"

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
        // Open with up to 128 inputs / 64 outputs by default — adjust later from UI.
        auto err = deviceManager.initialise (/*numInputs*/ 128, /*numOutputs*/ 64,
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

        // Apply persisted per-channel colour + name + gain/pan + routing.
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);

            if (stripColours.hasColour (i))
                t.colourARGB.store (stripColours.getColour (i).getARGB(),
                                    std::memory_order_relaxed);

            if (stripNames.hasName (i))
                t.name = stripNames.getName (i);

            if (stripGains.hasGain (i))
                t.gainDb.store (stripGains.getGainDb (i), std::memory_order_relaxed);
            if (stripGains.hasPan (i))
                t.pan   .store (stripGains.getPan    (i), std::memory_order_relaxed);

            // Identity routing by default; override if persisted.
            t.inputRouting .store (stripRouting.hasInput  (i) ? stripRouting.getInput  (i) : i,
                                   std::memory_order_relaxed);
            t.outputRouting.store (stripRouting.hasOutput (i) ? stripRouting.getOutput (i) : i,
                                   std::memory_order_relaxed);
        }
    }

    juce::Array<juce::File> AudioEngine::findIncompleteSessions (const juce::File& sessionsRoot)
    {
        juce::Array<juce::File> incomplete;
        if (! sessionsRoot.isDirectory()) return incomplete;

        const auto dirs = sessionsRoot.findChildFiles (juce::File::findDirectories, false, "Session_*");
        for (auto& d : dirs)
            if (d.getChildFile ("recording.session").existsAsFile())
                incomplete.add (d);
        return incomplete;
    }

    juce::File AudioEngine::getActiveSessionDir() const
    {
        if (recorder.isRecording())
            return recorder.getActiveSessionDir();
        if (player.isLoaded())
            return player.getSessionDir();
        return {};
    }

    bool AudioEngine::startOsc (int udpPort, int dialectIndex)
    {
        if (osc == nullptr) osc = std::make_unique<OscRemote> (*this);
        osc->setDialect ((OscRemote::Dialect) juce::jlimit (0, 4, dialectIndex));
        return osc->start (udpPort);
    }
    void AudioEngine::stopOsc()
    {
        if (osc != nullptr) osc->stop();
    }
    bool AudioEngine::isOscListening() const
    {
        return osc != nullptr && osc->isListening();
    }
    int AudioEngine::getOscPort() const
    {
        return osc != nullptr ? osc->getPort() : 0;
    }
    int AudioEngine::getOscDialect() const
    {
        return osc != nullptr ? (int) osc->getDialect() : 0;
    }

    void AudioEngine::setStreamOutputs (int l, int r)
    {
        streamOutL.store (l, std::memory_order_relaxed);
        streamOutR.store (r, std::memory_order_relaxed);
    }

    void AudioEngine::setTrackStream (int channelIndex, bool enabled)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        recorder.getTrack (channelIndex).streamSend.store (enabled, std::memory_order_relaxed);
    }

    int AudioEngine::getCurrentDeviceInputCount() const
    {
        if (auto* d = deviceManager.getCurrentAudioDevice())
            return d->getActiveInputChannels().countNumberOfSetBits();
        return 0;
    }

    int AudioEngine::getCurrentDeviceOutputCount() const
    {
        if (auto* d = deviceManager.getCurrentAudioDevice())
            return d->getActiveOutputChannels().countNumberOfSetBits();
        return 0;
    }

    void AudioEngine::setTrackInputRouting (int channelIndex, int deviceCh)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        deviceCh = juce::jmax (-1, deviceCh);
        recorder.getTrack (channelIndex).inputRouting.store (deviceCh, std::memory_order_relaxed);
        stripRouting.setInput (channelIndex, deviceCh);
    }

    void AudioEngine::setTrackOutputRouting (int channelIndex, int deviceCh)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        deviceCh = juce::jmax (-1, deviceCh);
        recorder.getTrack (channelIndex).outputRouting.store (deviceCh, std::memory_order_relaxed);
        stripRouting.setOutput (channelIndex, deviceCh);
    }

    void AudioEngine::setTrackGainDb (int channelIndex, float dB)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        dB = juce::jlimit (-60.0f, 0.0f, dB);
        recorder.getTrack (channelIndex).gainDb.store (dB, std::memory_order_relaxed);
        stripGains.setGainDb (channelIndex, dB);
    }

    void AudioEngine::setTrackPan (int channelIndex, float pan)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        recorder.getTrack (channelIndex).pan.store (pan, std::memory_order_relaxed);
        stripGains.setPan (channelIndex, pan);
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

        // Build a routed-input pointer array: routedInputs[stripIndex] points
        // to the device input that strip is patched from, or nullptr if -1.
        const int numTracks = recorder.getNumTracks();
        constexpr int kMaxStrips = 128;
        const float* routedInputs[kMaxStrips] = {};
        for (int i = 0; i < numTracks && i < kMaxStrips; ++i)
        {
            const int dev = recorder.getTrack (i).inputRouting.load (std::memory_order_relaxed);
            routedInputs[i] = (dev >= 0 && dev < numInputs) ? inputs[dev] : nullptr;
        }

        recorder.processBlock (routedInputs, juce::jmin (numTracks, kMaxStrips), numSamples);

        // Have the player render into a scratch buffer per track, then we
        // route each track to its configured output channel.
        if (playerScratch.getNumChannels() < numTracks || playerScratch.getNumSamples() < numSamples)
            playerScratch.setSize (juce::jmax (numTracks, 1), juce::jmax (numSamples, 64),
                                   false, false, true);
        playerScratch.clear (0, numSamples);
        player.processBlock (playerScratch.getArrayOfWritePointers(),
                             juce::jmin (playerScratch.getNumChannels(), numTracks),
                             numSamples);

        // Mute / solo: when any channel is soloed, only soloed channels pass.
        bool anySolo = false;
        for (int i = 0; i < numTracks; ++i)
            if (recorder.getTrack (i).soloed.load (std::memory_order_relaxed))
            { anySolo = true; break; }

        auto channelAudible = [&] (int i) -> bool
        {
            auto& t = recorder.getTrack (i);
            if (anySolo) return t.soloed.load (std::memory_order_relaxed);
            return ! t.muted.load (std::memory_order_relaxed);
        };

        // Mix the routed track scratch onto the device outputs honoring
        // mute/solo and per-channel gain.
        for (int i = 0; i < numTracks; ++i)
        {
            if (! channelAudible (i)) continue;

            auto& t = recorder.getTrack (i);
            const int devOut = t.outputRouting.load (std::memory_order_relaxed);
            if (devOut < 0 || devOut >= numOutputs || outputs[devOut] == nullptr) continue;

            const float dB   = t.gainDb.load (std::memory_order_relaxed);
            const float gain = juce::Decibels::decibelsToGain (dB, -60.0f);
            const float* src = playerScratch.getReadPointer (i);
            if (juce::approximatelyEqual (gain, 1.0f))
                juce::FloatVectorOperations::add (outputs[devOut], src, numSamples);
            else
                juce::FloatVectorOperations::addWithMultiply (outputs[devOut], src, gain, numSamples);
        }

        const int trackCount = numTracks;

        // Dedicated streaming stereo bus: sum every strip flagged as
        // streamSend into the configured outputs with their gain+pan.
        const int sL = streamOutL.load (std::memory_order_relaxed);
        const int sR = streamOutR.load (std::memory_order_relaxed);
        if (sL >= 0 && sR >= 0 && sL < numOutputs && sR < numOutputs)
        {
            for (int i = 0; i < trackCount; ++i)
            {
                auto& t = recorder.getTrack (i);
                if (! t.streamSend.load (std::memory_order_relaxed)) continue;
                if (! channelAudible (i)) continue;

                const float dB   = t.gainDb.load (std::memory_order_relaxed);
                const float gain = juce::Decibels::decibelsToGain (dB, -60.0f);
                const float pan  = juce::jlimit (-1.0f, 1.0f, t.pan.load (std::memory_order_relaxed));
                const float panNorm = (pan + 1.0f) * 0.5f;
                const float gL = gain * std::cos (panNorm * juce::MathConstants<float>::halfPi);
                const float gR = gain * std::sin (panNorm * juce::MathConstants<float>::halfPi);

                const float* src = playerScratch.getReadPointer (i);
                if (outputs[sL] != nullptr && gL > 0.0001f)
                    juce::FloatVectorOperations::addWithMultiply (outputs[sL], src, gL, numSamples);
                if (outputs[sR] != nullptr && gR > 0.0001f)
                    juce::FloatVectorOperations::addWithMultiply (outputs[sR], src, gR, numSamples);
            }
        }

        // Phase correlation between the selected pair (smoothed). The
        // pair refers to strip indices, so we use the routed input pointers.
        {
            const int li = phaseLeft .load (std::memory_order_relaxed);
            const int ri = phaseRight.load (std::memory_order_relaxed);
            if (li < numTracks && ri < numTracks
                && routedInputs[li] != nullptr && routedInputs[ri] != nullptr)
            {
                const float* L = routedInputs[li];
                const float* R = routedInputs[ri];
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

        // Sum monitored inputs into the stereo monitor bus (outputs 0 + 1)
        // with constant-power pan and per-channel gain. Uses the routed
        // device input pointer.
        const int monL = 0, monR = 1;
        for (int ch = 0; ch < trackCount; ++ch)
        {
            auto& t = recorder.getTrack (ch);
            if (! t.monitor.load (std::memory_order_relaxed)) continue;
            if (! channelAudible (ch)) continue;

            const float* src = (ch < kMaxStrips) ? routedInputs[ch] : nullptr;
            if (src == nullptr) continue;

            const float dB   = t.gainDb.load (std::memory_order_relaxed);
            const float gain = juce::Decibels::decibelsToGain (dB, -60.0f);
            const float pan  = juce::jlimit (-1.0f, 1.0f, t.pan.load (std::memory_order_relaxed));
            const float panNorm = (pan + 1.0f) * 0.5f;             // 0..1
            const float gL = gain * std::cos (panNorm * juce::MathConstants<float>::halfPi);
            const float gR = gain * std::sin (panNorm * juce::MathConstants<float>::halfPi);

            if (monL < numOutputs && outputs[monL] != nullptr && gL > 0.0001f)
                juce::FloatVectorOperations::addWithMultiply (outputs[monL], src, gL, numSamples);
            if (monR < numOutputs && outputs[monR] != nullptr && gR > 0.0001f)
                juce::FloatVectorOperations::addWithMultiply (outputs[monR], src, gR, numSamples);
        }
    }
}
