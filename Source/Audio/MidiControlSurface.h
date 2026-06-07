#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "McuProtocol.h"

namespace zynforge
{
    class AudioEngine;

    // Mackie-Control-compatible MIDI control surface (also covers FaderPort in
    // MCU mode). Bidirectional: the surface drives the app (faders -> gain,
    // transport, mute/solo/arm) and the app echoes state back (motor faders,
    // scribble-strip names, button LEDs). v1 = the first 8 channels (bank 0).
    //
    // Channel state (gain/mute/solo/arm) is written as plain atomic stores, so
    // it's safe straight off the MIDI thread; transport changes are marshalled
    // to the message thread.
    class MidiControlSurface final : private juce::MidiInputCallback,
                                     private juce::Timer
    {
    public:
        explicit MidiControlSurface (AudioEngine& e);
        ~MidiControlSurface() override;

        bool start (const juce::String& inputName, const juce::String& outputName);
        void stop();
        bool isActive() const noexcept { return input != nullptr; }

    private:
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
        void timerCallback() override;   // push fader / name / LED state to the surface

        AudioEngine& engine;
        std::unique_ptr<juce::MidiInput>  input;
        std::unique_ptr<juce::MidiOutput> output;

        static constexpr int kStrips = 8;
        std::atomic<int>  bankOffset   { 0 };       // first engine channel on fader 0
        std::atomic<bool> forceRefresh { false };   // full re-push after a bank move
        float        lastFaderDb [kStrips];
        bool         lastMute    [kStrips];
        bool         lastSolo    [kStrips];
        bool         lastArm     [kStrips];
        int          lastMeter   [kStrips];
        float        lastPan     [kStrips];
        juce::String lastName    [kStrips];
        bool         lastPlaying { false };
        bool         lastRecording { false };
    };
}
