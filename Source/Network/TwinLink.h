#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "SecureToken.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Twin Recorder link: two copies of this app on two Macs, each with its own
// interface, disks and session, started and stopped together, each watching
// the other's health. It covers a computer, drive or interface failure. Both
// Macs run the same software, so a bug in this app can hit both: never call
// this an independent recorder.
//
// What keeps a take safe:
//  * A link failure changes nothing on either Mac. The worker thread only
//    reports it; no code path turns "link down" into a transport action.
//  * The leader starts its own take first and only then queues `start`.
//    Queueing never waits on the network.
//  * `stop` is sent only from noteLocalStopAccepted, which the host calls
//    after the leader's confirmed STOP has ended its take. A take that ends
//    any other way (device loss, quit) leaves the follower rolling.
//  * The follower ends only a take the link started. Commands arriving at a
//    leader are counted and dropped, so a follower can never drive it.
//
// One owned thread does all the socket work, and a second one touches
// session.report.json so a slow disk cannot look like a dead link. Both are
// joined. Everything that reaches the host happens on the message thread:
// from tick() (the host's timer), or from the async drain of the same queue.

namespace zynforge::twin
{
    inline constexpr int    kProtocolVersion = 1;
    inline constexpr int    kDefaultPort     = 9310;
    inline constexpr size_t kMaxFrameBytes   = 16 * 1024;

    enum class Role : int { Leader = 0, Follower = 1 };

    enum class LinkState : int
    {
        Off,      // not enabled
        Waiting,  // listening or connecting; no peer yet
        Linked,   // authenticated, frames flowing
        Lost,     // was linked; the leader keeps retrying
        Refused,  // pairing failed; the leader stops retrying until re-enabled
        Failed    // could not listen
    };

    // ── Pairing code ────────────────────────────────────────────────────────
    // 16 Crockford base-32 symbols = 80 bits of OS entropy, shown as
    // XXXX-XXXX-XXXX-XXXX. Empty when the OS gave no entropy.
    juce::String generatePairingCode (const securetoken::EntropyProvider& provider = {});
    // Accepts lower case, spaces and dashes, and the O/I/L slips the alphabet
    // leaves out. Returns the 16 symbols, or empty when it is not a code.
    juce::String normalizePairingCode (const juce::String& typed);
    bool isIpv4Literal (const juce::String& address);

    // ── Clock offset ────────────────────────────────────────────────────────
    struct ClockSample { double offsetMs { 0.0 }, rttMs { 0.0 }; };

    // NTP's four timestamps, in milliseconds: t1 we sent, t2 the peer received,
    // t3 the peer sent, t4 we received. offsetMs is peer clock minus ours.
    ClockSample clockSample (juce::int64 t1, juce::int64 t2, juce::int64 t3, juce::int64 t4);

    // The lowest-delay sample of a recent window is the one least bent by a
    // queue on the path; an average of those steadies the reported figure.
    class ClockOffsetEstimator
    {
    public:
        void   add (const ClockSample&);
        bool   hasEstimate() const noexcept { return filled > 0; }
        double offsetMs()    const noexcept { return smoothedOffset; }
        double rttMs()       const noexcept { return smoothedRtt; }

    private:
        static constexpr int kWindow = 16;
        std::array<ClockSample, kWindow> window {};
        int filled { 0 }, next { 0 };
        double smoothedOffset { 0.0 }, smoothedRtt { 0.0 };
    };

    // ── Status ──────────────────────────────────────────────────────────────
    struct MacStatus
    {
        juce::String sessionName;
        juce::File   sessionDir;           // never sent to the peer
        bool         recording { false };
        bool         finalizing { false }; // never sent to the peer
        juce::int64  elapsedSamples { 0 };
        double       sampleRate { 0.0 };
        int          armedTracks { 0 };
        juce::int64  missedSamples { 0 };
        int          writeFailures { 0 };
        int          diskMinutesLeft { 0 };
        juce::String backupHealth;         // "off", "ok" or "failed"
    };

    struct TakeSummary
    {
        juce::String takeId;
        bool         reportAvailable { false };
        int          tracks { 0 };         // tracks that wrote audio
        juce::int64  frames { 0 };
        double       seconds { 0.0 };
        juce::int64  missedSamples { 0 };
        int          failures { 0 };
    };

