#pragma once

#include <juce_osc/juce_osc.h>

#include <functional>
#include <map>
#include <vector>

namespace zynforge
{
    // Outbound console control for virtual soundcheck -- Behringer X32 /
    // Midas M32 OSC (UDP 10023). Two jobs:
    //
    // 1. SOUNDCHECK <-> STAGE repatch. The #1 friction of virtual
    //    soundcheck is flipping the console's input sources from the
    //    stage preamps to the card returns and back. enterSoundcheck()
    //    first QUERIES the console's four input-routing blocks
    //    (/config/routing/IN/1-8 ... 25-32) and stashes whatever the
    //    show patch is (analog, AES50 stage boxes, anything), then sets
    //    the blocks to CARD1-8..CARD25-32. exitSoundcheck() restores the
    //    stashed patch verbatim -- no assumptions about the rig.
    //
    // 2. Head-amp gain capture / restore. captureGains() polls
    //    /headamp/NNN/gain for the first N head amps; the floats land in
    //    capturedGains() (raw X32 0..1 = -12..+60 dB). restoreGains()
    //    writes them back -- recall the preamps to exactly where they
    //    were the night of the show, so the virtual soundcheck hits the
    //    console at show levels.
    //
    // Transport seam: every outgoing message goes through sendMessage(),
    // which tests intercept via setSendHook(); console replies are
    // simulated with injectReply(). The real path shares one UDP socket
    // between an OSCSender and an OSCReceiver (the X32 replies to the
    // request's source port). Message-thread only, like the rest of the
    // OSC layer.
    class ConsoleLink final : private juce::OSCReceiver::Listener<
                                  juce::OSCReceiver::MessageLoopCallback>
    {
    public:
        ConsoleLink();
        ~ConsoleLink() override;

        bool connect (const juce::String& host, int port = 10023);
        void disconnect();
        bool isConnected() const noexcept { return connected; }
        juce::String getHost() const      { return host; }

        // X32 /config/routing/IN block enum indices (per the public X32
        // OSC protocol): AN1-8..AN25-32 = 0..3, AES50A = 4..9,
        // AES50B = 10..15, CARD1-8..CARD25-32 = 16..19.
        static constexpr int kCardBlockFirst = 16;
        static constexpr int kNumInBlocks    = 4;

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
        static float gainToDb (float v) noexcept { return -12.0f + 72.0f * v; }

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
        static juce::String inBlockAddress (int blockIdx);     // /config/routing/IN/1-8 ...
        static juce::String headampAddress (int headampIdx);   // /headamp/000/gain ...

        juce::DatagramSocket socket { false };
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
