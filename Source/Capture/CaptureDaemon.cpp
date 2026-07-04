#include "CaptureDaemon.h"

namespace zynforge::capture
{
    CaptureDaemon::~CaptureDaemon() { stop(); }

    bool CaptureDaemon::start (int port, int numInputs)
    {
        stop();

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

        recorder.setTrackCount (numInputs);

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
        if (statusThread.joinable()) statusThread.join();
        server.stop();
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
    }

    void CaptureDaemon::processTestBlock (const float* const* inputs, int numChannels, int numSamples)
    {
        recorder.processBlock (inputs, numChannels, numSamples);
    }

    void CaptureDaemon::audioDeviceAboutToStart (juce::AudioIODevice* device)
    {
        const auto sr    = device->getCurrentSampleRate();
        const auto block = device->getCurrentBufferSizeSamples();
        currentSampleRate.store (sr);
        currentBlockSize .store (block);
        recorder.prepare (sr, block, device->getActiveInputChannels().countNumberOfSetBits());
        recorder.setAudioWorkgroup (device->getWorkgroup());
    }

    void CaptureDaemon::audioDeviceStopped()
    {
        recorder.setAudioWorkgroup ({});
    }

    void CaptureDaemon::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                          int numInputChannels,
                                                          float* const* outputChannelData,
                                                          int numOutputChannels,
                                                          int numSamples,
                                                          const juce::AudioIODeviceCallbackContext&)
    {
        recorder.processBlock (inputChannelData, numInputChannels, numSamples);
        // Capture-only: silence any outputs the device forced on us.
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
    }

    void CaptureDaemon::handleCommand (const Command& c)
    {
        const std::lock_guard<std::mutex> g (commandLock);
        switch (c.action)
        {
            case Action::Hello:           // server already replied
            case Action::Ping:
                break;

            case Action::StartRecording:
            {
                Reply r; r.id = c.id;
                const juce::File dir (c.sessionDir);
                if (c.sessionDir.isEmpty())             { r.error = "no sessionDir"; }
                else
                {
                    dir.createDirectory();
                    if (recorder.startRecording (dir))  r.ok = true;
                    else                                r.error = "recorder failed to start";
                }
                server.sendReply (r);
                break;
            }

            case Action::StopRecording:
            {
                recorder.stopRecording();
                Reply r; r.ok = true; r.id = c.id;
                server.sendReply (r);
                break;
            }

            case Action::ArmTrack:
                if (c.trackIndex >= 0 && c.trackIndex < recorder.getNumTracks())
                    recorder.getTrack (c.trackIndex).armed.store (c.boolValue,
                                                                  std::memory_order_relaxed);
                break;

            case Action::SetCaptureFormat:
                recorder.setCaptureFormat ((CaptureFormat) c.intValue);
                break;

            case Action::SetTrackCount:
                recorder.setTrackCount (juce::jlimit (1, 256, c.intValue));
                break;

            case Action::SetSessionDir:
                // Reserved for Phase 1d (pre-arming the session before the
                // start command); StartRecording carries the dir today.
                break;

            case Action::StartPlayback:
            case Action::StopPlayback:
            {
                // Capture-only daemon: playback lives in the GUI process.
                Reply r; r.id = c.id; r.error = "daemon is capture-only; playback is GUI-side";
                server.sendReply (r);
                break;
            }

            case Action::Quit:
            {
                Reply r; r.id = c.id;
                if (recorder.isRecording())
                {
                    // Protect the take: a Quit can't stop a rolling record.
                    r.error = "refusing to quit mid-take; StopRecording first";
                    server.sendReply (r);
                }
                else
                {
                    r.ok = true;
                    // Send the ack BEFORE tripping the quit flag, so the reply
                    // is guaranteed on the wire before the main loop can tear
                    // the server down (which would otherwise race the reply).
                    server.sendReply (r);
                    if (onQuitRequest) onQuitRequest();
                }
                break;
            }
        }
    }

    EngineStatus CaptureDaemon::buildStatus()
    {
        EngineStatus s;
        s.recording      = recorder.isRecording();
        s.elapsedSamples = recorder.getSamplesSinceStart();
        s.sampleRate     = currentSampleRate.load();
        s.blockSize      = currentBlockSize.load();
        s.diskMBPerSec   = recorder.getDiskBytesPerSec() / (1024.0 * 1024.0);
        s.ringFillPct    = recorder.getRingFillPct();
        s.lastWriteMs    = recorder.getLastWriteMs();
        s.missedSamples  = recorder.getMissedSamples();
        s.numTracks      = recorder.getNumTracks();
        s.captureFormat  = (int) recorder.getCaptureFormat();
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
                EngineStatus s;
                {
                    const std::lock_guard<std::mutex> g (commandLock);
                    s = buildStatus();
                }
                server.sendStatus (s);
            }
            for (int i = 0; i < 10 && running.load(); ++i)
                juce::Thread::sleep (10);
        }
    }
}
