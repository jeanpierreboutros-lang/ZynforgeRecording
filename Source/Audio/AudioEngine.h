#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Markers.h"
#include "MultitrackRecorder.h"
#include "SessionPlayer.h"
#include "StripColours.h"

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

        bool startRecording (const juce::File& sessionDir);
        void stopRecording()                               { recorder.stopRecording(); }
        bool isRecording() const noexcept                  { return recorder.isRecording(); }

        int  loadSession (const juce::File& sessionDir);
        void startPlayback()                               { player.start(); }
        void stopPlayback()                                { player.stop(); }
        bool isPlaying() const noexcept                    { return player.isPlaying(); }

        MarkersManager& getMarkers() noexcept              { return markers; }
        StripColours&   getStripColours() noexcept         { return stripColours; }

        // Updates TrackState colour (atomic) AND persists via StripColours.
        // Pass an empty colour (alpha == 0) to revert to the default.
        void setTrackColour (int channelIndex, juce::Colour);

        // Phase correlation between two input channels (live, smoothed).
        // Channels are 1-based for display, stored 0-based.
        void setPhasePair (int leftCh1Based, int rightCh1Based) noexcept;
        int  getPhaseLeftChannel()  const noexcept { return phaseLeft .load (std::memory_order_relaxed) + 1; }
        int  getPhaseRightChannel() const noexcept { return phaseRight.load (std::memory_order_relaxed) + 1; }
        float getPhaseCorrelation() const noexcept { return phaseCorrelation.load (std::memory_order_relaxed); }

        // Drops a marker at the current record or playback position.
        // Returns the new marker count, or -1 if no session active.
        int dropMarkerAtCurrentPosition();

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
        MarkersManager           markers;
        StripColours             stripColours;

        std::atomic<int>   phaseLeft         { 0 };  // 0-based
        std::atomic<int>   phaseRight        { 1 };
        std::atomic<float> phaseCorrelation  { 0.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
    };
}
