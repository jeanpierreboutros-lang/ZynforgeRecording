// Twin Recorder link over real loopback sockets: two TwinLink endpoints in one
// process, each with a fake host, plus hand-driven sockets that misbehave and a
// relay that can act as a pulled cable. All socket work is real; only the
// engine is fake.

#include <juce_core/juce_core.h>

#include "../Audio/MultitrackRecorder.h"
#include "../Network/CaptureIdentity.h"
#include "../Network/CaptureProtocol.h"
#include "../Network/LineSocket.h"
#include "../Network/SecureToken.h"
#include "../Network/TwinLink.h"

#include <atomic>
#include <deque>
#include <functional>
#include <thread>
#include <vector>

namespace zynforge
{
    namespace
    {
        using namespace twin;
        namespace identity = capture::identity;

        const juce::String kCode = "7K3M-Q2XD-9HVB-T4WN";
        const juce::String kKey  = "7K3MQ2XD9HVBT4WN";    // the code as both ends hash it

        class FakeHost final : public TwinHost
        {
        public:
            MacStatus status;
            bool refuse { false }, holdStop { false };
            juce::String refusalText { "Arm at least one track" };
            int startCalls { 0 }, stopCalls { 0 };
            juce::StringArray markers, notices;
            std::function<void (bool, juce::String)> heldStop;

            MacStatus twinStatus() override { return status; }

            bool twinStartRecord (juce::String& refusal) override
            {
                ++startCalls;
                if (refuse) { refusal = refusalText; return false; }
                status.recording = true;
                return true;
            }

            void twinStopRecord (std::function<void (bool, juce::String)> done) override
            {
                ++stopCalls;
                status.recording = false;
                if (holdStop) { status.finalizing = true; heldStop = std::move (done); return; }
                done (true, {});
            }

            void twinDropMarker (const juce::String& name) override { markers.add (name); }
            void twinNotify (const juce::String& text) override     { notices.add (text); }

            bool noticed (const juce::String& text) const
            {
                for (const auto& notice : notices)
                    if (notice.containsIgnoreCase (text)) return true;
                return false;
            }
        };

        Config fastConfig (Role role, int port)
        {
            Config c;
            c.role = role;
            c.port = port;
            c.pairingCode = kCode;
            c.linkTimeoutMs = 1200;
            c.handshakeTimeoutMs = 600;
            c.reconnectMs = 50;
            c.statusPeriodMs = 40;
            if (role == Role::Follower) { c.bindAddress = "127.0.0.1"; c.localName = "mini"; }
            else                        { c.peerAddress = "127.0.0.1"; c.localName = "macbook"; }
            return c;
        }

        bool settle (const std::vector<TwinLink*>& links, const std::function<bool()>& done,
                     int timeoutMs = 4000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            for (;;)
            {
                for (auto* link : links) link->tick();
                if (done()) return true;
                if (juce::Time::getMillisecondCounterHiRes() > end) return false;
                juce::Thread::sleep (4);
            }
        }

        // Keeps `links` serviced for a fixed time: proves something did NOT happen.
        void run (const std::vector<TwinLink*>& links, int ms)
        {
            settle (links, [] { return false; }, ms);
        }

        bool hasWarning (const Snapshot& s, const juce::String& text)
        {
            for (const auto& warning : s.warnings)
                if (warning.containsIgnoreCase (text)) return true;
            return false;
        }

        // A socket the test drives by hand, speaking the wire format directly,
        // so it can do everything a well-behaved peer never would.
        struct RawPeer
        {
            std::unique_ptr<juce::StreamingSocket> socket;
            juce::MemoryBlock scratch;
            std::deque<juce::String> lines;
            juce::String sessionKey, sendTag { "L" }, receiveTag { "F" };
            juce::int64 sent { 0 };

            bool connect (int port)
            {
                socket = std::make_unique<juce::StreamingSocket>();
                return socket->connect ("127.0.0.1", port, 1000);
            }

            bool sendLine (const juce::String& line)
            {
                return linesocket::writeAll (*socket, line + "\n",
                                             linesocket::Clock::now() + std::chrono::milliseconds (1000));
            }

            bool sendJson (const juce::var& value) { return sendLine (juce::JSON::toString (value, true)); }

            juce::var read (int timeoutMs = 1500)
            {
                const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
                while (juce::Time::getMillisecondCounterHiRes() < end)
                {
                    if (! lines.empty())
                    {
                        const auto line = lines.front();
                        lines.pop_front();
                        return juce::JSON::parse (line);
                    }
                    if (! linesocket::pumpLines (*socket, scratch,
                            [this] (const juce::String& l) { lines.push_back (l); }, 20))
                        return {};
                }
                return {};
            }

            // True once the far end has closed this socket.
            bool closedWithin (int timeoutMs)
            {
                const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
                while (juce::Time::getMillisecondCounterHiRes() < end)
                {
                    lines.clear();
                    if (! linesocket::pumpLines (*socket, scratch,
                            [this] (const juce::String& l) { lines.push_back (l); }, 20))
                        return true;
                }
                return false;
            }

            static juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
            {
                auto* o = new juce::DynamicObject();
                for (const auto& [name, value] : fields) o->setProperty (name, value);
                return juce::var (o);
            }

            static juce::String transcript (const juce::String& followerNonce, const juce::String& leaderNonce)
            {
                return "zynforge-twin:1:" + followerNonce + ":" + leaderNonce;
            }

            juce::var leaderAuth (const juce::String& followerNonce, const juce::String& code,
                                  juce::String& leaderNonce)
            {
                leaderNonce = securetoken::generate();
                return object ({ { "type", "twin.auth" }, { "version", 1 }, { "role", "leader" },
                                 { "nonce", leaderNonce },
                                 { "proof", identity::authenticate (
                                       code, "leader:" + transcript (followerNonce, leaderNonce)) } });
            }

            // Plays the leader's half of the handshake with the right code.
            bool authenticateAsLeader (const juce::String& code)
            {
                const auto hello = read();
                const auto nonce = hello.getProperty ("nonce", "").toString();
                juce::String leaderNonce;
                if (! sendJson (leaderAuth (nonce, code, leaderNonce))) return false;
                const auto reply = read();
                sessionKey = identity::authenticate (code, "session:" + transcript (nonce, leaderNonce));
                return reply.getProperty ("type", "").toString() == "twin.accept"
                    && identity::equalProof (reply.getProperty ("proof", "").toString(),
                           identity::authenticate (code, "follower:" + transcript (nonce, leaderNonce)));
            }

            juce::String frameLine (const juce::String& body, juce::int64 sequence, const juce::String& key = {})
            {
                const auto mac = identity::authenticate (key.isNotEmpty() ? key : sessionKey,
                                                         sendTag + ":" + juce::String (sequence) + ":" + body);
                return juce::JSON::toString (object ({ { "type", "twin.frame" }, { "seq", sequence },
                                                       { "body", body }, { "mac", mac } }), true);
            }

            bool sendBody (const juce::var& body)
            {
                return sendLine (frameLine (juce::JSON::toString (body, true), ++sent));
            }

            static juce::String bodyOf (const juce::var& frame)
            {
                return frame.getProperty ("body", "").toString();
            }
        };

        // Plays a follower by hand, so a test can send a real leader anything.
        struct RawFollower
        {
            juce::StreamingSocket listener;
            RawPeer peer;

            int listen()
            {
                if (! listener.createListener (0, "127.0.0.1")) return -1;
                return listener.getBoundPort();
            }

            // Runs on its own thread: the leader under test needs tick() meanwhile.
            bool accept (const juce::String& code, int version = 1)
            {
                if (listener.waitUntilReady (true, 2000) <= 0) return false;
                peer.socket.reset (listener.waitForNextConnection());
                if (peer.socket == nullptr) return false;
                const auto nonce = securetoken::generate();
                peer.sendJson (RawPeer::object ({ { "type", "twin.hello" }, { "version", version },
                                                  { "role", "follower" }, { "nonce", nonce } }));
                if (version != 1) return true;
                const auto auth = peer.read();
                const auto leaderNonce = auth.getProperty ("nonce", "").toString();
                const auto text = RawPeer::transcript (nonce, leaderNonce);
                peer.sendTag = "F";
                peer.receiveTag = "L";
                peer.sessionKey = identity::authenticate (code, "session:" + text);
                return peer.sendJson (RawPeer::object ({ { "type", "twin.accept" },
                    { "proof", identity::authenticate (code, "follower:" + text) } }));
            }
        };

