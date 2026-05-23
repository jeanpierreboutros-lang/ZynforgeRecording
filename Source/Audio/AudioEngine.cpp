#include "AudioEngine.h"
#include "OscRemote.h"
#include "../Network/CompanionServer.h"

namespace zynforge
{
    void AudioEngine::rememberRecentSession (const juce::File& dir)
    {
        if (! dir.isDirectory() || appProps == nullptr) return;
        const auto path = dir.getFullPathName();

        // Pull existing list, drop any prior occurrence of this path,
        // push this path on the front, cap at kMaxRecent.
        juce::StringArray entries;
        for (int i = 0; i < kMaxRecent; ++i)
        {
            const auto p = appProps->getValue ("recentSession_" + juce::String (i));
            if (p.isNotEmpty() && p != path) entries.add (p);
        }
        entries.insert (0, path);
        while (entries.size() > kMaxRecent) entries.remove (entries.size() - 1);

        for (int i = 0; i < kMaxRecent; ++i)
            appProps->setValue ("recentSession_" + juce::String (i),
                                i < entries.size() ? entries[i] : juce::String());
        appProps->saveIfNeeded();
    }

    juce::Array<juce::File> AudioEngine::getRecentSessions() const
    {
        juce::Array<juce::File> out;
        if (appProps == nullptr) return out;
        for (int i = 0; i < kMaxRecent; ++i)
        {
            const auto p = appProps->getValue ("recentSession_" + juce::String (i));
            if (p.isEmpty()) continue;
            juce::File f (p);
            if (f.isDirectory()) out.add (f);
        }
        return out;
    }

    void AudioEngine::clearRecentSessions()
    {
        if (appProps == nullptr) return;
        for (int i = 0; i < kMaxRecent; ++i)
            appProps->setValue ("recentSession_" + juce::String (i), juce::String());
        appProps->saveIfNeeded();
    }

