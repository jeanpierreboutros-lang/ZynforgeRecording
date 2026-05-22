#include "AudioEngine.h"
#include "OscRemote.h"
#include "../Network/CompanionServer.h"

namespace zynforge
{
    bool AudioEngine::startRecording (const juce::File& sessionDir)
    {
        if (! recorder.startRecording (sessionDir)) return false;
        double sr = 48000.0;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            sr = device->getCurrentSampleRate();
        markers.setContext (sessionDir, sr);

        // Open the stereo mix file if the user opted in via setRecordStereoMix.
        if (recordStereoMixFlag.load (std::memory_order_acquire))
        {
            if (! mixWriterThread.isThreadRunning())
                mixWriterThread.startThread();

            const auto path = sessionDir.getChildFile ("StereoMix.wav");
            path.deleteFile();

            juce::WavAudioFormat wav;
            if (auto* out = path.createOutputStream().release())
            {
                juce::StringPairArray meta;
                if (auto* w = wav.createWriterFor (out, sr, 2, 24, meta, 0))
                {
                    stereoMixWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>
                                          (w, mixWriterThread, 32768);
                }
                else
                {
                    delete out;
                }
            }
        }
        return true;
    }

    void AudioEngine::stopRecording()
    {
        recorder.stopRecording();
        // Drop the threaded writer — its destructor flushes the queue and
        // closes the underlying AudioFormatWriter cleanly.
        stereoMixWriter.reset();
    }

    bool AudioEngine::startCompanionServer (int port)
    {
        if (companion == nullptr) companion = std::make_unique<CompanionServer> (*this);
        return companion->start (port);
    }
    void AudioEngine::stopCompanionServer()
    {
        if (companion != nullptr) companion->stop();
    }
    bool AudioEngine::isCompanionServerRunning() const noexcept
    {
        return companion != nullptr && companion->isRunning();
    }
    int AudioEngine::getCompanionServerPort() const noexcept
    {
        return companion != nullptr ? companion->getPort() : -1;
    }

