#pragma once

#include <juce_osc/juce_osc.h>

#include "ConsoleProfile.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace zynforge
{
    // Outbound console control for virtual soundcheck. The console-specific
    // behaviour lives in a pluggable `ConsoleProfile` (ConsoleProfile.h), so
    // this class is the transport + state machine, not an X32 hard-wire.
    //
    // For an OSC desk that exposes input routing (the X32 / M32 reference
    // profile), two jobs:
    //
    // 1. SOUNDCHECK <-> STAGE repatch. enterSoundcheck() QUERIES the profile's
    //    input-routing blocks and stashes whatever the show patch is (analog,
    //    AES50 stage boxes, anything), then sets them to the record-card
    //    returns. exitSoundcheck() restores the stash verbatim -- no
    //    assumptions about the rig.
    //
    // 2. Head-amp gain capture / restore -- recall the preamps to exactly
    //    where they were on show night so playback hits the console at show
    //    levels.
    //
    // Large-format desks (DiGiCo / Yamaha / SSL / A&H) carry profiles with
    // canRepatch=false + hasNativeVsc=true: ZynForge records + plays their
    // record card and the console's own Virtual Soundcheck does the repatch.
    //
    // Transport seam: every outgoing message goes through sendMessage(),
    // which tests intercept via setSendHook(); console replies are
    // simulated with injectReply(). The real path shares one UDP socket
    // between an OSCSender and an OSCReceiver (the desk replies to the
    // request's source port). Message-thread only, like the rest of the
    // OSC layer.
    class ConsoleLink final : private juce::OSCReceiver::Listener<
                                  juce::OSCReceiver::MessageLoopCallback>
    {
    public:
        ConsoleLink();
        ~ConsoleLink() override;

        // The active console family. Changing it picks new OSC addresses /
        // port / capabilities. Defaults to the X32 reference profile.
        void                  setProfile (ConsoleProfile::Kind);
        const ConsoleProfile& getProfile() const noexcept { return profile; }

        // 0 port = use the active profile's default port.
        bool connect (const juce::String& host, int port = 0);
        void disconnect();
        bool isConnected() const noexcept { return connected; }
        juce::String getHost() const      { return host; }

        // Patch state machine. Idle -> (query+stash) -> Soundcheck.
        enum class Patch { Unknown, Stage, Soundcheck };
        Patch getPatch() const noexcept { return patch; }

        void enterSoundcheck();   // stash current routing, set CARD blocks
        void exitSoundcheck();    // restore the stashed routing

        // Head-amp gains. capture polls indices [0, n); each reply lands
        // in capturedGains() keyed by head-amp index.
        void captureGains (int numHeadamps);
        void restoreGains();      // write capturedGains back to the desk
        const std::map<int, float>& getCapturedGains() const noexcept { return gains; }
        float gainToDb (float v) const noexcept { return profile.gainToDb (v); }

        // Round-trip the stashed patch + captured gains through the
        // session folder so show-night state survives to VSC day.
        juce::var  toJson() const;
        bool       loadJson (const juce::var&);
        bool       saveTo (const juce::File& sessionDir) const;
        bool       loadFrom (const juce::File& sessionDir);
        static constexpr const char* kStateFileName = "console_state.json";

        // Progress callback (message thread): fired when a routing reply
        // or gain reply arrives, and when enter/exit/restore complete.
        std::function<void (const juce::String& status)> onStatus;

        // ── Test seams ──────────────────────────────────────────────────
        void setSendHook (std::function<void (const juce::OSCMessage&)> hook)
        {
            sendHook = std::move (hook);
            connected = connected || sendHook != nullptr;   // hooked = "connected" for tests
        }
        void injectReply (const juce::OSCMessage& m) { oscMessageReceived (m); }

    private:
        void oscMessageReceived (const juce::OSCMessage&) override;
        void sendMessage (const juce::OSCMessage&);
        juce::String inBlockAddress (int blockIdx) const;      // via active profile
        juce::String headampAddress (int headampIdx) const;    // via active profile

        ConsoleProfile profile { x32Profile() };

        // Recreated on every connect(): juce::DatagramSocket::shutdown()
        // permanently invalidates the handle (handle = -1, never reopened),
        // so a reused socket can't re-bind after a disconnect. A fresh
        // socket per connect makes disconnect -> reconnect work.
        std::unique_ptr<juce::DatagramSocket> socket;
        juce::OSCSender      sender;
        juce::OSCReceiver    receiver;
        bool                 connected { false };
        juce::String         host;

        Patch                patch { Patch::Unknown };
        std::map<int, int>   stagePatch;     // block idx -> routing enum (stashed)
        int                  pendingPatchQueries { 0 };
        std::map<int, float> gains;          // headamp idx -> raw 0..1
        int                  expectedGainReplies { 0 };

        std::function<void (const juce::OSCMessage&)> sendHook;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConsoleLink)
    };
}
