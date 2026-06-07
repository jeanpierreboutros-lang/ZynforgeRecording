#pragma once

#include <juce_osc/juce_osc.h>

namespace zynforge
{
    class AudioEngine;

    // OSC receiver wired into AudioEngine. Listens on a configurable UDP
    // port and parses messages from any of the supported console dialects
    // -- DiGiCo (Quantum / SD OSC), Allen & Heath (Avantis / SQ),
    // Solid State Logic (SSL Live), Yamaha (DM7, RIVAGE PM), and a
    // generic /zynforge/* path for tablet apps / TouchOSC / homebrew
    // remotes.
    //
    // All callbacks fire on the message thread so AudioEngine setters
    // can be called safely.
    class OscRemote : private juce::OSCReceiver,
                      private juce::OSCReceiver::Listener<
                                  juce::OSCReceiver::MessageLoopCallback>
    {
    public:
        enum class Dialect
        {
            Generic,      // /zynforge/*
            DiGiCo,       // /Console/*
            AllenHeath,   // /sq/* (Avantis / SQ)
            SSL,          // /sslnet/* (SSL Live)
            Yamaha        // /Yamaha/* / /RIVAGE/* (DM7, RIVAGE PM)
        };

        explicit OscRemote (AudioEngine&);
        ~OscRemote() override;

        bool start (int udpPort);
        void stop();

        bool   isListening() const noexcept { return listening; }
        int    getPort()     const noexcept { return port; }
        Dialect getDialect() const noexcept { return dialect; }
        void    setDialect (Dialect d);

        // Test seam: route a message synchronously, exactly as the socket
        // path would (parse + dialect dispatch + engine setters). Lets unit
        // tests exercise the full OSC wiring without a UDP socket or the
        // message loop.
        void dispatchForTest (const juce::OSCMessage& m) { oscMessageReceived (m); }

    private:
        void oscMessageReceived (const juce::OSCMessage&) override;

        // Dialect dispatchers -- return true if the message was handled.
        bool handleGeneric    (const juce::OSCMessage&);
        bool handleDiGiCo     (const juce::OSCMessage&);
        bool handleAllenHeath (const juce::OSCMessage&);
        bool handleSSL        (const juce::OSCMessage&);
        bool handleYamaha     (const juce::OSCMessage&);

        // Helpers that touch the engine.
        void recordStart();
        void recordStop();
        void playStart();
        void playStop();
        void dropMarker (const juce::String& name);
        void dropSceneMarker (int sceneNumber);
        void setChannelName   (int oneBasedIndex, const juce::String&);
        void setChannelMute   (int oneBasedIndex, bool muted);
        void setChannelArm    (int oneBasedIndex, bool armed);
        void setChannelColour (int oneBasedIndex, juce::uint32 rgb);
        void setChannelGain   (int oneBasedIndex, float dB);   // console preamp/trim → trim-follow

        // Shared channel-op dispatch -- returns true if the {ch1, key}
        // pair matched a known action (name / mute / arm / colour).
        bool dispatchChannelOp (int oneBasedIndex,
                                const juce::String& key,
                                const juce::OSCMessage&);

        AudioEngine& engine;
        Dialect      dialect    { Dialect::Generic };
        int          port       { 8000 };
        bool         listening  { false };
    };
}
