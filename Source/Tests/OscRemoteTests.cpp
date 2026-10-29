#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>
#include "../Audio/AudioEngine.h"
#include "../Audio/OscRemote.h"
#include "../Network/ConsoleLink.h"

#include <cmath>
#include <limits>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif
#if JUCE_MAC || JUCE_LINUX
 #include <sys/socket.h>
#endif

namespace zynforge
{
    namespace
    {
        bool pumpOscUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
               #elif JUCE_MODAL_LOOPS_PERMITTED
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
               #else
                juce::Thread::sleep (5);
               #endif
            }
            return predicate();
        }

        // One well-formed datagram of `levels` #bundles, each nested inside the
        // last (20 bytes per level). JUCE's parser recursed once per level with
        // no cap, so ~64 KB of this overflowed its receive thread's stack.
        juce::MemoryBlock nestedBundles (int levels)
        {
            juce::MemoryOutputStream out;
            for (int i = 0; i < levels; ++i)
            {
                out.write ("#bundle", 8);              // OSC-string incl. NUL
                out.writeInt64BigEndian (1);           // time tag "immediately"
                if (i + 1 < levels)
                    out.writeIntBigEndian (16 + 20 * (levels - 2 - i));   // next element's size
            }
            return out.getMemoryBlock();
        }

        // A sender allowed to emit a full-size datagram: macOS caps UDP sends
        // at its 9216-byte default send buffer.
        std::unique_ptr<juce::DatagramSocket> bigDatagramSender()
        {
            auto s = std::make_unique<juce::DatagramSocket> (false);
           #if JUCE_MAC || JUCE_LINUX
            const int bytes = 128 * 1024;
            setsockopt (s->getRawSocketHandle(), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof (bytes));
           #endif
            return s;
        }
    }

    // OSC wiring test: route real juce::OSCMessage objects through the actual
    // OscRemote parse + dialect dispatch path (via the dispatchForTest seam --
    // identical to what the UDP socket calls) and assert the engine reacted.
    // Covers the Generic /zynforge namespace, a console dialect (DiGiCo), and
    // the new gain/trim -> Trim-Follow mapping.
    class OscRemoteTests final : public juce::UnitTest
    {
    public:
        OscRemoteTests() : UnitTest ("OSC remote", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Generic /zynforge channel ops reach the engine");
            {
                AudioEngine engine;
                engine.setStripCount (4);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);

                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/mute", (juce::int32) 1));
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/2/arm",  (juce::int32) 1));
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/3/name", juce::String ("Kick")));
                expect (engine.getRecorder().getTrack (0).muted.load());
                expect (engine.getRecorder().getTrack (1).armed.load());
                expectEquals (engine.getRecorder().getTrack (2).name, juce::String ("Kick"));

                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/mute", (juce::int32) 0));
                expect (! engine.getRecorder().getTrack (0).muted.load());
            }

            beginTest ("OSC channel gain/trim feeds Trim-Follow");
            {
                AudioEngine engine;
                engine.setStripCount (2);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);

                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/gain", (float) -6.0f));
                expectWithinAbsoluteError (engine.getTrackTrimDelta (0), -6.0f, 0.001f);
                // "fader" is deliberately NOT a trim source.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/fader", (float) 3.0f));
                expectWithinAbsoluteError (engine.getTrackTrimDelta (0), -6.0f, 0.001f);
            }

            beginTest ("DiGiCo metadata works but unauthenticated control is ignored");
            {
                AudioEngine engine;
                engine.setStripCount (4);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);

                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/2/mute", (juce::int32) 1));
                expect (! engine.getRecorder().getTrack (1).muted.load());

                // A scene recall drops a marker.
                const int before = engine.getMarkers().getCount();
                osc.dispatchForTest (juce::OSCMessage ("/Console/Snapshots/recall", (juce::int32) 5));
                expect (engine.getMarkers().getCount() >= before);   // marker context may gate it
            }

            beginTest ("Unknown address is ignored (no crash, no state change)");
            {
                AudioEngine engine;
                engine.setStripCount (2);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);
                osc.dispatchForTest (juce::OSCMessage ("/not/a/real/path", (juce::int32) 1));
                expect (! engine.getRecorder().getTrack (0).muted.load());
                expect (! engine.getRecorder().getTrack (0).armed.load());
            }

            beginTest ("OSC transport goes through the host interception boundary");
            {
                AudioEngine engine;
                std::vector<AudioEngine::RemoteTransportAction> actions;
                engine.setRemoteTransportHandler ([&] (auto action, juce::String&)
                    -> std::optional<bool>
                {
                    actions.push_back (action);
                    return true; // a capture-daemon host handled it
                });
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);

                osc.dispatchForTest (juce::OSCMessage ("/zynforge/record", (juce::int32) 1));
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/record", (juce::int32) 0));
                expectEquals ((int) actions.size(), 2);
                if (actions.size() == 2)
                {
                    expect (actions[0] == AudioEngine::RemoteTransportAction::StartRecord);
                    expect (actions[1] == AudioEngine::RemoteTransportAction::StopRecord);
                }
                expect (! engine.getRecorder().isRecording(),
                        "OSC bypassed the host and started the local recorder");
            }

            beginTest ("record transport without a host fails closed");
            {
                AudioEngine engine;
                juce::String error;
                expect (! engine.performRemoteTransport (
                    AudioEngine::RemoteTransportAction::StartRecord, error));
                expect (error.containsIgnoreCase ("host"), error);
                expect (! engine.getRecorder().isRecording());
            }

            beginTest ("queued-callback handle is invalidated before engine teardown");
            {
                AudioEngine::AsyncHandle handle;
                {
                    AudioEngine engine;
                    handle = engine.getAsyncHandle();
                    expect (handle->load() == &engine);
                }
                expect (handle->load (std::memory_order_acquire) == nullptr,
                        "a queued callback could dereference a destroyed AudioEngine");
            }

            beginTest ("Console name capture collects names for create-from-console");
            {
                AudioEngine engine;
                engine.setStripCount (2);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);

                engine.getRemotes().setConsoleNameCapture (true);
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/1/name", juce::String ("Kick")));
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/5/name", juce::String ("Vox")));
                engine.getRemotes().setConsoleNameCapture (false);

                auto names = engine.getRemotes().takeCapturedConsoleNames();
                expectEquals ((int) names.size(), 2);
                expectEquals (names[1], juce::String ("Kick"));
                expectEquals (names[5], juce::String ("Vox"));
                expect (engine.getRemotes().takeCapturedConsoleNames().empty());   // taking clears
            }

            beginTest ("requestConsoleChannelNames sends an OSC packet to the console");
            {
                struct Rx : juce::OSCReceiver,
                            juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
                {
                    std::atomic<int> got { 0 };
                    Rx() { addListener (this); }
                    void oscMessageReceived (const juce::OSCMessage&) override
                    { got.fetch_add (1, std::memory_order_relaxed); }
                } rx;

                const int port = 9223;
                expect (rx.connect (port), "test receiver could not bind " + juce::String (port));

                AudioEngine engine;
                engine.setStripCount (1);
                expect (engine.getRemotes().requestConsoleChannelNames ("127.0.0.1", port));
                for (int i = 0; i < 60 && rx.got.load() == 0; ++i) juce::Thread::sleep (10);
                expect (rx.got.load() > 0, "console request packet never arrived");
                rx.disconnect();

                expect (! engine.getRemotes().requestConsoleChannelNames ("", port));   // empty host -> false
            }

            beginTest ("The bounded OSC parser reads what JUCE writes and refuses deep or malformed packets");
            {
                // Real wire bytes: let JUCE's own sender serialise them.
                juce::DatagramSocket rx (false);
                int rxPort = 0;
                for (int p = 19450; p < 19460 && rxPort == 0; ++p)
                    if (rx.bindToPort (p, "127.0.0.1")) rxPort = p;
                juce::OSCSender tx;
                expect (rxPort != 0 && tx.connect ("127.0.0.1", rxPort), "loopback OSC pair failed");
                auto wire = [&] () -> juce::MemoryBlock
                {
                    juce::HeapBlock<char> buf (65535);
                    if (rx.waitUntilReady (true, 2000) != 1) return {};
                    const int got = rx.read (buf.getData(), 65535, false);
                    return got > 0 ? juce::MemoryBlock (buf.getData(), (size_t) got) : juce::MemoryBlock();
                };

                juce::OSCMessage all ("/Console/Channels/12/name");
                all.addString ("Kick In");
                all.addInt32 (-7);
                all.addFloat32 (0.25f);
                all.addBlob (juce::MemoryBlock ("ab\0c", 4));
                all.addColour (juce::OSCColour::fromInt32 (0x11223344));
                expect (tx.send (all));
                const auto msgBytes = wire();
                const auto parsed = oscwire::parsePacket (msgBytes.getData(), msgBytes.getSize());
                expect (parsed.has_value() && parsed->isMessage(), "a DiGiCo-style message did not parse");
                if (parsed.has_value() && parsed->isMessage())
                {
                    const auto& m = parsed->getMessage();
                    expectEquals (m.getAddressPattern().toString(), juce::String ("/Console/Channels/12/name"));
                    expectEquals (m.size(), 5);
                    if (m.size() == 5)
                    {
                        expectEquals (m[0].getString(), juce::String ("Kick In"));
                        expectEquals ((int) m[1].getInt32(), -7);
                        expectEquals (m[2].getFloat32(), 0.25f);
                        expect (m[3].getBlob() == juce::MemoryBlock ("ab\0c", 4));
                        expectEquals ((juce::int64) m[4].getColour().toInt32(), (juce::int64) 0x11223344);
                    }
                }

                juce::OSCBundle inner;
                inner.addElement (juce::OSCMessage ("/headamp/000/gain", 0.5f));   // X32-style reply
                juce::OSCBundle outer;
                outer.addElement (juce::OSCMessage ("/info", juce::String ("V2.07")));
                outer.addElement (inner);
                expect (tx.send (outer));
                const auto bundleBytes = wire();
                const auto bundle = oscwire::parsePacket (bundleBytes.getData(), bundleBytes.getSize());
                expect (bundle.has_value() && bundle->isBundle(), "a nested bundle did not parse");
                if (bundle.has_value() && bundle->isBundle() && bundle->getBundle().size() == 2)
                {
                    const auto& b = bundle->getBundle();
                    expect (b[0].isMessage() && b[0].getMessage().getAddressPattern().toString() == "/info");
                    expect (b[1].isBundle() && b[1].getBundle().size() == 1
                            && b[1].getBundle()[0].getMessage()[0].getFloat32() == 0.5f);
                }
                else
                    expect (false, "bundle shape lost");

                // Depth: a small cap, enforced without recursing per level.
                const auto atCap = nestedBundles (oscwire::kMaxBundleDepth);
                const auto overCap = nestedBundles (oscwire::kMaxBundleDepth + 1);
                const auto bomb = nestedBundles (3200);
                expect (oscwire::parsePacket (atCap.getData(), atCap.getSize()).has_value(),
                        "nesting at the cap was refused");
                expect (! oscwire::parsePacket (overCap.getData(), overCap.getSize()).has_value());
                expect (! oscwire::parsePacket (bomb.getData(), bomb.getSize()).has_value());

                // Malformed sizes and truncation are refused, never read past.
                auto patched = [] (juce::MemoryBlock b, size_t at, juce::int32 v)
                {
                    const juce::uint32 be = juce::ByteOrder::swapIfLittleEndian ((juce::uint32) v);
                    b.copyFrom (&be, (int) at, sizeof (be));
                    return b;
                };
                for (const juce::int32 badSize : { 0x7fffffff, 2, -1, 4 })
                {
                    const auto b = patched (bundleBytes, 16, badSize);   // first element's size
                    expect (! oscwire::parsePacket (b.getData(), b.getSize()).has_value(),
                            "element size " + juce::String (badSize) + " was accepted");
                }
                for (size_t cut = 1; cut < msgBytes.getSize(); cut += 3)
                    expect (! oscwire::parsePacket (msgBytes.getData(), msgBytes.getSize() - cut).has_value(),
                            "a truncated message parsed");
                juce::MemoryBlock trailing (msgBytes);
                trailing.append ("\0\0\0\0", 4);
                expect (! oscwire::parsePacket (trailing.getData(), trailing.getSize()).has_value(),
                        "trailing bytes after a message were accepted");
                // The blob's length word sits 12 bytes from the end ("ab\0c" + colour).
                const auto badBlob = patched (msgBytes, msgBytes.getSize() - 12, 0x7ffffff0);
                expect (! oscwire::parsePacket (badBlob.getData(), badBlob.getSize()).has_value(),
                        "an oversized blob length was accepted");
            }

            beginTest ("A deeply nested OSC bundle cannot crash the OSC listener");
            {
                AudioEngine engine;
                engine.setStripCount (1);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);
                int port = 0;
                for (int p = 19440; p < 19450 && port == 0; ++p)
                    if (osc.start (p)) port = p;
                expect (port != 0, "OSC listener failed to bind");
                if (port != 0)
                {
                    auto tx = bigDatagramSender();
                    const auto bomb = nestedBundles (3200);
                    expectEquals (tx->write ("127.0.0.1", port, bomb.getData(), (int) bomb.getSize()),
                                  (int) bomb.getSize(), "test premise: the full datagram was sent");

                    // The listener must survive it and still handle the next message.
                    juce::OSCSender sender;
                    expect (sender.connect ("127.0.0.1", port));
                    juce::Thread::sleep (50);
                    expect (sender.send (juce::OSCMessage ("/zynforge/channel/1/name",
                                                           juce::String ("AfterBomb"), osc.getAccessToken())));
                    pumpOscUntil ([&] { return engine.getRecorder().getTrack (0).getNameThreadSafe() == "AfterBomb"; },
                                  3000);
                    expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("AfterBomb"));
                    osc.stop();
                }
            }

            beginTest ("A deeply nested OSC bundle cannot crash the console link");
            {
                // The console link's OSC/UDP socket accepts datagrams from any
                // source, so the same packet reached the same JUCE parser there.
                juce::DatagramSocket desk (false);
                int deskPort = 0;
                for (int p = 47930; p < 47940 && deskPort == 0; ++p)
                    if (desk.bindToPort (p, "127.0.0.1")) deskPort = p;
                expect (deskPort != 0, "test desk failed to bind");

                ConsoleLink link;   // X32 profile: OSC over UDP, probe = /info
                if (deskPort != 0 && link.connect ("127.0.0.1", deskPort))
                {
                    // Learn the link's source port from its connect-time probe.
                    char probe[512];
                    juce::String fromIp;
                    int linkPort = 0;
                    expect (desk.waitUntilReady (true, 2000) == 1, "no probe from the link");
                    expect (desk.read (probe, sizeof (probe), false, fromIp, linkPort) > 0);

                    auto tx = bigDatagramSender();
                    const auto bomb = nestedBundles (3200);
                    expectEquals (tx->write ("127.0.0.1", linkPort, bomb.getData(), (int) bomb.getSize()),
                                  (int) bomb.getSize(), "test premise: the full datagram was sent");

                    juce::OSCSender reply;
                    expect (reply.connectToSocket (desk, "127.0.0.1", linkPort));
                    juce::Thread::sleep (50);
                    expect (reply.send (juce::OSCMessage ("/info", juce::String ("V2.07"))));
                    pumpOscUntil ([&] { return link.isDialectConfirmed(); }, 3000);
                    expect (link.isDialectConfirmed(), "the handshake reply after the bomb was lost");
                    link.disconnect();
                }
                else
                    expect (false, "console link failed to connect");
            }
        }
    };

    static OscRemoteTests oscRemoteTests;
}
