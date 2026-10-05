// Headless loopback tests for the capture transport (Source/Network/
// CaptureLink.{h,cpp}): a real CaptureServer + CaptureClient over 127.0.0.1
// exercise the Hello handshake, GUI->daemon commands, daemon->GUI status,
// and version rejection -- no daemon binary, no recorder.

#include <juce_core/juce_core.h>

#include "../Network/CaptureLink.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace zynforge
{
    class CaptureLinkTests final : public juce::UnitTest
    {
    public:
        CaptureLinkTests() : juce::UnitTest ("Capture link transport", "zynforge") {}

        // Poll a predicate up to timeoutMs (callbacks fire on reader threads).
        static bool waitUntil (std::function<bool()> pred, int timeoutMs)
        {
            for (int t = 0; t < timeoutMs; t += 10)
            {
                if (pred()) return true;
                juce::Thread::sleep (10);
            }
            return pred();
        }

        // Bring up a server on the first free port from a high candidate list.
        static int listenSomewhere (zynforge::capture::CaptureServer& server)
        {
            for (int p : { 49710, 49711, 49712, 49713, 49714, 49715 })
                if (server.listen (p)) return p;
            return -1;
        }

        void runTest() override
        {
            using namespace zynforge::capture;

            beginTest ("loopback: Hello handshake + command + status round-trip");
            {
                CaptureServer server;
                std::mutex rmx;
                std::vector<Command> received;
                server.onCommand = [&] (const Command& c)
                { const std::lock_guard<std::mutex> l (rmx); received.push_back (c); };

                const int port = listenSomewhere (server);
                expect (port > 0, "server failed to listen on any candidate port");
                if (port <= 0) return;

                CaptureClient client;
                std::mutex smx;
                EngineStatus lastStatus;
                std::atomic<int> statusCount { 0 };
                client.onStatus = [&] (const EngineStatus& s)
                { const std::lock_guard<std::mutex> l (smx); lastStatus = s; statusCount.fetch_add (1); };

                expect (client.connect ("127.0.0.1", port), "client failed to connect");
                expect (! server.hasClient(), "unverified socket became the active controller");

                // Hello handshake.
                const auto reply = client.hello (2000);
                expect (reply.ok, "hello was not acked ok");
                expect (waitUntil ([&] { return server.hasClient(); }, 2000), "server saw no authenticated client");
                expectEquals (reply.version, kProtocolVersion);
                expect (waitUntil ([&] { const std::lock_guard<std::mutex> l (rmx);
                                         return ! received.empty(); }, 2000),
                        "server never received the Hello command");
                {
                    const std::lock_guard<std::mutex> l (rmx);
                    if (! received.empty()) expect (received.front().action == Action::Hello);
                }

                // GUI -> daemon command.
                Command rec; rec.action = Action::StartRecording; rec.sessionDir = "/tmp/zf-show";
                expect (client.send (rec));
                expect (waitUntil ([&] { const std::lock_guard<std::mutex> l (rmx);
                                         return received.size() >= 2; }, 2000),
                        "server never received StartRecording");
                {
                    const std::lock_guard<std::mutex> l (rmx);
                    if (received.size() >= 2)
                    {
                        expect (received.back().action == Action::StartRecording);
                        expectEquals (received.back().sessionDir, juce::String ("/tmp/zf-show"));
                    }
                }

                // daemon -> GUI status push.
                EngineStatus st; st.recording = true; st.numTracks = 4; st.armedTracks = 2;
                expect (server.sendStatus (st), "server could not push status");
                expect (waitUntil ([&] { return statusCount.load() > 0; }, 2000),
                        "client never received status");
                {
                    const std::lock_guard<std::mutex> l (smx);
                    expect (lastStatus.recording);
                    expectEquals (lastStatus.numTracks, 4);
                    expectEquals (lastStatus.armedTracks, 2);
                }

                client.disconnect();
                server.stop();
            }

            beginTest ("Unauthenticated peer cannot publish positive replies, status, or supersession");
            {
                juce::StreamingSocket listener;
                const bool listening = listener.createListener (0, "127.0.0.1");
                expect (listening);
                if (listening)
                {
                    CaptureClient client;
                    std::atomic<int> positiveReplies { 0 }, diagnostics { 0 }, statuses { 0 };
                    client.onReply = [&] (const Reply& reply)
                    {
                        if (reply.ok) positiveReplies.fetch_add (1);
                        else if (reply.error == "fixture diagnostic") diagnostics.fetch_add (1);
                    };
                    client.onStatus = [&] (const EngineStatus&) { statuses.fetch_add (1); };
                    const bool connected = client.connect ("127.0.0.1", listener.getBoundPort());
                    expect (connected);
                    if (connected)
                    {
                        std::unique_ptr<juce::StreamingSocket> peer (listener.waitForNextConnection());
                        expect (peer != nullptr);
                        if (peer != nullptr)
                        {
                            Reply forged; forged.ok = forged.completed = true;
                            Reply diagnostic; diagnostic.error = "fixture diagnostic";
                            EngineStatus forgedStatus; forgedStatus.recording = true;
                            const auto payload = frame (forged.toJson())
                                               + frame (encodeStatus (forgedStatus))
                                               + frame (encodeBye ("superseded"))
                                               + frame (diagnostic.toJson());
                            expectEquals (peer->write (payload.toRawUTF8(), (int) payload.getNumBytesAsUTF8()),
                                          (int) payload.getNumBytesAsUTF8());
                            // This final diagnostic is a processing barrier for
                            // the preceding frames, not a timing-only sleep.
                            expect (waitUntil ([&] { return diagnostics.load() == 1; }, 1000),
                                    "pre-authentication diagnostic was not delivered");
                            expectEquals (positiveReplies.load(), 0);
                            expectEquals (statuses.load(), 0);
                            expect (! client.wasSuperseded());
                            expect (! client.hello (100).ok,
                                    "raw peer was trusted without endpoint authentication");
                            peer->close();
                        }
                    }
                    client.disconnect();
                    listener.close();
                }
            }

            beginTest ("loopback: a version-mismatched Hello is acked not-ok");
            {
                CaptureServer server;
                const int port = listenSomewhere (server);
                expect (port > 0);
                if (port <= 0) return;

                CaptureClient client;
                std::mutex mx;
                Reply got;
                std::atomic<bool> gotReply { false };
                client.onReply = [&] (const Reply& r)
                { const std::lock_guard<std::mutex> l (mx); got = r; gotReply.store (true); };

                expect (client.connect ("127.0.0.1", port));
                // Hand-built Hello from a "future" GUI.
                Command bad; bad.action = Action::Hello; bad.version = kProtocolVersion + 99;
                expect (client.send (bad));
                expect (waitUntil ([&] { return gotReply.load(); }, 2000), "no reply to bad Hello");
                {
                    const std::lock_guard<std::mutex> l (mx);
                    expect (! got.ok, "mismatched version should be rejected");
                    expect (got.error.isNotEmpty());
                }

                client.disconnect();
                server.stop();
            }

            beginTest ("loopback: commands are rejected until Hello succeeds");
            {
                CaptureServer server;
                std::atomic<int> delivered { 0 };
                server.onCommand = [&] (const Command&) { delivered.fetch_add (1); };
                const int port = listenSomewhere (server);
                expect (port > 0);
                if (port <= 0) return;

                CaptureClient client;
                expect (client.connect ("127.0.0.1", port));
                Command ping; ping.action = Action::Ping;
                const auto refused = client.request (ping, 2000);
                expect (! refused.ok, "pre-handshake command must be rejected");
                expect (refused.error.containsIgnoreCase ("handshake"));
                expectEquals (delivered.load(), 0,
                              "a rejected pre-handshake command reached the daemon");

                expect (client.hello (2000).ok);
                expect (client.send (ping));
                expect (waitUntil ([&] { return delivered.load() >= 2; }, 2000),
                        "Hello + post-handshake Ping were not delivered");
                client.disconnect();
                server.stop();
            }

            beginTest ("a second GUI connection supersedes the first (no accept-loop wedge)");
            {
                CaptureServer server;
                const int port = listenSomewhere (server);
                expect (port > 0);
                if (port <= 0) return;

                CaptureClient a;
                expect (a.connect ("127.0.0.1", port));
                expect (a.hello (2000).ok, "first client handshake failed");

                // Client A stays connected. B must still get through: the
                // server closes A's socket and serves B (the old code joined
                // A's reader first, wedging accept until A disconnected).
                CaptureClient b;
                expect (b.connect ("127.0.0.1", port));
                const auto reply = b.hello (3000);
                expect (reply.ok, "second client was never served -- accept loop wedged");

                a.disconnect();
                b.disconnect();
                server.stop();
            }

            beginTest ("server reports listening / port / no-client cleanly");
            {
                CaptureServer server;
                expect (! server.isListening());
                const int port = listenSomewhere (server);
                expect (port > 0);
                expect (server.isListening());
                expectEquals (server.getPort(), port);
                expect (! server.hasClient());
                expect (! server.sendStatus (EngineStatus{}), "sendStatus with no client should be a no-op false");
                server.stop();
                expect (! server.isListening());
            }
        }
    };

    static CaptureLinkTests captureLinkTests;
}
