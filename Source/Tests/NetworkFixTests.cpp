// Network and console-link regressions. The JSON guard is checked against the
// real JUCE parser on a thread with a worker's stack, and every server that
// parses network text is driven over a real loopback socket, so a guard that
// falls out of step with the parser fails here the way it would on a show.

#include <juce_core/juce_core.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MidiControlSurface.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/OscRemote.h"
#include "../Audio/SceneFollow.h"
#include "../Network/CaptureIdentity.h"
#include "../Network/CaptureLink.h"
#include "../Network/CompanionServer.h"
#include "../Network/ConsoleLink.h"
#include "../Network/LineSocket.h"
#include "../Network/OscPacketReceiver.h"
#include "../Network/SessionMirror.h"
#include "../Network/TwinLink.h"
#include "../UI/TwinRecorderDialog.h"

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace zynforge
{
    namespace
    {
        // The stack macOS gives a plain std::thread, which is where the capture
        // link, the twin link and the companion workers parse.
        constexpr size_t kWorkerStackBytes = 512 * 1024;

        // Deep enough to overflow that stack if a single level reaches the parser.
        constexpr int kOverflowLevels = 20000;

        void runOnWorkerStack (const std::function<void()>& body)
        {
            struct Call
            {
                static void* run (void* arg) { (*static_cast<const std::function<void()>*> (arg)) (); return nullptr; }
            };
            pthread_attr_t attributes;
            pthread_attr_init (&attributes);
            pthread_attr_setstacksize (&attributes, kWorkerStackBytes);
            pthread_t thread;
            pthread_create (&thread, &attributes, &Call::run, const_cast<std::function<void()>*> (&body));
            pthread_join (thread, nullptr);
            pthread_attr_destroy (&attributes);
        }

        // Keeps bytes as they are: broken UTF-8 reaches the guard the way
        // pumpLines hands it over, unrepaired.
        juce::String lineOf (const std::string& bytes)
        {
            return juce::String::fromUTF8 (bytes.data(), (int) bytes.size());
        }

        std::string repeated (const std::string& unit, int count)
        {
            std::string out;
            out.reserve (unit.size() * (size_t) count);
            for (int i = 0; i < count; ++i) out += unit;
            return out;
        }

        int depthOf (const juce::var& value)
        {
            int deepest = 0;
            if (const auto* array = value.getArray())
            {
                for (const auto& item : *array) deepest = juce::jmax (deepest, depthOf (item));
                return deepest + 1;
            }
            if (const auto* object = value.getDynamicObject())
            {
                for (const auto& property : object->getProperties())
                    deepest = juce::jmax (deepest, depthOf (property.value));
                return deepest + 1;
            }
            return 0;
        }

        // Tokens the parser takes as one complete value, JSON or not.
        const char* const kValues[] = {
            "\"plain\"", "'plain'", "'\"'", "\"'\"", "\"\\\"\"", "'\\''", "'\\\\'", "\"\\\\\"",
            "\"[[[[\"", "'{{{{'", "\"]]\"", "'}}'", "\"\\u0041\"", "'\\u0022'", "\"\\uD83D\\uDE00\"",
            "\"\\/\"", "'\\\"'", "\"-\"", "'-'", "\"caf\xC3\xA9\"", "'\xE2\x82\xAC'",
            "0", "7", "-1", "- 2", "-\t3", "1.5", "-0.25", "1e5", "1E-5", "-2.5e+3", "true", "false", "null",
            // Not JSON, accepted anyway.
            "-\"", "-'", "-]", "-}", "-[", "-{", "-\\", "- \"", "-x",
            // Not UTF-8.
            "\"\xF1\"", "'\xE2'",
        };

        const char* const kGaps[] = {
            "", "", "", " ", "\n", "\t", "\r\n", "\xE0\x80\xA0",
            // Not UTF-8.
            "\xF0\x80\xA0\"",
        };

        template <size_t N>
        const char* pick (juce::Random& random, const char* const (&options)[N])
        {
            return options[(size_t) random.nextInt ((int) N)];
        }

        void addValue (std::string& out, juce::Random& random, int levels)
        {
            out += pick (random, kGaps);
            if (levels <= 0 || random.nextInt (12) == 0) { out += pick (random, kValues); return; }

            const bool array = random.nextBool();
            out += array ? "[" : "{";
            const int items = 1 + random.nextInt (3);
            const int nested = random.nextInt (items);
            for (int i = 0; i < items; ++i)
            {
                if (i > 0) out += ",";
                out += pick (random, kGaps);
                if (! array) out += "\"k" + std::to_string (i) + "\":";
                if (i == nested) addValue (out, random, levels - 1);
                else             out += pick (random, kValues);
                out += pick (random, kGaps);
            }
            out += array ? "]" : "}";
        }

        std::vector<std::string> hostileLines()
        {
            const auto deep = repeated ("[", kOverflowLevels);
            return {
                deep,
                repeated ("{\"a\":", kOverflowLevels),
                "['\"'," + deep,
                "[\"'\"," + deep,
                "['\\'','\"'," + deep,
                "[\"\\\"\"," + deep,
                "[-\"," + deep,
                "[-'," + deep,
                "[- \"," + deep,
                "[-\\," + deep,
                repeated ("[-],", kOverflowLevels),
                repeated ("[-},", kOverflowLevels),
                repeated ("{\"a\":-],\"b\":", kOverflowLevels),
                "[\"\xF1\"," + deep,
                "[\xF0\x80\xA0\"" + deep,
            };
        }

        juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
        {
            auto* o = new juce::DynamicObject();
            for (const auto& [name, value] : fields) o->setProperty (name, value);
            return juce::var (o);
        }

        void pumpMessages() { CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true); }

        juce::File freshDir()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-network-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        bool waitFor (const std::function<bool()>& done, int timeoutMs = 4000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done())
            {
                if (juce::Time::getMillisecondCounterHiRes() > end) return false;
                juce::Thread::sleep (2);
            }
            return true;
        }

        // A socket a test drives by hand.
        struct Peer
        {
            juce::StreamingSocket socket;
            juce::MemoryBlock scratch;
            juce::StringArray lines;

            bool send (const juce::String& line)
            {
                return linesocket::writeAll (socket, line + "\n",
                                             linesocket::Clock::now() + std::chrono::milliseconds (2000));
            }

            bool pump (int waitMs = 20)
            {
                return linesocket::pumpLines (socket, scratch, [this] (const juce::String& l) { lines.add (l); }, waitMs);
            }

            bool readLine (int timeoutMs = 1500)
            {
                return waitFor ([this] { return ! lines.isEmpty() || ! pump(); }, timeoutMs) && ! lines.isEmpty();
            }

            bool closedWithin (int timeoutMs)
            {
                return waitFor ([this] { return ! pump(); }, timeoutMs);
            }
        };

        class SilentHost final : public twin::TwinHost
        {
        public:
            twin::MacStatus twinStatus() override { return {}; }
            bool twinStartRecord (juce::String& refusal) override { refusal = "fixture"; return false; }
            void twinStopRecord (std::function<void (bool, juce::String)> done) override { done (false, "fixture"); }
            void twinDropMarker (const juce::String&) override {}
            void twinNotify (const juce::String&) override {}
        };
    }

    class NetworkFixTests final : public juce::UnitTest
    {
    public:
        NetworkFixTests() : juce::UnitTest ("Network fixes", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            testJsonGuard();
            testJsonRoutes();
            testMirrorLayout();
            testOscFlood();
            testConsoleState();
            testConsoleAddress();
            testStream();
            testMcu();
        }

    private:
        void testJsonGuard()
        {
            beginTest ("The depth guard never counts less nesting than the parser builds");
            {
                int accepted = 0, deep = 0;
                bool underCounted = false;
                for (int seed = 0; seed < 60000; ++seed)
                {
                    juce::Random random (seed + 1);
                    std::string text = random.nextBool() ? "[" : "{\"k\":";
                    const bool array = text[0] == '[';
                    addValue (text, random, 4 + random.nextInt (28));
                    text += array ? "]" : "}";

                    // Only shallow text reaches the parser here, on a full stack.
                    const auto line = lineOf (text);
                    const auto parsed = juce::JSON::parse (line);
                    if (parsed.isVoid()) continue;
                    ++accepted;
                    const int real = depthOf (parsed);
                    if (real > 16) ++deep;
                    if (linesocket::jsonDepthWithin (line, real - 1))
                    {
                        expect (false, "guard under-counts: real depth " + juce::String (real) + " in " + line);
                        underCounted = true;
                        break;
                    }
                }
                expect (underCounted || accepted > 20000, "the generator stopped producing parsable text");
                expect (underCounted || deep > 1000, "the generator stopped producing deep text");
            }

            beginTest ("Everything JUCE writes passes the guard at exactly its own depth");
            {
                juce::Random random (20261101);
                const std::function<juce::var (int)> make = [&] (int levels) -> juce::var
                {
                    const auto leaf = [&]() -> juce::var
                    {
                        switch (random.nextInt (6))
                        {
                            case 0:  return "name [with] 'quotes' \"and\" \\ {braces}";
                            case 1:  return juce::String::fromUTF8 ("caf\xC3\xA9 \xE2\x82\xAC -1 - \xF7\xBF\xBF\xBF");
                            case 2:  return -96.5;
                            case 3:  return -3;
                            case 4:  return 1.0e-7;
                            default: return true;
                        }
                    };
                    if (levels <= 0) return leaf();
                    if (random.nextBool())
                    {
                        juce::Array<juce::var> items;
                        items.add (leaf());
                        items.add (make (levels - 1));
                        return items;
                    }
                    return object ({ { "first", leaf() }, { "next", make (levels - 1) } });
                };
                for (int i = 0; i < 2000; ++i)
                {
                    const auto value = make (random.nextInt (linesocket::kMaxJsonDepth));
                    const auto line = juce::JSON::toString (value, true);
                    const int depth = depthOf (value);
                    expect (linesocket::jsonDepthWithin (line, depth), line);
                    expect (depth == 0 || ! linesocket::jsonDepthWithin (line, depth - 1), line);
                    expect (depth == 0 || juce::JSON::toString (linesocket::parseJson (line), true) == line, line);
                }
            }

            beginTest ("A line ending inside an escape or after a minus stops at its own end");
            {
                for (const auto* text : { "[-", "[- ", "[\"\\", "['\\", "[\"\xE2", "[1,-" })
                    expect (linesocket::parseJson (lineOf (text)).isVoid());
            }

            beginTest ("Hostile nesting on a worker's stack is refused, never parsed");
            {
                const auto lines = hostileLines();
                std::vector<char> refused (lines.size(), 0);
                runOnWorkerStack ([&]
                {
                    for (size_t i = 0; i < lines.size(); ++i)
                        refused[i] = linesocket::parseJson (lineOf (lines[i])).isVoid() ? 1 : 0;
                });
                for (size_t i = 0; i < lines.size(); ++i)
                    expect (refused[i] != 0, "line " + juce::String ((int) i) + " was not refused");
            }

            beginTest ("A worker survives random text in front of deep nesting");
            {
                static const std::string units[] = { "[", "{\"a\":", "[-],", "[-},", "['\"',", "[\"'\",", "[-\",", "[1,", "[[]," };
                std::vector<std::string> lines;
                juce::Random random (20261102);
                for (int i = 0; i < 1200; ++i)
                {
                    std::string text;
                    const int atoms = 1 + random.nextInt (5);
                    for (int a = 0; a < atoms; ++a)
                    {
                        text += "[";
                        text += pick (random, kGaps);
                        text += pick (random, kValues);
                        text += pick (random, kGaps);
                        text += ",";
                    }
                    text += repeated (units[(size_t) random.nextInt ((int) std::size (units))], kOverflowLevels);
                    lines.push_back (std::move (text));
                }
                std::atomic<int> survived { 0 };
                runOnWorkerStack ([&]
                {
                    for (const auto& text : lines)
                    {
                        const auto parsed = linesocket::parseJson (lineOf (text));
                        if (depthOf (parsed) <= linesocket::kMaxJsonDepth) ++survived;
                    }
                });
                expectEquals (survived.load(), (int) lines.size());
            }
        }

        void testJsonRoutes()
        {
            using namespace capture;

            beginTest ("A twin follower refuses hostile lines before authentication and stays up");
            {
                SilentHost host;
                twin::TwinLink follower (host);
                twin::Config config;
                config.role = twin::Role::Follower;
                config.port = 0;
                config.pairingCode = "7K3M-Q2XD-9HVB-T4WN";
                config.bindAddress = "127.0.0.1";
                config.localName = "mini";
                config.linkTimeoutMs = 1200;
                config.handshakeTimeoutMs = 600;
                config.reconnectMs = 50;
                config.statusPeriodMs = 40;
                juce::String error;
                expect (follower.start (config, error), error);
                expect (waitFor ([&] { follower.tick(); return follower.getBoundPort() > 0; }));

                // Each of these fits inside a follower's frame limit.
                const auto within = juce::String::repeatedString ("[", 16000);
                int attempts = 0;
                for (const auto& line : { "['\"'," + within, "[-\"," + within, "[\"'\"," + within })
                {
                    ++attempts;
                    Peer peer;
                    expect (peer.socket.connect ("127.0.0.1", follower.getBoundPort(), 1000));
                    expect (peer.readLine(), "no hello from the follower");
                    expect (peer.send (line));
                    expect (peer.closedWithin (2000), "a hostile line left the socket open");
                    expect (waitFor ([&] { follower.tick(); return follower.snapshot().failedAuth == attempts; }));
                }
                follower.stop();
            }

            beginTest ("A capture endpoint refuses hostile lines before authentication and stays up");
            {
                CaptureServer server;
                int port = -1;
                for (int p : { 49740, 49741, 49742, 49743 })
                    if (server.listen (p)) { port = p; break; }
                expect (port > 0, "no free capture port");
                if (port > 0)
                {
                    const auto within = juce::String::repeatedString ("[", 16000);
                    for (const auto& line : { within, "['\"'," + within, "[-\"," + within })
                    {
                        Peer peer;
                        expect (peer.socket.connect ("127.0.0.1", port, 1000));
                        expect (peer.readLine(), "no challenge from the endpoint");
                        peer.send (line);
                        expect (peer.closedWithin (3000), "a hostile line left the handshake open");
                    }

                    // Nothing past the handshake's size either, whatever it holds.
                    Peer flat;
                    expect (flat.socket.connect ("127.0.0.1", port, 1000));
                    expect (flat.readLine());
                    flat.send ("{\"pad\":\"" + juce::String::repeatedString ("x", 64 * 1024) + "\"}");
                    expect (flat.closedWithin (1000), "an oversize handshake line left the socket open");

                    CaptureClient client;
                    expect (client.connect ("127.0.0.1", port));
                    expect (client.hello (3000).ok, "a real client was locked out");
                    client.disconnect();
                }
                server.stop();
            }

            beginTest ("A capture client survives a hostile line from whoever answered its port");
            {
                juce::StreamingSocket listener;
                expect (listener.createListener (0, "127.0.0.1"));
                CaptureClient client;
                std::atomic<int> replies { 0 };
                client.onReply = [&] (const Reply&) { ++replies; };
                expect (client.connect ("127.0.0.1", listener.getBoundPort()));
                std::unique_ptr<juce::StreamingSocket> accepted (listener.waitForNextConnection());
                expect (accepted != nullptr);
                if (accepted != nullptr)
                {
                    const auto deadline = linesocket::Clock::now() + std::chrono::seconds (3);
                    const auto deep = juce::String::repeatedString ("[", 200000);
                    expect (linesocket::writeAll (*accepted, deep + "\n" + "['\"'," + deep + "\n", deadline));
                    // A refusal this client must act on, sent after both lines.
                    expect (linesocket::writeAll (*accepted, juce::JSON::toString (
                        object ({ { "type", "reply" }, { "ok", false } }), true) + "\n", deadline));
                    expect (waitFor ([&] { return replies.load() == 1; }), "the reader stopped at the hostile line");
                }
                client.disconnect();
            }

            beginTest ("A hostile mirror response on the message thread is refused and changes nothing");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                const juce::ScopeGuard cleanup { [&] { engine.clearAllStripOverrides(); } };
                engine.setStripCount (2);
                engine.setTrackName (0, "preserved");

                SessionMirror mirror (engine);
                mirror.host = "fixture-no-network";
                mirror.fetchActive.store (true);
                const auto deep = juce::String::repeatedString ("[", SessionMirror::kMaxPayloadBytes);
                for (const auto& payload : { deep, "['\"'," + deep, "[-\"," + deep })
                {
                    mirror.resultPayload = payload;
                    mirror.resultReady.store (true);
                    mirror.timerCallback();
                    expectEquals (engine.getRecorder().getNumTracks(), 2);
                    expect (mirror.getLastError().isNotEmpty());
                    expectEquals (mirror.getLastSyncMs(), (juce::int64) 0);
                }
            }
        }

        void testMirrorLayout()
        {
            beginTest ("A primary with nothing to report never empties the safety Mac");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                const juce::ScopeGuard cleanup { [&] { engine.clearAllStripOverrides(); } };
                engine.setStripCount (4);
                engine.setTrackName (0, "KICK");
                engine.setTrackInputRouting (2, 7);

                SessionMirror mirror (engine);
                mirror.host = "fixture-no-network";
                mirror.fetchActive.store (true);
                const auto offer = [&] (const EngineStatus& status)
                {
                    mirror.resultPayload = juce::JSON::toString (status.toJson());
                    mirror.resultReady.store (true);
                    mirror.timerCallback();
                };

                EngineStatus unavailable;
                unavailable.source = "daemon-unavailable";
                unavailable.statusAgeMs = -1;
                unavailable.recording = true;
                offer (unavailable);
                expectEquals (engine.getRecorder().getNumTracks(), 4);
                expect (mirror.getLastError().containsIgnoreCase ("unavailable"), mirror.getLastError());
                expectEquals (mirror.getLastSyncMs(), (juce::int64) 0);

                offer (EngineStatus {});
                expectEquals (engine.getRecorder().getNumTracks(), 4);
                expect (mirror.getLastError().containsIgnoreCase ("no channels"), mirror.getLastError());
                if (engine.getRecorder().getNumTracks() == 4)
                    expectEquals (engine.getRecorder().getTrack (2).inputRouting.load(), 7,
                                  "the strips were freed and regrown");

                EngineStatus real;
                real.numTracks = 2;
                for (const auto* name : { "VOX", "GTR" })
                {
                    TrackStatus track;
                    track.name = name;
                    track.colourARGB = 0xff225577;
                    real.tracks.push_back (track);
                }
                offer (real);
                expectEquals (engine.getRecorder().getNumTracks(), 2);
                expect (mirror.getLastError().isEmpty(), mirror.getLastError());
            }
        }

        void testOscFlood()
        {
            struct SlowListener final : juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>
            {
                std::atomic<int> heard { 0 };
                void oscMessageReceived (const juce::OSCMessage&) override { ++heard; juce::Thread::sleep (1); }
                void oscBundleReceived (const juce::OSCBundle&) override {}
            };
            struct Ticker final : juce::Timer
            {
                std::atomic<int> ticks { 0 };
                void timerCallback() override { ++ticks; }
            };

            beginTest ("An OSC flood is capped, and the message thread keeps its timers");
            {
                SlowListener listener;
                OscPacketReceiver receiver;
                receiver.addListener (&listener);
                int port = 0;
                for (int p = 47950; p < 47970 && port == 0; ++p)
                    if (receiver.connect (p)) port = p;
                expect (port != 0, "no free UDP port");
                if (port != 0)
                {
                    juce::DatagramSocket sender (false);
                    const char packet[] = "/x\0\0,\0\0\0";
                    const auto flooded = [&]
                    {
                        for (int i = 0; i < 64; ++i) sender.write ("127.0.0.1", port, packet, 8);
                        return receiver.getOverflowCount() > 0;
                    };
                    expect (waitFor (flooded, 5000), "the backlog never filled");
                    const auto dropped = receiver.getOverflowCount();
                    expectGreaterThan (dropped, (juce::int64) 0);

                    Ticker ticker;
                    ticker.startTimer (10);
                    const auto end = juce::Time::getMillisecondCounterHiRes() + 30000;
                    int ticksAtHalf = -1;
                    while (juce::Time::getMillisecondCounterHiRes() < end && listener.heard.load() < 2000)
                    {
                        pumpMessages();
                        if (ticksAtHalf < 0 && listener.heard.load() >= 1000) ticksAtHalf = ticker.ticks.load();
                    }
                    ticker.stopTimer();
                    expectGreaterThan (ticksAtHalf, 10, "timers stood behind the whole backlog");
                    receiver.disconnect();
                    expectLessOrEqual (listener.heard.load(), 2200, "more was queued than the cap allows");
                }
                receiver.removeListener (&listener);
            }

            beginTest ("Remote markers stop at a rate a person could ask for");
            {
                scenefollow::MarkerBudget budget;
                juce::uint32 now = 5000000;
                int granted = 0;
                for (int i = 0; i < 100; ++i) granted += budget.take (now) ? 1 : 0;
                expectEquals (granted, (int) scenefollow::MarkerBudget::kBurst);
                expect (! budget.take (now + 999));
                expect (budget.take (now + 1000));
                expect (! budget.take (now + 1001));
                granted = 0;
                now += 3600000;
                for (int i = 0; i < 100; ++i) granted += budget.take (now) ? 1 : 0;
                expectEquals (granted, (int) scenefollow::MarkerBudget::kBurst, "idle time must not bank more than a burst");

                const auto dir = freshDir();
                AudioEngine engine;
                engine.getMarkers().setContext (dir, 48000.0);
                engine.setEditCursorSample (48000);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);
                for (int i = 0; i < 500; ++i)
                    osc.dispatchForTest (juce::OSCMessage ("/Console/Marker"));
                expectLessOrEqual ((int) engine.getMarkers().getAll().size(), (int) scenefollow::MarkerBudget::kBurst + 1);
                expectGreaterThan ((int) engine.getMarkers().getAll().size(), 0);
                engine.getMarkers().clearContext();
                dir.deleteRecursively();
            }
        }

        // The four X32 input blocks replying with the given routing.
        static void answerPatchRead (ConsoleLink& link, int firstValue, int step = 1)
        {
            const char* addresses[] { "/config/routing/IN/1-8", "/config/routing/IN/9-16",
                                      "/config/routing/IN/17-24", "/config/routing/IN/25-32" };
            for (int b = 0; b < 4; ++b)
                link.injectReply (juce::OSCMessage (juce::OSCAddressPattern (addresses[b]),
                                                    (juce::int32) (firstValue + b * step)));
        }

        static std::vector<int> routingWrites (const std::vector<ConsoleMessage>& sent)
        {
            std::vector<int> values;
            for (const auto& m : sent)
                if (m.address.startsWith ("/config/routing/IN/") && m.hasArgs())
                    values.push_back (m.intArg());
            return values;
        }

        void testConsoleState()
        {
            using Patch = ConsoleLink::Patch;

            beginTest ("The show patch is on disk before the console moves, and survives a crash");
            {
                const auto dir = freshDir();
                const auto state = dir.getChildFile (ConsoleLink::kStateFileName);
                {
                    ConsoleLink link;
                    bool firstWriteSawSavedPatch = false;
                    int writes = 0;
                    link.setMessageHook ([&] (const ConsoleMessage& m)
                    {
                        if (m.hasArgs() && writes++ == 0)
                        {
                            const auto saved = juce::JSON::parse (state).getProperty ("stagePatch", {});
                            firstWriteSawSavedPatch = saved.isArray() && (int) saved[0] == 10;
                        }
                    });
                    link.setSessionDir (dir);
                    link.enterSoundcheck();
                    answerPatchRead (link, 10);
                    expect (link.getPatch() == Patch::Soundcheck);
                    expectEquals (writes, 4);
                    expect (firstWriteSawSavedPatch, "the desk moved before the show patch was saved");
                }

                // Relaunch, with the desk still on card returns.
                ConsoleLink after;
                std::vector<ConsoleMessage> sent;
                after.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                after.setSessionDir (dir);
                expect (after.getPatch() == Patch::Soundcheck, "the relaunch forgot the desk is on card returns");
                after.enterSoundcheck();
                expect (sent.empty(), "a second soundcheck read the card returns as the show patch");
                after.exitSoundcheck();
                expect (routingWrites (sent) == std::vector<int> ({ 10, 11, 12, 13 }));

                // Back on STAGE, and known to be, so the next soundcheck reads the desk afresh.
                ConsoleLink third;
                third.setMessageHook ([] (const ConsoleMessage&) {});
                third.setSessionDir (dir);
                expect (third.getPatch() == Patch::Stage);
                dir.deleteRecursively();
            }

            beginTest ("Restoring gains from the session keeps the patch stashed since");
            {
                const auto dir = freshDir();
                {
                    ConsoleLink earlier;
                    earlier.setMessageHook ([] (const ConsoleMessage&) {});
                    earlier.captureGains (1);
                    earlier.injectReply (juce::OSCMessage (juce::OSCAddressPattern ("/headamp/000/gain"), 0.5f));
                    expect (earlier.saveTo (dir));
                }
                ConsoleLink link;
                std::vector<ConsoleMessage> sent;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                link.setSessionDir (dir);
                link.enterSoundcheck();
                answerPatchRead (link, 4);
                expect (link.loadFrom (dir));
                link.restoreGains();
                sent.clear();
                link.exitSoundcheck();
                expect (routingWrites (sent) == std::vector<int> ({ 4, 5, 6, 7 }), "the live stash was replaced from disk");
                dir.deleteRecursively();
            }

            beginTest ("A desk already on card returns is never saved as the show patch");
            {
                ConsoleLink link;
                std::vector<ConsoleMessage> sent;
                juce::String status;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                link.onStatus = [&] (const juce::String& s) { status = s; };
                link.enterSoundcheck();
                sent.clear();
                answerPatchRead (link, link.getProfile().cardBlockFirst);
                expect (routingWrites (sent).empty());
                expect (! link.hasStagePatch());
                expect (status.containsIgnoreCase ("card returns"), status);
            }

            beginTest ("A show patch that can't be saved leaves the console where it is");
            {
                const auto dir = freshDir();
                ConsoleLink link;
                std::vector<ConsoleMessage> sent;
                juce::String status;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                link.onStatus = [&] (const juce::String& s) { status = s; };
                link.setSessionDir (dir);
                link.enterSoundcheck();
                sent.clear();
                dir.deleteRecursively();
                answerPatchRead (link, 10);
                expect (routingWrites (sent).empty());
                expect (link.getPatch() == Patch::Unknown);
                expect (status.containsIgnoreCase ("left as it was"), status);
            }

            beginTest ("Console gains and patch stay with their session");
            {
                const auto a = freshDir(), b = freshDir();
                ConsoleLink link;
                std::vector<ConsoleMessage> sent;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                link.setSessionDir (a);
                link.enterSoundcheck();
                answerPatchRead (link, 10);
                link.exitSoundcheck();
                link.captureGains (1);
                link.injectReply (juce::OSCMessage (juce::OSCAddressPattern ("/headamp/000/gain"), 0.5f));
                expect (link.isGainCaptureComplete());
                expect (link.saveTo (a));

                link.setSessionDir (b);
                expect (link.getCapturedGains().empty(), "gains followed the engineer into another session");
                expect (! link.hasStagePatch());
                sent.clear();
                link.restoreGains();
                link.exitSoundcheck();
                expect (sent.empty(), "another show's values reached the desk");

                link.setSessionDir (a);
                expectEquals ((int) link.getCapturedGains().size(), 1);

                link.setSessionDir ({});
                expect (link.getCapturedGains().empty());
                a.deleteRecursively();
                b.deleteRecursively();
            }

            beginTest ("A desk left on card returns keeps its way back across a session change");
            {
                const auto a = freshDir(), b = freshDir();
                ConsoleLink link;
                std::vector<ConsoleMessage> sent;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                link.setSessionDir (a);
                link.enterSoundcheck();
                answerPatchRead (link, 10);
                link.setSessionDir (b);
                expect (link.getPatch() == Patch::Soundcheck);
                sent.clear();
                link.exitSoundcheck();
                expect (routingWrites (sent) == std::vector<int> ({ 10, 11, 12, 13 }));
                a.deleteRecursively();
                b.deleteRecursively();
            }
        }

        void testConsoleAddress()
        {
            beginTest ("A console address is a number, never a name to look up");
            {
                ConsoleLink link;
                for (const auto* bad : { "192.168.1.300", "x32.local", "mixer", "", "192.168.1", "0.0.0.0", "255.255.255.255" })
                {
                    expect (! link.connect (bad), bad);
                    expect (! link.isConnected(), bad);
                }

                AudioEngine engine;
                expect (! engine.getRemotes().requestConsoleChannelNames ("x32.local", 10023));

                juce::DatagramSocket desk (false);
                expect (desk.bindToPort (0, "127.0.0.1"));
                expect (link.connect (" 127.000.000.001 ", desk.getBoundPort()));
                expectEquals (link.getHost(), juce::String ("127.0.0.1"));
                link.disconnect();
            }

            beginTest ("A console link hears only the console");
            {
                juce::DatagramSocket shared (false);
                expect (shared.bindToPort (0, "127.0.0.1"));

                struct Heard final : juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>
                {
                    int count { 0 };
                    void oscMessageReceived (const juce::OSCMessage&) override { ++count; }
                    void oscBundleReceived (const juce::OSCBundle&) override {}
                } heard;

                OscPacketReceiver receiver;
                receiver.addListener (&heard);
                juce::StringArray refused;
                receiver.onRefusedPacket = [&] (const OscPacketReceiver::Source&, const juce::String& what) { refused.add (what); };
                expect (receiver.connectToSocket (shared, "127.0.0.2"));

                juce::DatagramSocket other (false);
                const char packet[] = "/x\0\0,\0\0\0";
                expectEquals (other.write ("127.0.0.1", shared.getBoundPort(), packet, 8), 8);
                expect (waitFor ([&] { pumpMessages();
                                       return receiver.getDroppedSourceCount() == 1 && refused.size() == 1; }));
                expectEquals (heard.count, 0);
                expectEquals (receiver.getLastDroppedSource(), juce::String ("127.0.0.1"));

                expect (receiver.connectToSocket (shared, "127.0.0.1"));
                expectEquals (other.write ("127.0.0.1", shared.getBoundPort(), packet, 8), 8);
                expect (waitFor ([&] { pumpMessages(); return heard.count == 1; }));
                receiver.disconnect();
                receiver.removeListener (&heard);

                // And ConsoleLink pins its own socket to the desk it dialled.
                ConsoleLink link;
                juce::StringArray traffic;
                link.onTraffic = [&] (const juce::String& line) { traffic.add (line); };
                expect (link.connect ("127.0.0.2", 47998));
                auto* udp = dynamic_cast<OscUdpTransport*> (link.transport.get());
                expect (udp != nullptr);
                if (udp != nullptr)
                {
                    expectEquals (other.write ("127.0.0.1", udp->localPortForTests(), packet, 8), 8);
                    expect (waitFor ([&] { pumpMessages();
                                           return traffic.joinIntoString ("\n").contains ("pinned"); }),
                            traffic.joinIntoString ("\n"));
                }
                link.disconnect();
            }

            beginTest ("No Yamaha read sends a verb that can change the desk");
            {
                expect (yamahaScpDialect().subscribe == nullptr, "a scene-recall verb was sent as a subscription");
                for (const auto& profile : consoleProfiles())
                {
                    for (const auto& builder : { profile.dialect.probe, profile.dialect.subscribe })
                        if (builder)
                        {
                            const auto message = builder();
                            expect (! message.address.containsIgnoreCase ("recall"), message.address);
                            expect (! message.address.startsWithIgnoreCase ("set"), message.address);
                        }
                }
            }
        }

        void testStream()
        {
            beginTest ("A listener that stalls loses audio, not the stream, and stays in step");
            {
                AudioEngine engine;
                CompanionServer server (engine);
                int port = 0;
                for (int p = 19460; p < 19480 && port == 0; ++p)
                    if (server.start (p)) port = p;
                expect (port != 0, "companion server failed to start");
                if (port == 0) return;

                juce::StreamingSocket client;
                expect (client.connect ("127.0.0.1", port, 1000));
                const auto request = "GET /stream.wav?t=" + server.getAccessToken() + " HTTP/1.1\r\nHost: x\r\n\r\n";
                expectEquals (client.write (request.toRawUTF8(), (int) request.getNumBytesAsUTF8()),
                              (int) request.getNumBytesAsUTF8());

                std::atomic<bool> feeding { true };
                std::thread producer ([&]
                {
                    std::vector<float> left (480, 0.25f), right (480, -0.25f);
                    while (feeding.load())
                    {
                        server.feedStreamSamples (left.data(), right.data(), 480);
                        juce::Thread::sleep (1);
                    }
                });

                std::vector<juce::uint8> received;
                char chunk[8192];
                const auto readFor = [&] (size_t bytes, int timeoutMs)
                {
                    const auto target = received.size() + bytes;
                    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
                    while (received.size() < target && juce::Time::getMillisecondCounterHiRes() < end)
                    {
                        const int ready = client.waitUntilReady (true, 50);
                        if (ready < 0) return false;
                        if (ready == 0) continue;
                        const int got = client.read (chunk, sizeof (chunk), false);
                        if (got <= 0) return false;
                        received.insert (received.end(), chunk, chunk + got);
                    }
                    return received.size() >= target;
                };

                expect (readFor (64 * 1024, 5000), "the stream never started");
                juce::Thread::sleep (2500);   // long enough to fill every buffer on the way
                const bool alive = readFor (2 * 1024 * 1024, 15000);
                feeding.store (false);
                producer.join();
                expect (alive, "the server ended the stream on a stall");

                // Past the WAV header every frame must still be left then right.
                const juce::uint8 tag[] { 'd', 'a', 't', 'a' };
                const auto start = std::search (received.begin(), received.end(), std::begin (tag), std::end (tag));
                expect (start != received.end());
                if (start != received.end())
                {
                    auto offset = (size_t) (start - received.begin()) + 8;
                    const auto sampleAt = [&] (size_t i)
                    { return (juce::int16) (received[i] | (received[i + 1] << 8)); };
                    int bad = 0;
                    for (; offset + 4 <= received.size(); offset += 4)
                        if (sampleAt (offset) != 8191 || sampleAt (offset + 2) != -8191) ++bad;
                    expectEquals (bad, 0, "the stream came apart mid-frame");
                }
                client.close();
                server.stop();
            }
        }

        void testMcu()
        {
            beginTest ("A bank move clears what the old bank left on the surface");
            {
                AudioEngine engine;
                engine.setStripCount (12);
                engine.setTrackMuted (0, true);
                engine.setTrackSoloed (0, true);
                engine.setTrackArmed (0, true);
                engine.setTrackName (0, "KICK");

                MidiControlSurface surface (engine);
                std::vector<juce::MidiMessage> sent;
                surface.sendHookForTests = [&] (const juce::MidiMessage& m) { sent.push_back (m); };
                const auto wasSent = [&] (const juce::MidiMessage& wanted)
                {
                    for (const auto& m : sent)
                        if (m.getRawDataSize() == wanted.getRawDataSize()
                            && std::memcmp (m.getRawData(), wanted.getRawData(), (size_t) m.getRawDataSize()) == 0)
                            return true;
                    return false;
                };

                surface.timerCallback();
                expect (wasSent (mcu::led (mcu::Action::Mute, 0, true)));
                expect (wasSent (mcu::scribble (0, engine.getRecorder().getTrack (0).getNameThreadSafe())));

                surface.bankOffset.store (8);
                surface.forceRefresh.store (true);
                sent.clear();
                surface.timerCallback();
                expect (wasSent (mcu::led (mcu::Action::Mute, 0, false)), "the old bank's MUTE stayed lit");
                expect (wasSent (mcu::led (mcu::Action::Solo, 0, false)), "the old bank's SOLO stayed lit");
                expect (wasSent (mcu::led (mcu::Action::Arm,  0, false)), "the old bank's REC stayed lit");
                expect (wasSent (mcu::scribble (0, engine.getRecorder().getTrack (8).getNameThreadSafe())));
                expect (wasSent (mcu::scribble (4, {})), "an empty slot kept the old name");

                sent.clear();
                surface.timerCallback();
                expect (sent.size() < 4, "an idle tick resent the whole surface");
            }
        }

    };

    static NetworkFixTests networkFixTests;
}
