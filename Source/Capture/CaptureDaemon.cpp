#include "CaptureDaemon.h"

namespace zynforge::capture
{
    CaptureDaemon::~CaptureDaemon() { stop(); }

    bool CaptureDaemon::start (int port, int numInputs)
    {
        stop();

        // Publish stable logical storage before registration can deliver the
        // first callback. The device may expose fewer physical inputs.
        recorder.setTrackCount (numInputs);

        if (! testMode.load())
        {
            // Inputs only -- the daemon never plays back.
            const auto err = deviceManager.initialiseWithDefaultDevices (numInputs, 0);
            if (err.isNotEmpty())
            {
                juce::Logger::writeToLog ("[capture] device init failed: " + err);
                return false;
            }
            deviceManager.addAudioCallback (this);
        }
        else if (registerCallbackForTests)
            registerCallbackForTests();

        server.onCommand = [this] (const Command& c) { handleCommand (c); };
        if (! server.listen (port))
        {
            juce::Logger::writeToLog ("[capture] listen failed on port " + juce::String (port));
            if (! testMode.load()) deviceManager.removeAudioCallback (this);
            return false;
        }

        running.store (true);
        statusThread = std::thread ([this] { statusLoop(); });
        return true;
    }

    void CaptureDaemon::stop()
    {
        if (! running.exchange (false))
            return;
        // The take outlives everything else in the teardown order: stop the
        // recorder FIRST so files close + the manifest is written, then take
        // down comms + device.
        if (recorder.isRecording()) recorder.stopRecording();
        // Closing the client interrupts a status write stuck on a peer that
        // stopped reading; join only after that write can return.
        server.stop();
        if (statusThread.joinable()) statusThread.join();
        if (! testMode.load())
        {
            deviceManager.removeAudioCallback (this);
            deviceManager.closeAudioDevice();
        }
    }

    void CaptureDaemon::prepareForTests (double sampleRate, int blockSize, int numInputs)
    {
        currentSampleRate.store (sampleRate);
        currentBlockSize .store (blockSize);
        recorder.prepare (sampleRate, blockSize, numInputs);
        deviceAvailable.store (true);
    }

    void CaptureDaemon::processTestBlock (const float* const* inputs, int numChannels, int numSamples)
    {
        recorder.processBlock (inputs, numChannels, numSamples);
    }

    void CaptureDaemon::audioDeviceAboutToStart (juce::AudioIODevice* device)
    {
        // prepare() finalises a running take. Keep the loss visible if the
        // device restarts without delivering audioDeviceStopped first.
        if (recorder.isRecording())
        {
            captureDeviceLost.store (true);
            recorder.markCaptureDeviceLost();
        }
        const auto sr    = device->getCurrentSampleRate();
        const auto block = device->getCurrentBufferSizeSamples();
        currentSampleRate.store (sr);
        currentBlockSize .store (block);
        recorder.prepare (sr, block, recorder.getNumTracks() > 0 ? recorder.getNumTracks()
                                    : device->getActiveInputChannels().countNumberOfSetBits());
        recorder.setAudioWorkgroup (device->getWorkgroup());
        deviceAvailable.store (true);
        audioLoadPct.store (0.0f);
    }

    void CaptureDaemon::audioDeviceStopped()
    {
        deviceAvailable.store (false);
        audioLoadPct.store (0.0f);
        // A stopped callback cannot deliver any more samples. Finalise now so
        // status never continues to claim a healthy, rolling take. Normal
        // shutdown stops the recorder before detaching the callback.
        if (recorder.isRecording())
        {
            const std::lock_guard<std::mutex> g (commandLock);
            if (recorder.isRecording())
            {
                captureDeviceLost.store (true);
                recorder.markCaptureDeviceLost();
                recorder.stopRecording();
            }
        }
        recorder.setAudioWorkgroup ({});
    }