        // Sits between the endpoints. Stopping the forwarding is what a pulled
        // cable looks like: no FIN, no error, just silence on both sides.
        class Relay
        {
        public:
            int start (int targetPort)
            {
                target = targetPort;
                if (! listener.createListener (0, "127.0.0.1")) return -1;
                thread = std::thread ([this] { run(); });
                return listener.getBoundPort();
            }

            void pullCable()   { pulled.store (true); }
            void plugIn()      { dropPair.store (true); pulled.store (false); }

            ~Relay()
            {
                quit.store (true);
                listener.close();
                if (thread.joinable()) thread.join();
            }

        private:
            static bool forward (juce::StreamingSocket& from, juce::StreamingSocket& to)
            {
                const int ready = from.waitUntilReady (true, 2);
                if (ready < 0) return false;
                if (ready == 0) return true;
                char bytes[4096];
                const int count = from.read (bytes, sizeof (bytes), false);
                return count > 0 && to.write (bytes, count) == count;
            }

            void run()
            {
                std::unique_ptr<juce::StreamingSocket> near, far;
                while (! quit.load())
                {
                    if (near == nullptr)
                    {
                        if (pulled.load()) { juce::Thread::sleep (5); continue; }
                        if (listener.waitUntilReady (true, 5) <= 0) continue;
                        near.reset (listener.waitForNextConnection());
                        far = std::make_unique<juce::StreamingSocket>();
                        if (near == nullptr || ! far->connect ("127.0.0.1", target, 500))
                        { near.reset(); far.reset(); }
                        continue;
                    }
                    if (dropPair.exchange (false)) { near.reset(); far.reset(); continue; }
                    if (pulled.load()) { juce::Thread::sleep (5); continue; }
                    if (! forward (*near, *far) || ! forward (*far, *near))
                    { near.reset(); far.reset(); }
                }
            }

            juce::StreamingSocket listener;
            std::thread thread;
            int target { 0 };
            std::atomic<bool> quit { false }, pulled { false }, dropPair { false };
        };

        struct Pair
        {
            FakeHost leaderHost, followerHost;
            std::unique_ptr<TwinLink> leader { std::make_unique<TwinLink> (leaderHost) };
            std::unique_ptr<TwinLink> follower { std::make_unique<TwinLink> (followerHost) };
            std::vector<TwinLink*> both { leader.get(), follower.get() };

            Pair()
            {
                leaderHost.status.sessionName = followerHost.status.sessionName = "Show";
                leaderHost.status.sampleRate = followerHost.status.sampleRate = 48000.0;
                leaderHost.status.armedTracks = followerHost.status.armedTracks = 2;
            }

            bool linked() const
            {
                return leader->snapshot().state == LinkState::Linked
                    && follower->snapshot().state == LinkState::Linked
                    && leader->snapshot().peer.has_value()
                    && follower->snapshot().peer.has_value();
            }
        };

        juce::File tempSession (const juce::String& name)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-twin-" + name + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        void writeReport (const juce::File& dir, int tracks, juce::int64 frames, double seconds,
                          juce::int64 missed = 0)
        {
            juce::Array<juce::var> list;
            for (int i = 0; i < tracks; ++i)
            {
                juce::Array<juce::var> files;
                files.add ("Track_" + juce::String (i + 1).paddedLeft ('0', 2) + ".wav");
                list.add (RawPeer::object ({ { "index", i }, { "files", files } }));
            }
            const auto report = RawPeer::object ({ { "numTracks", tracks }, { "totalSamples", frames },
                                                   { "totalSeconds", seconds }, { "missedSamples", missed },
                                                   { "sha256Pending", false }, { "tracks", list } });
            dir.getChildFile ("session.report.json").replaceWithText (juce::JSON::toString (report, true));
        }