    TakeSummary summaryFromReport (const juce::var& report);

    struct Comparison { juce::String line; bool mismatch { false }; };
    // A plain text comparison of two finished takes; `here` belongs to `role`.
    // No audio is compared and nothing is repaired.
    Comparison compareTakes (const TakeSummary& here, const TakeSummary& peer, Role role);

    // Writes `pass` into a session report's `twin` block. A pass replaces the
    // entry for its own take wherever it sits; a new take moves the current
    // block into twin.previousPasses, so a continued session keeps its history.
    void mergeTwinPass (juce::DynamicObject& report, const juce::var& pass);

    // ── Host ────────────────────────────────────────────────────────────────
    // The engine and UI as this link sees them. Every call arrives on the
    // message thread, and never from TwinLink's destructor.
    class TwinHost
    {
    public:
        virtual ~TwinHost() = default;

        virtual MacStatus twinStatus() = 0;
        // Runs this Mac's own full pre-flight. False leaves nothing changed,
        // with the reason in `refusal`.
        virtual bool twinStartRecord (juce::String& refusal) = 0;
        // Ends the take through this Mac's finalization path, then calls
        // `done` with the outcome. It may call `done` much later.
        virtual void twinStopRecord (std::function<void (bool ok, juce::String text)> done) = 0;
        virtual void twinDropMarker (const juce::String& name) = 0;
        virtual void twinNotify (const juce::String& text) = 0;
    };

    struct Config
    {
        Role         role { Role::Leader };
        int          port { kDefaultPort };
        juce::String peerAddress;     // leader: the follower's IPv4 address
        juce::String bindAddress;     // follower: empty listens on every interface
        juce::String pairingCode;
        juce::String localName;
        int          linkTimeoutMs { 3000 };
        int          handshakeTimeoutMs { 1500 };
        int          reconnectMs { 1000 };
        int          statusPeriodMs { 500 };
    };

    struct Snapshot
    {
        bool              enabled { false };
        Role              role { Role::Leader };
        LinkState         state { LinkState::Off };
        juce::String      detail;           // why the link is not up
        juce::String      peerName;
        std::optional<MacStatus> peer;      // while linked
        bool              clockValid { false };
        double            clockOffsetMs { 0.0 }, clockRttMs { 0.0 };
        int               failedAuth { 0 }, rejectedConnections { 0 }, ignoredCommands { 0 };
        juce::StringArray warnings;
        int               setupWarnings { 0 };   // the first N only compare the two setups; the rest are trouble on the other Mac
        juce::String      lastEvent;        // the latest refusal or notable outcome
        juce::String      compareLine;
        bool              compareMismatch { false };
        juce::String      headline;         // the header text; empty when off
        int               level { 0 };      // 0 fine, 1 warning, 2 alarm
    };

    class TwinLink final : private juce::AsyncUpdater
    {
    public:
        explicit TwinLink (TwinHost&);
        ~TwinLink() override;

        // Message thread. Off by default; a follower binds its port only here.
        bool start (const Config&, juce::String& error);
        void stop();
        bool isEnabled() const noexcept { return enabled; }
        int  getBoundPort() const noexcept { return boundPort.load(); }

        // Message thread, about 10 Hz: applies queued events, follows the
        // local take, publishes status and hands report work to its thread.
        void tick();

        // The leader's capture path reports what really happened. These only
        // queue; none can wait on the network.
        void noteLocalTakeStarted();
        void noteLocalStopAccepted();
        void noteLocalMarker (const juce::String& name);

        Snapshot snapshot() const;
        const Config& getConfig() const noexcept { return config; }

    private:
        struct Event
        {
            enum class Kind { Up, Down, Refused, Failed, Body, ReportDone };
            Kind         kind { Kind::Down };
            juce::String text;
            juce::var    body;
            bool         isStatus { false }, clockValid { false };
            double       clockOffsetMs { 0.0 }, clockRttMs { 0.0 };
            juce::uint64 recordId { 0 };
            TakeSummary  summary;
        };

