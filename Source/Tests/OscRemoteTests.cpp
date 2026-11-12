#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>
#include "../Audio/AudioEngine.h"
#include "../Audio/OscRemote.h"
#include "../Network/ConsoleLink.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
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

                const auto& t = engine.getRecorder().getTrack (0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/gain", (float) -6.0f));
                expectWithinAbsoluteError (t.liveInputGainDb.load(), -6.0f, 0.001f);
                expect (t.liveInputGainKnown.load());
                // "fader" is deliberately NOT a trim source.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/fader", (float) 3.0f));
                expectWithinAbsoluteError (t.liveInputGainDb.load(), -6.0f, 0.001f);
            }

            beginTest ("DiGiCo metadata works but unauthenticated control is ignored");
            {
                AudioEngine engine;
                engine.setStripCount (4);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);

                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/2/mute", (juce::int32) 1));
                expect (! engine.getRecorder().getTrack (1).muted.load());

                // A scene recall outside a take leaves no marker (ConsoleVscTests).
                const int before = engine.getMarkers().getCount();
                osc.dispatchForTest (juce::OSCMessage ("/Console/Snapshots/recall", (juce::int32) 5));
                expectEquals (engine.getMarkers().getCount(), before);
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
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop", (juce::int32) 1));
                expectEquals ((int) actions.size(), 3);
                if (actions.size() == 3)
                {
                    expect (actions[0] == AudioEngine::RemoteTransportAction::StartRecord);
                    expect (actions[1] == AudioEngine::RemoteTransportAction::StopPlay);
                    expect (actions[2] == AudioEngine::RemoteTransportAction::StopRecord);
                }
                expect (! engine.getRecorder().isRecording(),
                        "OSC bypassed the host and started the local recorder");
            }

            // What reached the host as a STOP tap.
            const auto stopTaps = [] (const std::vector<AudioEngine::RemoteTransportAction>& actions)
            {
                return (int) std::count (actions.begin(), actions.end(), AudioEngine::RemoteTransportAction::StopRecord);
            };

            beginTest ("A momentary OSC STOP button gives one STOP tap per press, so one brush cannot end a take");
            {
                AudioEngine engine;
                std::vector<AudioEngine::RemoteTransportAction> actions;
                engine.setRemoteTransportHandler ([&] (auto action, juce::String&) -> std::optional<bool>
                {
                    actions.push_back (action);
                    return true;
                });
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);
                // Press and release, 150 ms apart: a push button on a tablet.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop", (juce::int32) 1), 10000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop", (juce::int32) 0), 10150.0);
                expectEquals (stopTaps (actions), 1, "a press and its release were two STOP taps");
                // A float release, as TouchOSC sends it.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop", 0.0f), 10200.0);
                expectEquals (stopTaps (actions), 1, "a float release was a STOP tap");
                // A contact that bounces or a client that repeats a held button.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop", (juce::int32) 1), 10100.0 + 120.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop", (juce::int32) 1), 10100.0 + 240.0);
                expectEquals (stopTaps (actions), 1, "a repeat of one press was a second STOP tap");
                // A second, deliberate press is the second tap.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop", (juce::int32) 1), 11200.0);
                expectEquals (stopTaps (actions), 2, "a second press was not a STOP tap");
            }

            beginTest ("An OSC STOP with no value is refused, so one held press can't end a take, and setup sees why");
            {
                AudioEngine engine;
                std::vector<AudioEngine::RemoteTransportAction> actions;
                engine.setRemoteTransportHandler ([&] (auto action, juce::String&) -> std::optional<bool>
                {
                    actions.push_back (action);
                    return true;
                });
                juce::StringArray said;
                engine.getRemotes().setHost ({ [&] (const juce::String& s) { said.add (s); }, {}, {} });
                engine.getRemotes().setOscDebug (true);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);
                // A tablet button set to send only the address and token sends
                // the same packet on press and on release. Held for 400 ms, the
                // release passed the repeat filter and was the second tap.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop"), 30000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/stop"), 30400.0);
                expectEquals (stopTaps (actions), 0, "a STOP with no value reached the host");
                expect (said.joinIntoString (" | ").containsIgnoreCase ("value"),
                        "the refusal was silent: " + said.joinIntoString (" | "));
                expect (engine.getRemotes().getOscLog().joinIntoString ("\n").containsIgnoreCase ("value"),
                        "the refusal is not in the OSC log");
                engine.getRemotes().setOscDebug (false);
                engine.getRemotes().clearOscLog();
            }

            beginTest ("A momentary OSC RECORD button never stops a take, however often it is pressed");
            {
                AudioEngine engine;
                std::vector<AudioEngine::RemoteTransportAction> actions;
                engine.setRemoteTransportHandler ([&] (auto action, juce::String&) -> std::optional<bool>
                {
                    actions.push_back (action);
                    return true;
                });
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);
                double now = 20000.0;
                for (int press = 0; press < 3; ++press)
                {
                    osc.dispatchForTest (juce::OSCMessage ("/zynforge/record", (juce::int32) 1), now);
                    osc.dispatchForTest (juce::OSCMessage ("/zynforge/record", (juce::int32) 0), now + 120.0);
                    now += 600.0;
                }
                expectEquals (stopTaps (actions), 0, "releasing RECORD reached the host as a STOP tap");
                expect (! actions.empty() && actions.front() == AudioEngine::RemoteTransportAction::StartRecord);
            }

            beginTest ("A rolling take is not stopped by a repeated OSC RECORD");
            {
                // Only an explicit RECORD 0 stops. RECORD 1 starts and never
                // toggles, so a double-tap "to make sure" cannot end a take.
                AudioEngine engine;
                std::vector<AudioEngine::RemoteTransportAction> actions;
                engine.setRemoteTransportHandler ([&] (auto action, juce::String&) -> std::optional<bool>
                {
                    actions.push_back (action);
                    return true;
                });
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/record", (juce::int32) 1));
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/record", (juce::int32) 1));
                expectEquals ((int) actions.size(), 2);
                for (const auto action : actions)
                    expect (action == AudioEngine::RemoteTransportAction::StartRecord,
                            "OSC RECORD reached the host as a toggle that stops a rolling take");
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

            beginTest ("A momentary OSC MARKER or SCENE button gives one per press");
            {
                AudioEngine engine;
                const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("zf-osc-marker-" + juce::Uuid().toString());
                expect (dir.createDirectory().wasOk());
                const juce::ScopeGuard remove { [&] { dir.deleteRecursively(); } };
                auto& markers = engine.getMarkers();
                markers.setContext (dir, 48000);
                engine.setEditCursorSample (0);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);

                // Press 1, release 0: one marker, and it is not named after the button.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", (juce::int32) 1), 40000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", (juce::int32) 0), 40150.0);
                expectEquals (markers.getCount(), 1, "a press and its release dropped two markers");
                if (markers.getCount() > 0)
                {
                    const auto name = markers.getLast().name;
                    expect (name != "0" && name != "1", "the button's value became the marker name: " + name);
                }
                // A float release, as TouchOSC sends it.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", 1.0f), 41000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", 0.0f), 41100.0);
                expectEquals (markers.getCount(), 2, "a float release dropped a marker");
                // A bounce of one press.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", (juce::int32) 1), 42000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", (juce::int32) 1), 42100.0);
                expectEquals (markers.getCount(), 3, "a repeat of one press dropped a second marker");
                // A name still names it.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Verse")), 43000.0);
                expectEquals (markers.getCount(), 4);
                expectEquals (markers.getLast().name, juce::String ("Verse"));

                // Scenes: count what reached Scene Follow's rule (it logs every recall).
                engine.getRemotes().setOscDebug (true);
                engine.getRemotes().clearOscLog();
                const auto recalls = [&engine]
                {
                    int n = 0;
                    for (const auto& line : engine.getRemotes().getOscLog())
                        if (line.contains ("scene: Console scene")) ++n;
                    return n;
                };
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", (juce::int32) 2), 50000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", (juce::int32) 0), 50150.0);
                expectEquals (recalls(), 1, "a scene button's release was a second recall");
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", (juce::int32) 3), 51000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", (juce::int32) 3), 51100.0);
                expectEquals (recalls(), 2, "a repeat of one scene press recalled twice");
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", (juce::int32) 3), 52000.0);
                expectEquals (recalls(), 3, "a deliberate second press was lost");
                engine.getRemotes().setOscDebug (false);
                engine.getRemotes().clearOscLog();
            }

            beginTest ("A desk channel's name lands on the strip that hears that channel");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                const juce::ScopeGuard cleanup { [&] { engine.clearAllStripOverrides(); } };
                engine.setStripCount (3);
                for (int i = 0; i < 3; ++i) engine.setTrackName (i, "strip " + juce::String (i + 1));
                // Strip 1 was dragged to the end: strip positions no longer
                // match the desk channels they hear.
                engine.setTrackInputRouting (0, 1);   // hears desk channel 2
                engine.setTrackInputRouting (1, 2);   // hears desk channel 3
                engine.setTrackInputRouting (2, 0);   // hears desk channel 1
                auto& remotes = engine.getRemotes();
                remotes.onConsoleChannelName (1, "Kick");
                remotes.onConsoleChannelName (2, "Snare");
                remotes.onConsoleChannelName (3, "Vox");
                expectEquals (engine.getRecorder().getTrack (2).getNameThreadSafe(), juce::String ("Kick"));
                expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("Snare"));
                expectEquals (engine.getRecorder().getTrack (1).getNameThreadSafe(), juce::String ("Vox"));

                // A strip with no input hears no desk channel.
                engine.setTrackInputRouting (1, -1);
                remotes.onConsoleChannelName (2, "Bass");
                expectEquals (engine.getRecorder().getTrack (1).getNameThreadSafe(), juce::String ("Vox"),
                              "a strip with no input took a desk channel's name");
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

            beginTest ("Console trim values are finite and inside a real preamp range");
            {
                // Console trim arrives without the Generic token. With trim-follow
                // on, +inf / NaN / 200 dB went straight into the playback gain.
                AudioEngine engine;
                engine.setStripCount (1);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);

                const auto& t = engine.getRecorder().getTrack (0);
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/1/gain", -6.0f));
                expectWithinAbsoluteError (t.liveInputGainDb.load(), -6.0f, 0.001f);
                for (const float bad : { std::numeric_limits<float>::infinity(),
                                         -std::numeric_limits<float>::infinity(),
                                         std::numeric_limits<float>::quiet_NaN() })
                {
                    osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/1/gain", bad));
                    expectWithinAbsoluteError (t.liveInputGainDb.load(), -6.0f, 0.001f,
                                               "a non-finite trim replaced the last real reading");
                }
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/1/gain", 200.0f));
                expectWithinAbsoluteError (t.liveInputGainDb.load(), 72.0f, 0.001f,
                                           "a trim above any desk's preamp range was not clamped");
                engine.setTrackLiveInputGain (0, -1000.0f);   // every caller goes through the setter
                expectWithinAbsoluteError (t.liveInputGainDb.load(), -96.0f, 0.001f);
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

            beginTest ("A 64 KB packet of non-UTF-8 text costs the listener next to nothing");
            {
                // OSC-string: the bytes, a NUL, zero padding to 4.
                const auto oscString = [] (juce::MemoryOutputStream& out, const juce::MemoryBlock& bytes)
                {
                    out.write (bytes.getData(), bytes.getSize());
                    const auto pad = 4 - (bytes.getSize() % 4);
                    for (size_t i = 0; i < pad; ++i) out.writeByte (0);
                };
                const auto latin1 = [] (const char* lead, size_t count)
                {
                    juce::MemoryBlock b (lead, std::strlen (lead));
                    juce::MemoryBlock fill (count, false);
                    std::memset (fill.getData(), 0xE9, count);   // 'e acute' in Latin-1, never valid UTF-8 alone
                    b.append (fill.getData(), fill.getSize());
                    return b;
                };
                const auto fastest = [] (const std::function<void()>& work)
                {
                    double best = 1.0e9;
                    for (int run = 0; run < 3; ++run)
                    {
                        const auto t0 = juce::Time::getMillisecondCounterHiRes();
                        work();
                        best = juce::jmin (best, juce::Time::getMillisecondCounterHiRes() - t0);
                    }
                    return best;
                };

                // A name the parser keeps, as a desk's Latin-1 channel name.
                juce::MemoryOutputStream kept;
                oscString (kept, juce::MemoryBlock ("/x", 2));
                oscString (kept, juce::MemoryBlock (",s", 2));
                oscString (kept, latin1 ("", 64000));
                int parsedLength = -1;
                const auto parseMs = fastest ([&]
                {
                    const auto parsed = oscwire::parsePacket (kept.getData(), kept.getDataSize());
                    parsedLength = parsed.has_value() && parsed->isMessage() && parsed->getMessage().size() == 1
                                 ? parsed->getMessage()[0].getString().length() : -1;
                });
                expectEquals (parsedLength, 64000, "the Latin-1 packet was refused or cut");
                expect (parseMs < 25.0, "64 KB of Latin-1 took " + juce::String (parseMs, 1) + " ms to parse");

                // A packet the parser refuses, which the log describes.
                juce::MemoryOutputStream refused;
                oscString (refused, latin1 ("/", 64000));
                oscString (refused, juce::MemoryBlock (",d", 2));
                const auto describeMs = fastest ([&]
                {
                    if (oscwire::parsePacket (refused.getData(), refused.getDataSize()).has_value())
                        expect (false, "a packet with a double was accepted");
                    oscwire::describeRefused (refused.getData(), refused.getDataSize());
                });
                expect (describeMs < 25.0, "refusing 64 KB of Latin-1 took " + juce::String (describeMs, 1) + " ms");
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
