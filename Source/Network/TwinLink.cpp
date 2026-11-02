#include "TwinLink.h"

#include "CaptureIdentity.h"
#include "CaptureProtocol.h"
#include "LineSocket.h"
#include "../Audio/MultitrackRecorder.h"

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace zynforge::twin
{
    namespace
    {
        using Clock = linesocket::Clock;
        namespace identity = capture::identity;

        constexpr int    kCodeSymbols     = 16;
        constexpr int    kPollMs          = 25;
        constexpr int    kConnectMs       = 500;
        constexpr int    kNameChars       = 128;
        constexpr int    kTextChars       = 300;
        constexpr size_t kMaxQueued       = 64;
        constexpr size_t kMaxRecords      = 8;
        constexpr int    kMaxPastPasses   = 32;
        constexpr int    kRollingGraceMs  = 2500;
        constexpr int    kLowDiskMinutes  = 30;
        constexpr double kSmoothing       = 0.25;
        // The two Macs never start or stop on the same sample: each command
        // crosses a network and then a full record path.
        constexpr double kDurationToleranceSeconds = 2.0;

        const char* const kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

        juce::var makeObject (std::initializer_list<std::pair<const char*, juce::var>> fields)
        {
            auto* object = new juce::DynamicObject();
            for (const auto& [name, value] : fields) object->setProperty (name, value);
            return juce::var (object);
        }

        juce::String textOf (const juce::var& message, const char* field, int maxChars = kTextChars)
        {
            return message.getProperty (field, "").toString()
                          .removeCharacters ("\r\n\t").substring (0, maxChars);
        }

        juce::int64 countOf (const juce::var& message, const char* field)
        {
            return juce::jmax<juce::int64> (0, (juce::int64) message.getProperty (field, 0));
        }

        juce::int64 wallClockMs() { return juce::Time::currentTimeMillis(); }

        juce::String transcriptOf (const juce::String& followerNonce, const juce::String& leaderNonce)
        {
            return "zynforge-twin:" + juce::String (kProtocolVersion) + ":"
                 + followerNonce + ":" + leaderNonce;
        }

        juce::String roleName (Role role) { return role == Role::Leader ? "leader" : "follower"; }

        juce::var statusBody (const MacStatus& s, const juce::String& name)
        {
            return makeObject ({
                { "m", "status" }, { "name", name }, { "session", s.sessionName },
                { "rec", s.recording }, { "elapsed", s.elapsedSamples }, { "sr", s.sampleRate },
                { "armed", s.armedTracks }, { "missed", s.missedSamples },
                { "failures", s.writeFailures }, { "diskMin", s.diskMinutesLeft },
                { "backup", s.backupHealth } });
        }

        MacStatus statusFromBody (const juce::var& body)
        {
            MacStatus s;
            s.sessionName     = textOf (body, "session", kNameChars);
            s.recording       = (bool) body.getProperty ("rec", false);
            s.elapsedSamples  = countOf (body, "elapsed");
            const auto rate   = (double) body.getProperty ("sr", 0.0);
            s.sampleRate      = std::isfinite (rate) ? juce::jmax (0.0, rate) : 0.0;
            s.armedTracks     = (int) countOf (body, "armed");
            s.missedSamples   = countOf (body, "missed");
            s.writeFailures   = (int) countOf (body, "failures");
            s.diskMinutesLeft = (int) countOf (body, "diskMin");
            s.backupHealth    = textOf (body, "backup", 16);
            return s;
        }

        juce::var summaryBody (const TakeSummary& s)
        {
            return makeObject ({
                { "m", "summary" }, { "take", s.takeId }, { "report", s.reportAvailable },
                { "tracks", s.tracks }, { "frames", s.frames }, { "seconds", s.seconds },
                { "missed", s.missedSamples }, { "failures", s.failures } });
        }

        TakeSummary summaryFromBody (const juce::var& body)
        {
            TakeSummary s;
            s.takeId          = textOf (body, "take");
            s.reportAvailable = (bool) body.getProperty ("report", false);
            s.tracks          = (int) countOf (body, "tracks");
            s.frames          = countOf (body, "frames");
            const auto seconds = (double) body.getProperty ("seconds", 0.0);
            s.seconds         = std::isfinite (seconds) ? juce::jmax (0.0, seconds) : 0.0;
            s.missedSamples   = countOf (body, "missed");
            s.failures        = (int) countOf (body, "failures");
            return s;
        }

        juce::String asClock (double seconds)
        {
            const auto total = (juce::int64) std::llround (seconds);
            return juce::String::formatted ("%d:%02d:%02d", (int) (total / 3600),
                                            (int) ((total / 60) % 60), (int) (total % 60));
        }
    }

    // ── Pairing code ────────────────────────────────────────────────────────

    juce::String generatePairingCode (const securetoken::EntropyProvider& provider)
    {
        std::array<unsigned char, 10> bytes {};   // 80 bits
        if (! (provider ? provider (bytes.data(), bytes.size())
                        : securetoken::fillOSRandom (bytes.data(), bytes.size())))
            return {};
        juce::String code;
        unsigned int bitBuffer = 0;
        int bits = 0, symbols = 0;
        for (const auto byte : bytes)
        {
            bitBuffer = (bitBuffer << 8) | byte;
            bits += 8;
            while (bits >= 5)
            {
                bits -= 5;
                if (symbols > 0 && symbols % 4 == 0) code << "-";
                code << juce::String::charToString ((juce::juce_wchar) kCodeAlphabet[(bitBuffer >> bits) & 31u]);
                ++symbols;
            }
        }
        return code;
    }

    juce::String normalizePairingCode (const juce::String& typed)
    {
        const juce::String alphabet (kCodeAlphabet);
        juce::String symbols;
        for (auto p = typed.getCharPointer(); ! p.isEmpty(); ++p)
        {
            auto c = juce::CharacterFunctions::toUpperCase (*p);
            if (c == '-' || juce::CharacterFunctions::isWhitespace (c)) continue;
            if (c == 'O') c = '0';
            if (c == 'I' || c == 'L') c = '1';
            if (alphabet.indexOfChar (c) < 0 || symbols.length() >= kCodeSymbols) return {};
            symbols << juce::String::charToString (c);
        }
        return symbols.length() == kCodeSymbols ? symbols : juce::String();
    }

    bool isIpv4Literal (const juce::String& address)
    {
        juce::StringArray parts;
        parts.addTokens (address, ".", {});
        if (parts.size() != 4) return false;
        for (const auto& part : parts)
            if (part.isEmpty() || part.length() > 3 || ! part.containsOnly ("0123456789")
                || part.getIntValue() > 255)
                return false;
        return true;
    }

    // ── Clock offset ────────────────────────────────────────────────────────

    ClockSample clockSample (juce::int64 t1, juce::int64 t2, juce::int64 t3, juce::int64 t4)
    {
        return { ((double) (t2 - t1) + (double) (t3 - t4)) / 2.0,
                 (double) ((t4 - t1) - (t3 - t2)) };
    }

    void ClockOffsetEstimator::add (const ClockSample& sample)
    {
        // A negative round trip means a clock stepped mid-measurement.
        if (! std::isfinite (sample.offsetMs) || ! std::isfinite (sample.rttMs) || sample.rttMs < 0.0)
            return;
        const bool first = filled == 0;
        const int newest = next;
        window[(size_t) newest] = sample;
        next = (next + 1) % kWindow;
        filled = juce::jmin (filled + 1, kWindow);

        auto best = sample;
        for (int age = 1; age < filled; ++age)
        {
            const auto& candidate = window[(size_t) ((newest - age + kWindow) % kWindow)];
            if (candidate.rttMs < best.rttMs) best = candidate;
        }
        smoothedOffset = first ? best.offsetMs : smoothedOffset + kSmoothing * (best.offsetMs - smoothedOffset);
        smoothedRtt    = first ? best.rttMs    : smoothedRtt    + kSmoothing * (best.rttMs    - smoothedRtt);
    }

    // ── Reports ─────────────────────────────────────────────────────────────

    TakeSummary summaryFromReport (const juce::var& report)
    {
        TakeSummary s;
        s.reportAvailable = report.isObject();
        // One pass against one pass: the totals also hold earlier passes,
        // which the other Mac may never have recorded. A report written
        // before the pass fields existed has only the totals.
        const bool hasPass = report.hasProperty ("passSeconds");
        s.frames          = countOf (report, hasPass ? "passSamples" : "totalSamples");
        s.seconds         = (double) report.getProperty (hasPass ? "passSeconds" : "totalSeconds", 0.0);
        s.missedSamples   = countOf (report, hasPass ? "passMissedSamples" : "missedSamples");
        for (const auto* flag : { "primaryFailed", "backupFailed", "mirrorFailed", "punchSpliceFailed",
                                  "captureDeviceLost", "sha256Failed" })
            if ((bool) report.getProperty (flag, false)) ++s.failures;
        if (countOf (report, "mirrorsSkipped") > 0) ++s.failures;
        if (auto* tracks = report.getProperty ("tracks", {}).getArray())
            for (const auto& track : *tracks)
                if (auto* files = track.getProperty ("files", {}).getArray())
                    if (! files->isEmpty()) ++s.tracks;
        return s;
    }

    Comparison compareTakes (const TakeSummary& here, const TakeSummary& peer, Role role)
    {
        const auto& leader   = role == Role::Leader ? here : peer;
        const auto& follower = role == Role::Leader ? peer : here;
        Comparison result;
        result.line << "Leader / follower: " << leader.tracks << " / " << follower.tracks << " tracks, "
                    << asClock (leader.seconds) << " / " << asClock (follower.seconds) << ", missed "
                    << leader.missedSamples << " / " << follower.missedSamples << ", failures "
                    << leader.failures << " / " << follower.failures;

        juce::StringArray problems;
        if (! leader.reportAvailable)   problems.add ("the leader has no session report");
        if (! follower.reportAvailable) problems.add ("the follower has no session report");
        if (leader.tracks != follower.tracks) problems.add ("track counts differ");
        const auto drift = std::abs (leader.seconds - follower.seconds);
        if (drift > kDurationToleranceSeconds)
            problems.add ("durations differ by " + juce::String (drift, 1) + " s");
        if (leader.missedSamples > 0 || follower.missedSamples > 0) problems.add ("samples were missed");
        if (leader.failures > 0 || follower.failures > 0) problems.add ("a capture failure was reported");
        if (! problems.isEmpty())
        {
            result.mismatch = true;
            result.line << "  WARNING: " << problems.joinIntoString ("; ");
        }
        return result;
    }

    void mergeTwinPass (juce::DynamicObject& report, const juce::var& pass)
    {
        const auto takeId = pass.getProperty ("takeId", "").toString();
        const auto current = report.getProperty ("twin");
        auto updated = pass.clone();
        if (! current.isObject())
        {
            report.setProperty ("twin", updated);
            return;
        }

        juce::Array<juce::var> previous;
        if (auto* list = current.getProperty ("previousPasses", {}).getArray()) previous = *list;
        for (int i = 0; i < previous.size(); ++i)
            if (previous.getReference (i).getProperty ("takeId", "").toString() == takeId)
            {
                previous.set (i, updated);
                auto kept = current.clone();
                if (auto* object = kept.getDynamicObject()) object->setProperty ("previousPasses", previous);
                report.setProperty ("twin", kept);
                return;
            }

        if (current.getProperty ("takeId", "").toString() != takeId)
        {
            auto retired = current.clone();
            if (auto* object = retired.getDynamicObject()) object->removeProperty ("previousPasses");
            previous.add (retired);
            while (previous.size() > kMaxPastPasses) previous.remove (0);
        }
        if (! previous.isEmpty())
            if (auto* object = updated.getDynamicObject()) object->setProperty ("previousPasses", previous);
        report.setProperty ("twin", updated);
    }

    // ── Connection ──────────────────────────────────────────────────────────

    struct TwinLink::Connection
    {
        std::unique_ptr<juce::StreamingSocket> socket;
        juce::MemoryBlock scratch;
        std::deque<juce::String> lines;

        bool pump (int waitMs)
        {
            return linesocket::pumpLines (*socket, scratch,
                                          [this] (const juce::String& line) { lines.push_back (line); },
                                          waitMs, kMaxFrameBytes);
        }
        bool oversized() const noexcept { return scratch.getSize() > kMaxFrameBytes; }
    };

    struct TwinLink::Keys
    {
        juce::String sessionKey, sendTag, receiveTag;
        juce::int64  sent { 0 }, received { 0 };
    };

    // ── Lifetime ────────────────────────────────────────────────────────────

    TwinLink::TwinLink (TwinHost& h) : host (h) {}

    TwinLink::~TwinLink()
    {
        // No host calls here: the host may already be part-way through its own
        // destruction. A take on this Mac is not touched; the peer sees TWIN LOST.
        alive.reset();
        stopRequested.store (true);
        if (worker.joinable()) worker.join();
        {
            const std::lock_guard<std::mutex> lock (reportLock);
            reportStopping = true;
        }
        reportWake.notify_all();
        if (reportThread.joinable()) reportThread.join();
        cancelPendingUpdate();
    }

    bool TwinLink::start (const Config& requested, juce::String& error)
    {
        stop();
        const auto code = normalizePairingCode (requested.pairingCode);
        if (code.isEmpty())
        { error = "Enter the 16-character pairing code shown on the follower"; return false; }
        if (requested.port < 0 || requested.port > 65535)
        { error = "The port must be between 1024 and 65535"; return false; }
        if (requested.role == Role::Leader
            && (! isIpv4Literal (requested.peerAddress) || requested.port == 0))
        { error = "Enter the follower's IP address, for example 192.168.68.72"; return false; }

        config = requested;
        enabled = true;
        state = LinkState::Waiting;
        detail.clear();
        failedAuth.store (0);
        rejectedConnections.store (0);
        boundPort.store (-1);
        stopRequested.store (false);
        {
            const std::lock_guard<std::mutex> lock (queueLock);
            outbox.clear();
        }
        local = host.twinStatus();
        publishStatus (local);
        worker = std::thread ([this, workerConfig = config, code] { runLink (workerConfig, code); });
        return true;
    }

    void TwinLink::stop()
    {
        enabled = false;
        stopRequested.store (true);
        if (worker.joinable()) worker.join();
        drainEvents();
        state = LinkState::Off;
        linked = false;
        detail.clear();
        peerName.clear();
        peer.reset();
        clockValid = false;
        warnings.clear();
        boundPort.store (-1);
    }

    void TwinLink::post (Event event)
    {
        {
            const std::lock_guard<std::mutex> lock (queueLock);
            if (event.isStatus && ! inbox.empty() && inbox.back().isStatus)
                inbox.back() = std::move (event);
            else if (event.kind != Event::Kind::Body || inbox.size() < kMaxQueued)
                inbox.push_back (std::move (event));
        }
        triggerAsyncUpdate();
    }

    // ── Worker thread ───────────────────────────────────────────────────────

    bool TwinLink::stopRequestedOrWait (int ms)
    {
        for (int waited = 0; waited < ms && ! stopRequested.load(); waited += kPollMs)
            juce::Thread::sleep (kPollMs);
        return stopRequested.load();
    }

    void TwinLink::runLink (Config workerConfig, juce::String code)
    {
        std::unique_ptr<juce::StreamingSocket> listener;
        if (workerConfig.role == Role::Follower)
        {
            listener = std::make_unique<juce::StreamingSocket>();
            if (! listener->createListener (workerConfig.port, workerConfig.bindAddress))
            {
                Event failed;
                failed.kind = Event::Kind::Failed;
                failed.text = "Could not listen on port " + juce::String (workerConfig.port)
                            + "; another app may be using it";
                post (std::move (failed));
                return;
            }
            boundPort.store (listener->getBoundPort() > 0 ? listener->getBoundPort() : workerConfig.port);
        }

        while (! stopRequested.load())
        {
            Connection connection;
            if (listener != nullptr)
            {
                if (! acceptPeer (*listener, connection)) continue;
            }
            else if (! connectPeer (workerConfig, connection))
            {
                stopRequestedOrWait (workerConfig.reconnectMs);
                continue;
            }

            Keys keys;
            juce::String why;
            const auto outcome = listener != nullptr
                ? authenticateLeader (connection, workerConfig, code, keys)
                : authenticateFollower (connection, workerConfig, code, keys, why);
            if (outcome == Handshake::GiveUp)
            {
                Event refused;
                refused.kind = Event::Kind::Refused;
                refused.text = why;
                post (std::move (refused));
                return;
            }
            if (outcome == Handshake::Retry)
            {
                if (listener == nullptr) stopRequestedOrWait (workerConfig.reconnectMs);
                continue;
            }

            {
                // Nothing queued for a previous connection is ever delivered:
                // a late start or stop must not act on a take that outlived it.
                const std::lock_guard<std::mutex> lock (queueLock);
                outbox.clear();
            }
            Event up;
            up.kind = Event::Kind::Up;
            post (std::move (up));

            Event down;
            down.kind = Event::Kind::Down;
            down.text = serve (connection, workerConfig, keys, listener.get());
            post (std::move (down));

            if (listener == nullptr) stopRequestedOrWait (workerConfig.reconnectMs);
        }
    }

    bool TwinLink::acceptPeer (juce::StreamingSocket& listener, Connection& connection)
    {
        if (listener.waitUntilReady (true, 100) <= 0) return false;
        connection.socket.reset (listener.waitForNextConnection());
        return connection.socket != nullptr;
    }

    bool TwinLink::connectPeer (const Config& workerConfig, Connection& connection)
    {
        auto socket = std::make_unique<juce::StreamingSocket>();
        if (! socket->connect (workerConfig.peerAddress, workerConfig.port, kConnectMs)) return false;
        connection.socket = std::move (socket);
        return true;
    }

    bool TwinLink::readMessage (Connection& connection, juce::var& message, Clock::time_point deadline)
    {
        while (! stopRequested.load() && Clock::now() < deadline)
        {
            if (! connection.lines.empty())
            {
                const auto line = connection.lines.front();
                connection.lines.pop_front();
                message = linesocket::parseJson (line);
                return message.isObject();
            }
            if (! connection.pump (kPollMs)) return false;
        }
        return false;
    }

    TwinLink::Handshake TwinLink::authenticateLeader (Connection& connection, const Config& workerConfig,
                                                      const juce::String& code, Keys& keys)
    {
        const auto nonce = securetoken::generate();
        if (nonce.isEmpty()) return Handshake::Retry;   // no OS entropy: never a weak nonce

        const auto deadline = Clock::now() + std::chrono::milliseconds (workerConfig.handshakeTimeoutMs);
        const auto refuse = [&] (const char* reason)
        {
            failedAuth.fetch_add (1);
            linesocket::writeAll (*connection.socket,
                                  capture::frame (makeObject ({ { "type", "twin.refused" },
                                                                { "reason", reason } })), deadline);
            return Handshake::Retry;
        };

        if (! linesocket::writeAll (*connection.socket,
                capture::frame (makeObject ({ { "type", "twin.hello" }, { "version", kProtocolVersion },
                                              { "role", "follower" }, { "nonce", nonce } })), deadline))
            return Handshake::Retry;

        juce::var auth;
        if (! readMessage (connection, auth, deadline))
        {
            if (stopRequested.load()) return Handshake::Retry;
            return refuse ("protocol");
        }
        if ((int) auth.getProperty ("version", 0) != kProtocolVersion) return refuse ("version");
        if (auth.getProperty ("type", "").toString() != "twin.auth") return refuse ("protocol");

        const auto leaderNonce = auth.getProperty ("nonce", "").toString();
        const auto transcript = transcriptOf (nonce, leaderNonce);
        if (! identity::validNonce (leaderNonce) || leaderNonce == nonce
            || ! identity::equalProof (auth.getProperty ("proof", "").toString(),
                                       identity::authenticate (code, "leader:" + transcript)))
            return refuse ("code");

        if (! linesocket::writeAll (*connection.socket,
                capture::frame (makeObject ({ { "type", "twin.accept" },
                                              { "proof", identity::authenticate (code, "follower:" + transcript) } })),
                deadline))
            return Handshake::Retry;

        keys.sessionKey = identity::authenticate (code, "session:" + transcript);
        keys.sendTag    = "F";
        keys.receiveTag = "L";
        return Handshake::Done;
    }

    TwinLink::Handshake TwinLink::authenticateFollower (Connection& connection, const Config& workerConfig,
                                                        const juce::String& code, Keys& keys, juce::String& why)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds (workerConfig.handshakeTimeoutMs);
        // A busy or restarting follower closes the socket: an ordinary retry.
        // Only an explicit answer ends the attempts, so a wrong code is not
        // sent to the follower once a second for the rest of the show.
        const auto versionRefused = [&]
        {
            why = "The two Macs run different twin link versions; update both to the same build";
            return Handshake::GiveUp;
        };

        juce::var hello;
        if (! readMessage (connection, hello, deadline)
            || hello.getProperty ("type", "").toString() != "twin.hello")
            return Handshake::Retry;
        if ((int) hello.getProperty ("version", 0) != kProtocolVersion) return versionRefused();

        const auto followerNonce = hello.getProperty ("nonce", "").toString();
        const auto nonce = securetoken::generate();
        if (! identity::validNonce (followerNonce) || nonce.isEmpty()) return Handshake::Retry;

        const auto transcript = transcriptOf (followerNonce, nonce);
        if (! linesocket::writeAll (*connection.socket,
                capture::frame (makeObject ({ { "type", "twin.auth" }, { "version", kProtocolVersion },
                                              { "role", "leader" }, { "nonce", nonce },
                                              { "proof", identity::authenticate (code, "leader:" + transcript) } })),
                deadline))
            return Handshake::Retry;

        juce::var reply;
        if (! readMessage (connection, reply, deadline)) return Handshake::Retry;
        const auto type = reply.getProperty ("type", "").toString();
        if (type == "twin.refused")
        {
            if (reply.getProperty ("reason", "").toString() == "version") return versionRefused();
            why = "The follower refused the pairing code; check it and connect again";
            return Handshake::GiveUp;
        }
        if (type != "twin.accept"
            || ! identity::equalProof (reply.getProperty ("proof", "").toString(),
                                       identity::authenticate (code, "follower:" + transcript)))
        {
            failedAuth.fetch_add (1);
            why = "The other Mac could not prove the pairing code";
            return Handshake::GiveUp;
        }

        keys.sessionKey = identity::authenticate (code, "session:" + transcript);
        keys.sendTag    = "L";
        keys.receiveTag = "F";
        return Handshake::Done;
    }

    juce::String TwinLink::serve (Connection& connection, const Config& workerConfig, Keys& keys,
                                  juce::StreamingSocket* listener)
    {
        ClockOffsetEstimator clock;
        juce::int64 peerSentAt = 0, peerHeardAt = 0;
        auto lastHeard = Clock::now();
        auto nextStatus = lastHeard;
        const auto timeout = std::chrono::milliseconds (workerConfig.linkTimeoutMs);

        // Every frame carries its own MAC and the next sequence number, so a
        // connection cannot be steered by bytes injected after the handshake,
        // and a recorded frame cannot be played back.
        const auto openFrame = [&keys] (const juce::String& line, juce::var& body)
        {
            const auto frame = linesocket::parseJson (line);
            if (frame.getProperty ("type", "").toString() != "twin.frame") return false;
            const auto sequence = (juce::int64) frame.getProperty ("seq", 0);
            const auto text = frame.getProperty ("body", "").toString();
            if (sequence != keys.received + 1
                || ! identity::equalProof (frame.getProperty ("mac", "").toString(),
                       identity::authenticate (keys.sessionKey, keys.receiveTag + ":"
                                               + juce::String (sequence) + ":" + text)))
                return false;
            keys.received = sequence;
            body = linesocket::parseJson (text);
            return body.isObject();
        };

        const auto sendFrame = [&connection, &keys] (const juce::String& body)
        {
            const auto sequence = ++keys.sent;
            const auto mac = identity::authenticate (keys.sessionKey, keys.sendTag + ":"
                                                     + juce::String (sequence) + ":" + body);
            return linesocket::writeAll (*connection.socket,
                capture::frame (makeObject ({ { "type", "twin.frame" }, { "seq", sequence },
                                              { "body", body }, { "mac", mac } })),
                Clock::now() + linesocket::kDefaultWriteBudget);
        };

        while (! stopRequested.load())
        {
            while (! connection.lines.empty())
            {
                const auto line = connection.lines.front();
                connection.lines.pop_front();
                juce::var body;
                if (! openFrame (line, body))
                {
                    failedAuth.fetch_add (1);
                    return "A message from the other Mac failed its check";
                }
                lastHeard = Clock::now();

                Event event;
                event.kind = Event::Kind::Body;
                if (body.getProperty ("m", "").toString() == "status")
                {
                    event.isStatus = true;
                    const auto heardAt = wallClockMs();
                    const auto sentAt = (juce::int64) body.getProperty ("t1", 0);
                    const auto echoed = (juce::int64) body.getProperty ("echo", 0);
                    const auto held   = (juce::int64) body.getProperty ("hold", 0);
                    if (echoed > 0) clock.add (clockSample (echoed, sentAt - held, sentAt, heardAt));
                    peerSentAt = sentAt;
                    peerHeardAt = heardAt;
                    event.clockValid = clock.hasEstimate();
                    event.clockOffsetMs = clock.offsetMs();
                    event.clockRttMs = clock.rttMs();
                }
                event.body = std::move (body);
                post (std::move (event));
            }

            if (! connection.pump (kPollMs))
            {
                if (connection.oversized()) failedAuth.fetch_add (1);
                return "The connection closed";
            }

            if (listener != nullptr && listener->waitUntilReady (true, 0) > 0)
            {
                // One peer at a time. A second caller never touches this link.
                std::unique_ptr<juce::StreamingSocket> extra (listener->waitForNextConnection());
                if (extra != nullptr) rejectedConnections.fetch_add (1);
            }

            const auto now = Clock::now();
            if (now - lastHeard > timeout)
                return "Nothing heard from the other Mac for "
                     + juce::String (workerConfig.linkTimeoutMs / 1000.0, 1) + " s";

            std::deque<juce::String> pending;
            juce::var status;
            {
                const std::lock_guard<std::mutex> lock (queueLock);
                pending.swap (outbox);
                if (now >= nextStatus) status = statusBody (publishedStatus, workerConfig.localName);
            }
            for (const auto& body : pending)
                if (! sendFrame (body)) return "The other Mac stopped reading";
            if (auto* object = status.getDynamicObject())
            {
                const auto sentAt = wallClockMs();
                object->setProperty ("t1", sentAt);
                object->setProperty ("echo", peerSentAt);
                object->setProperty ("hold", peerHeardAt > 0 ? sentAt - peerHeardAt : (juce::int64) 0);
                if (! sendFrame (juce::JSON::toString (status, true))) return "The other Mac stopped reading";
                nextStatus = now + std::chrono::milliseconds (workerConfig.statusPeriodMs);
            }
        }
        return "Link switched off";
    }

    // ── Report thread ───────────────────────────────────────────────────────

    void TwinLink::postReportJob (ReportJob job)
    {
        {
            const std::lock_guard<std::mutex> lock (reportLock);
            reportJobs.push_back (std::move (job));
            if (! reportThread.joinable()) reportThread = std::thread ([this] { runReportJobs(); });
        }
        reportWake.notify_one();
    }

    void TwinLink::runReportJobs()
    {
        for (;;)
        {
            ReportJob job;
            {
                std::unique_lock<std::mutex> lock (reportLock);
                reportWake.wait (lock, [this] { return reportStopping || ! reportJobs.empty(); });
                if (reportJobs.empty()) return;
                job = std::move (reportJobs.front());
                reportJobs.pop_front();
            }

            TakeSummary summary;
            if (job.sessionDir.isDirectory())
                MultitrackRecorder::editSessionReport (job.sessionDir, [&] (juce::DynamicObject& report)
                {
                    summary = summaryFromReport (juce::var (&report));
                    mergeTwinPass (report, job.pass);
                });

            Event done;
            done.kind = Event::Kind::ReportDone;
            done.recordId = job.recordId;
            done.summary = std::move (summary);
            post (std::move (done));
        }
    }

    // ── Message thread ──────────────────────────────────────────────────────

    void TwinLink::handleAsyncUpdate() { drainEvents(); }

    void TwinLink::drainEvents()
    {
        // twinStartRecord can run a long way inside the host. A nested drain
        // would reorder commands, so the outer loop keeps them in sequence.
        if (draining) return;
        const juce::ScopedValueSetter<bool> guard (draining, true);
        for (;;)
        {
            Event event;
            {
                const std::lock_guard<std::mutex> lock (queueLock);
                if (inbox.empty()) return;
                event = std::move (inbox.front());
                inbox.pop_front();
            }
            handleEvent (event);
        }
    }

    void TwinLink::handleEvent (Event& event)
    {
        switch (event.kind)
        {
            case Event::Kind::Up:   onLinkUp(); break;
            case Event::Kind::Down: onLinkDown (event.text); break;
            case Event::Kind::Body: handleBody (event); break;
            case Event::Kind::Refused:
            case Event::Kind::Failed:
                if (enabled)
                {
                    state = event.kind == Event::Kind::Refused ? LinkState::Refused : LinkState::Failed;
                    detail = event.text;
                    host.twinNotify (event.text);
                }
                break;
            case Event::Kind::ReportDone:
                if (auto* record = recordById (event.recordId))
                {
                    record->jobInFlight = false;
                    const bool first = ! record->local.has_value();
                    record->local = event.summary;
                    record->local->takeId = record->takeId;
                    if (first)
                    {
                        const auto body = summaryBody (*record->local);
                        lastSummaryBody = juce::JSON::toString (body, true);
                        if (linked) queueBody (body);
                    }
                    compareIfReady (*record);
                }
                break;
        }
    }

    void TwinLink::handleBody (const Event& event)
    {
        // Whatever was still queued when the link was switched off, or while
        // the host shuts down, must not start a take now.
        if (! enabled) return;
        const auto& body = event.body;
        const auto message = body.getProperty ("m", "").toString();
        if (message == "status")  { onPeerStatus (event); return; }
        if (message == "summary") { onPeerSummary (body); return; }

        if (config.role == Role::Follower)
        {
            if (message == "start") onStartCommand (body);
            else if (message == "stop") onStopCommand (body);
            else if (message == "marker")
            {
                if (host.twinStatus().recording) host.twinDropMarker (textOf (body, "name", kNameChars));
            }
            return;
        }

        // A follower commands nothing on a leader.
        if (message == "start" || message == "stop" || message == "marker")
        {
            ++ignoredCommands;
            return;
        }

        const auto text = textOf (body, "text");
        const auto ok = (bool) body.getProperty ("ok", false);
        if (message == "startResult")
        {
            if (! ok)
            {
                lastEvent = "FOLLOWER REFUSED RECORD: " + text;
                host.twinNotify (lastEvent);
            }
            else if (text.isNotEmpty())
            {
                lastEvent = "FOLLOWER: " + text;
                host.twinNotify (lastEvent);
            }
            else lastEvent = "Follower is recording";
        }
        else if (message == "stopResult")
        {
            if (ok) lastEvent = "Follower stopped";
            else
            {
                lastEvent = "FOLLOWER STOP PROBLEM: " + text;
                host.twinNotify (lastEvent);
            }
        }
    }

    void TwinLink::onLinkUp()
    {
        linked = true;
        if (enabled) state = LinkState::Linked;
        detail.clear();
        const auto now = wallClockMs();
        for (auto& record : records)
            for (auto& loss : record.losses)
                if (loss.toMs == 0)
                {
                    loss.toMs = now;
                    record.dirty = record.finished;
                }
        // The summaries may have been made while the link was down.
        if (lastSummaryBody.isNotEmpty())
        {
            const std::lock_guard<std::mutex> lock (queueLock);
            outbox.push_back (lastSummaryBody);
        }
        offerAdoptedTake();
        publishStatus (local);
    }

    void TwinLink::onLinkDown (const juce::String& why)
    {
        const bool wasLinked = linked;
        linked = false;
        peer.reset();
        if (auto* record = openRecordOrNull())
            if (wasLinked) record->losses.push_back ({ wallClockMs(), 0 });
        if (! enabled) return;
        state = LinkState::Lost;
        detail = why;
        if (wasLinked && localRolling) host.twinNotify ("TWIN LOST -- this take keeps rolling");
    }

    void TwinLink::onPeerStatus (const Event& event)
    {
        const bool wasRolling = peer.has_value() && peer->recording;
        peer = statusFromBody (event.body);
        if (peer->recording != wasRolling) rollingChangedAtMs = juce::Time::getMillisecondCounter();
        const auto name = textOf (event.body, "name", kNameChars);
        if (name.isNotEmpty()) peerName = name;
        clockValid = event.clockValid;
        clockOffsetMs = event.clockOffsetMs;
        clockRttMs = event.clockRttMs;
        if (auto* record = openRecordOrNull())
        {
            record->peerName = peerName;
            record->clockValid = clockValid;
            record->clockOffsetMs = clockOffsetMs;
            record->clockRttMs = clockRttMs;
        }
    }

    void TwinLink::onPeerSummary (const juce::var& body)
    {
        const auto summary = summaryFromBody (body);
        if (auto* record = recordForTake (summary.takeId))
        {
            record->peer = summary;
            record->dirty = record->finished;
            compareIfReady (*record);
        }
    }

    void TwinLink::onStartCommand (const juce::var& body)
    {
        const auto take = textOf (body, "take");
        if (take.isEmpty()) return;
        const auto reply = [this, &take] (bool ok, const juce::String& text)
        {
            queueBody (makeObject ({ { "m", "startResult" }, { "take", take }, { "ok", ok }, { "text", text } }));
        };

        const auto status = host.twinStatus();
        if (status.recording)
        {
            // The leader restarted while this Mac kept rolling its linked take
            // (its interface died, say): the leader's next STOP must reach it.
            if (followerTakeId.isNotEmpty())
            {
                followerTakeId = take;
                if (auto* record = openRecordOrNull()) record->takeId = take;
                reply (true, "already recording; the take it kept rolling is now linked to this one");
            }
            else reply (true, "already recording its own take; STOP on the leader will not end it");
            return;
        }
        if ((bool) body.getProperty ("relink", false))
        {
            reply (true, "nothing is rolling here, so the leader's take has no follower");
            return;
        }

        juce::String refusal;
        if (! host.twinStartRecord (refusal))
        {
            reply (false, refusal.isNotEmpty() ? refusal : juce::String ("recording could not start"));
            return;
        }
        followerTakeId = take;
        localRolling = true;
        rollingChangedAtMs = juce::Time::getMillisecondCounter();
        const auto started = host.twinStatus();
        if (auto* record = openRecordOrNull()) record->takeId = take;
        else openRecord (take, started);
        reply (true, {});
    }

    void TwinLink::onStopCommand (const juce::var& body)
    {
        const auto take = textOf (body, "take");
        const auto reply = [this, take] (bool ok, const juce::String& text)
        {
            queueBody (makeObject ({ { "m", "stopResult" }, { "take", take }, { "ok", ok }, { "text", text } }));
        };

        if (! host.twinStatus().recording) { reply (true, {}); return; }
        if (take.isEmpty() || take != followerTakeId)
        {
            reply (false, "this take was started on the follower; stop it there");
            return;
        }
        host.twinStopRecord ([reply, guard = std::weak_ptr<bool> (alive)] (bool ok, juce::String text)
        {
            if (! guard.expired()) reply (ok, text);
        });
    }

    void TwinLink::onLocalTakeEnded()
    {
        localRolling = false;
        localFinishing = true;
        rollingChangedAtMs = juce::Time::getMillisecondCounter();
        followerTakeId.clear();
        if (config.role == Role::Leader && leaderTakeActive)
        {
            // This take ended without a confirmed STOP: the interface died, or
            // a disk did. That is the failure the follower is there for.
            leaderTakeActive = false;
            if (linked)
            {
                lastEvent = "This take ended without STOP; the follower keeps recording";
                host.twinNotify (lastEvent);
            }
        }
    }

    void TwinLink::onLocalTakeFinished()
    {
        localFinishing = false;
        for (auto& record : records)
            if (! record.finished)
            {
                record.finished = true;
                record.dirty = true;
            }
    }

    void TwinLink::openRecord (const juce::String& takeId, const MacStatus& status)
    {
        TakeRecord record;
        record.id = nextRecordId++;
        record.takeId = takeId;
        record.peerName = peerName;
        record.sessionDir = status.sessionDir;
        record.clockValid = clockValid;
        record.clockOffsetMs = clockOffsetMs;
        record.clockRttMs = clockRttMs;
        if (! linked) record.losses.push_back ({ wallClockMs(), 0 });
        records.push_back (std::move (record));
        while (records.size() > kMaxRecords) records.pop_front();
    }

    TwinLink::TakeRecord* TwinLink::openRecordOrNull()
    {
        return ! records.empty() && ! records.back().finished ? &records.back() : nullptr;
    }

    TwinLink::TakeRecord* TwinLink::recordById (juce::uint64 id)
    {
        for (auto& record : records)
            if (record.id == id) return &record;
        return nullptr;
    }

    TwinLink::TakeRecord* TwinLink::recordForTake (const juce::String& takeId)
    {
        for (auto it = records.rbegin(); it != records.rend(); ++it)
            if (it->takeId == takeId) return &*it;
        return nullptr;
    }

    void TwinLink::compareIfReady (TakeRecord& record)
    {
        if (! record.local.has_value() || ! record.peer.has_value() || record.compared) return;
        record.compared = true;
        const auto comparison = compareTakes (*record.local, *record.peer, config.role);
        compareLine = comparison.line;
        compareMismatch = comparison.mismatch;
        if (config.role == Role::Leader) host.twinNotify (compareLine);
    }

    void TwinLink::serviceReports()
    {
        // Never touch a session volume while a take is rolling.
        if (localRolling || localFinishing) return;
        bool busy = false;
        for (auto& record : records)
        {
            if (record.finished && record.dirty && ! record.jobInFlight)
            {
                record.dirty = false;
                record.jobInFlight = true;
                postReportJob ({ record.id, record.sessionDir, twinPass (record) });
            }
            busy = busy || record.jobInFlight;
        }
        if (! enabled && ! busy) records.clear();
    }

    juce::var TwinLink::twinPass (const TakeRecord& record) const
    {
        juce::Array<juce::var> losses;
        for (const auto& loss : record.losses)
            losses.add (makeObject ({
                { "from", juce::Time (loss.fromMs).toISO8601 (true) },
                { "to", loss.toMs > 0 ? juce::Time (loss.toMs).toISO8601 (true) : juce::String() },
                { "seconds", loss.toMs > 0 ? (double) (loss.toMs - loss.fromMs) / 1000.0 : 0.0 } }));

        auto pass = makeObject ({ { "takeId", record.takeId }, { "role", roleName (config.role) },
                                  { "peerName", record.peerName }, { "linkLosses", losses },
                                  { "peerReported", record.peer.has_value() } });
        auto* object = pass.getDynamicObject();
        if (record.clockValid)
        {
            object->setProperty ("clockOffsetMs", std::round (record.clockOffsetMs * 10.0) / 10.0);
            object->setProperty ("clockRttMs",    std::round (record.clockRttMs    * 10.0) / 10.0);
        }
        if (record.peer.has_value())
        {
            object->setProperty ("peerTracks", record.peer->tracks);
            object->setProperty ("peerFrames", record.peer->frames);
            object->setProperty ("peerSeconds", record.peer->seconds);
            object->setProperty ("peerMissedSamples", record.peer->missedSamples);
            object->setProperty ("peerFailures", record.peer->failures);
        }
        return pass;
    }

    void TwinLink::refreshWarnings()
    {
        const int previousCount = warnings.size();
        warnings.clear();
        if (! linked || ! peer.has_value()) return;
        const auto who = peerLabel();
        if (local.sampleRate > 0.0 && peer->sampleRate > 0.0
            && std::abs (local.sampleRate - peer->sampleRate) > 1.0)
            warnings.add ("SAMPLE RATE differs: " + juce::String (local.sampleRate, 0) + " Hz here, "
                          + juce::String (peer->sampleRate, 0) + " Hz on " + who);
        if (local.armedTracks != peer->armedTracks)
            warnings.add ("ARMED TRACKS differ: " + juce::String (local.armedTracks) + " here, "
                          + juce::String (peer->armedTracks) + " on " + who);
        const auto localSession = local.sessionName.substring (0, kNameChars);
        if (localSession != peer->sessionName)
            warnings.add ("SESSION NAME differs: \"" + localSession + "\" here, \""
                          + peer->sessionName + "\" on " + who);

        if (localRolling != peer->recording
            && juce::Time::getMillisecondCounter() - rollingChangedAtMs > (juce::uint32) kRollingGraceMs)
            warnings.add (who + (peer->recording ? " IS STILL RECORDING" : " IS NOT RECORDING"));
        if (peer->missedSamples > 0)
            warnings.add (who + " MISSED " + juce::String (peer->missedSamples) + " SAMPLES");
        if (peer->writeFailures > 0)
            warnings.add (who + " REPORTS " + juce::String (peer->writeFailures) + " WRITE FAILURE(S)");
        if (peer->recording && peer->backupHealth == "failed")
            warnings.add (who + " BACKUP FAILED");
        if (peer->recording && peer->diskMinutesLeft > 0 && peer->diskMinutesLeft < kLowDiskMinutes)
            warnings.add (who + " HAS " + juce::String (peer->diskMinutesLeft) + " DISK MINUTES LEFT");
        if (warnings.size() > previousCount) host.twinNotify ("TWIN: " + warnings[warnings.size() - 1]);
    }

    void TwinLink::queueBody (const juce::var& body)
    {
        const std::lock_guard<std::mutex> lock (queueLock);
        if (outbox.size() < kMaxQueued) outbox.push_back (juce::JSON::toString (body, true));
    }

    void TwinLink::publishStatus (const MacStatus& status)
    {
        const std::lock_guard<std::mutex> lock (queueLock);
        publishedStatus = status;
    }

    juce::String TwinLink::peerLabel() const
    {
        return config.role == Role::Leader ? "FOLLOWER" : "LEADER";
    }

    void TwinLink::tick()
    {
        drainEvents();
        if (! enabled && ! localRolling && ! localFinishing && records.empty()) return;

        const auto status = host.twinStatus();
        local = status;
        if (status.recording && ! localRolling)
        {
            localRolling = true;
            rollingChangedAtMs = juce::Time::getMillisecondCounter();
            if (enabled && openRecordOrNull() == nullptr)
                openRecord (juce::Uuid().toString(), status);
        }
        else if (! status.recording && localRolling)
            onLocalTakeEnded();

        if (localFinishing && ! status.recording && ! status.finalizing)
            onLocalTakeFinished();

        if (auto* record = openRecordOrNull())
            if (status.sessionDir != juce::File()) record->sessionDir = status.sessionDir;

        // Unsigned subtraction: the counter wraps on a Mac up for 49 days.
        const auto now = juce::Time::getMillisecondCounter();
        if (enabled && now - lastPublishMs >= (juce::uint32) (config.statusPeriodMs / 2))
        {
            publishStatus (status);
            lastPublishMs = now;
        }
        serviceReports();
        refreshWarnings();
    }

    void TwinLink::noteLocalTakeAdopted()
    {
        if (leaderTakeActive) return;
        adoptedTake = true;
        offerAdoptedTake();
    }

    void TwinLink::offerAdoptedTake()
    {
        if (! adoptedTake || ! linked) return;
        adoptedTake = false;
        if (config.role != Role::Leader || leaderTakeActive || ! host.twinStatus().recording) return;
        leaderTakeId = juce::Uuid().toString();
        leaderTakeActive = true;
        queueBody (makeObject ({ { "m", "start" }, { "take", leaderTakeId }, { "relink", true } }));
    }

    void TwinLink::noteLocalTakeStarted()
    {
        adoptedTake = false;
        if (! enabled || config.role != Role::Leader) return;
        leaderTakeId = juce::Uuid().toString();
        leaderTakeActive = true;
        localRolling = true;
        rollingChangedAtMs = juce::Time::getMillisecondCounter();
        const auto status = host.twinStatus();
        if (auto* record = openRecordOrNull()) record->takeId = leaderTakeId;
        else openRecord (leaderTakeId, status);

        if (linked)
            queueBody (makeObject ({ { "m", "start" }, { "take", leaderTakeId } }));
        else
        {
            lastEvent = "FOLLOWER NOT STARTED: the link is down; start it there";
            host.twinNotify (lastEvent);
        }
    }

    void TwinLink::noteLocalStopAccepted()
    {
        if (config.role != Role::Leader || ! leaderTakeActive) return;
        leaderTakeActive = false;
        if (linked)
            queueBody (makeObject ({ { "m", "stop" }, { "take", leaderTakeId } }));
        else
        {
            lastEvent = "FOLLOWER NOT STOPPED: the link is down; stop it there";
            host.twinNotify (lastEvent);
        }
    }

    void TwinLink::noteLocalMarker (const juce::String& name)
    {
        if (! linked || config.role != Role::Leader || ! leaderTakeActive) return;
        queueBody (makeObject ({ { "m", "marker" }, { "take", leaderTakeId },
                                 { "name", name.substring (0, kNameChars) } }));
    }

    Snapshot TwinLink::snapshot() const
    {
        Snapshot s;
        s.enabled = enabled;
        s.role = config.role;
        s.state = state;
        s.detail = detail;
        s.peerName = peerName;
        s.peer = peer;
        s.clockValid = clockValid;
        s.clockOffsetMs = clockOffsetMs;
        s.clockRttMs = clockRttMs;
        s.failedAuth = failedAuth.load();
        s.rejectedConnections = rejectedConnections.load();
        s.ignoredCommands = ignoredCommands;
        s.warnings = warnings;
        s.lastEvent = lastEvent;
        s.compareLine = compareLine;
        s.compareMismatch = compareMismatch;

        switch (state)
        {
            case LinkState::Off:
                break;
            case LinkState::Waiting:
                s.headline = localRolling ? "TWIN NOT LINKED" : "TWIN WAITING";
                s.level = localRolling ? 2 : 1;
                break;
            case LinkState::Linked:
                s.headline = warnings.isEmpty() ? juce::String ("TWIN OK")
                           : "TWIN ! " + juce::String (warnings.size())
                                 + (warnings.size() == 1 ? " WARNING" : " WARNINGS");
                s.level = warnings.isEmpty() ? 0 : 1;
                break;
            case LinkState::Lost:
                s.headline = "TWIN LOST";
                s.level = 2;
                break;
            case LinkState::Refused:
            case LinkState::Failed:
                s.headline = "TWIN ERROR";
                s.level = 2;
                break;
        }
        return s;
    }
}
