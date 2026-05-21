#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "MultitrackRecorder.h"
#include "SessionPlayer.h"

namespace zynforge
{
    class AudioEngine final : public juce::AudioIODeviceCallback
    {
    public:
        AudioEngine();
        ~AudioEngine() override;

        juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
        MultitrackRecorder&       getRecorder()      noexcept { return recorder; }
        SessionPlayer&            getPlayer()        noexcept { return player; }

        bool startRecording (const juce::File& sessionDir) { return recorder.startRecording (sessionDir); }
        void stopRecording()                               { recorder.stopRecording(); }
        bool isRecording() const noexcept                  { return recorder.isRecording(); }

        int  loadSession (const juce::File& sessionDir)    { return player.loadSession (sessionDir); }
        void startPlayback()                               { player.start(); }
        void stopPlayback()                                { player.stop(); }
        bool isPlaying() const noexcept                    { return player.isPlaying(); }

        // AudioIODeviceCallback
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                               float* const* outputs, int numOutputs,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override;

    private:
        juce::AudioDeviceManager deviceManager;
        MultitrackRecorder       recorder;
        SessionPlayer            player;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
    };
}