        struct LinkLoss { juce::int64 fromMs { 0 }, toMs { 0 }; };

        // One take that rolled here while the link was enabled.
        struct TakeRecord
        {
            juce::uint64 id { 0 };
            juce::String takeId, peerName;
            juce::File   sessionDir;
            std::vector<LinkLoss> losses;
            std::optional<TakeSummary> local, peer;
            bool clockValid { false };
            double clockOffsetMs { 0.0 }, clockRttMs { 0.0 };
            bool finished { false }, dirty { false }, jobInFlight { false }, compared { false };
        };

        struct ReportJob { juce::uint64 recordId { 0 }; juce::File sessionDir; juce::var pass; };

        struct Connection;
        struct Keys;
        enum class Handshake { Done, Retry, GiveUp };

        // Worker thread.
        void runLink (Config, juce::String code);
        bool acceptPeer (juce::StreamingSocket& listener, Connection&);
        bool connectPeer (const Config&, Connection&);
        Handshake authenticateLeader (Connection&, const Config&, const juce::String& code, Keys&);
        Handshake authenticateFollower (Connection&, const Config&, const juce::String& code,
                                        Keys&, juce::String& why);
        juce::String serve (Connection&, const Config&, Keys&, juce::StreamingSocket* listener);
        bool readMessage (Connection&, juce::var& message, std::chrono::steady_clock::time_point deadline);
        bool stopRequestedOrWait (int ms);
        void post (Event);

        // Report thread.
        void runReportJobs();
        void postReportJob (ReportJob);

        // Message thread.
        void handleAsyncUpdate() override;
        void drainEvents();
        void handleEvent (Event&);
        void handleBody (const Event&);
        void onLinkUp();
        void onLinkDown (const juce::String& why);
        void onPeerStatus (const Event&);
        void onPeerSummary (const juce::var& body);
        void onStartCommand (const juce::var& body);
        void onStopCommand (const juce::var& body);
        void onLocalTakeEnded();
        void onLocalTakeFinished();
        void openRecord (const juce::String& takeId, const MacStatus&);
        TakeRecord* openRecordOrNull();
        TakeRecord* recordById (juce::uint64);
        TakeRecord* recordForTake (const juce::String& takeId);
        void compareIfReady (TakeRecord&);
        void serviceReports();
        juce::var twinPass (const TakeRecord&) const;
        void refreshWarnings();
        void queueBody (const juce::var& body);
        void publishStatus (const MacStatus&);
        juce::String peerLabel() const;

        TwinHost& host;

        std::thread       worker;
        std::atomic<bool> stopRequested { false };
        std::atomic<int>  boundPort { -1 };
        std::atomic<int>  failedAuth { 0 }, rejectedConnections { 0 };

        // Crosses threads under queueLock.
        mutable std::mutex       queueLock;
        std::deque<Event>        inbox;
        std::deque<juce::String> outbox;
        MacStatus                publishedStatus;

        std::thread             reportThread;
        std::mutex              reportLock;
        std::condition_variable reportWake;
        std::deque<ReportJob>   reportJobs;
        bool                    reportStopping { false };

        // Message thread only.
        Config       config;
        bool         enabled { false }, linked { false };
        LinkState    state { LinkState::Off };
        juce::String detail, peerName, lastEvent, compareLine;
        bool         compareMismatch { false };
        std::optional<MacStatus> peer;
        bool         clockValid { false };
        double       clockOffsetMs { 0.0 }, clockRttMs { 0.0 };
        MacStatus    local;
        bool         localRolling { false }, localFinishing { false };
        juce::uint32 lastPublishMs { 0 }, rollingChangedAtMs { 0 };
        juce::String leaderTakeId;
        bool         leaderTakeActive { false };
        juce::String followerTakeId;     // the take the link started here
        juce::String lastSummaryBody;
        std::deque<TakeRecord> records;
        juce::uint64 nextRecordId { 1 };
        juce::StringArray warnings;
        int          setupWarnings { 0 };
        int          ignoredCommands { 0 };
        bool         draining { false };
        // A host may complete a STOP after this object has gone.
        std::shared_ptr<bool> alive { std::make_shared<bool> (true) };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TwinLink)
    };
}