    void CaptureDaemon::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                          int numInputChannels,
                                                          float* const* outputChannelData,
                                                          int numOutputChannels,
                                                          int numSamples,
                                                          const juce::AudioIODeviceCallbackContext&)
    {
        const auto cbStart = juce::Time::getHighResolutionTicks();
        const float* routed[256] {};
        const int count = juce::jmin (256, recorder.getNumTracks());
        for (int ch = 0; ch < count; ++ch)
        {
            int input = recorder.getTrack (ch).inputRouting.load();
            if (input == -2) input = ch;
            if (input >= 0 && input < numInputChannels) routed[ch] = inputChannelData[input];
        }
        recorder.processBlock (routed, count, numSamples);
        // Capture-only: silence any outputs the device forced on us.
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
        const double sr = currentSampleRate.load (std::memory_order_relaxed);
        if (sr > 0.0 && numSamples > 0)
        {
            const auto elapsed = juce::Time::highResolutionTicksToSeconds (
                juce::Time::getHighResolutionTicks() - cbStart);
            const float pct = (float) juce::jlimit (0.0, 100.0,
                                                    elapsed * sr * 100.0 / numSamples);
            const float prev = audioLoadPct.load (std::memory_order_relaxed);
            audioLoadPct.store (pct > prev ? pct : prev * 0.85f + pct * 0.15f,
                                std::memory_order_relaxed);
        }
    }

    void CaptureDaemon::handleCommand (const Command& c)
    {
        std::unique_lock<std::mutex> g (commandLock);
        const auto sendReplyUnlocked = [this, &g] (const Reply& reply)
        {
            g.unlock();
            server.sendReply (reply);
            g.lock();
        };
        const auto sendStatusUnlocked = [this, &g] (const EngineStatus& status)
        {
            g.unlock();
            server.sendStatus (status);
            g.lock();
        };
        switch (c.action)
        {
            case Action::Hello:           // server already replied
            case Action::Ping:
                break;

            case Action::StartRecording:
            {
                Reply r; r.id = c.id;
                const juce::File dir (c.sessionDir);
                if (recorder.isRecording())              { r.error = "capture is already recording"; }
                else if (c.sessionDir.isEmpty())         { r.error = "no sessionDir"; }
                else if (! testMode.load() && ! deviceAvailable.load())
                    { r.error = "capture audio device is stopped"; }
                else
                {
                    dir.createDirectory();
                    // Continue into a new part even after reattaching/restarting
                    // the GUI. Existing takes must never be truncated.
                    const auto audio = dir.getChildFile ("Audio Files");
                    for (const auto& file : audio.findChildFiles (juce::File::findFiles, false, "Track_*"))
                    {
                        if (file.getFileName().containsIgnoreCase (".punchbase")) continue;
                        const auto number = file.getFileNameWithoutExtension().substring (6);
                        if (number.isNotEmpty() && number.containsOnly ("0123456789"))
                        { recorder.armContinue (0); break; }
                    }
                    if (recorder.startRecording (dir))
                    { captureDeviceLost.store (false); r.ok = true; }
                    else r.error = recorder.getStartError().isNotEmpty()
                                     ? recorder.getStartError() : "recorder failed to start";
                }
                sendStatusUnlocked (buildStatus());
                sendReplyUnlocked (r);
                break;
            }

            case Action::StopRecording:
            {
                recorder.stopRecording();
                const bool clean = ! recorder.hasPrimaryFailed()
                                && ! captureDeviceLost.load()
                                && ! recorder.hasBackupFailed()
                                && ! recorder.anyMirrorFailed()
                                && recorder.getMirrorsSkippedAtStart() == 0
                                && ! recorder.hasRecoveryMarkerFailed()
                                && ! recorder.hasReportWriteFailed();
                Reply r; r.ok = clean; r.completed = true; r.id = c.id;
                if (! clean)
                {
                    juce::StringArray failures;
                    if (recorder.hasPrimaryFailed()) failures.add ("primary audio write failed");
                    if (captureDeviceLost.load()) failures.add ("capture audio device stopped during take");
                    if (recorder.hasBackupFailed()) failures.add ("backup audio write failed");
                    if (recorder.anyMirrorFailed()) failures.add ("mirror audio write failed");
                    if (recorder.getMirrorsSkippedAtStart() > 0) failures.add ("configured mirror did not open");
                    if (recorder.hasRecoveryMarkerFailed()) failures.add ("recovery marker failed");
                    if (recorder.hasReportWriteFailed()) failures.add ("integrity report failed");
                    r.error = "recording stopped, but finalisation failed: " + failures.joinIntoString (", ");
                }
                sendStatusUnlocked (buildStatus());
                sendReplyUnlocked (r);
                break;
            }

            case Action::ArmTrack:
                if (! recorder.isRecording() && c.trackIndex >= 0 && c.trackIndex < recorder.getNumTracks())
                    recorder.getTrack (c.trackIndex).armed.store (c.boolValue,
                                                                  std::memory_order_relaxed);
                break;

            case Action::SetCaptureFormat:
                recorder.setCaptureFormat ((CaptureFormat) c.intValue);
                break;

            case Action::SetTrackCount:
            {
                Reply r; r.id = c.id;
                if (c.intValue == recorder.getNumTracks())
                { r.ok = true; sendReplyUnlocked (r); break; }
                if (recorder.isRecording())
                {
                    // setTrackCount is a no-op while recording anyway; say so
                    // rather than silently ignoring the command.
                    r.error = "refusing to resize tracks mid-take";
                    sendReplyUnlocked (r);
                    break;
                }
                // setTrackCount ADDS/REMOVES entries in the recorder's tracks +
                // fifos vectors. We're on the socket reader thread, and the
                // CoreAudio callback reads those same vectors every block --
                // resizing under it is a use-after-free on the audio thread.
                // Detach the callback for the resize, exactly like
                // AudioEngine::setStripCount does on the GUI side.
                // (removeAudioCallback blocks until any in-flight callback
                // returns, so after it we're the sole owner.)
                if (! testMode.load()) deviceManager.removeAudioCallback (this);
                recorder.setTrackCount (juce::jlimit (1, 256, c.intValue));
                if (! testMode.load()) deviceManager.addAudioCallback (this);
                r.ok = true;
                sendReplyUnlocked (r);
                break;
            }

            case Action::SetSessionDir:
                // Reserved for Phase 1d (pre-arming the session before the
                // start command); StartRecording carries the dir today.
                break;

            case Action::ConfigureCapture:
            {
                Reply r; r.id = c.id;
                if (recorder.isRecording()) { r.error = "stop recording before configuration"; sendReplyUnlocked (r); break; }
                const auto& cfg = c.configuration;
                auto* tracks = cfg["tracks"].getArray();
                if (tracks == nullptr || tracks->isEmpty() || tracks->size() > 256)
                { r.error = "invalid track configuration"; sendReplyUnlocked (r); break; }
                if (! testMode.load())
                {
                    auto xml = juce::parseXML (cfg["deviceState"].toString());
                    if (xml == nullptr) { r.error = "missing device configuration"; sendReplyUnlocked (r); break; }
                    deviceManager.removeAudioCallback (this);
                    const auto error = deviceManager.initialise (256, 0, xml.get(), false);
                    if (error.isNotEmpty())
                    {
                        // Keep the previously usable callback alive after a
                        // rejected reconfiguration. The device manager may
                        // have retained/recovered its old device.
                        deviceManager.addAudioCallback (this);
                        r.error = error; sendReplyUnlocked (r); break;
                    }
                    auto* device = deviceManager.getCurrentAudioDevice();
                    if (device == nullptr || std::abs (device->getCurrentSampleRate() - (double) cfg["sampleRate"]) > 0.5)
                    {
                        deviceManager.addAudioCallback (this);
                        r.error = "requested capture sample rate unavailable"; sendReplyUnlocked (r); break;
                    }
                }
                recorder.setTrackCount (tracks->size());
                for (int i = 0; i < tracks->size(); ++i)
                {
                    auto& t = recorder.getTrack (i); const auto& v = (*tracks)[i];
                    t.setNameThreadSafe (v["name"].toString()); t.stripId = v["uid"].toString();
                    t.inputRouting.store ((int) v["input"]); t.armed.store ((bool) v["armed"]);
                    t.isStereo.store ((bool) v["stereo"]); t.isBus.store ((bool) v["bus"]);
                }
                recorder.setPreRollSeconds (juce::jlimit (0, 30, (int) cfg["preRoll"]));
                recorder.setBackupDirectory (juce::File (cfg["backup"].toString()));
                recorder.setBackupCaptureFormat ((CaptureFormat) (int) cfg["backupFormat"]);
                std::vector<MultitrackRecorder::MirrorConfig> mirrors;
                if (auto* ms = cfg["mirrors"].getArray())
                    for (const auto& m : *ms)
                        mirrors.push_back ({ juce::File (m["root"].toString()), (CaptureFormat) (int) m["format"] });
                recorder.setMirrors (mirrors);
                if (! testMode.load()) deviceManager.addAudioCallback (this);
                r.ok = true; sendReplyUnlocked (r); break;
            }

            case Action::StartPlayback:
            case Action::StopPlayback:
            {
                // Capture-only daemon: playback lives in the GUI process.
                Reply r; r.id = c.id; r.error = "daemon is capture-only; playback is GUI-side";
                sendReplyUnlocked (r);
                break;
            }

            case Action::Quit:
            {
                Reply r; r.id = c.id;
                if (recorder.isRecording())
                {
                    // Protect the take: a Quit can't stop a rolling record.
                    r.error = "refusing to quit mid-take; StopRecording first";
                    sendReplyUnlocked (r);
                }
                else
                {
                    r.ok = true;
                    // Send the ack BEFORE tripping the quit flag, so the reply
                    // is guaranteed on the wire before the main loop can tear
                    // the server down (which would otherwise race the reply).
                    sendReplyUnlocked (r);
                    if (onQuitRequest) onQuitRequest();
                }
                break;
            }
        }
    }

    EngineStatus CaptureDaemon::buildStatus()
    {
        const auto nowMs = juce::Time::getMillisecondCounterHiRes();
        if (! recorder.isRecording())
        {
            recorder.updateDiskHealth (0);
            lastDiskHealthUpdateMs = nowMs;
        }
        else if (nowMs - lastDiskHealthUpdateMs >= 1000.0)
        {
            recorder.updateDiskHealth (recorder.estimateBytesPerSecondForArmedTracks());
            lastDiskHealthUpdateMs = nowMs;
        }
        EngineStatus s;
        s.recording      = recorder.isRecording();
        s.sessionPath    = recorder.getActiveSessionDir().getFullPathName();
        s.positionSamples = recorder.getRecordTimelineSamples();
        s.elapsedSamples = recorder.getSamplesSinceStart();
        if (s.recording)
            s.minutesRemaining = recorder.estimateMinutesRemaining (
                recorder.getActiveSessionDir(), recorder.getBackupDirectory());
        s.sampleRate     = currentSampleRate.load();
        s.blockSize      = currentBlockSize.load();
        s.audioLoadPct    = audioLoadPct.load();
        s.captureDeviceLost = captureDeviceLost.load();
        s.diskMBPerSec   = recorder.getDiskBytesPerSec() / (1024.0 * 1024.0);
        s.ringFillPct    = recorder.getRingFillPct();
        s.lastWriteMs    = recorder.getLastWriteMs();
        s.missedSamples  = recorder.getMissedSamples();
        s.numTracks      = recorder.getNumTracks();
        s.captureFormat  = (int) recorder.getCaptureFormat();
        s.backupActive   = recorder.isBackupActive();
        s.primaryFailed  = recorder.hasPrimaryFailed();
        s.backupFailed   = recorder.hasBackupFailed();
        s.mirrorFailed   = recorder.anyMirrorFailed();
        s.mirrorsSkipped = recorder.getMirrorsSkippedAtStart();
        s.recoveryMarkerFailed = recorder.hasRecoveryMarkerFailed();
        s.reportWriteFailed = recorder.hasReportWriteFailed();
        s.diskStruggling = recorder.isDiskStruggling();
        s.tracks.reserve ((size_t) s.numTracks);
        for (int i = 0; i < s.numTracks; ++i)
        {
            auto& t = recorder.getTrack (i);
            TrackStatus ts;
            ts.name  = t.name;
            ts.peak  = t.peak .load (std::memory_order_relaxed);
            ts.rms   = t.rms  .load (std::memory_order_relaxed);
            ts.armed = t.armed.load (std::memory_order_relaxed);
            if (ts.armed) ++s.armedTracks;
            s.tracks.push_back (std::move (ts));
        }
        return s;
    }

    void CaptureDaemon::statusLoop()
    {
        // 10 Hz status push while a client is attached. Plain thread -- the
        // daemon has no message loop to depend on.
        while (running.load())
        {
            if (server.hasClient())
            {
                EngineStatus snapshot;
                {
                    const std::lock_guard<std::mutex> g (commandLock);
                    snapshot = buildStatus();
                }
                // Socket writes can wait on a connected peer that stopped
                // reading. Never hold the command lock across that wait: STOP
                // must still be able to finalise a rolling take.
                server.sendStatus (snapshot);
            }
            for (int i = 0; i < 10 && running.load(); ++i)
                juce::Thread::sleep (10);
        }
    }
}
