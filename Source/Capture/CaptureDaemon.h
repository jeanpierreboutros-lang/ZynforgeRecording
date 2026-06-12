#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "../Audio/MultitrackRecorder.h"
#include "../Network/CaptureLink.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace zynforge::capture
{
    // Phase 1c of the capture-process split: the engine of the headless
    // `zynforge-capture` daemon. Owns the audio device callback + the
    // MultitrackRecorder + a CaptureServer, and maps protocol Commands to
    // recorder actions. CAPTURE-ONLY by design: playback (virtual
    // soundcheck) stays in the GUI process -- the point of the daemon is
    // that a GUI death never stops a take, and playback dying with the GUI
    // is acceptable.
    //
    // Threading: commands are handled inline on the CaptureServer reader
    // thread (exactly one exists at a time), serialised against the status
    // ticker by `commandLock`. The audio callback touches only the
    // recorder's RT-safe path. No message thread required, so the daemon
    // runs as a plain console process and the class is testable headlessly
    // (test mode skips the device; tests feed blocks via processTestBlock,
    // mirroring the GUI suite's CallbackFixture).
    class CaptureDaemon final : public juce::AudioIODeviceCallback
    {
    public:
        CaptureDaemon() = default;
        ~CaptureDaemon() override;

        // Test seam: skip AudioDeviceManager entirely. Call before start().
        void setTestModeNoDevice (bool noDevice) { testMode = noDevice; }

        // Bring up the server (and, outside test mode, the audio device).
        // numInputs requests that many device input channels; the recorder's
        // track count follows SetTrackCount commands (default = numInputs).
        bool start (int port, int numInputs = 32);
        void stop();
        bool isRunning() const noexcept   { return running.load(); }
        int  getPort() const noexcept     { return server.getPort(); }
        bool isRecording() const noexcept { return recorder.isRecording(); }

        MultitrackRecorder& getRecorder() noexcept { return recorder; }

        // Test seams (mirror AudioEngine::prepareForTests + the fixture's
        // input feed): prepare the recorder without a device, then push
        // synthetic input blocks as if the callback fired.
        void prepareForTests (double sampleRate, int blockSize, int numInputs);
        void processTestBlock (const float* const* inputs, int numChannels, int numSamples);

        // juce::AudioIODeviceCallback
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override;
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;

    private:
        void handleCommand (const Command&);
        void statusLoop();
        EngineStatus buildStatus();

        juce::AudioDeviceManager deviceManager;
        MultitrackRecorder       recorder;
        CaptureServer            server;

        std::mutex        commandLock;     // command handling vs status build
        std::thread       statusThread;
        std::atomic<bool> running   { false };
        std::atomic<bool> testMode  { false };
        std::atomic<double> currentSampleRate { 0.0 };
        std::atomic<int>    currentBlockSize  { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureDaemon)
    };
}
