#include "AudioEngine.h"
#include "OscRemote.h"
#include "MidiControlSurface.h"
#include <juce_osc/juce_osc.h>
#include "FastAccumulate.h"
#include "TransientDetector.h"
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
        // Skip entries that no longer exist OR live under the temp dir (a
        // scratch / test session) -- those shouldn't show as "recent".
        const auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory);
        for (int i = 0; i < kMaxRecent; ++i)
        {
            const auto p = appProps->getValue ("recentSession_" + juce::String (i));
            if (p.isEmpty()) continue;
            juce::File f (p);
            if (f.isDirectory() && ! f.isAChildOf (tempRoot)) out.add (f);
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
            // Clear persistent overrides -- name, colour, gain, pan, routing.
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
            t.setNameThreadSafe (juce::String (i + 1));
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

    bool AudioEngine::isSampleRateMismatched() const noexcept
    {
        const double target = sessionSampleRate.load (std::memory_order_relaxed);
        if (target <= 0.0) return false;   // no session rate set -> no guard
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const double devSR = device->getCurrentSampleRate();
            return devSR > 0.0 && std::abs (devSR - target) > 1.0;
        }
        return false;
    }

    void AudioEngine::setTrackLiveInputGain (int channelIndex, float dB)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        auto& t = recorder.getTrack (channelIndex);
        t.liveInputGainDb.store (dB, std::memory_order_relaxed);
        // A channel whose gain we learn for the first time mid-take has no
        // capture reference yet -- seed it so the trim delta starts at 0.
        if (recorder.isRecording()
            && t.captureInputGainDb.load (std::memory_order_relaxed) == 0.0f
            && t.armed.load (std::memory_order_relaxed))
            t.captureInputGainDb.store (dB, std::memory_order_relaxed);
    }

    float AudioEngine::getTrackTrimDelta (int channelIndex)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return 0.0f;
        auto& t = recorder.getTrack (channelIndex);
        return t.liveInputGainDb.load (std::memory_order_relaxed)
             - t.captureInputGainDb.load (std::memory_order_relaxed);
    }

    bool AudioEngine::startRecording (const juce::File& sessionDir)
    {
        // Trim-follow: snapshot each channel's current console input gain as the
        // capture reference, so the delta applied during soundcheck is measured
        // from the gain the take was actually printed at.
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);
            t.captureInputGainDb.store (t.liveInputGainDb.load (std::memory_order_relaxed),
                                        std::memory_order_relaxed);
        }

        // Bypass-proof guard: refuse to capture if the device clock disagrees
        // with the session rate (silent wrong-speed/pitch take). Covers every
        // caller -- record button, transport bar, punch auto-record, companion.
        if (isSampleRateMismatched())
        {
            DBG ("startRecording refused: sample-rate mismatch (device "
                 << (deviceManager.getCurrentAudioDevice()
                        ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 0.0)
                 << " vs session " << sessionSampleRate.load (std::memory_order_relaxed) << ")");
            return false;
        }

        if (! recorder.startRecording (sessionDir)) return false;
        rememberRecentSession (sessionDir);
        double sr = 48000.0;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            sr = device->getCurrentSampleRate();
        markers.setContext (sessionDir, sr);

        // Free-space pre-flight: estimate how long this session can
        // capture before either drive fills. UI polls
        // estimatedMinutesRemaining for the live "X min remaining"
        // indicator; this initial value is also exposed via
        // startRecordingDiskWarning so the host can show a one-shot
        // warning if we're under 30 min.
        diskMinutesRemaining.store (
            recorder.estimateMinutesRemaining (sessionDir,
                                                recorder.getBackupDirectory()),
            std::memory_order_relaxed);

        // Open the stereo mix file if the user opted in via setRecordStereoMix.
        if (recordStereoMixFlag.load (std::memory_order_acquire))
        {
            if (! mixWriterThread.isThreadRunning())
                mixWriterThread.startThread();

            // Stereo bus mix lands in Export Files/ -- the location for any
            // rendered mix / stem export.
            auto exportDir = sessionDir.getChildFile ("Export Files");
            exportDir.createDirectory();
            const auto path = exportDir.getChildFile ("StereoMix.wav");
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
        const auto sessionDir = recorder.getActiveSessionDir();
        recorder.stopRecording();
        // Fresh audio on disk -- the transient cache from the last
        // session is now stale.
        invalidateTransientCache();
        // Drop the threaded writer -- its destructor flushes the queue and
        // closes the underlying AudioFormatWriter cleanly.
        stereoMixWriter.reset();

        // Auto-load the just-recorded session into the SessionPlayer so
        // PLAY / spacebar can play back immediately without an explicit
        // 'open session' step. Skip if the dir somehow vanished.
        if (sessionDir.isDirectory())
        {
            player.loadSession (sessionDir);
            setActiveSessionDir (sessionDir);
            seedDefaultClips();
        }
    }

    bool AudioEngine::startCompanionServer (int port)
    {
        if (companion == nullptr) companion = std::make_unique<CompanionServer> (*this);
        // Default to loopback -- the engineer opts into LAN exposure
        // via startCompanionServerOnLan when they actually need it.
        return companion->start (port, "127.0.0.1");
    }

    bool AudioEngine::startCompanionServerOnLan (int port)
    {
        if (companion == nullptr) companion = std::make_unique<CompanionServer> (*this);
        return companion->start (port, "0.0.0.0");
    }

    juce::String AudioEngine::getCompanionAccessUrl() const
    {
        return companion != nullptr ? companion->getAccessUrl() : juce::String();
    }

    bool AudioEngine::isCompanionExposedOnLan() const
    {
        return companion != nullptr && companion->isExposedOnLan();
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

    juce::StringArray AudioEngine::getMidiInputNames() const
    {
        juce::StringArray names;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            names.add (d.name);
        return names;
    }

    bool AudioEngine::setMtcInputByName (const juce::String& name)
    {
        if (mtcInput != nullptr) { mtcInput->stop(); mtcInput.reset(); }
        mtcInputName.clear();
        if (name.isEmpty()) return true;   // closed = success

        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name == name)
            {
                mtcInput = juce::MidiInput::openDevice (d.identifier, this);
                if (mtcInput != nullptr) { mtcInput->start(); mtcInputName = name; return true; }
                return false;
            }
        return false;
    }

    bool AudioEngine::requestConsoleChannelNames (const juce::String& host, int port, int dialect)
    {
        if (host.trim().isEmpty() || port <= 0 || port > 65535) return false;
        juce::OSCSender sender;
        if (! sender.connect (host.trim(), port)) return false;

        // Best-effort "report your channels" nudge per console dialect -- their
        // OSC implementations differ, so we fire a couple of plausible refresh /
        // subscribe forms. The channel-name replies come back on the normal
        // receive path (each dialect parser already handles channel/N/name).
        switch (dialect)
        {
            case 2:   // Allen & Heath
                sender.send (juce::OSCMessage ("/sq/channels/?"));
                sender.send (juce::OSCMessage ("/sq/refresh"));
                break;
            case 3:   // SSL Live
                sender.send (juce::OSCMessage ("/sslnet/channel/?"));
                sender.send (juce::OSCMessage ("/sslnet/refresh"));
                break;
            case 4:   // Yamaha / RIVAGE
                sender.send (juce::OSCMessage ("/Yamaha/Channels/?"));
                sender.send (juce::OSCMessage ("/RIVAGE/Channels/?"));
                break;
            default:  // DiGiCo (1) / generic
                sender.send (juce::OSCMessage ("/Console/Channels/?"));
                sender.send (juce::OSCMessage ("/Console/Channels", (juce::int32) 1));
                sender.send (juce::OSCMessage ("/info"));
                break;
        }
        sender.disconnect();
        if (isOscDebug())
            logOsc ("-> " + host.trim() + ":" + juce::String (port)
                    + "  request channel names (dialect " + juce::String (dialect) + ")");
        return true;
    }

    void AudioEngine::logOsc (const juce::String& line)
    {
        const juce::ScopedLock sl (oscLogLock);
        oscLog.add (juce::Time::getCurrentTime().formatted ("%H:%M:%S  ") + line);
        while (oscLog.size() > 200) oscLog.remove (0);
    }
    juce::StringArray AudioEngine::getOscLog() const
    {
        const juce::ScopedLock sl (oscLogLock);
        return oscLog;
    }
    void AudioEngine::clearOscLog()
    {
        const juce::ScopedLock sl (oscLogLock);
        oscLog.clear();
    }

    void AudioEngine::setConsoleNameCapture (bool on)
    {
        if (on) capturedConsoleNames.clear();
        consoleCapture.store (on, std::memory_order_relaxed);
    }

    void AudioEngine::onConsoleChannelName (int oneBasedIndex, const juce::String& name)
    {
        if (oneBasedIndex >= 1 && consoleCapture.load (std::memory_order_relaxed))
            capturedConsoleNames[oneBasedIndex] = name;
        const int i = oneBasedIndex - 1;
        if (i >= 0 && i < recorder.getNumTracks())
            setTrackName (i, name);
    }

    std::map<int, juce::String> AudioEngine::takeCapturedConsoleNames()
    {
        auto out = capturedConsoleNames;
        capturedConsoleNames.clear();
        return out;
    }

    bool AudioEngine::enableControlSurface (const juce::String& key,
                                            const juce::String& inName, const juce::String& outName)
    {
        disableControlSurface();
        surface = std::make_unique<MidiControlSurface> (*this);
        const bool ok = surface->start (inName, outName);
        if (ok) activeSurfaceKey = key;
        else    { surface.reset(); activeSurfaceKey.clear(); }
        return ok;
    }

    void AudioEngine::disableControlSurface()
    {
        if (surface != nullptr) { surface->stop(); surface.reset(); }
        activeSurfaceKey.clear();
    }

    void AudioEngine::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
    {
        // Decoded on the MIDI thread -> TimecodeChase atomics (no locks). The
        // message-thread chase poll reads getChaseTargetSamples() to seek.
        if (m.isQuarterFrame())
            timecodeChase.feedMtcQuarterFrame (m.getRawData()[1]);
        else if (m.isFullFrame())
        {
            int h, mn, s, f;
            juce::MidiMessage::SmpteTimecodeType tc;
            m.getFullFrameParameters (h, mn, s, f, tc);
            timecodeChase.feedMtcFullFrame ((juce::uint8) h, (juce::uint8) mn,
                                            (juce::uint8) s, (juce::uint8) f);
        }
    }

    juce::int64 AudioEngine::getChaseTargetSamples() const
    {
        double fps = timecodeChase.getFps();
        if (fps <= 0.0) fps = 30.0;   // not yet learned -> sane default
        const double rate = player.getSampleRate() > 0.0 ? player.getSampleRate()
                          : (deviceManager.getCurrentAudioDevice() != nullptr
                                ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 48000.0);
        const double secs = (double) timecodeChase.getHours()   * 3600.0
                          + (double) timecodeChase.getMinutes() * 60.0
                          + (double) timecodeChase.getSeconds()
                          + (double) timecodeChase.getFrames()  / fps;
        return (juce::int64) (secs * rate + 0.5);
    }

    juce::String AudioEngine::getChaseTimecodeString() const
    {
        return juce::String::formatted ("%02d:%02d:%02d:%02d",
                                        timecodeChase.getHours(), timecodeChase.getMinutes(),
                                        timecodeChase.getSeconds(), timecodeChase.getFrames());
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

    void AudioEngine::setAutoArmOnInputDetect (bool on)
    {
        autoArmOnInputFlag.store (on, std::memory_order_release);
        if (! on) std::fill (autoArmStreaks.begin(), autoArmStreaks.end(), 0);
        if (appProps != nullptr)
        {
            appProps->setValue ("autoArmOnInput", on);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::serviceAutoArm (int periodTicks, float ampThreshold)
    {
        if (! autoArmOnInputFlag.load (std::memory_order_acquire)) return;
        if (recorder.isRecording()) return;            // mid-take arming is audio thread's job

        const int n = recorder.getNumTracks();
        if ((int) autoArmStreaks.size() < n)
            autoArmStreaks.resize ((size_t) n, 0);

        for (int i = 0; i < n; ++i)
        {
            auto& t = recorder.getTrack (i);
            if (t.armed.load (std::memory_order_relaxed)) { autoArmStreaks[(size_t) i] = 0; continue; }
            if (t.isBus.load (std::memory_order_relaxed)) continue;

            const float pk = t.peak.load (std::memory_order_relaxed);
            if (pk >= ampThreshold)
                ++autoArmStreaks[(size_t) i];
            else
                autoArmStreaks[(size_t) i] = 0;

            if (autoArmStreaks[(size_t) i] >= periodTicks)
            {
                t.armed.store (true, std::memory_order_relaxed);
                autoArmStreaks[(size_t) i] = 0;
            }
        }
    }

    void AudioEngine::setMirrors (const std::vector<MultitrackRecorder::MirrorConfig>& configs)
    {
        recorder.setMirrors (configs);
        if (appProps == nullptr) return;

        // Wipe any prior persisted mirror slots so we don't leave a
        // stale entry behind when the engineer reduces the count.
        for (int i = 0; i < 16; ++i)
        {
            appProps->removeValue ("mirror_root_"   + juce::String (i));
            appProps->removeValue ("mirror_format_" + juce::String (i));
        }
        appProps->setValue ("mirror_count", (int) configs.size());
        for (size_t i = 0; i < configs.size() && i < 16; ++i)
        {
            appProps->setValue ("mirror_root_"   + juce::String ((int) i),
                                configs[i].root.getFullPathName());
            appProps->setValue ("mirror_format_" + juce::String ((int) i),
                                (int) configs[i].format);
        }
        appProps->saveIfNeeded();
    }

    int AudioEngine::loadSession (const juce::File& sessionDir)
    {
        const auto n = player.loadSession (sessionDir);
        if (n > 0)
        {
            markers.setContext (sessionDir, player.getSampleRate());
            rememberRecentSession (sessionDir);
            seedDefaultClips();
            invalidateTransientCache();   // different session = different audio
            // (Background transient-cache warmup was attempted here
            // earlier but launched a detached lambda capturing
            // `this`, which races the engine destructor in unit-test
            // scenarios. The cache builds lazily on first Tab press
            // -- acceptable cost, no use-after-free risk.)
        }
        return n;
    }

    EngineStatus AudioEngine::captureStatus()
    {
        EngineStatus s;
        s.recording        = isRecording();
        s.playing          = player.isPlaying();
        s.positionSamples  = player.getPositionSamples();
        s.elapsedSamples   = recorder.getSamplesSinceStart();
        s.sampleRate       = getDeviceSampleRate();
        s.blockSize        = getDeviceBlockSize();
        s.audioLoadPct     = getAudioLoadPct();
        s.ringFillPct      = (float) getRingFillPct();
        s.diskMBPerSec     = (double) getDiskMBPerSec();
        s.lastWriteMs      = recorder.getLastWriteMs();
        s.minutesRemaining = getEstimatedMinutesRemaining();
        s.missedSamples    = recorder.getMissedSamples();
        s.integratedLufs   = getIntegratedLufs();
        s.momentaryLufs    = getMomentaryLufs();
        s.truePeakDb       = getTruePeakDb();
        s.numTracks        = recorder.getNumTracks();
        s.backupActive     = recorder.isBackupActive();
        s.captureFormat    = (int) recorder.getCaptureFormat();

        constexpr juce::uint32 kDefaultSwatch = 0xff3a3f44;   // neutral graphite
        s.tracks.reserve ((size_t) s.numTracks);
        for (int i = 0; i < s.numTracks; ++i)
        {
            auto& t = recorder.getTrack (i);
            TrackStatus ts;
            ts.name       = t.getNameThreadSafe();   // cross-thread (companion server)
            ts.peak       = t.peak  .load (std::memory_order_relaxed);
            ts.rms        = t.rms   .load (std::memory_order_relaxed);
            ts.armed      = t.armed .load (std::memory_order_relaxed);
            ts.muted      = t.muted .load (std::memory_order_relaxed);
            ts.soloed     = t.soloed.load (std::memory_order_relaxed);
            ts.monitor    = t.monitor.load (std::memory_order_relaxed);
            ts.stereoLeft = t.isStereo.load (std::memory_order_relaxed);
            const auto argb = (juce::uint32) t.colourARGB.load (std::memory_order_relaxed);
            ts.colourARGB = argb != 0 ? argb : kDefaultSwatch;
            if (ts.armed) ++s.armedTracks;
            s.tracks.push_back (std::move (ts));
        }
        return s;
    }

    void AudioEngine::saveSessionMixTo (const juce::File& sessionDir)
    {
        if (! sessionDir.isDirectory()) return;

        juce::Array<juce::var> arr;
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);
            juce::DynamicObject::Ptr o (new juce::DynamicObject());
            o->setProperty ("index",     i);
            o->setProperty ("name",      t.name);
            o->setProperty ("colour",    (juce::int64) t.colourARGB.load (std::memory_order_relaxed));
            o->setProperty ("gainDb",    (double) t.gainDb.load (std::memory_order_relaxed));
            o->setProperty ("pan",       (double) t.pan.load    (std::memory_order_relaxed));
            o->setProperty ("muted",     t.muted  .load (std::memory_order_relaxed));
            o->setProperty ("soloed",    t.soloed .load (std::memory_order_relaxed));
            o->setProperty ("monitor",   t.monitor.load (std::memory_order_relaxed));
            o->setProperty ("armed",     t.armed  .load (std::memory_order_relaxed));
            o->setProperty ("inRoute",   t.inputRouting .load (std::memory_order_relaxed));
            o->setProperty ("outRoute",  t.outputRouting.load (std::memory_order_relaxed));
            o->setProperty ("stereo",    t.isStereo .load (std::memory_order_relaxed));
            o->setProperty ("isBus",     t.isBus    .load (std::memory_order_relaxed));
            o->setProperty ("vcaGroup",  t.vcaGroup .load (std::memory_order_relaxed));
            o->setProperty ("editGroup", t.editGroup.load (std::memory_order_relaxed));
            // Aux sends -- per session now (was global appProps, which leaked
            // routing across sessions). 4 slots: target bus, level, post-fader.
            juce::Array<juce::var> sendsArr;
            for (int s = 0; s < TrackState::kNumSends; ++s)
            {
                juce::DynamicObject::Ptr so (new juce::DynamicObject());
                so->setProperty ("bus",  t.sends[(size_t) s].targetBus.load (std::memory_order_relaxed));
                so->setProperty ("dB",   (double) t.sends[(size_t) s].levelDb.load (std::memory_order_relaxed));
                so->setProperty ("post", t.sends[(size_t) s].postFader.load (std::memory_order_relaxed));
                sendsArr.add (juce::var (so.get()));
            }
            o->setProperty ("sends", sendsArr);
            arr.add (juce::var (o.get()));
        }

        juce::DynamicObject::Ptr root (new juce::DynamicObject());
        // Schema version for session_mix.json. v1 = first versioned schema
        // (adds isBus to the per-strip record). Absence => pre-v1 (legacy):
        // every field is still read with hasProperty guards, so old files
        // load fine and missing fields fall back to safe defaults.
        root->setProperty ("formatVersion", 1);
        root->setProperty ("trackCount", recorder.getNumTracks());
        root->setProperty ("strips", arr);

        // Session-level tempo state (BPM, time signature, and the multi-point
        // tempo map) -- per session, not global appProps.
        root->setProperty ("tempoBpm",   (double) currentTempoBpm.load (std::memory_order_relaxed));
        root->setProperty ("timeSigNum",   timeSigNumerator  .load (std::memory_order_relaxed));
        root->setProperty ("timeSigDenom", timeSigDenominator.load (std::memory_order_relaxed));
        juce::Array<juce::var> tmap;
        for (const auto& tc : tempoMap)
        {
            juce::DynamicObject::Ptr o (new juce::DynamicObject());
            o->setProperty ("pos", (juce::int64) tc.samplePos);
            o->setProperty ("bpm", (double) tc.bpm);
            tmap.add (juce::var (o.get()));
        }
        root->setProperty ("tempoMap", tmap);

        sessionDir.getChildFile ("session_mix.json")
                  .replaceWithText (juce::JSON::toString (juce::var (root.get()), true));
    }

    void AudioEngine::loadSessionMixFrom (const juce::File& sessionDir)
    {
        const auto f = sessionDir.getChildFile ("session_mix.json");
        if (! f.existsAsFile()) return;   // older session -- leave current state alone

        const auto parsed = juce::JSON::parse (f);
        auto* root = parsed.getDynamicObject();
        if (root == nullptr) return;

        // Grow the recorder to the saved strip count so every saved strip has
        // a track to land on (the file lists ALL strips, so applying each one
        // makes the session fully authoritative for its mixer state -- no need
        // to pre-clear; every field is written below).
        const int want = (int) root->getProperty ("trackCount");
        if (want > recorder.getNumTracks())
            setStripCount (want);

        // Session tempo state. Guarded so an older session (no keys) keeps
        // whatever's current rather than snapping to 0 BPM / 4-4.
        if (root->hasProperty ("tempoBpm"))
            setSessionTempoBpm ((float) (double) root->getProperty ("tempoBpm"));
        if (root->hasProperty ("timeSigNum") && root->hasProperty ("timeSigDenom"))
            setTimeSignature ((int) root->getProperty ("timeSigNum"),
                              (int) root->getProperty ("timeSigDenom"));
        if (auto* tm = root->getProperty ("tempoMap").getArray())
        {
            std::vector<TempoChange> map;
            map.reserve ((size_t) tm->size());
            for (auto& v : *tm)
                if (auto* o = v.getDynamicObject())
                    map.push_back ({ (juce::int64) o->getProperty ("pos"),
                                     (float) (double) o->getProperty ("bpm") });
            setTempoMap (std::move (map));
        }

        if (auto* arr = root->getProperty ("strips").getArray())
            for (auto& v : *arr)
                if (auto* o = v.getDynamicObject())
                {
                    const int idx = (int) o->getProperty ("index");
                    if (idx < 0 || idx >= recorder.getNumTracks()) continue;
                    auto& t = recorder.getTrack (idx);

                    if (o->hasProperty ("name"))     setTrackName          (idx, o->getProperty ("name").toString());
                    if (o->hasProperty ("colour"))   setTrackColour        (idx, juce::Colour ((juce::uint32) (juce::int64) o->getProperty ("colour")));
                    if (o->hasProperty ("gainDb"))   setTrackGainDb        (idx, (float) (double) o->getProperty ("gainDb"));
                    if (o->hasProperty ("pan"))      setTrackPan           (idx, (float) (double) o->getProperty ("pan"));
                    if (o->hasProperty ("inRoute"))  setTrackInputRouting  (idx, (int) o->getProperty ("inRoute"));
                    if (o->hasProperty ("outRoute")) setTrackOutputRouting (idx, (int) o->getProperty ("outRoute"));
                    if (o->hasProperty ("stereo"))   setTrackStereo        (idx, (bool) o->getProperty ("stereo"));
                    // isBus is per-session authoritative (was global appProps,
                    // which leaked the bus layout across sessions). Pre-v1
                    // session_mix.json has no key -> default to a normal strip.
                    setTrackIsBus     (idx, o->hasProperty ("isBus") && (bool) o->getProperty ("isBus"));
                    setTrackVcaGroup  (idx, o->hasProperty ("vcaGroup")  ? (int) o->getProperty ("vcaGroup")  : -1);
                    setTrackEditGroup (idx, o->hasProperty ("editGroup") ? (int) o->getProperty ("editGroup") : -1);

                    // mute / solo / monitor / arm have no dedicated engine
                    // setter -- the UI flips the atomics directly, so do the same.
                    t.muted  .store (o->hasProperty ("muted")   && (bool) o->getProperty ("muted"),   std::memory_order_relaxed);
                    t.soloed .store (o->hasProperty ("soloed")  && (bool) o->getProperty ("soloed"),  std::memory_order_relaxed);
                    t.monitor.store (o->hasProperty ("monitor") && (bool) o->getProperty ("monitor"), std::memory_order_relaxed);
                    t.armed  .store (o->hasProperty ("armed")   && (bool) o->getProperty ("armed"),   std::memory_order_relaxed);

                    // Aux sends (per-session, authoritative). Absent on an
                    // older session_mix.json -> leave the slots at their
                    // default "no send" so nothing routes unexpectedly.
                    if (auto* sa = o->getProperty ("sends").getArray())
                        for (int s = 0; s < juce::jmin ((int) sa->size(), TrackState::kNumSends); ++s)
                            if (auto* so = (*sa)[s].getDynamicObject())
                            {
                                t.sends[(size_t) s].targetBus.store (
                                    so->hasProperty ("bus") ? (int) so->getProperty ("bus") : -1,
                                    std::memory_order_relaxed);
                                t.sends[(size_t) s].levelDb.store (
                                    (float) (double) so->getProperty ("dB"), std::memory_order_relaxed);
                                t.sends[(size_t) s].postFader.store (
                                    ! so->hasProperty ("post") || (bool) so->getProperty ("post"),
                                    std::memory_order_relaxed);
                            }
                }
    }

    void AudioEngine::setPhasePair (int leftCh1Based, int rightCh1Based) noexcept
    {
        phaseLeft .store (juce::jmax (0, leftCh1Based  - 1), std::memory_order_relaxed);
        phaseRight.store (juce::jmax (0, rightCh1Based - 1), std::memory_order_relaxed);
    }

    void AudioEngine::setEditCursorSample (juce::int64 sample) noexcept
    {
        editCursorSample.store (sample, std::memory_order_release);
    }
    juce::int64 AudioEngine::getEditCursorSample() const noexcept
    {
        return editCursorSample.load (std::memory_order_acquire);
    }

    void AudioEngine::invalidateTransientCache()
    {
        transientCacheValid = false;
        transientCache.clear();
        transientPerTrack.clear();
    }

    // Lazy build. Scans every Track_NN.wav (in either Audio Files/
    // or the session root) and stores onsets BOTH per-track AND
    // pooled+dedupe into one sorted list. Sequential, no threading
    // -- the engineer presses Tab once and the first hit pays the
    // analysis cost (typically sub-second for a 24-track 5-min
    // session); the cache survives for the rest of the session.
    static void
    buildTransientCacheInto (const juce::File& sessionDir,
                             std::vector<juce::int64>& pooledOut,
                             std::vector<std::vector<juce::int64>>& perTrackOut)
    {
        pooledOut.clear();
        perTrackOut.clear();
        if (! sessionDir.isDirectory()) return;

        const auto audioDir = sessionDir.getChildFile ("Audio Files");
        auto files = audioDir.isDirectory()
                       ? audioDir.findChildFiles (juce::File::findFiles, false, "Track_*.wav")
                       : sessionDir.findChildFiles (juce::File::findFiles, false, "Track_*.wav");
        for (auto& f : files)
        {
            const auto stem = f.getFileNameWithoutExtension();
            const int idx = stem.fromLastOccurrenceOf ("_", false, false).getIntValue();
            if (idx <= 0) continue;
            if ((int) perTrackOut.size() < idx) perTrackOut.resize ((size_t) idx);
            auto onsets = zynforge::TransientDetector::detectInFile (f);
            perTrackOut[(size_t) idx - 1] = onsets;
            pooledOut.insert (pooledOut.end(), onsets.begin(), onsets.end());
        }
        std::sort (pooledOut.begin(), pooledOut.end());
        constexpr juce::int64 kDedupeWindow = 48000 / 40;   // ~25 ms @ 48k
        if (! pooledOut.empty())
        {
            std::vector<juce::int64> dedup;
            dedup.reserve (pooledOut.size());
            dedup.push_back (pooledOut.front());
            for (size_t i = 1; i < pooledOut.size(); ++i)
                if (pooledOut[i] - dedup.back() >= kDedupeWindow)
                    dedup.push_back (pooledOut[i]);
            pooledOut.swap (dedup);
        }
    }

    static const std::vector<juce::int64> emptyTransients;

    const std::vector<juce::int64>& AudioEngine::getTransientsForTrack (int trackIdx)
    {
        if (! transientCacheValid)
        {
            buildTransientCacheInto (getActiveSessionDir(), transientCache, transientPerTrack);
            transientCacheValid = true;
        }
        if (trackIdx < 1 || trackIdx > (int) transientPerTrack.size()) return emptyTransients;
        return transientPerTrack[(size_t) trackIdx - 1];
    }

    juce::int64 AudioEngine::nextTransientSample (juce::int64 fromSample, int restrictToTrack)
    {
        if (! transientCacheValid)
        {
            buildTransientCacheInto (getActiveSessionDir(), transientCache, transientPerTrack);
            transientCacheValid = true;
        }
        const auto* list = (restrictToTrack >= 1 && restrictToTrack <= (int) transientPerTrack.size())
                              ? &transientPerTrack[(size_t) restrictToTrack - 1]
                              : &transientCache;
        if (list->empty()) return -1;
        auto it = std::upper_bound (list->begin(), list->end(), fromSample);
        return it == list->end() ? -1 : *it;
    }

    juce::int64 AudioEngine::prevTransientSample (juce::int64 fromSample, int restrictToTrack)
    {
        if (! transientCacheValid)
        {
            buildTransientCacheInto (getActiveSessionDir(), transientCache, transientPerTrack);
            transientCacheValid = true;
        }
        const auto* list = (restrictToTrack >= 1 && restrictToTrack <= (int) transientPerTrack.size())
                              ? &transientPerTrack[(size_t) restrictToTrack - 1]
                              : &transientCache;
        if (list->empty()) return -1;
        auto it = std::lower_bound (list->begin(), list->end(), fromSample);
        if (it == list->begin()) return -1;
        --it;
        return *it;
    }

    int AudioEngine::dropMarkerAtCurrentPosition()
    {
        if (! markers.hasContext()) return -1;

        // Pro Tools-style: when the engineer has placed the edit
        // cursor in the EDIT view, the marker lands there. Otherwise
        // fall back to the live transport position (recorder while
        // recording, player otherwise) so the M-key shortcut still
        // works during a take.
        juce::int64 pos = -1;
        const auto cursor = editCursorSample.load (std::memory_order_acquire);
        if (cursor >= 0)
            pos = cursor;
        else if (recorder.isRecording())
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

    void AudioEngine::clearAllStripOverrides()
    {
        // Wipe every per-strip key in appProps that survives across
        // runs -- gains, pans, colours, names, routing, stereo flags,
        // strip UUIDs. Called on new-session creation so the engineer
        // starts from a clean slate rather than inheriting (e.g.) a
        // hard-pan they set up for last weekend's gig.
        for (int i = 0; i < 256; ++i)
        {
            stripGains  .clearGain (i);
            stripGains  .clearPan  (i);
            stripColours.clearColour (i);
            stripNames  .clearName (i);
            stripRouting.clearInput  (i);
            stripRouting.clearOutput (i);
        }
        if (appProps != nullptr)
        {
            for (int i = 0; i < 256; ++i)
            {
                const auto suffix = juce::String (i);
                appProps->removeValue ("strip_stereo_"    + suffix);
                appProps->removeValue ("strip_uid_"       + suffix);
                appProps->removeValue ("strip_vca_"       + suffix);
                appProps->removeValue ("strip_editgroup_" + suffix);
                appProps->removeValue ("strip_outmute_"   + suffix);
                appProps->removeValue ("strip_isbus_"     + suffix);
                // Aux sends -- 4 slots per strip can route to a bus.
                for (int s = 0; s < 4; ++s)
                    appProps->removeValue ("strip_send_" + juce::String (s)
                                           + "_" + suffix);
            }
            // CRITICAL: also reset the persisted strip COUNT. Wiping only the
            // per-index overrides left `stripCount` pointing at last gig's
            // number, so a "new / cleared session" silently revived that many
            // (now blank) strips on the next launch -- the gig-prep "why are
            // there tracks?" bug. A cleared/new session is empty; callers that
            // build real strips (New-from-CSV/console, or opening a session
            // with a saved mix) set the count explicitly afterward.
            appProps->setValue ("stripCount", 0);
            appProps->saveIfNeeded();
        }

        // Reset live state on whatever strips currently exist so the
        // change is visible immediately, not only after the recorder
        // grows.
        for (int i = 0, n = recorder.getNumTracks(); i < n; ++i)
        {
            auto& t = recorder.getTrack (i);
            t.gainDb  .store (0.0f,  std::memory_order_relaxed);
            t.pan     .store (0.0f,  std::memory_order_relaxed);
            t.muted   .store (false, std::memory_order_relaxed);
            t.soloed  .store (false, std::memory_order_relaxed);
            t.armed   .store (false, std::memory_order_relaxed);
            t.monitor .store (false, std::memory_order_relaxed);
            t.isStereo.store (false, std::memory_order_relaxed);
            t.vcaGroup.store (-1,    std::memory_order_relaxed);
            t.colourARGB.store (0,   std::memory_order_relaxed);
            t.stripId.clear();
        }
    }

    void AudioEngine::clearStripOverridesRange (int firstIndex, int lastIndexExclusive)
    {
        for (int i = firstIndex; i < lastIndexExclusive; ++i)
        {
            stripGains  .clearGain   (i);
            stripGains  .clearPan    (i);
            stripColours.clearColour (i);
            stripNames  .clearName   (i);
            stripRouting.clearInput  (i);
            stripRouting.clearOutput (i);

            if (appProps != nullptr)
            {
                const auto suffix = juce::String (i);
                appProps->removeValue ("strip_stereo_"    + suffix);
                appProps->removeValue ("strip_uid_"       + suffix);
                appProps->removeValue ("strip_vca_"       + suffix);
                appProps->removeValue ("strip_editgroup_" + suffix);
                appProps->removeValue ("strip_outmute_"   + suffix);
                appProps->removeValue ("strip_isbus_"     + suffix);
                for (int s = 0; s < TrackState::kNumSends; ++s)
                {
                    const auto key = "strip_send_" + suffix + "_" + juce::String (s);
                    appProps->removeValue (key + "_bus");
                    appProps->removeValue (key + "_dB");
                    appProps->removeValue (key + "_post");
                }
            }
        }
        if (appProps != nullptr) appProps->saveIfNeeded();
    }

    void AudioEngine::setStripCount (int n)
    {
        if (recorder.isRecording()) return;
        n = juce::jlimit (0, 256, n);

        const int oldCount = recorder.getNumTracks();

        if (appProps != nullptr)
        {
            appProps->setValue ("stripCount", n);
            appProps->saveIfNeeded();
        }

        deviceManager.removeAudioCallback (this);
        recorder.setTrackCount (n);
        // Freshly-added strips must NOT inherit a previous session's per-index
        // metadata that lingers in appProps -- names, colours, and especially
        // edit-group / VCA assignments. Edit groups + VCAs are stored globally
        // by array position (not in the .zfproj), so without this, adding a
        // channel (or a fresh session re-adding strips after the empty-on-open
        // start) silently revived last gig's edit-group links -- strips showed
        // an unexpected "EG" badge and moved together. Wipe the new slots
        // before applyPersistedStripState re-reads them; callers that import a
        // real session set names/routing explicitly afterwards.
        if (n > oldCount)
            clearStripOverridesRange (oldCount, n);
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
                t.setNameThreadSafe (stripNames.getName (i));
            if (stripGains.hasGain (i))
                t.gainDb.store (stripGains.getGainDb (i), std::memory_order_relaxed);
            if (stripGains.hasPan (i))
                t.pan.store    (stripGains.getPan (i), std::memory_order_relaxed);

            // Stable per-strip UUID -- generated once on first sight,
            // persisted under strip_uid_<n>. Cue snapshots reference
            // strips by this ID instead of array index, so a reorder
            // doesn't silently break every cue in the show.
            if (appProps != nullptr)
            {
                const auto key = "strip_uid_" + juce::String (i);
                auto uid = appProps->getValue (key, juce::String());
                if (uid.isEmpty())
                {
                    uid = juce::Uuid().toString();
                    appProps->setValue (key, uid);
                    appProps->saveIfNeeded();
                }
                t.stripId = uid;
            }

            // VCA group, edit group, bus flag + aux sends are NOT read here.
            // All four live in session_mix.json (loadSessionMixFrom) and are
            // applied there. Reading an index-keyed *global* appProps copy
            // would (a) leak the previous session's grouping/bus layout into
            // this one, and (b) clobber the freshly-loaded value on a device
            // restart, which re-runs this function. (The appProps copies were
            // also never swapped by swapTracks, so they went stale after any
            // reorder anyway.) loadSessionMixFrom sets vca / editGroup / isBus
            // authoritatively for every strip on session open; new strips
            // default to "ungrouped / normal channel" via TrackState
            // construction.

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

    int AudioEngine::physicalFromLogical (int logical)
    {
        int phys = 0;
        for (int k = 0; k < logical && phys < recorder.getNumTracks(); ++k)
            phys += recorder.getTrack (phys).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
        return phys;
    }

    int AudioEngine::logicalFromPhysical (int physical)
    {
        int logical = 0, p = 0;
        while (p < physical && p < recorder.getNumTracks())
        {
            p += recorder.getTrack (p).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
            ++logical;
        }
        return logical;
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
            // Grow the vector PRESERVING existing flags. std::atomic isn't
            // copy/move-constructible, so a plain resize won't do -- build a
            // bigger vector and carry the old values across. The previous
            // implementation replaced the whole vector, which silently
            // wiped every already-armed lower track whenever a higher index
            // triggered the grow (punch-arm track 1 -> track 0 lost its arm).
            const auto target = (size_t) channel + 1;
            std::vector<std::atomic<bool>> grown (target);
            for (size_t i = 0; i < punchArmed.size(); ++i)
                grown[i].store (punchArmed[i].load (std::memory_order_relaxed),
                                std::memory_order_relaxed);
            punchArmed = std::move (grown);
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

        { const auto an = ta.getNameThreadSafe(), bn = tb.getNameThreadSafe();
          ta.setNameThreadSafe (bn); tb.setNameThreadSafe (an); }
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

    namespace { std::atomic<bool> s_testSkipAudioInit { false }; }

    void AudioEngine::setTestModeSkipAudioInit (bool skip) noexcept
    {
        s_testSkipAudioInit.store (skip, std::memory_order_release);
    }

    void AudioEngine::prepareForTests (double sr, int blockSize)
    {
        // Mirrors audioDeviceAboutToStart minus the device-specific
        // calls (workgroup join, etc.). Safe to call repeatedly --
        // recorder.prepare / player.prepare are idempotent on the
        // same sr/blockSize.
        deviceSampleRate.store (sr, std::memory_order_relaxed);
        audioLoadPct    .store (0.0f, std::memory_order_relaxed);
        recorder.setAudioWorkgroup ({});   // empty workgroup -- no scheduler hint in tests
        click.prepare (sr);
        click.setTempoBpm (currentTempoBpm.load (std::memory_order_relaxed));
        loudness.prepare (sr);
        recorder.prepare (sr, blockSize, recorder.getNumTracks());   // start empty; preserve count across device restarts
        player  .prepare (sr, blockSize);
        stereoMixScratch.setSize (2, blockSize, false, true, true);
        monitorAccum    .setSize (2, blockSize, false, true, true);
        outputAccum     .setSize (64, blockSize, false, true, true);
        applyPersistedStripState();
    }

    AudioEngine::AudioEngine()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Zynforge Recording";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Zynforge Recording";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat       = juce::PropertiesFile::storeAsXML;

        if (s_testSkipAudioInit.load (std::memory_order_acquire))
        {
            // TEST ISOLATION: the in-binary test harness must NEVER touch the
            // user's real .settings file. A test that records or sets a pref
            // would otherwise persist (e.g.) a throwaway temp session path into
            // `activeSessionDir`, so the next real launch tries to reopen a
            // deleted folder. Point test engines at ONE fixed throwaway file in
            // temp -- shared across test engines (as the real file effectively
            // was), so the harness doesn't litter a file per construction.
            const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zynforge-test.settings");
            appProps = std::make_unique<juce::PropertiesFile> (tmp, opts);
        }
        else
            appProps = std::make_unique<juce::PropertiesFile> (opts);

        // Restore the stereo-mix-recording flag from the prefs file so the
        // engineer's preference survives restart.
        recordStereoMixFlag.store (appProps->getBoolValue ("recordStereoMix", false),
                                   std::memory_order_release);
        autoArmOnInputFlag.store (appProps->getBoolValue ("autoArmOnInput", false),
                                   std::memory_order_release);

        // Restore N-way mirror destinations from prefs. Skip any whose
        // root no longer exists (drive unplugged); the engineer can
        // re-add it via the UI when it's back.
        {
            const int count = appProps->getIntValue ("mirror_count", 0);
            std::vector<MultitrackRecorder::MirrorConfig> mirrors;
            for (int i = 0; i < count && i < 16; ++i)
            {
                const auto rootStr = appProps->getValue ("mirror_root_" + juce::String (i), {});
                if (rootStr.isEmpty()) continue;
                juce::File f (rootStr);
                if (! f.exists()) continue;
                MultitrackRecorder::MirrorConfig c;
                c.root   = f;
                c.format = (CaptureFormat) appProps->getIntValue (
                    "mirror_format_" + juce::String (i), (int) CaptureFormat::Wav24);
                mirrors.push_back (c);
            }
            if (! mirrors.empty()) recorder.setMirrors (mirrors);
        }

        currentTempoBpm.store ((float) appProps->getDoubleValue ("sessionTempoBpm", 120.0),
                               std::memory_order_relaxed);
        timeSigNumerator  .store (appProps->getIntValue ("timeSigNumerator",   4),
                                  std::memory_order_relaxed);
        timeSigDenominator.store (appProps->getIntValue ("timeSigDenominator", 4),
                                  std::memory_order_relaxed);
        // Sync the metronome bar length to the restored meter.
        click.setBeatsPerBar (timeSigNumerator.load (std::memory_order_relaxed));

        // Restore the active session folder (set by New Session... / Open
        // Session...) so Save / Save As stay enabled across app restarts.
        {
            const auto saved = appProps->getValue ("activeSessionDir", {});
            if (saved.isNotEmpty())
            {
                juce::File f (saved);
                // Only restore a real, still-present session folder. Reject a
                // path under the temp dir (a leftover scratch / test session)
                // so a stale entry can't make the app reopen a deleted folder.
                const auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory);
                if (f.isDirectory() && ! f.isAChildOf (tempRoot))
                    activeSession = f;
                else
                {
                    appProps->removeValue ("activeSessionDir");   // tidy the stale key
                    appProps->saveIfNeeded();                     // flush so it's gone on disk too
                }
            }
        }

        // Restore the LTC source strip (1-based stored, internal 0-based).
        ltcSourceStrip.store (appProps->getIntValue ("ltcSourceStrip", 0) - 1,
                              std::memory_order_release);

        // Master bus state -- gain + mute + L/R output routing + stereo mode.
        masterState.name  = "MASTER";
        masterStateR.name = "MASTER R";
        masterState.gainDb.store ((float) appProps->getDoubleValue ("masterGainDb", 0.0));
        masterState.muted .store (appProps->getBoolValue ("masterMuted", false));
        masterOutL  .store (appProps->getIntValue  ("masterOutL", 0));
        masterOutR  .store (appProps->getIntValue  ("masterOutR", 1));
        masterStereo.store (appProps->getBoolValue ("masterStereo", true),
                            std::memory_order_release);

        // Open with up to 256 inputs / 64 outputs by default -- adjust later from UI.
        if (! s_testSkipAudioInit.load (std::memory_order_acquire))
        {
            auto err = deviceManager.initialise (/*numInputs*/ 256, /*numOutputs*/ 64,
                                                 /*savedState*/ nullptr,
                                                 /*selectDefault*/ true);
            if (err.isNotEmpty())
                DBG ("AudioDeviceManager init: " << err);
        }

        // Restore persisted VCA names + colours so a relaunch shows the
        // engineer's "DRUMS" / "VOX" labels instead of "VCA 1..8".
        for (int i = 0; i < kNumVcas; ++i)
        {
            const auto nameKey = "vca_" + juce::String (i) + "_name";
            const auto colKey  = "vca_" + juce::String (i) + "_colour";
            const auto n  = appProps->getValue (nameKey, "VCA " + juce::String (i + 1));
            const auto cc = (juce::uint32) appProps->getDoubleValue (colKey, 0.0);
            vcas[(size_t) i].name = n;
            vcas[(size_t) i].colourARGB.store (cc, std::memory_order_relaxed);
        }

        if (! s_testSkipAudioInit.load (std::memory_order_acquire))
            deviceManager.addAudioCallback (this);
    }

    AudioEngine::~AudioEngine()
    {
        if (! s_testSkipAudioInit.load (std::memory_order_acquire))
            deviceManager.removeAudioCallback (this);
    }

    void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
    {
        const auto sr        = device->getCurrentSampleRate();
        const auto blockSize = device->getCurrentBufferSizeSamples();

        deviceSampleRate.store (sr, std::memory_order_relaxed);
        deviceBlockSize .store (blockSize, std::memory_order_relaxed);
        audioLoadPct    .store (0.0f, std::memory_order_relaxed);

        // Hand the device's audio workgroup to the recorder so its
        // writer threads can join it. On Apple Silicon the macOS
        // scheduler then co-schedules the writers with the CoreAudio
        // IO thread -- no priority inversion under load.
        recorder.setAudioWorkgroup (device->getWorkgroup());

        click.prepare (sr);
        click.setTempoBpm (currentTempoBpm.load (std::memory_order_relaxed));
        loudness.prepare (sr);

        // Strip count is user-controlled: the app opens with NO channels
        // (engineer adds them via +CH), and the count is preserved across
        // device restarts -- it is NOT restored from a previous session.
        recorder.prepare (sr, blockSize, recorder.getNumTracks());   // start empty; preserve count across device restarts
        player  .prepare (sr, blockSize);

        // Pre-allocate the stereo mix scratch buffer at the device block
        // size so the audio thread never allocates when mix capture is on.
        stereoMixScratch.setSize (2, blockSize, false, true, true);
        monitorAccum    .setSize (2, blockSize, false, true, true);
        // 64-bit output accumulator -- sized large enough for typical
        // device output channel counts. Resized lazily inside the
        // callback if a wider device shows up.
        outputAccum     .setSize (64, blockSize, false, true, true);

        applyPersistedStripState();
    }

    juce::Array<juce::File> AudioEngine::findIncompleteSessions (const juce::File& sessionsRoot)
    {
        juce::Array<juce::File> incomplete;
        if (! sessionsRoot.isDirectory()) return incomplete;

        // Match every session subdir, not just the auto-named
        // `Session_*` ones. User-named sessions (created via
        // File > New Session with a custom name) carry the same
        // `recording.session` marker when they crash, and were
        // previously invisible to the recovery scan.
        const auto dirs = sessionsRoot.findChildFiles (juce::File::findDirectories, false);
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

    void AudioEngine::setTimeSignature (int numerator, int denominator)
    {
        numerator   = juce::jlimit (1, 32, numerator);
        // Powers of two from 1 to 32 -- the typical music range.
        if (denominator < 1) denominator = 4;
        if (denominator > 32) denominator = 32;
        timeSigNumerator  .store (numerator,   std::memory_order_relaxed);
        timeSigDenominator.store (denominator, std::memory_order_relaxed);
        // Keep the metronome's bar length in sync so the accent (downbeat)
        // lands on beat 1 of the actual meter, not a fixed 4.
        click.setBeatsPerBar (numerator);
        if (appProps != nullptr)
        {
            appProps->setValue ("timeSigNumerator",   numerator);
            appProps->setValue ("timeSigDenominator", denominator);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setSessionTempoBpm (float bpm)
    {
        bpm = juce::jlimit (20.0f, 999.0f, bpm);
        currentTempoBpm.store (bpm, std::memory_order_relaxed);
        // Hand the new tempo to the real-time click immediately -- atomic
        // store, so the audio thread picks it up at the next block.
        click.setTempoBpm (bpm);
        midiClockOut.setTempoBpm (bpm);
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
        // Single source of truth -- sets BOTH sides of the patch so the
        // PATCH page + the per-strip dropdowns stay in lockstep.
        setTrackInputRouting  (channelIndex, deviceCh);
        setTrackOutputRouting (channelIndex, deviceCh);
    }

    void AudioEngine::setTrackGainDb (int channelIndex, float dB)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        // Fader range is -60..+12 dB -- earlier clamp at 0 silently
        // snapped any positive value back to unity, which looked like
        // the fader was 'resetting itself' when the engineer pushed
        // it past the centre point.
        dB = juce::jlimit (-60.0f, 12.0f, dB);
        auto& t = recorder.getTrack (channelIndex);
        t.gainDb.store (dB, std::memory_order_relaxed);
        // Direct set cancels any in-flight ramp so the engineer's slow
        // physical fader move doesn't fight a finishing cue ramp.
        t.rampTargetGainDb.store (dB, std::memory_order_relaxed);
        t.rampSamplesRemaining.store (0, std::memory_order_relaxed);
        stripGains.setGainDb (channelIndex, dB);
    }

    void AudioEngine::setTrackGainDbRamped (int channelIndex, float dB, double seconds)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        dB = juce::jlimit (-60.0f, 12.0f, dB);
        auto& t = recorder.getTrack (channelIndex);
        const auto sr = deviceSampleRate.load (std::memory_order_relaxed);
        const juce::int64 samples = sr > 0.0
            ? (juce::int64) (sr * juce::jmax (0.0, seconds))
            : 0;
        t.rampTargetGainDb     .store (dB,      std::memory_order_relaxed);
        t.rampSamplesRemaining .store (samples, std::memory_order_relaxed);
        if (samples == 0)
            t.gainDb.store (dB, std::memory_order_relaxed);   // instant
        stripGains.setGainDb (channelIndex, dB);              // persisted target
    }

    void AudioEngine::setTrackPanRamped (int channelIndex, float pan, double seconds)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        auto& t = recorder.getTrack (channelIndex);
        const auto sr = deviceSampleRate.load (std::memory_order_relaxed);
        const juce::int64 samples = sr > 0.0
            ? (juce::int64) (sr * juce::jmax (0.0, seconds))
            : 0;
        t.rampTargetPan       .store (pan,     std::memory_order_relaxed);
        // Reuse rampSamplesRemaining for pan -- gain + pan ramp together
        // during a cue recall, same duration, so one counter is enough.
        // (Pan ramp uses the same countdown as the gain ramp.)
        t.rampSamplesRemaining.store (samples, std::memory_order_relaxed);
        if (samples == 0)
            t.pan.store (pan, std::memory_order_relaxed);
    }

    void AudioEngine::setVcaGainDb (int idx, float dB)
    {
        if (idx < 0 || idx >= kNumVcas) return;
        dB = juce::jlimit (-60.0f, 12.0f, dB);
        vcas[(size_t) idx].gainDb.store (dB, std::memory_order_relaxed);
        vcas[(size_t) idx].rampTargetGainDb.store (dB, std::memory_order_relaxed);
        vcas[(size_t) idx].rampSamplesRemaining.store (0, std::memory_order_relaxed);
    }

    void AudioEngine::setVcaGainDbRamped (int idx, float dB, double seconds)
    {
        if (idx < 0 || idx >= kNumVcas) return;
        dB = juce::jlimit (-60.0f, 12.0f, dB);
        const auto sr = deviceSampleRate.load (std::memory_order_relaxed);
        const juce::int64 samples = sr > 0.0
            ? (juce::int64) (sr * juce::jmax (0.0, seconds))
            : 0;
        vcas[(size_t) idx].rampTargetGainDb.store (dB, std::memory_order_relaxed);
        vcas[(size_t) idx].rampSamplesRemaining.store (samples, std::memory_order_relaxed);
        if (samples == 0)
            vcas[(size_t) idx].gainDb.store (dB, std::memory_order_relaxed);
    }

    void AudioEngine::setVcaMuted (int idx, bool muted)
    {
        if (idx < 0 || idx >= kNumVcas) return;
        vcas[(size_t) idx].muted.store (muted, std::memory_order_relaxed);
    }

    void AudioEngine::setVcaSoloed (int idx, bool soloed)
    {
        if (idx < 0 || idx >= kNumVcas) return;
        vcas[(size_t) idx].soloed.store (soloed, std::memory_order_relaxed);
    }

    void AudioEngine::setVcaName (int idx, const juce::String& name)
    {
        if (idx < 0 || idx >= kNumVcas) return;
        vcas[(size_t) idx].name = name;
        if (appProps != nullptr)
        {
            appProps->setValue ("vca_" + juce::String (idx) + "_name", name);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setVcaColour (int idx, juce::Colour c)
    {
        if (idx < 0 || idx >= kNumVcas) return;
        vcas[(size_t) idx].colourARGB.store (c.getARGB(), std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            appProps->setValue ("vca_" + juce::String (idx) + "_colour",
                                (juce::int64) c.getARGB());
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setTrackIsBus (int channelIndex, bool isBus)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        auto& t = recorder.getTrack (channelIndex);
        t.isBus.store (isBus, std::memory_order_relaxed);
        // Bus tracks have no input -- clear routing so the audio
        // callback's routedInputs[i] resolves to nullptr.
        if (isBus)
        {
            t.inputRouting.store (-1, std::memory_order_relaxed);
            t.armed.store (false, std::memory_order_relaxed);
            t.monitor.store (false, std::memory_order_relaxed);
        }
        // isBus is authoritative in session_mix.json (saveSessionMixTo /
        // loadSessionMixFrom). It is deliberately NOT written to global
        // appProps -- a global, index-keyed copy leaked the bus layout into
        // the next session opened (same bug class the aux sends had).
    }

    void AudioEngine::setTrackSend (int channelIndex, int slot,
                                     int targetBus, float levelDb, bool postFader)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        if (slot < 0 || slot >= TrackState::kNumSends) return;
        levelDb = juce::jlimit (-60.0f, 12.0f, levelDb);

        auto& s = recorder.getTrack (channelIndex).sends[(size_t) slot];
        s.targetBus.store (targetBus,  std::memory_order_relaxed);
        s.levelDb  .store (levelDb,    std::memory_order_relaxed);
        s.postFader.store (postFader,  std::memory_order_relaxed);
        // Sends persist per-session via session_mix.json (saveSessionMixTo),
        // NOT global appProps -- writing them to appProps keyed by index made
        // routing leak from one session into the next.
    }

    void AudioEngine::setTrackVcaGroup (int channelIndex, int vcaIdx)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        const int clamped = (vcaIdx < 0 || vcaIdx >= kNumVcas) ? -1 : vcaIdx;
        recorder.getTrack (channelIndex).vcaGroup.store (clamped, std::memory_order_relaxed);
        // VCA group is authoritative in session_mix.json -- not written to
        // global appProps (an index-keyed global copy leaked across sessions
        // and went stale after a reorder).
    }

    juce::int64 AudioEngine::snapSampleToGrid (juce::int64 sample)
    {
        const auto mode = getSnapMode();
        if (mode == SnapMode::Off || sample < 0) return sample;

        // Markers: pick the nearest marker. Empty marker list -> no snap.
        const auto& list = markers.getAll();
        if (list.empty()) return sample;
        juce::int64 best = list.front().sampleOffset;
        juce::int64 bestDist = std::abs (best - sample);
        for (const auto& m : list)
        {
            const auto d = std::abs (m.sampleOffset - sample);
            if (d < bestDist) { bestDist = d; best = m.sampleOffset; }
        }
        return best;
    }

    void AudioEngine::setTrackEditGroup (int channelIndex, int groupId)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        recorder.getTrack (channelIndex).editGroup.store (groupId, std::memory_order_relaxed);
        // Edit group is authoritative in session_mix.json -- not written to
        // global appProps (same leak/stale-on-reorder class as VCA + isBus).
    }

    int AudioEngine::getTrackEditGroup (int channelIndex) noexcept
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return -1;
        return recorder.getTrack (channelIndex).editGroup.load (std::memory_order_relaxed);
    }

    std::vector<int> AudioEngine::getStripsInEditGroup (int groupId)
    {
        std::vector<int> out;
        if (groupId < 0) return out;
        const int n = recorder.getNumTracks();
        for (int i = 0; i < n; ++i)
            if (recorder.getTrack (i).editGroup.load (std::memory_order_relaxed) == groupId)
                out.push_back (i);
        return out;
    }

    void AudioEngine::tickRamps (int numSamples) noexcept
    {
        if (numSamples <= 0) return;

        // VCA gain ramps -- same per-block linear step as the strip ramps.
        for (auto& vca : vcas)
        {
            const juce::int64 remaining = vca.rampSamplesRemaining.load (std::memory_order_relaxed);
            if (remaining <= 0) continue;
            const float curG = vca.gainDb.load (std::memory_order_relaxed);
            const float tgtG = vca.rampTargetGainDb.load (std::memory_order_relaxed);
            const juce::int64 denom = juce::jmax ((juce::int64) numSamples, remaining);
            const float frac = (float) numSamples / (float) denom;
            const float newG = curG + (tgtG - curG) * frac;
            const juce::int64 next = remaining - (juce::int64) numSamples;
            if (next <= 0)
            {
                vca.gainDb.store (tgtG, std::memory_order_relaxed);
                vca.rampSamplesRemaining.store (0, std::memory_order_relaxed);
            }
            else
            {
                vca.gainDb.store (newG, std::memory_order_relaxed);
                vca.rampSamplesRemaining.store (next, std::memory_order_relaxed);
            }
        }

        const int n = recorder.getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            auto& t = recorder.getTrack (i);
            const juce::int64 remaining = t.rampSamplesRemaining.load (std::memory_order_relaxed);
            if (remaining <= 0) continue;

            const float curG = t.gainDb.load (std::memory_order_relaxed);
            const float tgtG = t.rampTargetGainDb.load (std::memory_order_relaxed);
            const float curP = t.pan   .load (std::memory_order_relaxed);
            const float tgtP = t.rampTargetPan   .load (std::memory_order_relaxed);

            // Defensive: clamp denominator so a stale remaining can't
            // produce NaN/inf gain values.
            const juce::int64 denom = juce::jmax ((juce::int64) numSamples, remaining);
            const float frac = (float) numSamples / (float) denom;
            const float newG = curG + (tgtG - curG) * frac;
            const float newP = curP + (tgtP - curP) * frac;

            const juce::int64 next = remaining - (juce::int64) numSamples;
            if (next <= 0)
            {
                t.gainDb.store (tgtG, std::memory_order_relaxed);
                t.pan   .store (tgtP, std::memory_order_relaxed);
                t.rampSamplesRemaining.store (0, std::memory_order_relaxed);
            }
            else
            {
                t.gainDb.store (newG, std::memory_order_relaxed);
                t.pan   .store (newP, std::memory_order_relaxed);
                t.rampSamplesRemaining.store (next, std::memory_order_relaxed);
            }
        }
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
            recorder.getTrack (channelIndex).setNameThreadSafe (juce::String (channelIndex + 1));
        }
        else
        {
            stripNames.setName (channelIndex, name);
            recorder.getTrack (channelIndex).setNameThreadSafe (name);
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

        // 64-bit output accumulator -- every per-strip / VCA / stream
        // sum lands here in double precision; the final cast to the
        // device's float buffers happens at the end of the callback.
        // Cheap to ensure the size every block -- JUCE no-ops when
        // already big enough.
        if (outputAccum.getNumChannels() < numOutputs || outputAccum.getNumSamples() < numSamples)
            outputAccum.setSize (juce::jmax (outputAccum.getNumChannels(), numOutputs),
                                 juce::jmax (outputAccum.getNumSamples(), numSamples),
                                 false, false, true);
        for (int ch = 0; ch < numOutputs; ++ch)
            juce::FloatVectorOperations::clear (outputAccum.getWritePointer (ch), numSamples);

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

        // Soft-takeover ramps (cue recall) -- step gain / pan per block
        // toward their target values before they're read downstream by
        // the monitor / per-channel output / stream-bus loops.
        tickRamps (numSamples);

        // Have the player render into a scratch buffer per track, then we
        // route each track to its configured output channel.
        if (playerScratch.getNumChannels() < numTracks || playerScratch.getNumSamples() < numSamples)
            playerScratch.setSize (juce::jmax (numTracks, 1), juce::jmax (numSamples, 64),
                                   false, false, true);
        playerScratch.clear (0, numSamples);
        player.processBlock (playerScratch.getArrayOfWritePointers(),
                             juce::jmin (playerScratch.getNumChannels(), numTracks),
                             numSamples);

        // Strip's effective dB = its own gain + its VCA bus gain. Defined
        // once here so every consumer below (aux sends, per-channel mix,
        // stream bus) shares one definition instead of re-deriving it.
        // Volume automation overrides the fader during playback. This is
        // applied HERE (inside effectiveGainDb) so it reaches the per-track
        // routed hardware outputs, aux sends, and the stream bus -- not just
        // the stereo monitor mix. Previously only the monitor mix honoured
        // automation, so a session routed straight to outputs (Out 1-2 etc.)
        // played at the static fader level and a drawn curve did nothing.
        const juce::int64 autoPlayPos = player.isPlaying() ? player.getPositionSamples()
                                                           : (juce::int64) -1;
        const auto effectiveGainDb = [this, autoPlayPos] (int i) noexcept -> float
        {
            auto& t = recorder.getTrack (i);
            float dB = t.gainDb.load (std::memory_order_relaxed);
            if (autoPlayPos >= 0)
            {
                // Stereo pairs store automation on the LEFT track, so the
                // right partner reads its left neighbour's volume lane.
                const int autoCh = (i > 0 && recorder.getTrack (i - 1).isStereo.load (std::memory_order_relaxed))
                                     ? i - 1 : i;
                dB = automationValueAt (autoCh, AutomationParam::Volume, autoPlayPos, dB);
            }
            const int g = t.vcaGroup.load (std::memory_order_relaxed);
            if (g >= 0 && g < kNumVcas)
                dB += vcas[(size_t) g].gainDb.load (std::memory_order_relaxed);
            // Trim-follow: track the console's input-gain change since capture
            // so soundcheck preamp moves shift the recorded track like a live mic.
            if (trimFollowEnabled.load (std::memory_order_relaxed))
                dB += t.liveInputGainDb.load (std::memory_order_relaxed)
                    - t.captureInputGainDb.load (std::memory_order_relaxed);
            return dB;
        };

        // Aux sends → bus tracks. For every non-bus strip with a send
        // pointing at a bus track, sum (strip_audio × send_gain × (post
        // ? strip_gain : 1)) into that bus's row of playerScratch. The
        // bus track is then summed into the master / per-channel outputs
        // by the standard output loop below, exactly like a normal
        // strip -- its 'audio' is the mix of every send routed at it.
        for (int i = 0; i < numTracks; ++i)
        {
            auto& src = recorder.getTrack (i);
            if (src.isBus.load (std::memory_order_relaxed)) continue;
            const float* srcAudio = playerScratch.getReadPointer (i);
            if (srcAudio == nullptr) continue;

            // Strip's effective gain for post-fader sends.
            const float stripGain = juce::Decibels::decibelsToGain (effectiveGainDb (i), -60.0f);

            for (int s = 0; s < TrackState::kNumSends; ++s)
            {
                const auto& snd = src.sends[(size_t) s];
                const int bus = snd.targetBus.load (std::memory_order_relaxed);
                if (bus < 0 || bus >= numTracks) continue;
                if (! recorder.getTrack (bus).isBus.load (std::memory_order_relaxed)) continue;

                const float sendDb = snd.levelDb.load (std::memory_order_relaxed);
                const float post   = snd.postFader.load (std::memory_order_relaxed) ? stripGain : 1.0f;
                const float gain = juce::Decibels::decibelsToGain (sendDb, -60.0f) * post;
                if (gain < 0.00001f) continue;

                float* dst = playerScratch.getWritePointer (bus);
                juce::FloatVectorOperations::addWithMultiply (dst, srcAudio, gain, numSamples);
            }
        }

        // Drive per-strip meters from the player's output ONLY when the
        // player is actually rolling. The recorder has already written
        // the input-side peak / rms for this block; we max the player
        // contribution on top so a strip with BOTH input AND playback
        // shows whichever is louder. When the player is stopped we
        // leave the recorder's values alone -- otherwise this would
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

        // Any VCA bus soloed? Solo'd VCAs follow the same "only soloed
        // passes" rule, applied across all strips assigned to that bus.
        bool anyVcaSolo = false;
        for (const auto& v : vcas)
            if (v.soloed.load (std::memory_order_relaxed)) { anyVcaSolo = true; break; }

        auto channelAudible = [&] (int i) -> bool
        {
            auto& t = recorder.getTrack (i);
            const int g = t.vcaGroup.load (std::memory_order_relaxed);
            if (g >= 0 && g < kNumVcas)
            {
                // VCA mute gates the strip.
                if (vcas[(size_t) g].muted.load (std::memory_order_relaxed)) return false;
                if (anyVcaSolo && ! vcas[(size_t) g].soloed.load (std::memory_order_relaxed)) return false;
            }
            else if (anyVcaSolo)
            {
                // Strip has no VCA -- when any VCA is soloed, ungrouped
                // strips drop out (matches console behaviour).
                return false;
            }
            if (anySolo) return t.soloed.load (std::memory_order_relaxed);
            return ! t.muted.load (std::memory_order_relaxed);
        };

        // Mix the routed track scratch onto the device outputs honoring
        // mute/solo and per-channel gain.
        for (int i = 0; i < numTracks; ++i)
        {
            if (! channelAudible (i)) continue;

            auto& t = recorder.getTrack (i);
            // outputMuted gates the physical output independently of the
            // monitor-side `muted` flag -- so an engineer can drop a track
            // from FOH while still hearing it in their cans (or the
            // reverse).
            if (t.outputMuted.load (std::memory_order_relaxed)) continue;
            const int devOut = t.outputRouting.load (std::memory_order_relaxed);
            if (devOut < 0 || devOut >= numOutputs || outputs[devOut] == nullptr) continue;

            const float dB   = effectiveGainDb (i);
            const double gain = (double) juce::Decibels::decibelsToGain (dB, -60.0f);
            const float* src = playerScratch.getReadPointer (i);
            double* dst = outputAccum.getWritePointer (devOut);

            // Prefetch the next strip's playerScratch row + its target
            // output column into L1 -- at 256+ tracks this hides the
            // ~10-cycle memory miss on each iteration boundary.
            if (i + 1 < numTracks)
            {
                __builtin_prefetch (playerScratch.getReadPointer (i + 1), 0, 1);
                const int nextOut = recorder.getTrack (i + 1).outputRouting
                                      .load (std::memory_order_relaxed);
                if (nextOut >= 0 && nextOut < numOutputs)
                    __builtin_prefetch (outputAccum.getWritePointer (nextOut), 1, 1);
            }

            // NEON-vectorised float-into-double FMA -- about 2.5x
            // faster than the auto-vectorised scalar loop on M1+.
            fastaccum::addFloatScaledIntoDouble (dst, src, gain, numSamples);
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

                const float dB   = effectiveGainDb (i);
                const double gain = (double) juce::Decibels::decibelsToGain (dB, -60.0f);
                const float pan  = juce::jlimit (-1.0f, 1.0f, t.pan.load (std::memory_order_relaxed));
                const double panNorm = ((double) pan + 1.0) * 0.5;
                const double gL = gain * std::cos (panNorm * juce::MathConstants<double>::halfPi);
                const double gR = gain * std::sin (panNorm * juce::MathConstants<double>::halfPi);

                const float* src = playerScratch.getReadPointer (i);
                // Stream bus also goes through the 64-bit accumulator
                // so the L/R sum stays full precision across N strips.
                // Same NEON helper as the per-channel sum above.
                if (sL < numOutputs && gL > 0.00001)
                    fastaccum::addFloatScaledIntoDouble (outputAccum.getWritePointer (sL),
                                                         src, gL, numSamples);
                if (sR < numOutputs && gR > 0.00001)
                    fastaccum::addFloatScaledIntoDouble (outputAccum.getWritePointer (sR),
                                                         src, gR, numSamples);

                if (wantMixCapture)
                {
                    juce::FloatVectorOperations::addWithMultiply (stereoMixScratch.getWritePointer (0),
                                                                  src, (float) gL, numSamples);
                    juce::FloatVectorOperations::addWithMultiply (stereoMixScratch.getWritePointer (1),
                                                                  src, (float) gR, numSamples);
                }
            }
        }

        if (wantMixCapture)
        {
            const float* chans[2] = { stereoMixScratch.getReadPointer (0),
                                      stereoMixScratch.getReadPointer (1) };
            stereoMixWriter->write (chans, numSamples);
        }

        // LTC presence detection -- analyzes the input of a designated
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
                // A stereo pair stores its automation on the LEFT track only.
                // The RIGHT partner (the channel after an isStereo=true track)
                // has no lane, so it reads the left track's volume + mute lanes
                // -- otherwise the right side ignores the curve entirely and a
                // volume move barely changes the (still full-level) stereo sum.
                // Pan stays per-channel so the stereo image isn't collapsed.
                const int autoCh = (ch > 0 && recorder.getTrack (ch - 1).isStereo.load (std::memory_order_relaxed))
                                     ? ch - 1 : ch;
                dBVal    = automationValueAt (autoCh, AutomationParam::Volume, playPos, dBVal);
                panVal   = automationValueAt (ch,     AutomationParam::Pan,    playPos, panVal);
                const float muteV = automationValueAt (autoCh, AutomationParam::Mute,
                                                       playPos, t.muted.load() ? 1.0f : 0.0f);
                muteAuto = muteV > 0.5f;
            }
            if (muteAuto) continue;

            // VCA group gain. The monitor sum was applying ONLY the strip's
            // own fader, so pulling a VCA master down did nothing here even
            // though it worked on the routed outputs (which go through
            // effectiveGainDb). Fold the VCA gain in so the monitor matches.
            const int vgrp = t.vcaGroup.load (std::memory_order_relaxed);
            if (vgrp >= 0 && vgrp < kNumVcas)
                dBVal += vcas[(size_t) vgrp].gainDb.load (std::memory_order_relaxed);

            const double dB   = (double) dBVal;
            const double gain = juce::Decibels::decibelsToGain (dB, -60.0);
            const double pan  = juce::jlimit (-1.0, 1.0, (double) panVal);
            const double panNorm = (pan + 1.0) * 0.5;
            const double gL = gain * std::cos (panNorm * juce::MathConstants<double>::halfPi);
            const double gR = gain * std::sin (panNorm * juce::MathConstants<double>::halfPi);

            // (a) VSC playback -- always summed to master when audible.
            // Trim-follow applies HERE (recorded content) but NOT to live
            // input (b): the desk's input-gain change since capture is added
            // so soundcheck preamp moves track the recorded track. Computed as
            // a separate gain so the live mic path stays untouched.
            if (ch < playerScratch.getNumChannels())
            {
                const float* psrc = playerScratch.getReadPointer (ch);
                if (psrc != nullptr)
                {
                    double gLp = gL, gRp = gR;
                    if (trimFollowEnabled.load (std::memory_order_relaxed))
                    {
                        const double trim = (double) (t.liveInputGainDb.load (std::memory_order_relaxed)
                                                    - t.captureInputGainDb.load (std::memory_order_relaxed));
                        const double m = juce::Decibels::decibelsToGain (dB + trim, -60.0) / juce::jmax (1.0e-9, gain);
                        gLp = gL * m; gRp = gR * m;
                    }
                    for (int i = 0; i < blk; ++i)
                    {
                        const double s = (double) psrc[i];
                        accL[i] += s * gLp;
                        accR[i] += s * gRp;
                    }
                }
            }

            // (b) Live input -- reaches the master when the channel is
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

        // Feed the BS.1770 loudness + true-peak meter from the post-fader
        // master (mGain applied inside). RT-safe -- the meter keeps only
        // fixed-size filter / histogram state.
        loudness.processMasterDouble (accL, accR, mGain, blk);

        // Guard against a device with zero outputs (e.g. an input-only
        // interface selected as the output device): numOutputs - 1 == -1
        // would make jlimit return -1 and getWritePointer(-1) segfault.
        const int outL = numOutputs > 0
                       ? juce::jlimit (0, numOutputs - 1, masterOutL.load (std::memory_order_relaxed))
                       : -1;
        if (outL >= 0 && outL < numOutputs)
        {
            // Monitor sum lands in the 64-bit accumulator so the master
            // bus stays double-precision until the final downcast below.
            double* dstL = outputAccum.getWritePointer (outL);
            for (int i = 0; i < blk; ++i)
                dstL[i] += accL[i] * mGain;
        }

        if (stereo && numOutputs > 0)
        {
            const int outR = juce::jlimit (0, numOutputs - 1, masterOutR.load (std::memory_order_relaxed));
            if (outR < numOutputs && outR != outL)
            {
                double* dstR = outputAccum.getWritePointer (outR);
                for (int i = 0; i < blk; ++i)
                    dstR[i] += accR[i] * mGain;
            }
        }

        // Final downcast: the 64-bit accumulator → device-output float
        // buffers. Done in one place at the very end so per-strip /
        // VCA / stream / monitor sums all stayed at double precision.
        // NEON-vectorised on Apple Silicon; scalar fallback elsewhere.
        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr) continue;
            fastaccum::downcastDoubleToFloat (outputs[ch],
                                               outputAccum.getReadPointer (ch),
                                               numSamples);
        }

        // Companion stream feed -- runs at the very end so it captures
        // EVERYTHING the engineer hears on the monitor bus (per-channel
        // routing + stream-bus + monitor sum, all collapsed into the
        // float output buffers by now).
        // ── Real-time click mix ────────────────────────────────────
        // Click runs on the audio thread so a tempo / voice change
        // takes effect on the next beat -- no file reload, no glitch.
        // Routed through the master output pair (masterOutL / masterOutR)
        // so it follows the engineer's monitor-bus choice instead of
        // pinning to hardware outs 0+1.
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
            const int cOutL = juce::jlimit (0, numOutputs - 1,
                                            masterOutL.load (std::memory_order_relaxed));
            const int cOutR = juce::jlimit (0, numOutputs - 1,
                                            masterOutR.load (std::memory_order_relaxed));
            float* L = (cOutL >= 0 && cOutL < numOutputs) ? outputs[cOutL] : nullptr;
            float* R = (cOutR >= 0 && cOutR < numOutputs && cOutR != cOutL) ? outputs[cOutR] : nullptr;
            click.processBlock (L, R, numSamples);
        }

        if (companion != nullptr && companion->isRunning())
        {
            const int sOutL = juce::jlimit (0, numOutputs - 1,
                                            masterOutL.load (std::memory_order_relaxed));
            const int sOutR = juce::jlimit (0, numOutputs - 1,
                                            masterOutR.load (std::memory_order_relaxed));
            const float* L = (sOutL >= 0 && sOutL < numOutputs) ? outputs[sOutL] : nullptr;
            const float* R = (sOutR >= 0 && sOutR < numOutputs) ? outputs[sOutR] : nullptr;
            companion->feedStreamSamples (L, R, numSamples);
        }

        // NDI Audio transmit -- push the master mix onto the LAN as an
        // NDI source. Reads from the monitor-bus output channels so the
        // network feed matches what the engineer hears at the console.
        // Cheap: pushStereo is a memcpy + one libndi call; a no-op when
        // the runtime isn't installed.
        if (ndi.isEnabled() && numOutputs >= 2)
        {
            const int nOutL = juce::jlimit (0, numOutputs - 1,
                                            masterOutL.load (std::memory_order_relaxed));
            const int nOutR = juce::jlimit (0, numOutputs - 1,
                                            masterOutR.load (std::memory_order_relaxed));
            if (outputs[nOutL] != nullptr && outputs[nOutR] != nullptr)
                ndi.pushStereo (outputs[nOutL], outputs[nOutR], numSamples);
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
