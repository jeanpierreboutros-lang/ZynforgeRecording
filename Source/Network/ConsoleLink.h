#pragma once

#include <juce_osc/juce_osc.h>

#include "ConsoleProfile.h"
#include "ConsoleTransports.h"

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
    class ConsoleLink final : private juce::Timer
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
        // True only when the LAST captureGains() run received every head-amp
        // reply it asked for. A timed-out capture leaves a partial map behind;
        // persisting that as if it were the show's gains would restore only
        // some channels and silently leave the rest at soundcheck settings.
        // A capture loaded from console_state.json counts as complete.
        bool isGainCaptureComplete() const noexcept { return gainCaptureComplete; }
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

        // ── READ TIER ───────────────────────────────────────────────────
        // The two things a RECORDER actually wants from a console, both
        // read-only and therefore safe on any desk verified or not.
        //
        // A channel name arrived (1-based console channel). The host renames
        // the matching strip so takes land labelled.
        std::function<void (int ch1, const juce::String& name)> onChannelName;
        // A scene / snapshot was recalled. The host drops a marker, which is
        // what makes a show navigable the next morning.
        std::function<void (int sceneIdx, const juce::String& name)> onSceneRecalled;

        // Ask the desk for every channel name in [1, count]. No-op unless the
        // profile declares canReadNames.
        void requestChannelNames (int count);

        // ── HANDSHAKE / SELF-VERIFICATION ───────────────────────────────
        // On connect the link sends the dialect's probe. A parseable reply
        // means the address model matches what we think this desk is, and the
        // WRITE tier unlocks. No reply (or an unparseable one) leaves the link
        // in READ-ONLY mode -- which is why a dialect written from published
        // docs, with no hardware to test against, is safe to ship: a wrong
        // guess degrades to "no console control", never to writing garbage.
        bool isDialectConfirmed() const noexcept { return dialectConfirmed; }
        // True when writes are permitted: the profile claims the capability AND
        // either the dialect is verified-by-hand or the handshake confirmed it.
        bool canWrite() const noexcept
        {
            return profile.dialectTrusted || dialectConfirmed;
        }

        // ── Test seams ──────────────────────────────────────────────────
        // Intercept every outgoing message and play the console's side, with
        // no socket. The OSC overloads are kept so the existing X32 tests read
        // unchanged; they translate to/from ConsoleMessage.
        void setSendHook (std::function<void (const juce::OSCMessage&)> hook);
        void setMessageHook (std::function<void (const ConsoleMessage&)> hook)
        {
            msgHook = std::move (hook);
            connected = connected || msgHook != nullptr;   // hooked = "connected" for tests
        }
        void injectReply (const juce::OSCMessage& m);
        void injectReply (const ConsoleMessage& m) { handleMessage (m); }
        // Pretend the handshake landed, for tests that exercise the write tier.
        void confirmDialectForTests() noexcept { dialectConfirmed = true; }

    private:
        void handleMessage (const ConsoleMessage&);
        // ONE repeating heartbeat, two jobs.
        //
        // 1. SUBSCRIPTION RENEWAL. The X32's /xremote subscription expires after
        //    about 10 s. subscribe() was sent once on connect and never again,
        //    so the desk stopped pushing ~10 s in and the entire pushed read
        //    tier -- scene recalls dropping markers, live name changes -- went
        //    silently dead for the rest of the show. The dialect's own comment
        //    said it had to be re-sent periodically; nothing did.
        // 2. REPLY TIMEOUT. A query (enterSoundcheck / captureGains) waits for N
        //    replies; a lost one would hang the op forever ("Reading console
        //    patch..."). Deadlines are timestamps checked on each beat rather
        //    than a one-shot timer, so the two jobs can share the timer.
        void timerCallback() override;
        void startHeartbeat();
        static constexpr int kHeartbeatMs      = 1000;
        static constexpr int kResubscribeMs    = 5000;   // < the X32's ~10 s expiry
        static constexpr int kPatchTimeoutMs   = 1200;
        static constexpr int kGainTimeoutMs    = 1500;
        void sendMessage (const ConsoleMessage&);
        // Refuse a write the profile/handshake doesn't permit, with a status
        // line explaining why rather than silently doing nothing.
        bool guardWrite (const char* what);

        ConsoleProfile profile { x32Profile() };

        // Built per connect() from the profile's transport kind.
        std::unique_ptr<ConsoleTransport> transport;
        bool                 connected { false };
        bool                 dialectConfirmed { false };
        juce::String         host;

        Patch                patch { Patch::Unknown };
        std::map<int, int>   stagePatch;     // block idx -> routing enum (stashed)
        int                  pendingPatchQueries { 0 };
        std::map<int, float> gains;          // headamp idx -> raw 0..1
        int                  expectedGainReplies { 0 };
        bool                 gainCaptureComplete { false };
        // Deadlines (ms since start, 0 = inactive) rather than one-shot timers.
        juce::uint32         patchDeadlineMs { 0 };
        juce::uint32         gainDeadlineMs  { 0 };
        juce::uint32         nextSubscribeMs { 0 };

        std::function<void (const ConsoleMessage&)> msgHook;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConsoleLink)
    };
}