        juce::var twinBlock (const juce::File& dir)
        {
            return juce::JSON::parse (dir.getChildFile ("session.report.json")).getProperty ("twin", {});
        }
    }

    class TwinLinkTests final : public juce::UnitTest
    {
    public:
        TwinLinkTests() : juce::UnitTest ("Twin recorder link", "zynforge") {}

        bool bring (Pair& pair, int viaPort = 0, Config leaderConfig = {}, Config followerConfig = {})
        {
            juce::String error;
            auto f = followerConfig.pairingCode.isNotEmpty() ? followerConfig : fastConfig (Role::Follower, 0);
            expect (pair.follower->start (f, error), error);
            expect (settle (pair.both, [&] { return pair.follower->getBoundPort() > 0; }), "follower never bound");
            auto l = leaderConfig.pairingCode.isNotEmpty() ? leaderConfig : fastConfig (Role::Leader, 0);
            l.port = viaPort > 0 ? viaPort : pair.follower->getBoundPort();
            expect (pair.leader->start (l, error), error);
            const bool up = settle (pair.both, [&] { return pair.linked(); });
            expect (up, "the two endpoints never linked");
            return up;
        }

        // Leader's normal path: its take is rolling, then it tells the link.
        void startLeaderTake (Pair& pair)
        {
            pair.leaderHost.status.recording = true;
            pair.leader->noteLocalTakeStarted();
        }

        void testPairingCode()
        {
            beginTest ("A pairing code is 80 bits of OS entropy, shown for typing");
            {
                const auto code = generatePairingCode();
                expectEquals (code.length(), 19);
                expectEquals (normalizePairingCode (code).length(), 16);
                expect (code != generatePairingCode(), "two codes matched");

                const auto zeroes = generatePairingCode ([] (void* bytes, size_t size)
                { std::memset (bytes, 0, size); return true; });
                expectEquals (zeroes, juce::String ("0000-0000-0000-0000"));
                const auto ones = generatePairingCode ([] (void* bytes, size_t size)
                { std::memset (bytes, 0xff, size); return true; });
                expectEquals (ones, juce::String ("ZZZZ-ZZZZ-ZZZZ-ZZZZ"));
                expect (generatePairingCode ([] (void*, size_t) { return false; }).isEmpty(),
                        "a failed entropy source still produced a code");

                expectEquals (normalizePairingCode ("7k3m q2xd 9hvb t4wn"), juce::String ("7K3MQ2XD9HVBT4WN"));
                expectEquals (normalizePairingCode ("ooii-llll-0000-1111"), juce::String ("0011111100001111"));
                expect (normalizePairingCode ("7K3M-Q2XD-9HVB").isEmpty(), "short code accepted");
                expect (normalizePairingCode ("7K3M-Q2XD-9HVB-T4WNX").isEmpty(), "long code accepted");
                expect (normalizePairingCode ("7K3M-Q2XD-9HVB-T4WU").isEmpty(), "bad symbol accepted");
                expect (isIpv4Literal ("192.168.68.72"));
                expect (! isIpv4Literal ("mini.local"), "a name needs a blocking resolver");
                expect (! isIpv4Literal ("192.168.68"));
                expect (! isIpv4Literal ("192.168.68.300"));
            }
        }

        void testClockMath()
        {
            beginTest ("Clock offset: NTP arithmetic, smoothed, delayed samples discounted");
            {
                // Peer clock 250 ms ahead, 10 ms each way, peer holds 10 ms.
                const auto sample = clockSample (1000, 1260, 1270, 1030);
                expectWithinAbsoluteError (sample.offsetMs, 250.0, 1.0e-9);
                expectWithinAbsoluteError (sample.rttMs, 20.0, 1.0e-9);

                // A path slower one way bends only that sample.
                ClockOffsetEstimator estimator;
                expect (! estimator.hasEstimate());
                for (int i = 0; i < 15; ++i) estimator.add ({ 250.0, 2.0 });
                estimator.add ({ 900.0, 400.0 });
                expectWithinAbsoluteError (estimator.offsetMs(), 250.0, 1.0e-9);

                // A real change moves the figure gradually, not at once.
                ClockOffsetEstimator stepped;
                stepped.add ({ 100.0, 2.0 });
                stepped.add ({ 200.0, 2.0 });
                expect (stepped.offsetMs() > 100.0 && stepped.offsetMs() < 200.0, "not smoothed");
                for (int i = 0; i < 40; ++i) stepped.add ({ 200.0, 2.0 });
                expectWithinAbsoluteError (stepped.offsetMs(), 200.0, 0.01);

                ClockOffsetEstimator stepping;
                stepping.add ({ 50.0, -5.0 });
                expect (! stepping.hasEstimate(), "a negative round trip was believed");
            }

            beginTest ("Clock offset: a real link measures its own loopback");
            {
                Pair pair;
                if (! bring (pair)) return;
                expect (settle (pair.both, [&] { return pair.leader->snapshot().clockValid
                                                      && pair.follower->snapshot().clockValid; }));
                const auto a = pair.leader->snapshot(), b = pair.follower->snapshot();
                expect (std::abs (a.clockOffsetMs) < 50.0, juce::String (a.clockOffsetMs));
                expect (std::abs (b.clockOffsetMs) < 50.0, juce::String (b.clockOffsetMs));
                expect (a.clockRttMs >= 0.0 && a.clockRttMs < 200.0, juce::String (a.clockRttMs));
            }
        }

        void testAuthentication()
        {
            beginTest ("Auth success: both prove the code and learn each other's name");
            {
                Pair pair;
                if (! bring (pair)) return;
                expectEquals (pair.leader->snapshot().peerName, juce::String ("mini"));
                expectEquals (pair.follower->snapshot().peerName, juce::String ("macbook"));
                expectEquals (pair.follower->snapshot().failedAuth, 0);
                expectEquals (pair.leader->snapshot().headline, juce::String ("TWIN OK"));
            }

            beginTest ("Wrong code: refused, counted, and the leader stops trying");
            {
                Pair pair;
                juce::String error;
                expect (pair.follower->start (fastConfig (Role::Follower, 0), error), error);
                expect (settle (pair.both, [&] { return pair.follower->getBoundPort() > 0; }));
                auto config = fastConfig (Role::Leader, pair.follower->getBoundPort());
                config.pairingCode = "0000-0000-0000-0000";
                expect (pair.leader->start (config, error), error);
                expect (settle (pair.both, [&] { return pair.leader->snapshot().state == LinkState::Refused; }),
                        "the leader never saw the refusal");
                run (pair.both, 300);
                expectEquals (pair.follower->snapshot().failedAuth, 1);
                expect (pair.follower->snapshot().state == LinkState::Waiting, "a wrong code linked");
                expect (pair.leader->snapshot().detail.containsIgnoreCase ("pairing code"));
                expectEquals (pair.leader->snapshot().level, 2);
            }

            beginTest ("start() refuses a missing code or a name instead of an address");
            {
                FakeHost host;
                TwinLink link (host);
                juce::String error;
                auto config = fastConfig (Role::Leader, 9310);
                config.pairingCode = "nonsense";
                expect (! link.start (config, error));
                expect (error.containsIgnoreCase ("pairing code"), error);
                config = fastConfig (Role::Leader, 9310);
                config.peerAddress = "mini.local";
                expect (! link.start (config, error));
                expect (error.containsIgnoreCase ("IP address"), error);
                expect (! link.isEnabled());
                expectEquals (host.startCalls + host.stopCalls, 0);
            }

            beginTest ("Replay: a captured response is useless against a fresh nonce");
            {
                FakeHost host;
                TwinLink follower (host);
                juce::String error;
                expect (follower.start (fastConfig (Role::Follower, 0), error), error);
                expect (settle ({ &follower }, [&] { return follower.getBoundPort() > 0; }));

                juce::var captured;
                {
                    RawPeer first;
                    expect (first.connect (follower.getBoundPort()));
                    const auto nonce = first.read().getProperty ("nonce", "").toString();
                    juce::String leaderNonce;
                    captured = first.leaderAuth (nonce, kKey, leaderNonce);
                    expect (first.sendJson (captured));
                    expect (first.read().getProperty ("type", "").toString() == "twin.accept",
                            "the first, genuine response was refused");
                }
                expect (settle ({ &follower }, [&] { return follower.snapshot().state != LinkState::Linked; }));

                RawPeer second;
                expect (settle ({ &follower }, [&] { return second.connect (follower.getBoundPort()); }));
                const auto hello = second.read();
                expect (hello.isObject(), "no hello on the second connection");
                expect (second.sendJson (captured));
                expect (second.read().getProperty ("type", "").toString() == "twin.refused",
                        "a replayed response was accepted");
                expect (second.closedWithin (1000), "the socket stayed open after a failed proof");
                expect (settle ({ &follower }, [&] { return follower.snapshot().failedAuth == 1; }));
                expect (follower.snapshot().state != LinkState::Linked);
            }

            beginTest ("The leader's proof cannot be reflected back as the follower's");
            {
                FakeHost host;
                TwinLink leader (host);
                RawFollower raw;
                const int port = raw.listen();
                expect (port > 0);
                auto server = std::thread ([&]
                {
                    if (raw.listener.waitUntilReady (true, 2000) <= 0) return;
                    raw.peer.socket.reset (raw.listener.waitForNextConnection());
                    const auto nonce = securetoken::generate();
                    raw.peer.sendJson (RawPeer::object ({ { "type", "twin.hello" }, { "version", 1 },
                                                          { "role", "follower" }, { "nonce", nonce } }));
                    const auto auth = raw.peer.read();
                    raw.peer.sendJson (RawPeer::object ({ { "type", "twin.accept" },
                                                          { "proof", auth.getProperty ("proof", "") } }));
                });
                juce::String error;
                expect (leader.start (fastConfig (Role::Leader, port), error), error);
                expect (settle ({ &leader }, [&] { return leader.snapshot().state == LinkState::Refused; }),
                        "a reflected proof authenticated the follower");
                expect (leader.snapshot().detail.containsIgnoreCase ("prove"));
                server.join();
            }

            beginTest ("A different protocol version is refused plainly on both sides");
            {
                FakeHost host;
                TwinLink follower (host);
                juce::String error;
                expect (follower.start (fastConfig (Role::Follower, 0), error), error);
                expect (settle ({ &follower }, [&] { return follower.getBoundPort() > 0; }));
                RawPeer peer;
                expect (peer.connect (follower.getBoundPort()));
                peer.read();
                expect (peer.sendJson (RawPeer::object ({ { "type", "twin.auth" }, { "version", 99 } })));
                expectEquals (peer.read().getProperty ("reason", "").toString(), juce::String ("version"));

                FakeHost leaderHost;
                TwinLink leader (leaderHost);
                RawFollower raw;
                const int port = raw.listen();
                auto server = std::thread ([&] { raw.accept (kKey, 99); });
                expect (leader.start (fastConfig (Role::Leader, port), error), error);
                expect (settle ({ &leader }, [&] { return leader.snapshot().state == LinkState::Refused; }));
                expect (leader.snapshot().detail.containsIgnoreCase ("versions"));
                server.join();
            }
        }

        void testHostileInput()
        {
            const auto withFollower = [this] (const std::function<void (TwinLink&, FakeHost&)>& body)
            {
                FakeHost host;
                TwinLink follower (host);
                juce::String error;
                expect (follower.start (fastConfig (Role::Follower, 0), error), error);
                expect (settle ({ &follower }, [&] { return follower.getBoundPort() > 0; }));
                body (follower, host);
                expectEquals (host.startCalls + host.stopCalls, 0, "hostile input reached the host");
            };

            beginTest ("Garbage before hello closes the socket and is counted");
            withFollower ([this] (TwinLink& follower, FakeHost&)
            {
                RawPeer peer;
                expect (peer.connect (follower.getBoundPort()));
                expect (peer.sendLine ("GET / HTTP/1.1"));
                expect (peer.closedWithin (1500), "garbage left the socket open");
                expect (settle ({ &follower }, [&] { return follower.snapshot().failedAuth == 1; }));
            });

            beginTest ("No command is obeyed before authentication");
            withFollower ([this] (TwinLink& follower, FakeHost&)
            {
                RawPeer peer;
                expect (peer.connect (follower.getBoundPort()));
                peer.read();
                expect (peer.sendJson (RawPeer::object ({ { "m", "start" }, { "take", "x" } })));
                expect (peer.closedWithin (1500), "an unauthenticated command left the socket open");
                run ({ &follower }, 60);
            });

            beginTest ("Oversize frame before authentication");
            withFollower ([this] (TwinLink& follower, FakeHost&)
            {
                RawPeer peer;
                expect (peer.connect (follower.getBoundPort()));
                peer.read();
                juce::String huge;
                huge.preallocateBytes (kMaxFrameBytes + 2048);
                for (size_t i = 0; i < kMaxFrameBytes + 1024; ++i) huge << "a";
                peer.sendLine (huge);
                expect (peer.closedWithin (1500), "an oversize line left the socket open");
                expect (settle ({ &follower }, [&] { return follower.snapshot().failedAuth == 1; }));
            });

            beginTest ("Deep JSON before authentication never reaches the parser");
            withFollower ([this] (TwinLink& follower, FakeHost&)
            {
                RawPeer peer;
                expect (peer.connect (follower.getBoundPort()));
                peer.read();
                expect (peer.sendLine (juce::String::repeatedString ("[", 12000)));
                expect (peer.closedWithin (1500), "deep JSON left the socket open");
                expect (settle ({ &follower }, [&] { return follower.snapshot().failedAuth == 1; }));
            });

            beginTest ("A stalled peer before hello is dropped on a deadline and the port stays usable");
            withFollower ([this] (TwinLink& follower, FakeHost&)
            {
                RawPeer sleeper;
                expect (sleeper.connect (follower.getBoundPort()));
                expect (sleeper.closedWithin (2500), "a silent peer held the handshake open");
                expect (settle ({ &follower }, [&] { return follower.snapshot().failedAuth == 1; }));
                RawPeer next;
                expect (next.connect (follower.getBoundPort()));
                expect (next.authenticateAsLeader (kKey), "the port stopped answering after a stall");
            });

            beginTest ("An authenticated peer that stalls is dropped; tick never waits on it");
            {
                FakeHost host;
                host.status.recording = true;
                TwinLink follower (host);
                juce::String error;
                expect (follower.start (fastConfig (Role::Follower, 0), error), error);
                expect (settle ({ &follower }, [&] { return follower.getBoundPort() > 0; }));
                RawPeer peer;
                expect (peer.connect (follower.getBoundPort()));
                expect (peer.authenticateAsLeader (kKey));
                if (! settle ({ &follower }, [&] { return follower.snapshot().state == LinkState::Linked; }))
                {
                    expect (false, "the raw leader never linked");
                    return;
                }

                double worstTick = 0.0;
                const auto end = juce::Time::getMillisecondCounterHiRes() + 3000;
                while (follower.snapshot().state == LinkState::Linked
                       && juce::Time::getMillisecondCounterHiRes() < end)
                {
                    const auto before = juce::Time::getMillisecondCounterHiRes();
                    follower.tick();
                    worstTick = juce::jmax (worstTick, juce::Time::getMillisecondCounterHiRes() - before);
                    juce::Thread::sleep (4);
                }
                expect (follower.snapshot().state == LinkState::Lost, "a stalled peer was never dropped");
                expect (worstTick < 200.0, "tick waited on a stalled peer: " + juce::String (worstTick));
                expectEquals (host.stopCalls, 0, "a stalled peer ended the take");
                expect (host.status.recording);
            }

            beginTest ("After authentication: replayed, tampered, oversize and deep frames all end the link");
            {
                const auto attack = [this] (const std::function<void (RawPeer&)>& misbehave, int expectStarts)
                {
                    FakeHost host;
                    TwinLink follower (host);
                    juce::String error;
                    expect (follower.start (fastConfig (Role::Follower, 0), error), error);
                    expect (settle ({ &follower }, [&] { return follower.getBoundPort() > 0; }));
                    RawPeer peer;
                    expect (peer.connect (follower.getBoundPort()));
                    expect (peer.authenticateAsLeader (kKey));
                    misbehave (peer);
                    expect (settle ({ &follower }, [&] { return follower.snapshot().state == LinkState::Lost; }),
                            "the link survived a bad frame");
                    expectEquals (host.startCalls, expectStarts, "a bad frame reached the host");
                    expectEquals (follower.snapshot().failedAuth, 1);
                };

                const auto start = RawPeer::object ({ { "m", "start" }, { "take", "t1" } });
                attack ([&] (RawPeer& peer)        // the same signed frame, twice
                {
                    const auto line = peer.frameLine (juce::JSON::toString (start, true), 1);
                    peer.sendLine (line);
                    peer.sendLine (line);
                }, 1);
                attack ([&] (RawPeer& peer)        // a body changed after signing
                {
                    auto line = peer.frameLine (juce::JSON::toString (start, true), 1);
                    peer.sendLine (line.replace ("t1", "t2"));
                }, 0);
                attack ([&] (RawPeer& peer)        // signed with the code, not the session key
                {
                    peer.sendLine (peer.frameLine (juce::JSON::toString (start, true), 1, kKey));
                }, 0);
                attack ([&] (RawPeer& peer)        // a frame the follower itself could have sent
                {
                    peer.sendTag = "F";
                    peer.sendLine (peer.frameLine (juce::JSON::toString (start, true), 1));
                }, 0);
                attack ([&] (RawPeer& peer)
                {
                    juce::String huge;
                    huge.preallocateBytes (kMaxFrameBytes + 2048);
                    for (size_t i = 0; i < kMaxFrameBytes + 1024; ++i) huge << "a";
                    peer.sendLine (huge);
                }, 0);
                attack ([&] (RawPeer& peer)        // correctly signed, but nested past the limit
                {
                    peer.sendLine (peer.frameLine (juce::String::repeatedString ("[", 40), 1));
                }, 0);
            }

            beginTest ("Unknown fields and unknown messages are ignored");
            {
                FakeHost host;
                host.status.sessionName = "Show";
                TwinLink follower (host);
                juce::String error;
                expect (follower.start (fastConfig (Role::Follower, 0), error), error);
                expect (settle ({ &follower }, [&] { return follower.getBoundPort() > 0; }));
                RawPeer peer;
                expect (peer.connect (follower.getBoundPort()));
                expect (peer.authenticateAsLeader (kKey));
                expect (peer.sendBody (RawPeer::object ({ { "m", "fromTheFuture" }, { "x", 1 } })));
                expect (peer.sendBody (RawPeer::object ({ { "m", "status" }, { "session", "Show" },
                                                          { "name", "future" }, { "fromTheFuture", true } })));
                expect (settle ({ &follower }, [&] { return follower.snapshot().peerName == "future"; }),
                        "an unknown field broke status");
                expect (follower.snapshot().state == LinkState::Linked);
            }

            beginTest ("One peer at a time: a second caller never disturbs the link");
            {
                Pair pair;
                if (! bring (pair)) return;
                RawPeer intruder;
                expect (intruder.connect (pair.follower->getBoundPort()));
                expect (intruder.closedWithin (1500), "a second connection stayed open");
                expect (settle (pair.both, [&] { return pair.follower->snapshot().rejectedConnections == 1; }));
                run (pair.both, 100);
                expect (pair.linked(), "a second caller disturbed the link");
            }

            beginTest ("A port already in use is reported, not silently dead");
            {
                FakeHost hostA, hostB;
                TwinLink first (hostA), second (hostB);
                juce::String error;
                expect (first.start (fastConfig (Role::Follower, 0), error), error);
                expect (settle ({ &first }, [&] { return first.getBoundPort() > 0; }));
                expect (second.start (fastConfig (Role::Follower, first.getBoundPort()), error), error);
                expect (settle ({ &second }, [&] { return second.snapshot().state == LinkState::Failed; }),
                        "a second bind claimed to listen");
                expect (hostB.noticed ("could not listen"));
            }
        }

        void testTransport()
        {
            beginTest ("RECORD: the leader rolls first, then the follower starts through its own path");
            {
                Pair pair;
                if (! bring (pair)) return;
                expect (! pair.leaderHost.status.recording);
                startLeaderTake (pair);
                expectEquals (pair.followerHost.startCalls, 0, "the follower ran before the leader had rolled");
                expect (settle (pair.both, [&] { return pair.followerHost.startCalls == 1; }), "no start arrived");
                expect (pair.followerHost.status.recording);
                expect (settle (pair.both, [&] { return pair.leader->snapshot().lastEvent == "Follower is recording"; }));
                expectEquals (pair.leaderHost.startCalls, 0, "the link started the leader");
                run (pair.both, 300);
                expect (! hasWarning (pair.leader->snapshot(), "NOT RECORDING"));
            }

            beginTest ("RECORD with a dead follower: nothing waits and the leader is told");
            {
                FakeHost host;
                host.status.sessionName = "Show";
                TwinLink leader (host);
                juce::String error;
                auto config = fastConfig (Role::Leader, 9);   // discard port: nothing there
                expect (leader.start (config, error), error);
                host.status.recording = true;
                const auto before = juce::Time::getMillisecondCounterHiRes();
                leader.noteLocalTakeStarted();
                expect (juce::Time::getMillisecondCounterHiRes() - before < 100.0, "the leader waited for the link");
                expect (host.noticed ("FOLLOWER NOT STARTED"));
                expect (host.status.recording);
                leader.tick();
                expectEquals (leader.snapshot().headline, juce::String ("TWIN NOT LINKED"));
                expectEquals (leader.snapshot().level, 2);
            }

            beginTest ("Follower refusal: its own text reaches the leader, and the leader keeps rolling");
            {
                Pair pair;
                if (! bring (pair)) return;
                pair.followerHost.refuse = true;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.leaderHost.noticed ("FOLLOWER REFUSED RECORD"); }));
                expect (pair.leaderHost.noticed (pair.followerHost.refusalText));
                expect (pair.leaderHost.status.recording);
                expect (! pair.followerHost.status.recording);
                expect (settle (pair.both, [&] { return hasWarning (pair.leader->snapshot(), "FOLLOWER IS NOT RECORDING"); },
                                5000));
            }

            beginTest ("STOP: nothing is sent until the leader's STOP is accepted; the follower reports completion");
            {
                Pair pair;
                if (! bring (pair)) return;
                pair.followerHost.holdStop = true;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));

                run (pair.both, 150);            // a first tap arms; the take rolls on
                expectEquals (pair.followerHost.stopCalls, 0);

                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                expect (settle (pair.both, [&] { return pair.followerHost.stopCalls == 1; }), "stop never arrived");
                expect (pair.leader->snapshot().lastEvent != "Follower stopped",
                        "the leader claimed completion before the follower finalized");
                pair.followerHost.status.finalizing = false;
                pair.followerHost.heldStop (true, {});
                expect (settle (pair.both, [&] { return pair.leader->snapshot().lastEvent == "Follower stopped"; }));
                expectEquals (pair.leaderHost.stopCalls, 0, "the link stopped the leader");
            }

            beginTest ("A follower that cannot finish cleanly says so on the leader");
            {
                Pair pair;
                if (! bring (pair)) return;
                pair.followerHost.holdStop = true;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                expect (settle (pair.both, [&] { return pair.followerHost.stopCalls == 1; }));
                pair.followerHost.heldStop (false, "backup audio write failed");
                expect (settle (pair.both, [&] { return pair.leaderHost.noticed ("backup audio write failed"); }));
            }

            beginTest ("A take ended by anything but a confirmed STOP leaves the follower rolling");
            {
                Pair pair;
                if (! bring (pair)) return;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));
                pair.leaderHost.status.recording = false;   // the interface died
                expect (settle (pair.both, [&] { return pair.leaderHost.noticed ("follower keeps recording"); }));
                pair.leader->noteLocalStopAccepted();       // and a late STOP cannot follow
                run (pair.both, 200);
                expectEquals (pair.followerHost.stopCalls, 0);
                expect (pair.followerHost.status.recording);
                expect (settle (pair.both, [&] { return hasWarning (pair.leader->snapshot(), "IS STILL RECORDING"); }, 5000));

                // The operator restarts the leader; its STOP reaches the follower again.
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.leaderHost.noticed ("now linked"); }));
                expectEquals (pair.followerHost.startCalls, 1);
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                expect (settle (pair.both, [&] { return pair.followerHost.stopCalls == 1; }));
            }

            beginTest ("The follower never ends a take the link did not start");
            {
                Pair pair;
                if (! bring (pair)) return;
                pair.followerHost.status.recording = true;   // started by hand on the follower
                run (pair.both, 60);
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.leaderHost.noticed ("will not end it"); }));
                expectEquals (pair.followerHost.startCalls, 0);
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                expect (settle (pair.both, [&] { return pair.leaderHost.noticed ("started on the follower"); }));
                expectEquals (pair.followerHost.stopCalls, 0);
                expect (pair.followerHost.status.recording);
            }

            beginTest ("Follower-local STOP keeps working and never reaches the leader");
            {
                Pair pair;
                if (! bring (pair)) return;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));
                pair.followerHost.status.recording = false;
                expect (settle (pair.both, [&] { return hasWarning (pair.leader->snapshot(), "FOLLOWER IS NOT RECORDING"); },
                                5000));
                expect (pair.leaderHost.status.recording);
                expectEquals (pair.leaderHost.stopCalls, 0);
            }

            beginTest ("A punch on the leader never reaches the follower");
            {
                Pair pair;
                if (! bring (pair)) return;
                pair.leaderHost.status.recording = true;   // no noteLocalTakeStarted: the host calls it for takes only
                run (pair.both, 150);
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                run (pair.both, 150);
                expectEquals (pair.followerHost.startCalls, 0);
                expectEquals (pair.followerHost.stopCalls, 0);
            }

            beginTest ("The follower cannot command the leader");
            {
                FakeHost host;
                host.status.sessionName = "Show";
                host.status.recording = true;
                TwinLink leader (host);
                RawFollower raw;
                const int port = raw.listen();
                std::atomic<bool> accepted { false };
                auto server = std::thread ([&] { accepted.store (raw.accept (kKey)); });
                juce::String error;
                expect (leader.start (fastConfig (Role::Leader, port), error), error);
                expect (settle ({ &leader }, [&] { return leader.snapshot().state == LinkState::Linked; }));
                server.join();
                expect (accepted.load());
                expect (raw.peer.sendBody (RawPeer::object ({ { "m", "start" }, { "take", "evil" } })));
                expect (raw.peer.sendBody (RawPeer::object ({ { "m", "stop" }, { "take", "evil" } })));
                expect (raw.peer.sendBody (RawPeer::object ({ { "m", "marker" }, { "name", "evil" } })));
                expect (settle ({ &leader }, [&] { return leader.snapshot().ignoredCommands == 3; }));
                expectEquals (host.startCalls + host.stopCalls, 0);
                expect (host.markers.isEmpty());
                expect (host.status.recording);
            }

            beginTest ("Markers relay during a take, at the follower's own position, and only then");
            {
                Pair pair;
                if (! bring (pair)) return;
                pair.leader->noteLocalMarker ("before the take");
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));
                const auto accented = juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9 \xe2\x99\xab"));
                pair.leader->noteLocalMarker ("Chorus");
                pair.leader->noteLocalMarker (accented);
                expect (settle (pair.both, [&] { return pair.followerHost.markers.size() == 2; }));
                expectEquals (pair.followerHost.markers[0], juce::String ("Chorus"));
                expectEquals (pair.followerHost.markers[1], accented, "a signed frame mangled UTF-8");
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                pair.leader->noteLocalMarker ("after the take");
                expect (settle (pair.both, [&] { return pair.followerHost.stopCalls == 1; }));
                run (pair.both, 80);
                expectEquals (pair.followerHost.markers.size(), 2);
            }

            beginTest ("Mismatches warn on both Macs, and never refuse anything");
            {
                Pair pair;
                pair.followerHost.status.sampleRate = 44100.0;
                pair.followerHost.status.armedTracks = 3;
                pair.followerHost.status.sessionName = "Other";
                if (! bring (pair)) return;
                for (auto* link : pair.both)
                {
                    expect (settle (pair.both, [&] { return link->snapshot().warnings.size() >= 3; }));
                    const auto s = link->snapshot();
                    expect (hasWarning (s, "SAMPLE RATE"));
                    expect (hasWarning (s, "ARMED TRACKS"));
                    expect (hasWarning (s, "SESSION NAME"));
                    expectEquals (s.level, 1);
                    expect (s.headline.startsWith ("TWIN !"), s.headline);
                }
                expect (pair.leaderHost.noticed ("differs") || pair.leaderHost.noticed ("differ"));
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }), "a mismatch refused a take");
                pair.followerHost.status.sampleRate = 48000.0;
                expect (settle (pair.both, [&] { return ! hasWarning (pair.leader->snapshot(), "SAMPLE RATE"); }),
                        "a warning outlived its cause");
            }

            beginTest ("Each Mac sees the other's health");
            {
                Pair pair;
                if (! bring (pair)) return;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));
                pair.followerHost.status.missedSamples = 64;
                pair.followerHost.status.writeFailures = 1;
                pair.followerHost.status.diskMinutesLeft = 12;
                pair.followerHost.status.backupHealth = "failed";
                expect (settle (pair.both, [&] { return pair.leader->snapshot().warnings.size() >= 4; }));
                const auto s = pair.leader->snapshot();
                expect (hasWarning (s, "MISSED 64"));
                expect (hasWarning (s, "WRITE FAILURE"));
                expect (hasWarning (s, "12 DISK MINUTES"));
                expect (hasWarning (s, "BACKUP FAILED"));
                expect (s.peer.has_value() && s.peer->missedSamples == 64);
            }
        }

        void testLinkLoss()
        {
            beginTest ("Link loss mid-take leaves both takes rolling; reconnect starts nothing");
            {
                Pair pair;
                Relay relay;
                juce::String error;
                expect (pair.follower->start (fastConfig (Role::Follower, 0), error), error);
                expect (settle (pair.both, [&] { return pair.follower->getBoundPort() > 0; }));
                const int port = relay.start (pair.follower->getBoundPort());
                expect (port > 0);
                expect (pair.leader->start (fastConfig (Role::Leader, port), error), error);
                expect (settle (pair.both, [&] { return pair.linked(); }));

                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));

                relay.pullCable();
                expect (settle (pair.both, [&] { return pair.leader->snapshot().state == LinkState::Lost
                                                     && pair.follower->snapshot().state == LinkState::Lost; }),
                        "a pulled cable was never noticed");
                for (auto* link : pair.both)
                {
                    expectEquals (link->snapshot().headline, juce::String ("TWIN LOST"));
                    expectEquals (link->snapshot().level, 2);
                }
                expect (pair.leaderHost.noticed ("TWIN LOST") && pair.followerHost.noticed ("TWIN LOST"));
                expect (pair.leaderHost.status.recording, "the leader's take was touched");
                expect (pair.followerHost.status.recording, "the follower's take was touched");
                expectEquals (pair.followerHost.stopCalls, 0, "link loss stopped a take");
                expectEquals (pair.leaderHost.stopCalls, 0, "link loss stopped a take");

                relay.plugIn();
                expect (settle (pair.both, [&] { return pair.linked(); }), "the link did not come back");
                expect (pair.followerHost.status.recording);
                expectEquals (pair.followerHost.startCalls, 1, "reconnecting started something");
                expectEquals (pair.leaderHost.startCalls, 0, "reconnecting started something");
                expectEquals (pair.followerHost.stopCalls, 0);

                // Status resumes on the new connection.
                pair.followerHost.status.missedSamples = 7;
                expect (settle (pair.both, [&] { const auto s = pair.leader->snapshot();
                                                 return s.peer.has_value() && s.peer->missedSamples == 7; }),
                        "status did not resume");

                // The take the link started is still the link's to stop.
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                expect (settle (pair.both, [&] { return pair.followerHost.stopCalls == 1; }));
            }

            beginTest ("A STOP pressed during link loss is never delivered later");
            {
                Pair pair;
                Relay relay;
                juce::String error;
                expect (pair.follower->start (fastConfig (Role::Follower, 0), error), error);
                expect (settle (pair.both, [&] { return pair.follower->getBoundPort() > 0; }));
                const int port = relay.start (pair.follower->getBoundPort());
                expect (pair.leader->start (fastConfig (Role::Leader, port), error), error);
                expect (settle (pair.both, [&] { return pair.linked(); }));
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));

                relay.pullCable();
                expect (settle (pair.both, [&] { return pair.leader->snapshot().state == LinkState::Lost; }));
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();
                expect (pair.leaderHost.noticed ("FOLLOWER NOT STOPPED"));
                relay.plugIn();
                expect (settle (pair.both, [&] { return pair.linked(); }));
                run (pair.both, 200);
                expectEquals (pair.followerHost.stopCalls, 0, "a stale STOP ended a later take");
                expect (pair.followerHost.status.recording);
                expect (settle (pair.both, [&] { return hasWarning (pair.leader->snapshot(), "IS STILL RECORDING"); }, 5000));
            }

            beginTest ("A STOP queued just as the link died never reaches the next connection");
            {
                Pair pair;
                Relay relay;
                juce::String error;
                expect (pair.follower->start (fastConfig (Role::Follower, 0), error), error);
                expect (settle (pair.both, [&] { return pair.follower->getBoundPort() > 0; }));
                const int port = relay.start (pair.follower->getBoundPort());
                expect (pair.leader->start (fastConfig (Role::Leader, port), error), error);
                expect (settle (pair.both, [&] { return pair.linked(); }));
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));

                // The leader's worker sees the loss; its message thread has not
                // yet, so it still believes the link is up when STOP lands.
                relay.pullCable();
                expect (settle ({ pair.follower.get() },
                                [&] { return pair.follower->snapshot().state == LinkState::Lost; }));
                juce::Thread::sleep (200);
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();

                relay.plugIn();
                expect (settle (pair.both, [&] { return pair.linked(); }));
                run (pair.both, 200);
                expectEquals (pair.followerHost.stopCalls, 0, "a STOP crossed onto a new connection");
                expect (pair.followerHost.status.recording);
            }

            beginTest ("Quit with a take rolling: bounded, and the take is left alone");
            {
                Pair pair;
                if (! bring (pair)) return;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));

                const auto before = juce::Time::getMillisecondCounterHiRes();
                pair.leader.reset();
                expect (juce::Time::getMillisecondCounterHiRes() - before < 1000.0, "quit waited on the link");
                expectEquals (pair.leaderHost.stopCalls, 0);
                expect (pair.leaderHost.status.recording);
                pair.both = { pair.follower.get() };
                expect (settle (pair.both, [&] { return pair.follower->snapshot().state == LinkState::Lost; }));
                expect (pair.followerHost.status.recording, "a peer quitting ended the take");
                expectEquals (pair.followerHost.stopCalls, 0);

                const auto beforeFollower = juce::Time::getMillisecondCounterHiRes();
                pair.follower.reset();
                expect (juce::Time::getMillisecondCounterHiRes() - beforeFollower < 1000.0);
                expectEquals (pair.followerHost.stopCalls, 0);
            }

            beginTest ("Quit while connecting to nothing is bounded too");
            {
                FakeHost host;
                auto leader = std::make_unique<TwinLink> (host);
                juce::String error;
                auto config = fastConfig (Role::Leader, 9310);
                config.peerAddress = "192.0.2.1";    // reserved for documentation; never answers
                expect (leader->start (config, error), error);
                juce::Thread::sleep (30);
                const auto before = juce::Time::getMillisecondCounterHiRes();
                leader.reset();
                expect (juce::Time::getMillisecondCounterHiRes() - before < 1500.0, "quit waited on a connect");
            }

            beginTest ("A STOP that completes after the link object has gone is harmless");
            {
                FakeHost host;
                auto follower = std::make_unique<TwinLink> (host);
                host.holdStop = true;
                juce::String error;
                expect (follower->start (fastConfig (Role::Follower, 0), error), error);
                expect (settle ({ follower.get() }, [&] { return follower->getBoundPort() > 0; }));
                RawPeer peer;
                expect (peer.connect (follower->getBoundPort()));
                expect (peer.authenticateAsLeader (kKey));
                expect (peer.sendBody (RawPeer::object ({ { "m", "start" }, { "take", "t1" } })));
                expect (settle ({ follower.get() }, [&] { return host.status.recording; }));
                expect (peer.sendBody (RawPeer::object ({ { "m", "stop" }, { "take", "t1" } })));
                const bool stopped = settle ({ follower.get() }, [&] { return host.stopCalls == 1; });
                expect (stopped, "the stop never reached the host");
                follower.reset();
                if (stopped) host.heldStop (true, {});
            }

            beginTest ("A start still queued when the link is switched off starts nothing");
            {
                Pair pair;
                if (! bring (pair)) return;
                pair.leaderHost.status.recording = true;
                pair.leader->noteLocalTakeStarted();
                // Give the frame time to arrive, without letting the follower act on it.
                juce::Thread::sleep (150);
                pair.follower->stop();
                expectEquals (pair.followerHost.startCalls, 0, "switching off ran a queued command");
            }

            beginTest ("Disabling the link never touches a take");
            {
                Pair pair;
                if (! bring (pair)) return;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));
                pair.follower->stop();
                expect (pair.follower->getBoundPort() < 0, "the port stayed bound while disabled");
                expect (pair.follower->snapshot().state == LinkState::Off);
                expect (pair.follower->snapshot().headline.isEmpty());
                expect (settle (pair.both, [&] { return pair.leader->snapshot().state == LinkState::Lost; }));
                expect (pair.followerHost.status.recording);
                expectEquals (pair.followerHost.stopCalls, 0);
            }
        }

        void testReports()
        {
            beginTest ("Both reports gain a twin block; a compare line lands on the leader");
            {
                Pair pair;
                const auto a = tempSession ("leader"), b = tempSession ("follower");
                pair.leaderHost.status.sessionDir = a;
                pair.followerHost.status.sessionDir = b;
                Relay relay;
                juce::String error;
                expect (pair.follower->start (fastConfig (Role::Follower, 0), error), error);
                expect (settle (pair.both, [&] { return pair.follower->getBoundPort() > 0; }));
                const int port = relay.start (pair.follower->getBoundPort());
                expect (pair.leader->start (fastConfig (Role::Leader, port), error), error);
                expect (settle (pair.both, [&] { return pair.linked()
                                                     && pair.leader->snapshot().clockValid
                                                     && pair.follower->snapshot().clockValid; }));
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));

                relay.pullCable();
                expect (settle (pair.both, [&] { return pair.leader->snapshot().state == LinkState::Lost
                                                     && pair.follower->snapshot().state == LinkState::Lost; }));
                relay.plugIn();
                expect (settle (pair.both, [&] { return pair.linked(); }));
                expect (settle (pair.both, [&] { return pair.leader->snapshot().clockValid; }));

                writeReport (a, 2, 480000, 10.0);
                writeReport (b, 2, 479520, 9.99);
                pair.leaderHost.status.recording = false;
                pair.leader->noteLocalStopAccepted();

                expect (settle (pair.both, [&]
                {
                    return twinBlock (a).getProperty ("peerReported", false)
                        && twinBlock (b).getProperty ("peerReported", false);
                }, 6000), "the twin blocks never completed");

                for (const auto& dir : { a, b })
                {
                    const auto block = twinBlock (dir);
                    expect (block.getProperty ("takeId", "").toString().isNotEmpty());
                    expectEquals ((int) block.getProperty ("peerTracks", 0), 2);
                    auto* losses = block.getProperty ("linkLosses", {}).getArray();
                    expect (losses != nullptr && losses->size() == 1, "link-loss interval missing");
                    if (losses != nullptr && losses->size() == 1)
                    {
                        expect (losses->getReference (0).getProperty ("from", "").toString().isNotEmpty());
                        expect (losses->getReference (0).getProperty ("to", "").toString().isNotEmpty());
                        expect ((double) losses->getReference (0).getProperty ("seconds", 0.0) > 0.0);
                    }
                    expect (block.hasProperty ("clockOffsetMs"), "no clock offset in the report");
                }
                expectEquals (twinBlock (a).getProperty ("role", "").toString(), juce::String ("leader"));
                expectEquals (twinBlock (b).getProperty ("role", "").toString(), juce::String ("follower"));
                expectEquals (twinBlock (a).getProperty ("peerName", "").toString(), juce::String ("mini"));
                expectEquals (twinBlock (b).getProperty ("peerName", "").toString(), juce::String ("macbook"));
                expectEquals ((juce::int64) twinBlock (a).getProperty ("peerFrames", 0), (juce::int64) 479520);
                expectEquals ((juce::int64) twinBlock (b).getProperty ("peerFrames", 0), (juce::int64) 480000);
                expectEquals (twinBlock (a).getProperty ("takeId", "").toString(),
                              twinBlock (b).getProperty ("takeId", "").toString());

                // The rest of the report is untouched.
                const auto report = juce::JSON::parse (a.getChildFile ("session.report.json"));
                expectEquals ((int) report.getProperty ("numTracks", 0), 2);
                expectEquals ((juce::int64) report.getProperty ("totalSamples", 0), (juce::int64) 480000);

                expect (settle (pair.both, [&] { return pair.leader->snapshot().compareLine.isNotEmpty(); }));
                const auto s = pair.leader->snapshot();
                expect (s.compareLine.contains ("2 / 2 tracks"), s.compareLine);
                expect (! s.compareMismatch, s.compareLine);
                expect (pair.leaderHost.noticed ("Leader / follower"));
                a.deleteRecursively();
                b.deleteRecursively();
            }

            beginTest ("A mismatch between the two finished takes gets a plain warning");
            {
                TakeSummary here, peer;
                here.reportAvailable = peer.reportAvailable = true;
                here.tracks = 32; peer.tracks = 30;
                here.seconds = 3600.0; peer.seconds = 3500.0;
                peer.missedSamples = 128; peer.failures = 1;
                const auto result = compareTakes (here, peer, Role::Leader);
                expect (result.mismatch);
                expect (result.line.contains ("32 / 30 tracks"), result.line);
                expect (result.line.contains ("1:00:00 / 0:58:20"), result.line);
                expect (result.line.contains ("missed 0 / 128"), result.line);
                expect (result.line.contains ("WARNING"), result.line);
                expect (result.line.contains ("track counts differ"), result.line);
                expect (result.line.contains ("durations differ"), result.line);

                peer = here;
                peer.seconds = here.seconds - 0.5;   // ordinary start/stop latency
                expect (! compareTakes (here, peer, Role::Follower).mismatch);
                peer.reportAvailable = false;
                expect (compareTakes (here, peer, Role::Leader).mismatch);
            }

            beginTest ("A report summary counts tracks that wrote audio, and every failure flag");
            {
                const auto dir = tempSession ("summary");
                writeReport (dir, 3, 960, 0.02, 5);
                auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
                auto* object = report.getDynamicObject();
                object->setProperty ("backupFailed", true);
                object->setProperty ("mirrorsSkipped", 2);
                const auto summary = summaryFromReport (report);
                expect (summary.reportAvailable);
                expectEquals (summary.tracks, 3);
                expectEquals (summary.frames, (juce::int64) 960);
                expectEquals (summary.missedSamples, (juce::int64) 5);
                expectEquals (summary.failures, 2);
                expect (! summaryFromReport ({}).reportAvailable);
                dir.deleteRecursively();
            }

            beginTest ("Passes merge by take: same take replaces, a new take keeps the old one");
            {
                juce::DynamicObject::Ptr report (new juce::DynamicObject());
                report->setProperty ("numTracks", 2);
                mergeTwinPass (*report, RawPeer::object ({ { "takeId", "A" }, { "peerFrames", 1 } }));
                mergeTwinPass (*report, RawPeer::object ({ { "takeId", "A" }, { "peerFrames", 2 } }));
                expectEquals ((int) report->getProperty ("twin").getProperty ("peerFrames", 0), 2);
                expect (! report->getProperty ("twin").hasProperty ("previousPasses"));

                mergeTwinPass (*report, RawPeer::object ({ { "takeId", "B" }, { "peerFrames", 3 } }));
                const auto twinNow = report->getProperty ("twin");
                expectEquals (twinNow.getProperty ("takeId", "").toString(), juce::String ("B"));
                auto* previous = twinNow.getProperty ("previousPasses", {}).getArray();
                expect (previous != nullptr && previous->size() == 1);

                // A late answer about A lands on A, wherever it now sits.
                mergeTwinPass (*report, RawPeer::object ({ { "takeId", "A" }, { "peerFrames", 9 } }));
                const auto after = report->getProperty ("twin");
                expectEquals (after.getProperty ("takeId", "").toString(), juce::String ("B"));
                previous = after.getProperty ("previousPasses", {}).getArray();
                expect (previous != nullptr && previous->size() == 1);
                if (previous != nullptr && previous->size() == 1)
                    expectEquals ((int) previous->getReference (0).getProperty ("peerFrames", 0), 9);
                expectEquals ((int) report->getProperty ("numTracks"), 2);
            }

            beginTest ("Nothing touches a session folder while a take is rolling or finalizing");
            {
                Pair pair;
                const auto a = tempSession ("quiet");
                pair.leaderHost.status.sessionDir = a;
                if (! bring (pair)) return;
                startLeaderTake (pair);
                expect (settle (pair.both, [&] { return pair.followerHost.status.recording; }));
                writeReport (a, 2, 480, 0.01);
                const auto file = a.getChildFile ("session.report.json");
                const auto written = file.loadFileAsString();

                run (pair.both, 120);
                expectEquals (file.loadFileAsString(), written, "the report changed during a take");

                pair.leaderHost.status.recording = false;
                pair.leaderHost.status.finalizing = true;
                pair.leader->noteLocalStopAccepted();
                run (pair.both, 120);
                expectEquals (file.loadFileAsString(), written, "the report changed during finalization");

                pair.leaderHost.status.finalizing = false;
                expect (settle (pair.both, [&] { return twinBlock (a).isObject(); }), "the block never landed");
                a.deleteRecursively();
            }

            beginTest ("With the link down the report still says what is known");
            {
                FakeHost host;
                host.status.sessionName = "Show";
                const auto dir = tempSession ("alone");
                host.status.sessionDir = dir;
                TwinLink leader (host);
                juce::String error;
                expect (leader.start (fastConfig (Role::Leader, 9), error), error);
                host.status.recording = true;
                leader.noteLocalTakeStarted();
                run ({ &leader }, 40);
                writeReport (dir, 1, 100, 0.01);
                host.status.recording = false;
                leader.noteLocalStopAccepted();
                expect (settle ({ &leader }, [&] { return twinBlock (dir).isObject(); }));
                const auto block = twinBlock (dir);
                expect (! (bool) block.getProperty ("peerReported", true));
                auto* losses = block.getProperty ("linkLosses", {}).getArray();
                expect (losses != nullptr && losses->size() == 1, "a take with no link recorded no loss");
                if (losses != nullptr && losses->size() == 1)
                    expect (losses->getReference (0).getProperty ("to", "x").toString().isEmpty(),
                            "an unresolved loss claimed an end");
                dir.deleteRecursively();
            }

            const auto makeTake = [this] (MultitrackRecorder& recorder, const juce::File& dir)
            {
                recorder.prepare (48000.0, 256, 1);
                recorder.getTrack (0).armed.store (true);
                expect (recorder.startRecording (dir));
                std::vector<float> audio (256, 0.1f);
                const float* input = audio.data();
                for (int block = 0; block < 8; ++block) recorder.processBlock (&input, 1, 256);
            };

            beginTest ("A twin block added during hashing survives the final report");
            {
                const auto dir = tempSession ("hash");
                juce::WaitableEvent atPublish, resume, finished;
                {
                    MultitrackRecorder recorder;
                    makeTake (recorder, dir);
                    auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
                    hooks->beforePublish = [&] { atPublish.signal(); resume.wait (3000); };
                    hooks->finished = [&] { finished.signal(); };
                    recorder.reportTestHooks = hooks;
                    recorder.stopRecording();
                    expect (atPublish.wait (5000), "the hash pass never reached its publish");
                    expect ((bool) juce::JSON::parse (dir.getChildFile ("session.report.json"))
                                .getProperty ("sha256Pending", false));
                    expect (MultitrackRecorder::editSessionReport (dir, [] (juce::DynamicObject& report)
                    {
                        mergeTwinPass (report, RawPeer::object ({ { "takeId", "A" } }));
                    }));
                    resume.signal();
                    expect (finished.wait (5000));
                }
                const auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
                expect (! (bool) report.getProperty ("sha256Pending", true), "the hashed report never landed");
                expectEquals (report.getProperty ("twin", {}).getProperty ("takeId", "").toString(),
                              juce::String ("A"), "the final report dropped the twin block");
                dir.deleteRecursively();
            }

            beginTest ("A continued take keeps the session's twin history");
            {
                const auto dir = tempSession ("continue");
                {
                    MultitrackRecorder recorder;
                    makeTake (recorder, dir);
                    recorder.stopRecording();
                }
                expect (MultitrackRecorder::editSessionReport (dir, [] (juce::DynamicObject& report)
                {
                    mergeTwinPass (report, RawPeer::object ({ { "takeId", "A" } }));
                }));
                {
                    MultitrackRecorder recorder;
                    recorder.prepare (48000.0, 256, 1);
                    recorder.getTrack (0).armed.store (true);
                    recorder.armContinue (2048);
                    expect (recorder.startRecording (dir));
                    std::vector<float> audio (256, 0.1f);
                    const float* input = audio.data();
                    for (int block = 0; block < 8; ++block) recorder.processBlock (&input, 1, 256);
                    recorder.stopRecording();
                }
                expectEquals (twinBlock (dir).getProperty ("takeId", "").toString(), juce::String ("A"),
                              "a continued report lost the earlier twin block");
                dir.deleteRecursively();
            }

            beginTest ("editSessionReport leaves a missing or unreadable report alone");
            {
                const auto dir = tempSession ("missing");
                bool called = false;
                expect (! MultitrackRecorder::editSessionReport (dir, [&] (juce::DynamicObject&) { called = true; }));
                dir.getChildFile ("session.report.json").replaceWithText ("not json");
                expect (! MultitrackRecorder::editSessionReport (dir, [&] (juce::DynamicObject&) { called = true; }));
                expect (! called);
                expectEquals (dir.getChildFile ("session.report.json").loadFileAsString(), juce::String ("not json"));
                dir.deleteRecursively();
            }
        }

        void runTest() override
        {
            testPairingCode();
            testClockMath();
            testAuthentication();
            testHostileInput();
            testTransport();
            testLinkLoss();
            testReports();
        }
    };

    static TwinLinkTests twinLinkTests;
}