    void AudioEngine::resetAllStripState()
    {
        const int n = recorder.getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            // Clear persistent overrides — name, colour, gain, pan, routing.
            stripNames  .clearName   (i);
            stripColours.clearColour (i);
            // Gains has no clear-by-index helper; just reset to defaults
            // via the engine setters so the persistent file is rewritten.
            setTrackGainDb (i, 0.0f);
            setTrackPan    (i, 0.0f);
            setTrackInputRouting  (i, i);   // identity
            setTrackOutputRouting (i, -1);  // master-only

            // Live atomics so the UI flips back immediately.
            auto& t = recorder.getTrack (i);
            t.name = juce::String (i + 1);
            t.colourARGB.store (0, std::memory_order_relaxed);
            t.armed   .store (false, std::memory_order_relaxed);
            t.muted   .store (false, std::memory_order_relaxed);
            t.soloed  .store (false, std::memory_order_relaxed);
            t.monitor .store (false, std::memory_order_relaxed);
            t.isStereo.store (false, std::memory_order_relaxed);
            if (appProps != nullptr)
                appProps->setValue (juce::String ("strip_stereo_") + juce::String (i), false);
        }
        if (appProps != nullptr) appProps->saveIfNeeded();
    }

    bool AudioEngine::startRecording (const juce::File& sessionDir)
    {
        if (! recorder.startRecording (sessionDir)) return false;
        rememberRecentSession (sessionDir);
        double sr = 48000.0;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            sr = device->getCurrentSampleRate();
        markers.setContext (sessionDir, sr);

        // Open the stereo mix file if the user opted in via setRecordStereoMix.
        if (recordStereoMixFlag.load (std::memory_order_acquire))
        {
            if (! mixWriterThread.isThreadRunning())
                mixWriterThread.startThread();

            // Stereo bus mix lands in Bounced Files/ — that's the
            // intended Pro Tools-style location for any rendered mix.
            auto bouncedDir = sessionDir.getChildFile ("Bounced Files");
            bouncedDir.createDirectory();
            const auto path = bouncedDir.getChildFile ("StereoMix.wav");
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
    void AudioEngine::setMasterStereo (bool s)
    {
        masterStereo.store (s, std::memory_order_release);
        if (appProps != nullptr)
        {
            appProps->setValue ("masterStereo", s);
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
        {
            markers.setContext (sessionDir, player.getSampleRate());
            rememberRecentSession (sessionDir);
        }
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

            // Default to channel i, but clamp to the device's active
            // input count so a 4-strip session on a 1-input device still
            // captures sound (strip i wraps to i % numInputs).
            int wantIn = stripRouting.hasInput (i) ? stripRouting.getInput (i) : i;
            const int numInputs = (deviceManager.getCurrentAudioDevice() != nullptr)
                ? deviceManager.getCurrentAudioDevice()->getActiveInputChannels().countNumberOfSetBits()
                : 0;
            if (numInputs > 0 && (wantIn < 0 || wantIn >= numInputs))
                wantIn = i % numInputs;
            t.inputRouting .store (wantIn, std::memory_order_relaxed);
            // Default per-channel output is -1 (unrouted) → audio only
            // reaches the hardware via the master bus. Engineer picks a
            // specific output explicitly (e.g. for Virtual Soundcheck)
            // when they need a dedicated per-track destination.
            t.outputRouting.store (stripRouting.hasOutput (i) ? stripRouting.getOutput (i) : -1,
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

    std::vector<Clip>& AudioEngine::clipsFor (int track)
    {
        if (track < 0) track = 0;
        if (track >= (int) trackClips.size()) trackClips.resize ((size_t) track + 1);
        return trackClips[(size_t) track];
    }

    const std::vector<Clip>* AudioEngine::tryClipsFor (int track) const
    {
        if (track < 0 || track >= (int) trackClips.size()) return nullptr;
        const auto& v = trackClips[(size_t) track];
        return v.empty() ? nullptr : &v;
    }

    bool AudioEngine::splitTrackAtPlayhead (int track)
    {
        if (track < 0 || track >= recorder.getNumTracks()) return false;
        const auto pos = player.isLoaded() ? player.getPositionSamples() : juce::int64 (0);
        if (pos <= 0) return false;

        auto sessionDir = getActiveSessionDir();
        const auto trackFile = sessionDir.isDirectory()
            ? sessionDir.getChildFile ("Audio Files")
                       .getChildFile (juce::String::formatted ("Track_%02d.wav", track + 1))
            : juce::File();
        if (! trackFile.existsAsFile()) return false;

        // Lazy bootstrap: if the track has no clip list yet, create one
        // full-range clip spanning the whole file.
        auto& list = clipsFor (track);
        if (list.empty())
        {
            Clip c;
            c.name                 = juce::String::formatted ("Track_%02d", track + 1);
            c.audioFile            = trackFile;
            c.timelineStartSamples = 0;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = trackFile.getSize() > 0
                ? (juce::int64) (trackFile.getSize() / 4)  // 24-bit WAV mono ≈ 3 B/sample, rough
                : player.getTotalLengthSamples();
            list.push_back (c);
        }

        // Find the clip whose timeline range contains the playhead.
        bool did = false;
        for (int i = 0; i < (int) list.size(); ++i)
        {
            const auto& c = list[(size_t) i];
            const auto tEnd = c.timelineStartSamples + c.fileLengthSamples;
            if (pos > c.timelineStartSamples && pos < tEnd)
            {
                const auto fileOffset = c.fileStartSamples + (pos - c.timelineStartSamples);
                did = splitClipAt (list, i, fileOffset);
                break;
            }
        }
        // Publish the updated list to the player so playback honours
        // the cut on the next block.
        if (did) player.setTrackClips (track, list);
        return did;
    }

    bool AudioEngine::editClip (int track, int clipIndex, ClipEdit mode, juce::int64 deltaSamples)
    {
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;   // edits refused on locked clips

        // Min length: 1024 samples (~20 ms at 48 k) so a clip never
        // collapses to invisibility on a fast drag.
        constexpr juce::int64 kMinLen = 1024;

        switch (mode)
        {
            case ClipEdit::TrimLeft:
            {
                // Slip-trim: timelineStart + fileStart move together,
                // fileLength contracts/expands by the inverse.
                const juce::int64 newFileStart = juce::jmax<juce::int64> (0,
                                                                          c.fileStartSamples + deltaSamples);
                const juce::int64 realDelta = newFileStart - c.fileStartSamples;
                if (c.fileLengthSamples - realDelta < kMinLen) return false;
                c.fileStartSamples     = newFileStart;
                c.timelineStartSamples = juce::jmax<juce::int64> (0,
                                                                   c.timelineStartSamples + realDelta);
                c.fileLengthSamples   -= realDelta;
                break;
            }
            case ClipEdit::TrimRight:
            {
                const juce::int64 newLen = c.fileLengthSamples + deltaSamples;
                if (newLen < kMinLen) return false;
                c.fileLengthSamples = newLen;
                break;
            }
            case ClipEdit::Move:
            {
                c.timelineStartSamples = juce::jmax<juce::int64> (0,
                                                                   c.timelineStartSamples + deltaSamples);
                break;
            }
        }
        // Publish the updated list to the player so playback honours
        // the edit on the next audio block.
        player.setTrackClips (track, list);
        return true;
    }

    bool AudioEngine::setClipFades (int track, int clipIndex,
                                    juce::int64 fadeInSamples, juce::int64 fadeOutSamples)
    {
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;

        fadeInSamples  = juce::jmax<juce::int64> (0, fadeInSamples);
        fadeOutSamples = juce::jmax<juce::int64> (0, fadeOutSamples);
        if (fadeInSamples + fadeOutSamples > c.fileLengthSamples) return false;

        c.fadeInSamples  = fadeInSamples;
        c.fadeOutSamples = fadeOutSamples;
        player.setTrackClips (track, list);
        return true;
    }

    // ─── Clip-edit helpers ──────────────────────────────────────────
    namespace
    {
        std::vector<Clip>* validClipList (std::vector<std::vector<Clip>>& all,
                                          int track, int clipIndex)
        {
            if (track < 0 || track >= (int) all.size()) return nullptr;
            auto& list = all[(size_t) track];
            if (clipIndex < 0 || clipIndex >= (int) list.size()) return nullptr;
            return &list;
        }
    }

    bool AudioEngine::setClipMuted (int track, int clipIndex, bool muted)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].muted = muted;
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::setClipLocked (int track, int clipIndex, bool locked)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].locked = locked;
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::setClipGainDb (int track, int clipIndex, float dB)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].gainDb = juce::jlimit (-60.0f, 12.0f, dB);
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::deleteClip (int track, int clipIndex)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        list->erase (list->begin() + clipIndex);
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::duplicateClip (int track, int clipIndex)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        Clip copy = (*list)[(size_t) clipIndex];
        copy.timelineStartSamples += copy.fileLengthSamples;   // place after the source
        copy.name = copy.name + " (copy)";
        copy.locked = false;
        list->insert (list->begin() + clipIndex + 1, std::move (copy));
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::isTrackPunchArmed (int channel) const noexcept
    {
        if (channel < 0 || channel >= (int) punchArmed.size()) return false;
        return punchArmed[(size_t) channel].load (std::memory_order_relaxed);
    }

    void AudioEngine::setTrackPunchArmed (int channel, bool armed)
    {
        if (channel < 0) return;
        if (channel >= (int) punchArmed.size())
        {
            const auto target = (size_t) channel + 1;
            punchArmed = std::vector<std::atomic<bool>> (target);
        }
        punchArmed[(size_t) channel].store (armed, std::memory_order_relaxed);
    }

    bool AudioEngine::swapTracks (int a, int b)
    {
        if (recorder.isRecording()) return false;
        const int n = recorder.getNumTracks();
        if (a == b || a < 0 || b < 0 || a >= n || b >= n) return false;

        // ── Swap on-disk Track_NN.wav (and any backup mirror) ────────
        auto sessionDir = activeSession.isDirectory() ? activeSession : juce::File();
        if (sessionDir.isDirectory())
        {
            auto audioDir = sessionDir.getChildFile ("Audio Files");
            if (! audioDir.isDirectory()) audioDir = sessionDir;
            const auto nameOf = [] (int idx) {
                return juce::String::formatted ("Track_%02d.wav", idx + 1);
            };
            auto fA  = audioDir.getChildFile (nameOf (a));
            auto fB  = audioDir.getChildFile (nameOf (b));
            auto tmp = audioDir.getChildFile (juce::String::formatted ("Track_%02d_swap.wav", a + 1));
            // 3-step rename so the OS never sees a name collision.
            if (fA.existsAsFile() && fB.existsAsFile())
            {
                fA .moveFileTo (tmp);
                fB .moveFileTo (fA);
                tmp.moveFileTo (fB);
            }
            else if (fA.existsAsFile())
            {
                fA.moveFileTo (fB);
            }
            else if (fB.existsAsFile())
            {
                fB.moveFileTo (fA);
            }
        }

        // ── Swap persisted overrides (PropertiesFile keys are 1-based) ──
        const int oneA = a + 1, oneB = b + 1;
        auto swapProp = [&] (const juce::String& keyA, const juce::String& keyB)
        {
            if (appProps == nullptr) return;
            const auto va = appProps->getValue (keyA, {});
            const auto vb = appProps->getValue (keyB, {});
            appProps->setValue (keyA, vb);
            appProps->setValue (keyB, va);
        };
        // Names / colours / gains / pan / routings / stereo flag.
        swapProp ("strip_name_"   + juce::String (oneA), "strip_name_"   + juce::String (oneB));
        swapProp ("strip_colour_" + juce::String (oneA), "strip_colour_" + juce::String (oneB));
        swapProp ("strip_gain_"   + juce::String (oneA), "strip_gain_"   + juce::String (oneB));
        swapProp ("strip_pan_"    + juce::String (oneA), "strip_pan_"    + juce::String (oneB));
        swapProp ("strip_in_"     + juce::String (oneA), "strip_in_"     + juce::String (oneB));
        swapProp ("strip_out_"    + juce::String (oneA), "strip_out_"    + juce::String (oneB));
        swapProp ("strip_stereo_" + juce::String (a),    "strip_stereo_" + juce::String (b));
        if (appProps != nullptr) appProps->saveIfNeeded();

        // ── Swap the live TrackState contents (NOT the objects: the UI
        //    holds references and must keep them valid). ──
        auto& ta = recorder.getTrack (a);
        auto& tb = recorder.getTrack (b);

        std::swap (ta.name, tb.name);
        const auto cA = ta.colourARGB.load();
        const auto cB = tb.colourARGB.load();
        ta.colourARGB.store (cB); tb.colourARGB.store (cA);
        const auto gA = ta.gainDb.load(), gB = tb.gainDb.load();
        ta.gainDb.store (gB); tb.gainDb.store (gA);
        const auto pA = ta.pan.load(), pB = tb.pan.load();
        ta.pan.store (pB); tb.pan.store (pA);
        const auto inA = ta.inputRouting.load(),  inB = tb.inputRouting.load();
        ta.inputRouting.store (inB); tb.inputRouting.store (inA);
        const auto outA = ta.outputRouting.load(), outB = tb.outputRouting.load();
        ta.outputRouting.store (outB); tb.outputRouting.store (outA);
        const auto sA = ta.isStereo.load(),  sB = tb.isStereo.load();
        ta.isStereo.store (sB); tb.isStereo.store (sA);
        const auto mA = ta.muted .load(),    mB = tb.muted .load();
        ta.muted .store (mB); tb.muted .store (mA);
        const auto soA = ta.soloed.load(),   soB = tb.soloed.load();
        ta.soloed.store (soB); tb.soloed.store (soA);
        const auto onA = ta.monitor.load(),  onB = tb.monitor.load();
        ta.monitor.store (onB); tb.monitor.store (onA);
        const auto arA = ta.armed .load(),   arB = tb.armed .load();
        ta.armed .store (arB); tb.armed .store (arA);

        // ── Re-load player so it picks up the renamed audio files ───
        if (sessionDir.isDirectory()) loadSession (sessionDir);
        return true;
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

        currentTempoBpm.store ((float) appProps->getDoubleValue ("sessionTempoBpm", 120.0),
                               std::memory_order_relaxed);

        // Restore the active session folder (set by New Session… / Open
        // Session…) so Save / Save As stay enabled across app restarts.
        {
            const auto saved = appProps->getValue ("activeSessionDir", {});
            if (saved.isNotEmpty())
            {
                juce::File f (saved);
                if (f.isDirectory()) activeSession = f;
            }
        }

        // Restore the LTC source strip (1-based stored, internal 0-based).
        ltcSourceStrip.store (appProps->getIntValue ("ltcSourceStrip", 0) - 1,
                              std::memory_order_release);

        // Master bus state — gain + mute + L/R output routing + stereo mode.
        masterState.name  = "MASTER";
        masterStateR.name = "MASTER R";
        masterState.gainDb.store ((float) appProps->getDoubleValue ("masterGainDb", 0.0));
        masterState.muted .store (appProps->getBoolValue ("masterMuted", false));
        masterOutL  .store (appProps->getIntValue  ("masterOutL", 0));
        masterOutR  .store (appProps->getIntValue  ("masterOutR", 1));
        masterStereo.store (appProps->getBoolValue ("masterStereo", true),
                            std::memory_order_release);

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

        deviceSampleRate.store (sr, std::memory_order_relaxed);
        audioLoadPct    .store (0.0f, std::memory_order_relaxed);

        // Hand the device's audio workgroup to the recorder so its
        // writer threads can join it. On Apple Silicon the macOS
        // scheduler then co-schedules the writers with the CoreAudio
        // IO thread — no priority inversion under load.
        recorder.setAudioWorkgroup (device->getWorkgroup());

        click.prepare (sr);
        click.setTempoBpm (currentTempoBpm.load (std::memory_order_relaxed));

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
        if (activeSession.isDirectory())
            return activeSession;
        return {};
    }

    void AudioEngine::setActiveSessionDir (const juce::File& dir)
    {
        activeSession = (dir.isDirectory() ? dir : juce::File());
        if (appProps != nullptr)
        {
            appProps->setValue ("activeSessionDir", activeSession.getFullPathName());
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setSessionTempoBpm (float bpm)
    {
        bpm = juce::jlimit (20.0f, 999.0f, bpm);
        currentTempoBpm.store (bpm, std::memory_order_relaxed);
        // Hand the new tempo to the real-time click immediately — atomic
        // store, so the audio thread picks it up at the next block.
        click.setTempoBpm (bpm);
        if (appProps != nullptr)
        {
            appProps->setValue ("sessionTempoBpm", (double) bpm);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setTempoMap (std::vector<TempoChange> newMap)
    {
        std::sort (newMap.begin(), newMap.end(),
                   [] (const auto& a, const auto& b) { return a.samplePos < b.samplePos; });
        tempoMap = std::move (newMap);
    }

    void AudioEngine::addTempoChange (juce::int64 samplePos, float bpm)
    {
        bpm = juce::jlimit (20.0f, 999.0f, bpm);
        // Replace if a change already exists within a small tolerance
        // (one block at 48k = ~10ms) so duplicate drops don't pile up.
        constexpr juce::int64 kSnap = 256;
        for (auto& c : tempoMap)
        {
            if (std::abs (c.samplePos - samplePos) < kSnap)
            {
                c.bpm = bpm;
                return;
            }
        }
        tempoMap.push_back ({ samplePos, bpm });
        std::sort (tempoMap.begin(), tempoMap.end(),
                   [] (const auto& a, const auto& b) { return a.samplePos < b.samplePos; });
    }

    void AudioEngine::removeTempoChangeNear (juce::int64 samplePos, juce::int64 tolerance)
    {
        if (tempoMap.empty()) return;
        auto best = tempoMap.end();
        juce::int64 bestDist = tolerance + 1;
        for (auto it = tempoMap.begin(); it != tempoMap.end(); ++it)
        {
            const auto d = std::abs (it->samplePos - samplePos);
            if (d < bestDist) { bestDist = d; best = it; }
        }
        if (best != tempoMap.end()) tempoMap.erase (best);
    }

    void AudioEngine::clearTempoMap()
    {
        tempoMap.clear();
    }

    // ─── Automation storage ──────────────────────────────────────────
    std::vector<AudioEngine::AutomationPoint>* AudioEngine::findLane (int track, AutomationParam p)
    {
        if (track < 0) return nullptr;
        if (track >= (int) automationData.size())
            automationData.resize ((size_t) track + 1);
        auto& a = automationData[(size_t) track];
        switch (p)
        {
            case AutomationParam::Volume: return &a.volume;
            case AutomationParam::Pan:    return &a.pan;
            case AutomationParam::Mute:   return &a.mute;
        }
        return nullptr;
    }

    const std::vector<AudioEngine::AutomationPoint>* AudioEngine::findLane (int track, AutomationParam p) const
    {
        if (track < 0 || track >= (int) automationData.size()) return nullptr;
        auto& a = automationData[(size_t) track];
        switch (p)
        {
            case AutomationParam::Volume: return &a.volume;
            case AutomationParam::Pan:    return &a.pan;
            case AutomationParam::Mute:   return &a.mute;
        }
        return nullptr;
    }

    const std::vector<AudioEngine::AutomationPoint>&
    AudioEngine::getAutomation (int track, AutomationParam p) const
    {
        if (auto* lane = findLane (track, p)) return *lane;
        return emptyAutomation;
    }

    void AudioEngine::addAutomationPoint (int track, AutomationParam p,
                                          juce::int64 samplePos, float value)
    {
        const juce::ScopedLock sl (automationLock);
        auto* lane = findLane (track, p);
        if (lane == nullptr) return;

        constexpr juce::int64 kSnap = 4096;   // ~85 ms at 48 k — replaces nearby points
        for (auto& pt : *lane)
        {
            if (std::abs (pt.samplePos - samplePos) < kSnap)
            {
                pt.value = value;
                return;
            }
        }
        lane->push_back ({ samplePos, value });
        std::sort (lane->begin(), lane->end(),
                   [] (const auto& a, const auto& b) { return a.samplePos < b.samplePos; });
    }

    float AudioEngine::automationValueAt (int track, AutomationParam p,
                                          juce::int64 samplePos,
                                          float fallback) const noexcept
    {
        const juce::ScopedTryLock stl (automationLock);
        if (! stl.isLocked()) return fallback;     // UI mid-edit — use the slider value
        if (track < 0 || track >= (int) automationData.size()) return fallback;
        const auto& a = automationData[(size_t) track];
        const std::vector<AutomationPoint>* lane = nullptr;
        switch (p)
        {
            case AutomationParam::Volume: lane = &a.volume; break;
            case AutomationParam::Pan:    lane = &a.pan;    break;
            case AutomationParam::Mute:   lane = &a.mute;   break;
        }
        if (lane == nullptr || lane->empty()) return fallback;

        // Step automation — value held until the next point. Linear
        // interpolation can come later; for now this matches what the
        // EDIT row visually paints.
        float v = (*lane)[0].value;
        for (const auto& pt : *lane)
        {
            if (pt.samplePos <= samplePos) v = pt.value;
            else break;
        }
        return v;
    }

    void AudioEngine::removeAutomationPointNear (int track, AutomationParam p,
                                                 juce::int64 samplePos, juce::int64 tolerance)
    {
        const juce::ScopedLock sl (automationLock);
        auto* lane = findLane (track, p);
        if (lane == nullptr || lane->empty()) return;

        // Pick the single closest point inside the tolerance window.
        auto best = lane->end();
        juce::int64 bestDist = tolerance + 1;
        for (auto it = lane->begin(); it != lane->end(); ++it)
        {
            const auto d = std::abs (it->samplePos - samplePos);
            if (d < bestDist) { bestDist = d; best = it; }
        }
        if (best != lane->end()) lane->erase (best);
    }

    void AudioEngine::clearAutomation (AutomationParam p)
    {
        const juce::ScopedLock sl (automationLock);
        for (auto& a : automationData)
        {
            switch (p)
            {
                case AutomationParam::Volume: a.volume.clear(); break;
                case AutomationParam::Pan:    a.pan   .clear(); break;
                case AutomationParam::Mute:   a.mute  .clear(); break;
            }
        }
    }

    void AudioEngine::clearAutomationForTrack (int track, AutomationParam p)
    {
        if (auto* lane = findLane (track, p)) lane->clear();
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
        // Fader range is -60..+12 dB — earlier clamp at 0 silently
        // snapped any positive value back to unity, which looked like
        // the fader was 'resetting itself' when the engineer pushed
        // it past the centre point.
        dB = juce::jlimit (-60.0f, 12.0f, dB);
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
            recorder.getTrack (channelIndex).name = juce::String (channelIndex + 1);
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
        // CPU-load measurement: capture start time so we can report
        // (callback time / available time) as a load percentage. The
        // dashboard polls audioLoadPct at 4 Hz to drive the LED.
        const auto cbStart = juce::Time::getHighResolutionTicks();

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

        // Playback position drives automation lookup: when the player
        // is rolling, gain / pan curves take precedence over the live
        // slider value; while stopped, the slider value is used.
        const juce::int64 playPos = player.isPlaying() ? player.getPositionSamples()
                                                       : juce::int64 (-1);

        for (int ch = 0; ch < trackCount; ++ch)
        {
            auto& t = recorder.getTrack (ch);
            if (! channelAudible (ch)) continue;

            float dBVal  = t.gainDb.load (std::memory_order_relaxed);
            float panVal = t.pan   .load (std::memory_order_relaxed);
            bool  muteAuto = false;
            if (playPos >= 0)
            {
                dBVal    = automationValueAt (ch, AutomationParam::Volume, playPos, dBVal);
                panVal   = automationValueAt (ch, AutomationParam::Pan,    playPos, panVal);
                const float muteV = automationValueAt (ch, AutomationParam::Mute,
                                                       playPos, t.muted.load() ? 1.0f : 0.0f);
                muteAuto = muteV > 0.5f;
            }
            if (muteAuto) continue;

            const double dB   = (double) dBVal;
            const double gain = juce::Decibels::decibelsToGain (dB, -60.0);
            const double pan  = juce::jlimit (-1.0, 1.0, (double) panVal);
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

            // (b) Live input — reaches the master when the channel is
            //     armed (so the engineer hears what's about to hit disk)
            //     OR monitor is on (live audition without recording).
            const bool wantInput = t.armed  .load (std::memory_order_relaxed)
                                || t.monitor.load (std::memory_order_relaxed);
            if (wantInput)
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

        // If mono, collapse L+R into accL (and zero accR) so a single
        // output gets the sum and the meter reads as one bar.
        const bool stereo = masterStereo.load (std::memory_order_relaxed);
        if (! stereo)
        {
            for (int i = 0; i < blk; ++i)
            {
                accL[i] = (accL[i] + accR[i]) * 0.5;   // average to keep headroom
                accR[i] = 0.0;
            }
        }

        // Drive the L/R master meters from the accumulator BEFORE mGain.
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
            const float pkL  = (float) mPeakL;
            const float pkR  = (float) mPeakR;
            const float rmsL = (float) std::sqrt (mRmsL * inv);
            const float rmsR = (float) std::sqrt (mRmsR * inv);

            const float prevPkL  = masterState .peak.load (std::memory_order_relaxed);
            const float prevPkR  = masterStateR.peak.load (std::memory_order_relaxed);
            const float prevRmsL = masterState .rms .load (std::memory_order_relaxed);
            const float prevRmsR = masterStateR.rms .load (std::memory_order_relaxed);

            masterState .peak.store (juce::jmax (pkL,  prevPkL  * 0.92f), std::memory_order_relaxed);
            masterStateR.peak.store (juce::jmax (pkR,  prevPkR  * 0.92f), std::memory_order_relaxed);
            masterState .rms .store (juce::jmax (rmsL, prevRmsL * 0.85f), std::memory_order_relaxed);
            masterStateR.rms .store (juce::jmax (rmsR, prevRmsR * 0.85f), std::memory_order_relaxed);
            if (pkL >= 0.999f) masterState .clipped.store (true, std::memory_order_relaxed);
            if (pkR >= 0.999f) masterStateR.clipped.store (true, std::memory_order_relaxed);
        }

        const int outL = juce::jlimit (0, numOutputs - 1, masterOutL.load (std::memory_order_relaxed));
        if (outL < numOutputs && outputs[outL] != nullptr)
            for (int i = 0; i < blk; ++i)
                outputs[outL][i] += (float) (accL[i] * mGain);

        if (stereo)
        {
            const int outR = juce::jlimit (0, numOutputs - 1, masterOutR.load (std::memory_order_relaxed));
            if (outR < numOutputs && outputs[outR] != nullptr && outR != outL)
                for (int i = 0; i < blk; ++i)
                    outputs[outR][i] += (float) (accR[i] * mGain);
        }

        // Companion stream feed — runs at the very end so it captures
        // EVERYTHING the engineer hears at outputs 0+1 (per-channel
        // routing + stream-bus + monitor sum, all collapsed into the
        // float output buffers by now).
        // ── Real-time click mix ────────────────────────────────────
        // Click runs on the audio thread so a tempo / voice change
        // takes effect on the next beat — no file reload, no glitch.
        // Mixed into outputs 0+1 (the engineer's monitor bus).
        if (click.isEnabled())
        {
            // If a tempo map exists and the player is rolling, look
            // up the BPM at the current playhead and feed it to the
            // click engine for THIS block. Tempo changes ride along
            // with the audio without any file regenerate.
            if (! tempoMap.empty() && player.isPlaying())
            {
                const auto playPosNow = player.getPositionSamples();
                float bpm = currentTempoBpm.load (std::memory_order_relaxed);
                for (const auto& tc : tempoMap)
                {
                    if (tc.samplePos <= playPosNow) bpm = tc.bpm;
                    else break;
                }
                click.setTempoBpm (bpm);
            }
            float* L = (numOutputs > 0) ? outputs[0] : nullptr;
            float* R = (numOutputs > 1) ? outputs[1] : nullptr;
            click.processBlock (L, R, numSamples);
        }

        if (companion != nullptr && companion->isRunning())
        {
            const float* L = (numOutputs > 0) ? outputs[0] : nullptr;
            const float* R = (numOutputs > 1) ? outputs[1] : nullptr;
            companion->feedStreamSamples (L, R, numSamples);
        }

        // CPU-load: (callback wall time) / (block period). EMA-smoothed
        // with a fast attack and slower release so the engineer sees
        // spikes immediately but the meter doesn't twitch on each block.
        {
            const auto cbEnd       = juce::Time::getHighResolutionTicks();
            const double cbSeconds = juce::Time::highResolutionTicksToSeconds (cbEnd - cbStart);
            const double sr        = deviceSampleRate.load (std::memory_order_relaxed);
            const double available = (sr > 0.0) ? ((double) numSamples / sr) : 0.0;
            if (available > 0.0)
            {
                const float pct  = (float) juce::jlimit (0.0, 100.0, (cbSeconds / available) * 100.0);
                const float prev = audioLoadPct.load (std::memory_order_relaxed);
                const float ema  = (pct > prev) ? pct : (prev * 0.85f + pct * 0.15f);
                audioLoadPct.store (ema, std::memory_order_relaxed);
            }
        }
    }
}
