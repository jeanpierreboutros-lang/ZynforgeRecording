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
    //    assumptions about the rig -- and reads it back, because only the
    //    desk can say it landed.
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

        // 0 port = use the active profile's default port. `host` is a numeric
        // IPv4 address; anything else (a name, a typo) is refused.
        bool connect (const juce::String& host, int port = 0);
        void disconnect();
        bool isConnected() const noexcept { return connected; }
        juce::String getHost() const      { return host; }
        // The desk dropped a TCP link and the watchdog closed it. Stands until
        // a link is made again, so the loss is not one status line that the
        // next one covers: names and scene markers stopped with it.
        bool wasLinkLost() const noexcept   { return lostHost.isNotEmpty(); }
        juce::String getLostHost() const    { return lostHost; }

        // isConnected says a socket is open, and on UDP that holds from connect
        // to disconnect with nobody there. Only a reply to the dialect's probe
        // says a desk is. A desk that pushes but doesn't answer the probe stays
        // NotAnswered rather than falling Silent between its pushes, and a
        // dialect with no probe can't tell at all.
        enum class Health { CannotTell, NotAnswered, Answering, Silent };
        Health getHealth() const;
        juce::String healthText() const;

        // Patch state machine. Idle -> (query+stash) -> Soundcheck -> (restore,
        // read back) -> Stage. Where the desk was last known to be: closing the
        // link or changing session moves no desk, so neither changes this. A
        // session file's word stands until the desk has been asked.
        enum class Patch { Unknown, Stage, Soundcheck };
        Patch getPatch() const noexcept { return patch; }
        // The one definition behind the chip, the menu and pre-flight. A desk
        // this app cannot repatch has no soundcheck state, whatever a file says:
        // its own Virtual Soundcheck can't be seen from here.
        bool isOnSoundcheckPatch() const noexcept
        { return profile.canRepatch && patch == Patch::Soundcheck && hasStagePatch(); }
        bool hasStagePatch() const noexcept
        { return profile.numInBlocks > 0 && (int) stagePatch.size() == profile.numInBlocks; }
        // The same fact for a screen, including what can't be known.
        juce::String deskPatchText() const;

        void enterSoundcheck();   // stash current routing, set CARD blocks
        void exitSoundcheck();    // restore the stashed routing, then read it back
        // Asks the desk where its inputs are and writes nothing to it. This is
        // the way out when a desk was put back by hand: the state follows the
        // answer, and the routing it has now is left alone.
        void checkDeskPatch();

        // Head-amp gains. capture polls indices [0, n); each reply lands
        // in capturedGains() keyed by head-amp index.
        void captureGains (int numHeadamps);
        void restoreGains();      // write capturedGains back, then read them back
        const std::map<int, float>& getCapturedGains() const noexcept { return gains; }
        // True only when the LAST captureGains() run received every head-amp
        // reply it asked for. A timed-out capture leaves a partial map behind;
        // persisting that as if it were the show's gains would restore only
        // some channels and silently leave the rest at soundcheck settings.
        // A capture loaded from console_state.json counts as complete.
        bool isGainCaptureComplete() const noexcept { return gainCaptureComplete; }
        // A capture ended: every reply in (true), or its deadline passed
        // (false). Message thread. This, not a guess at how long the desk
        // takes, is when the host saves the gains.
        std::function<void (bool complete)> onGainCaptureFinished;
        float gainToDb (float v) const noexcept { return profile.gainToDb (v); }

        // Round-trip the stashed patch + captured gains through the
        // session folder so show-night state survives to VSC day.
        juce::var  toJson() const;
        bool       loadJson (const juce::var&);
        bool       saveTo (const juce::File& sessionDir) const;
        bool       loadFrom (const juce::File& sessionDir);
        static constexpr const char* kStateFileName = "console_state.json";

        // The session this state belongs to. Called for every session swap,
        // however it came about, and before any action on the desk: gains and a
        // saved patch from another show must never reach this one. An empty
        // file means no session.
        // With mayReadDisk false (a take is on disk, and this runs on the message
        // thread) the new session's saved state is read by a later call.
        void       setSessionDir (const juce::File&, bool mayReadDisk = true);
        // The state file written off this thread, for mid-take saves.
        void       saveInBackground (const juce::File& sessionDir) const;

        // Progress callback (message thread): fired when a routing reply
        // or gain reply arrives, and when enter/exit/restore complete.
        std::function<void (const juce::String& status)> onStatus;

        // True from RECORD until the take is safe. Nothing moves the desk then,
        // whichever reply arrives, and this link stays off the session's drive.
        std::function<bool()> takeHoldsDesk;

        // ── READ TIER ───────────────────────────────────────────────────
        // The two things a RECORDER actually wants from a console, both
        // read-only and therefore safe on any desk verified or not.
        //
        // A channel name arrived (1-based console channel). The host renames
        // the matching strip so takes land labelled.
        std::function<void (int ch1, const juce::String& name)> onChannelName;
        // A scene / snapshot was recalled. What that does is one shared rule:
        // RemoteControl::onConsoleScene.
        std::function<void (int sceneIdx, const juce::String& name)> onSceneRecalled;
        // The desk says this odd channel and the next one are a stereo pair.
        std::function<void (int leftCh1, bool linked)> onStereoLink;

        // Ask the desk for every channel name in [1, count]. No-op unless the
        // profile declares canReadNames.
        void requestChannelNames (int count);
        // Ask for every pair's link state, returning how many were asked. 0
        // unless the profile declares canReadStereoLinks.
        int requestStereoLinks();

        // Every message in and out, one formatted line each, for the debug log.
        std::function<void (const juce::String& line)> onTraffic;

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
        void injectReply (const ConsoleMessage& m) { logInbound (m); handleMessage (m); }
        // Pretend the handshake landed, for tests that exercise the write tier.
        void confirmDialectForTests() noexcept { dialectConfirmed = true; }

    private:
        bool stateLoadPending { false };
        friend class NetworkFixTests;
        friend class NetworkStateTests;
        void handleMessage (const ConsoleMessage&);
        // ONE repeating heartbeat; it runs while the link is up.
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
        // 3. LINK WATCHDOG. A TCP transport (Yamaha SCP, A&H MIDI) notices the
        //    desk dropping the connection; each beat asks it, and a dead link
        //    is reported and disconnected instead of showing as up all show.
        // 4. CLAIM CHECK. A SOUNDCHECK that came from a file, or that a link
        //    drop left standing, is asked about once the link is up.
        // 5. IS ANYONE THERE. The probe goes out again every few seconds, because
        //    nothing else makes every dialect answer. A desk that stops is
        //    reported once, and again when it comes back.
        void timerCallback() override;
        void startHeartbeat();
        // Every read of the input blocks collects into deskRead, so a failed
        // one leaves the saved patch and the state as they were.
        enum class PatchRead { None, Stash, Restore, Check };
        void startPatchRead (PatchRead);
        void finishPatchRead();
        void patchReadTimedOut();
        void reportHealthChange (juce::int64 now);
        void finishPatchStash();
        void finishPatchRestore();
        void finishPatchCheck();
        void finishGainCheck();
        void cancelDeskReads();
        void loadSessionState (const juce::File& dir);
        bool persistPatch();
        bool takeIsHoldingDesk() const { return takeHoldsDesk != nullptr && takeHoldsDesk(); }
        bool isRoutingValue (int value) const noexcept
        { return value >= 0 && value < profile.cardBlockFirst + profile.numInBlocks; }
        static constexpr int kHeartbeatMs      = 1000;
        static constexpr int kResubscribeMs    = 5000;   // < the X32's ~10 s expiry
        static constexpr int kProbeMs          = 4000;
        static constexpr int kDeskSilentMs     = 13000;  // three probes, on a network that drops some
        static constexpr int kPatchTimeoutMs   = 1200;
        static constexpr int kGainTimeoutMs    = 1500;
        static constexpr float kGainMatchDb    = 0.5f;   // one X32 head-amp step
        void sendMessage (const ConsoleMessage&);
        void logInbound (const ConsoleMessage&);
        // Refuse a write the profile/handshake doesn't permit, with a status
        // line explaining why rather than silently doing nothing.
        bool guardWrite (const char* what);
        // The same, for a write that changes the desk rather than one that
        // feeds a later write: those wait for the take.
        bool guardDeskChange (const char* what);

        ConsoleProfile profile { x32Profile() };

        // Built per connect() from the profile's transport kind.
        std::unique_ptr<ConsoleTransport> transport;
        bool                 connected { false };
        bool                 dialectConfirmed { false };
        juce::String         host;
        juce::String         lostHost;     // see wasLinkLost
        int                  connectedPort { 0 };

        juce::File           sessionDir;
        Patch                patch { Patch::Unknown };
        std::map<int, int>   stagePatch;     // block idx -> routing enum (stashed)
        PatchRead            patchRead { PatchRead::None };
        std::map<int, int>   deskRead;       // what the read in progress has heard
        // Off until a saved SOUNDCHECK has been put to the desk on this link.
        bool                 deskAsked { true };
        // Every session whose file says SOUNDCHECK: all of them must hear when
        // it ends, or reopening one lights the chip over a desk that is fine.
        juce::Array<juce::File> sessionsOnSoundcheck;
        std::map<int, float> gains;          // headamp idx -> raw 0..1
        int                  expectedGainReplies { 0 };
        bool                 gainCaptureComplete { false };
        bool                 gainCheckPending { false };   // a restore waits on these
        std::map<int, float> gainReadBack;
        // Deadlines (ms since start, 0 = inactive) rather than one-shot timers.
        juce::int64          patchDeadlineMs { 0 };
        juce::int64          gainDeadlineMs  { 0 };
        juce::int64          nextSubscribeMs { 0 };
        juce::int64          nextProbeMs { 0 };
        juce::int64          connectedAtMs { 0 };
        juce::int64          lastAnswerMs { 0 };    // 0 = no answer on this link yet
        Health               reportedHealth { Health::NotAnswered };
        bool                 neverAnsweredReported { false };

        std::function<void (const ConsoleMessage&)> msgHook;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConsoleLink)
    };
}