    void AudioEngine::setMasterGainDb (float dB)
    {
        masterState.gainDb.store (dB, std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            appProps->setValue ("masterGainDb", (double) dB);
            appProps->saveIfNeeded();
        }
    }
    void AudioEngine::setMasterMuted (bool m)
    {
        masterState.muted.store (m, std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            appProps->setValue ("masterMuted", m);
            appProps->saveIfNeeded();
        }
    }
    void AudioEngine::setMasterOutputs (int l, int r)
    {
        masterOutL.store (l, std::memory_order_relaxed);
        masterOutR.store (r, std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            appProps->setValue ("masterOutL", l);
            appProps->setValue ("masterOutR", r);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setLtcSourceStrip (int oneBasedIndex) noexcept
    {
        ltcSourceStrip.store (oneBasedIndex - 1, std::memory_order_release);
        if (oneBasedIndex <= 0) timecodeChase.reset();
        if (appProps != nullptr)
        {
            appProps->setValue ("ltcSourceStrip", oneBasedIndex);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setRecordStereoMix (bool enabled)
    {
        recordStereoMixFlag.store (enabled, std::memory_order_release);
        if (appProps != nullptr)
        {
            appProps->setValue ("recordStereoMix", enabled);
            appProps->saveIfNeeded();
        }
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

    int AudioEngine::getStripCount() const
    {
        return appProps ? juce::jlimit (0, 256, appProps->getIntValue ("stripCount", 0)) : 0;
    }

    void AudioEngine::setStripCount (int n)
    {
        if (recorder.isRecording()) return;
        n = juce::jlimit (0, 256, n);

        if (appProps != nullptr)
        {
            appProps->setValue ("stripCount", n);
            appProps->saveIfNeeded();
        }

        deviceManager.removeAudioCallback (this);
        recorder.setTrackCount (n);
        applyPersistedStripState();
        deviceManager.addAudioCallback (this);
    }

    void AudioEngine::addOneStrip()
    {
        setStripCount (recorder.getNumTracks() + 1);
    }

    void AudioEngine::removeStripAt (int index)
    {
        if (recorder.isRecording()) return;
        const int n = recorder.getNumTracks();
        if (index < 0 || index >= n)  return;
        if (n < 1) return;

        deviceManager.removeAudioCallback (this);
        recorder.removeTrackAt (index);

        // The persistent stores key by index, so shift everything after the
        // removed slot down by one to keep colour / name / gain / routing
        // consistent with the new track positions.
        for (int i = index; i < recorder.getNumTracks(); ++i)
        {
            // Pull entry (i + 1)'s persistent values into slot i.
            if (stripColours.hasColour (i + 1))
                setTrackColour (i, stripColours.getColour (i + 1));
            else
                setTrackColour (i, juce::Colour ((juce::uint32) 0));

            if (stripNames.hasName (i + 1))
                setTrackName (i, stripNames.getName (i + 1));
            else
                setTrackName (i, {});

            if (stripGains.hasGain (i + 1))
                setTrackGainDb (i, stripGains.getGainDb (i + 1));
            else
                setTrackGainDb (i, 0.0f);

            setTrackPan (i, stripGains.hasPan (i + 1) ? stripGains.getPan (i + 1) : 0.0f);

            if (stripRouting.hasInput (i + 1))
                setTrackInputRouting (i, stripRouting.getInput (i + 1));
            if (stripRouting.hasOutput (i + 1))
                setTrackOutputRouting (i, stripRouting.getOutput (i + 1));
        }

        // Clear the now-orphan slot at the end of the persistent stores.
        const int lastIdx = recorder.getNumTracks();
        setTrackColour       (lastIdx, juce::Colour ((juce::uint32) 0));
        setTrackName         (lastIdx, {});
        setTrackGainDb       (lastIdx, 0.0f);
        setTrackPan          (lastIdx, 0.0f);

        if (appProps != nullptr)
        {
            appProps->setValue ("stripCount", recorder.getNumTracks());
            appProps->saveIfNeeded();
        }

        deviceManager.addAudioCallback (this);
    }

    void AudioEngine::applyPersistedStripState()
    {
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
                t.pan.store    (stripGains.getPan (i), std::memory_order_relaxed);

            t.inputRouting .store (stripRouting.hasInput  (i) ? stripRouting.getInput  (i) : i,
                                   std::memory_order_relaxed);
            t.outputRouting.store (stripRouting.hasOutput (i) ? stripRouting.getOutput (i) : i,
                                   std::memory_order_relaxed);

            // Restore the mono / stereo flag from appProps. The key is
            // 'strip_stereo_N' → bool. Defaults to false.
            if (appProps != nullptr)
            {
                const auto key = juce::String ("strip_stereo_") + juce::String (i);
                t.isStereo.store (appProps->getBoolValue (key, false),
                                  std::memory_order_relaxed);
            }
        }
    }

    void AudioEngine::setTrackStereo (int channelIndex, bool isStereoPair)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        recorder.getTrack (channelIndex).isStereo.store (isStereoPair,
                                                          std::memory_order_release);
        if (appProps != nullptr)
        {
            const auto key = juce::String ("strip_stereo_") + juce::String (channelIndex);
            appProps->setValue (key, isStereoPair);
            appProps->saveIfNeeded();
        }
    }

    AudioEngine::AudioEngine()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Zynforge Recording";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Zynforge Recording";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat       = juce::PropertiesFile::storeAsXML;
        appProps = std::make_unique<juce::PropertiesFile> (opts);

        // Restore the stereo-mix-recording flag from the prefs file so the
        // engineer's preference survives restart.
        recordStereoMixFlag.store (appProps->getBoolValue ("recordStereoMix", false),
                                   std::memory_order_release);

        // Restore the LTC source strip (1-based stored, internal 0-based).
        ltcSourceStrip.store (appProps->getIntValue ("ltcSourceStrip", 0) - 1,
                              std::memory_order_release);

        // Master bus state — gain + mute + L/R output routing.
        masterState.name = "MASTER";
        masterState.gainDb.store ((float) appProps->getDoubleValue ("masterGainDb", 0.0));
        masterState.muted .store (appProps->getBoolValue ("masterMuted", false));
        masterOutL.store (appProps->getIntValue ("masterOutL", 0));
        masterOutR.store (appProps->getIntValue ("masterOutR", 1));

        // Open with up to 256 inputs / 64 outputs by default — adjust later from UI.
        auto err = deviceManager.initialise (/*numInputs*/ 256, /*numOutputs*/ 64,
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

        // Strip count is user-controlled (persisted; defaults to 1) —
        // it's no longer tied to the device's input channel count.
        recorder.prepare (sr, blockSize, getStripCount());
        player  .prepare (sr, blockSize);

        // Pre-allocate the stereo mix scratch buffer at the device block
        // size so the audio thread never allocates when mix capture is on.
        stereoMixScratch.setSize (2, blockSize, false, true, true);
        monitorAccum    .setSize (2, blockSize, false, true, true);

        applyPersistedStripState();
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

    void AudioEngine::setTrackLinkedRouting (int channelIndex, int deviceCh)
    {
        // Single source of truth — sets BOTH sides of the patch so the
        // PATCH page + the per-strip dropdowns stay in lockstep.
        setTrackInputRouting  (channelIndex, deviceCh);
        setTrackOutputRouting (channelIndex, deviceCh);
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
        constexpr int kMaxStrips = 256;
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

        // Drive per-strip meters from the player's output ONLY when the
        // player is actually rolling. The recorder has already written
        // the input-side peak / rms for this block; we max the player
        // contribution on top so a strip with BOTH input AND playback
        // shows whichever is louder. When the player is stopped we
        // leave the recorder's values alone — otherwise this would
        // clobber the live input meters with zeros from an idle
        // playerScratch buffer.
        if (player.isPlaying())
        {
            const int playerTracks = juce::jmin (playerScratch.getNumChannels(), numTracks);
            for (int ch = 0; ch < playerTracks; ++ch)
            {
                const float* src = playerScratch.getReadPointer (ch);
                if (src == nullptr) continue;
                float pk = 0.0f, sumSq = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float a = std::abs (src[i]);
                    if (a > pk) pk = a;
                    sumSq += src[i] * src[i];
                }
                const float rms = std::sqrt (sumSq / juce::jmax (1, numSamples));
                auto& t = recorder.getTrack (ch);
                // Apply the same 0.92 release decay the recorder uses, so
                // when a transient passes the meter falls back to the
                // ambient level instead of latching at the loudest peak
                // ever observed. Without this, any stereo track whose R
                // partner had no input routing would stick at full scale.
                const float prevPk  = t.peak.load (std::memory_order_relaxed);
                const float prevRms = t.rms .load (std::memory_order_relaxed);
                t.peak.store (juce::jmax (pk,  prevPk  * 0.92f), std::memory_order_relaxed);
                t.rms .store (juce::jmax (rms, prevRms * 0.85f), std::memory_order_relaxed);
                if (pk >= 0.999f)
                {
                    t.clipped.store (true, std::memory_order_relaxed);
                    t.clipCount.fetch_add (1, std::memory_order_relaxed);
                }
            }
        }

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
        // Also captured to StereoMix.wav when recordStereoMix is on, so
        // the bounce hits disk alongside the per-channel multitrack.
        const int sL = streamOutL.load (std::memory_order_relaxed);
        const int sR = streamOutR.load (std::memory_order_relaxed);
        const bool wantMixCapture = (stereoMixWriter != nullptr);

        if (wantMixCapture)
        {
            stereoMixScratch.setSize (2, numSamples, false, false, true);
            stereoMixScratch.clear();
        }

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

                if (wantMixCapture)
                {
                    juce::FloatVectorOperations::addWithMultiply (stereoMixScratch.getWritePointer (0),
                                                                  src, gL, numSamples);
                    juce::FloatVectorOperations::addWithMultiply (stereoMixScratch.getWritePointer (1),
                                                                  src, gR, numSamples);
                }
            }
        }

        if (wantMixCapture)
        {
            const float* chans[2] = { stereoMixScratch.getReadPointer (0),
                                      stereoMixScratch.getReadPointer (1) };
            stereoMixWriter->write (chans, numSamples);
        }

        // LTC presence detection — analyzes the input of a designated
        // strip every block. Engineer routes the desk's timecode line
        // to that strip and toggles 'LTC source' to it from the UI.
        {
            const int srcStrip = ltcSourceStrip.load (std::memory_order_relaxed);
            if (srcStrip >= 0 && srcStrip < trackCount && routedInputs[srcStrip] != nullptr)
            {
                const auto sr = deviceManager.getCurrentAudioDevice() != nullptr
                                ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                                : 48000.0;
                timecodeChase.feedLtc (routedInputs[srcStrip], numSamples, sr);
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

        // ── Master bus ────────────────────────────────────────────────
        // Every audible channel's VSC playback contributes to the master
        // by default. Channels with MON=on ALSO contribute their live
        // input on top, so the engineer can hear mics during a take.
        // Mute / solo gates apply: if any channel is soloed, only soloed
        // channels contribute (this is what gives the user "solo a track
        // and hear it via the master fader" behaviour).
        const int blk = juce::jmin (numSamples, monitorAccum.getNumSamples());
        monitorAccum.clear (0, 0, blk);
        monitorAccum.clear (1, 0, blk);

        auto* accL = monitorAccum.getWritePointer (0);
        auto* accR = monitorAccum.getWritePointer (1);

        for (int ch = 0; ch < trackCount; ++ch)
        {
            auto& t = recorder.getTrack (ch);
            if (! channelAudible (ch)) continue;

            const double dB   = t.gainDb.load (std::memory_order_relaxed);
            const double gain = juce::Decibels::decibelsToGain (dB, -60.0);
            const double pan  = juce::jlimit (-1.0, 1.0,
                                              (double) t.pan.load (std::memory_order_relaxed));
            const double panNorm = (pan + 1.0) * 0.5;
            const double gL = gain * std::cos (panNorm * juce::MathConstants<double>::halfPi);
            const double gR = gain * std::sin (panNorm * juce::MathConstants<double>::halfPi);

            // (a) VSC playback — always summed to master when audible.
            if (ch < playerScratch.getNumChannels())
            {
                const float* psrc = playerScratch.getReadPointer (ch);
                if (psrc != nullptr)
                    for (int i = 0; i < blk; ++i)
                    {
                        const double s = (double) psrc[i];
                        accL[i] += s * gL;
                        accR[i] += s * gR;
                    }
            }

            // (b) Live input — only when MON is on (per-channel monitor).
            if (t.monitor.load (std::memory_order_relaxed))
            {
                const float* isrc = (ch < kMaxStrips) ? routedInputs[ch] : nullptr;
                if (isrc != nullptr)
                    for (int i = 0; i < blk; ++i)
                    {
                        const double s = (double) isrc[i];
                        accL[i] += s * gL;
                        accR[i] += s * gR;
                    }
            }
        }

        // Master fader + master mute act on the summed bus.
        const bool   mMute = masterState.muted.load (std::memory_order_relaxed);
        const double mGain = mMute ? 0.0
                                   : juce::Decibels::decibelsToGain (
                                         (double) masterState.gainDb.load (std::memory_order_relaxed),
                                         -60.0);

        // Drive the master meter (peak + RMS) from the accumulator BEFORE
        // mGain so the meter shows what's hitting the bus, then write to
        // the configured master outputs scaled by mGain.
        {
            double mPeakL = 0.0, mPeakR = 0.0;
            double mRmsL  = 0.0, mRmsR  = 0.0;
            for (int i = 0; i < blk; ++i)
            {
                const double aL = std::abs (accL[i]);
                const double aR = std::abs (accR[i]);
                if (aL > mPeakL) mPeakL = aL;
                if (aR > mPeakR) mPeakR = aR;
                mRmsL += accL[i] * accL[i];
                mRmsR += accR[i] * accR[i];
            }
            const double inv = 1.0 / (double) juce::jmax (1, blk);
            const float pk  = (float) juce::jmax (mPeakL, mPeakR);
            const float rms = (float) std::sqrt (juce::jmax (mRmsL, mRmsR) * inv);
            const float prevPk  = masterState.peak.load (std::memory_order_relaxed);
            const float prevRms = masterState.rms .load (std::memory_order_relaxed);
            masterState.peak.store (juce::jmax (pk,  prevPk  * 0.92f), std::memory_order_relaxed);
            masterState.rms .store (juce::jmax (rms, prevRms * 0.85f), std::memory_order_relaxed);
            if (pk >= 0.999f) masterState.clipped.store (true, std::memory_order_relaxed);
        }

        const int outL = juce::jlimit (0, numOutputs - 1, masterOutL.load (std::memory_order_relaxed));
        const int outR = juce::jlimit (0, numOutputs - 1, masterOutR.load (std::memory_order_relaxed));
        if (outL < numOutputs && outputs[outL] != nullptr)
            for (int i = 0; i < blk; ++i)
                outputs[outL][i] += (float) (accL[i] * mGain);
        if (outR < numOutputs && outputs[outR] != nullptr && outR != outL)
            for (int i = 0; i < blk; ++i)
                outputs[outR][i] += (float) (accR[i] * mGain);

        // Companion stream feed — runs at the very end so it captures
        // EVERYTHING the engineer hears at outputs 0+1 (per-channel
        // routing + stream-bus + monitor sum, all collapsed into the
        // float output buffers by now).
        if (companion != nullptr && companion->isRunning())
        {
            const float* L = (numOutputs > 0) ? outputs[0] : nullptr;
            const float* R = (numOutputs > 1) ? outputs[1] : nullptr;
            companion->feedStreamSamples (L, R, numSamples);
        }
    }
}
