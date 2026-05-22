#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Markers.h"
#include "MultitrackRecorder.h"
#include "SessionPlayer.h"
#include "StripColours.h"
#include "StripGains.h"
#include "StripNames.h"
#include "StripRouting.h"

#include <memory>

namespace zynforge
{
    class OscRemote;

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

        // Updates TrackState::name AND persists via StripNames.
        // Empty string reverts to the default "In N" label.
        void setTrackName (int channelIndex, const juce::String&);

        // Per-channel playback gain (dB) + pan (-1..+1). Both persist.
        void  setTrackGainDb (int channelIndex, float dB);
        void  setTrackPan    (int channelIndex, float pan);

        // Routing. -1 = unrouted; values clamped to current device's range.
        void  setTrackInputRouting  (int channelIndex, int deviceCh);
        void  setTrackOutputRouting (int channelIndex, int deviceCh);
        int   getCurrentDeviceInputCount()  const;
        int   getCurrentDeviceOutputCount() const;

        // Dedicated streaming stereo bus. setStreamOutputs(-1, -1) disables.
        // Tracks with TrackState::streamSend=true mix into these outputs.
        void  setStreamOutputs (int leftCh, int rightCh);
        int   getStreamOutputL() const noexcept { return streamOutL.load (std::memory_order_relaxed); }
        int   getStreamOutputR() const noexcept { return streamOutR.load (std::memory_order_relaxed); }
        void  setTrackStream   (int channelIndex, bool enabled);

        // OSC remote: starts/stops a juce::OSCReceiver bound to UDP port,
        // with a dialect parser for DiGiCo / A&H / SSL / Yamaha consoles
        // plus a generic /zynforge/* schema for tablet apps.
        bool  startOsc (int udpPort, int dialectIndex);
        void  stopOsc();
        bool  isOscListening() const;
        int   getOscPort() const;
        int   getOscDialect() const;

        // Returns recording dir if recording, else loaded playback session,
        // else an empty File.
        juce::File getActiveSessionDir() const;

        // Forwards to MultitrackRecorder.
        void setBackupDirectory (const juce::File& dir) { recorder.setBackupDirectory (dir); }
        juce::File getBackupDirectory() const           { return recorder.getBackupDirectory(); }
        bool       isBackupActive() const noexcept       { return recorder.isBackupActive(); }
        bool       hasBackupFailed() const noexcept      { return recorder.hasBackupFailed(); }

        // Scans the standard Sessions root and returns any directories that
        // still have a `recording.session` marker (i.e. were not stopped
        // cleanly).
        static juce::Array<juce::File> findIncompleteSessions (const juce::File& sessionsRoot);

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
        StripNames               stripNames;
        StripGains               stripGains;
        StripRouting             stripRouting;

        std::atomic<int>   phaseLeft         { 0 };  // 0-based
        std::atomic<int>   phaseRight        { 1 };
        std::atomic<float> phaseCorrelation  { 0.0f };

        std::atomic<int> streamOutL { -1 };
        std::atomic<int> streamOutR { -1 };

        std::unique_ptr<OscRemote> osc;

        // Audio-thread scratch for routed VSC playback: track i fills
        // channel i, then engine copies into the real device output that
        // strip i is routed to.
        juce::AudioBuffer<float> playerScratch { 32, 4096 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
    };
}
