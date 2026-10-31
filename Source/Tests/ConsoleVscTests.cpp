// Scene Follow, desk macro control, stereo pairs from the desk and the console
// log. A real MainComponent stands in wherever a guard lives in the host: LOCK,
// sessionIoBusy, RECORD pre-flight, the two-tap STOP and stereo linking.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_osc/juce_osc.h>

#include "../Audio/OscRemote.h"
#include "../Audio/SceneFollow.h"
#include "../Network/ConsoleLog.h"
#include "../Network/DeskMacro.h"
#include "../UI/DeskMacroDialog.h"
#include "../UI/MainComponent.h"

#include <array>
#include <vector>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    namespace
    {
        constexpr int kRate = 48000;

        bool pump (const std::function<bool()>& predicate, int timeoutMs)
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

        juce::File tempDir (const juce::String& stem)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile (stem + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        // A one-track session the player can load, so a locate has room to move.
        void writeTake (const juce::File& session, int seconds)
        {
            auto audio = session.getChildFile ("Audio Files");
            audio.createDirectory();
            auto file = audio.getChildFile ("Track_01.wav");
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
                stream.release(), kRate, 1, 24, {}, 0));
            juce::AudioBuffer<float> silence (1, kRate);
            silence.clear();
            for (int s = 0; s < seconds; ++s)
                writer->writeFromAudioSampleBuffer (silence, 0, kRate);
        }

        juce::int64 at (double seconds) { return (juce::int64) (seconds * kRate); }

        std::vector<Marker> markerList (std::initializer_list<std::pair<juce::int64, const char*>> items)
        {
            std::vector<Marker> out;
            for (const auto& item : items)
            {
                Marker m;
                m.sampleOffset = item.first;
                m.name = item.second;
                out.push_back (m);
            }
            return out;
        }

        // Every remote transport a macro could ask for, counted and answered.
        struct TransportSpy
        {
            std::vector<AudioEngine::RemoteTransportAction> actions;
            void attach (AudioEngine& engine)
            {
                engine.setRemoteTransportHandler ([this] (auto action, juce::String&) -> std::optional<bool>
                {
                    actions.push_back (action);
                    return true;
                });
            }
        };
    }

    class ConsoleVscTests final : public juce::UnitTest
    {
    public:
        ConsoleVscTests() : juce::UnitTest ("Console VSC", "zynforge") {}

        // A host with a loaded session, reachable down both console paths.
        struct SceneBench
        {
            MainComponent main;
            OscRemote     osc { main.engine };
            juce::File    session { tempDir ("zf-scene") };

            SceneBench()
            {
                main.engine.clearAllStripOverrides();
                main.engine.setStripCount (1);
                writeTake (session, 20);
                main.engine.setActiveSessionDir (session);
                main.engine.loadSession (session);
                osc.setDialect (OscRemote::Dialect::DiGiCo);
                main.consoleLink.setProfile (ConsoleProfile::Kind::DiGiCo);
                main.consoleLink.setMessageHook ([] (const ConsoleMessage&) {});
            }

            ~SceneBench()
            {
                main.engine.setSceneFollowEnabled (false);
                main.engine.clearAllStripOverrides();
                main.engine.getMarkers().clearContext();
                session.deleteRecursively();
            }

            MarkersManager& markers() { return main.engine.getMarkers(); }
            SessionPlayer&  player()  { return main.engine.getPlayer(); }
            juce::String    status()  { return main.statusLabel.getText(); }
        };

        enum class Path { ConsoleLink, OscListener };

        static juce::String nameOf (Path p) { return p == Path::ConsoleLink ? "console link" : "OSC listener"; }

        static void fire (SceneBench& b, Path p, int number, const juce::String& name)
        {
            if (p == Path::OscListener)
                b.osc.dispatchForTest (juce::OSCMessage ("/Console/Snapshots/recall", (juce::int32) number, name));
            else
                b.main.consoleLink.injectReply (ConsoleMessage ("/Console/Snapshots/recall", { number, name }));
        }

        void testSceneRule()
        {
            using namespace scenefollow;

            beginTest ("Scene rule: every branch of the decision");
            {
                const auto markers = markerList ({ { 100, "Verse" } });
                Facts f;
                f.playerLoaded = true;
                expect (decide (f, markers, "Verse", 0).action == Action::Nothing);

                auto capturing = f;
                capturing.capturing = true;
                capturing.followOn  = true;
                capturing.locked    = true;
                expect (decide (capturing, markers, "Verse", 0).action == Action::DropMarker,
                        "a take must mark the recall whatever Follow or LOCK say");

                auto follow = f;
                follow.followOn = true;
                const auto hit = decide (follow, markers, "Verse", 0);
                expect (hit.action == Action::Locate);
                expectEquals (hit.markerIndex, 0);
                expect (decide (follow, markers, "Chorus", 0).action == Action::NoMatch);

                for (auto block : { &Facts::locked, &Facts::seekLocked, &Facts::sessionBusy })
                {
                    auto refused = follow;
                    refused.*block = true;
                    const auto d = decide (refused, markers, "Verse", 0);
                    expect (d.action == Action::Refused);
                    expect (d.refusal.isNotEmpty(), "a refusal must say why");
                }
                auto nothingLoaded = follow;
                nothingLoaded.playerLoaded = false;
                expect (decide (nothingLoaded, markers, "Verse", 0).action == Action::Refused);
            }

            beginTest ("Scene rule: a scene with no name goes by its number");
            {
                expectEquals (sceneLabel (7, {}), juce::String ("Scene 7"));
                expectEquals (sceneLabel (7, "   "), juce::String ("Scene 7"));
                expectEquals (sceneLabel (7, "  Verse "), juce::String ("Verse"));
            }

            beginTest ("Scene rule: names match trimmed and without case");
            {
                const auto markers = markerList ({ { 100, "  Big CHORUS " } });
                expectEquals (findMarker (markers, "big chorus", 0), 0);
                expectEquals (findMarker (markers, "Big Chorus  ", 0), 0);
                expectEquals (findMarker (markers, "big chorus 2", 0), -1);
            }

            beginTest ("Scene rule: duplicates pick the next ahead, otherwise the first");
            {
                const auto markers = markerList ({ { 100, "Verse" }, { 200, "Other" },
                                                   { 300, "Verse" }, { 400, "Verse" } });
                expectEquals (findMarker (markers, "Verse", 0),   0);
                expectEquals (findMarker (markers, "Verse", 100), 2, "a marker AT the playhead is not ahead of it");
                expectEquals (findMarker (markers, "Verse", 350), 3);
                expectEquals (findMarker (markers, "Verse", 400), 0);
                expectEquals (findMarker (markers, "Verse", 999), 0);
            }

            beginTest ("Marker navigation helpers");
            {
                const auto markers = markerList ({ { 100, "a" }, { 200, "b" }, { 300, "c" } });
                expectEquals (nextMarker (markers, 0), 0);
                expectEquals (nextMarker (markers, 100), 1);
                expectEquals (nextMarker (markers, 300), -1);
                expectEquals (previousMarker (markers, 300, 0), 1);
                expectEquals (previousMarker (markers, 100, 0), -1);
                expectEquals (previousMarker (markers, 310, 50), 1,
                              "a rolling playhead just past a marker must step to the one before");
            }
        }

        void testSceneFollowOnBothPaths()
        {
            SceneBench b;
            for (const auto path : { Path::ConsoleLink, Path::OscListener })
            {
                const auto where = " (" + nameOf (path) + ")";
                b.markers().clear();
                b.markers().drop (at (5.0), "Verse");
                b.markers().drop (at (10.0), "Chorus");
                b.markers().drop (at (15.0), "Verse");
                b.player().setPositionSamples (at (1.0));
                b.main.engine.setSceneFollowEnabled (false);

                beginTest ("Follow off, no take: a recall leaves no marker and no jump" + where);
                fire (b, path, 3, "Verse");
                expectEquals (b.markers().getCount(), 3, "a stray marker was dropped outside a take");
                expectEquals (b.player().getPositionSamples(), at (1.0));

                beginTest ("Follow on: stopped stays stopped, and no marker" + where);
                b.main.engine.setSceneFollowEnabled (true);
                b.main.engine.setEditCursorSample (at (2.0));
                fire (b, path, 3, "verse  ");
                expectEquals (b.player().getPositionSamples(), at (5.0));
                expect (! b.player().isPlaying(), "a locate started the transport");
                expectEquals (b.markers().getCount(), 3);
                expect (b.status().contains ("followed"), "Follow must say so when it fires: " + b.status());
                expectEquals (b.main.engine.getEditCursorSample(), (juce::int64) -1,
                              "a stale cursor would send the next PLAY away from the song");

                beginTest ("Follow on: playing stays playing" + where);
                b.player().start();
                fire (b, path, 4, "Chorus");
                expect (b.player().isPlaying(), "a locate stopped the transport");
                expectEquals (b.player().getPositionSamples(), at (10.0));
                b.player().stop();

                beginTest ("Follow on: duplicates walk forward, then wrap to the first" + where);
                fire (b, path, 3, "Verse");
                expectEquals (b.player().getPositionSamples(), at (15.0));
                fire (b, path, 3, "Verse");
                expectEquals (b.player().getPositionSamples(), at (5.0));

                beginTest ("Follow on: no match says so, jumps nowhere, creates nothing" + where);
                b.player().setPositionSamples (at (7.0));
                fire (b, path, 9, "Encore");
                expectEquals (b.player().getPositionSamples(), at (7.0));
                expectEquals (b.markers().getCount(), 3);
                expect (b.status().contains ("no marker"), b.status());

                beginTest ("Follow refuses during a punch's seek lock" + where);
                b.player().setSeekLocked (true);
                fire (b, path, 3, "Verse");
                b.player().setSeekLocked (false);
                expectEquals (b.player().getPositionSamples(), at (7.0));
                expectEquals (b.markers().getCount(), 3);
                expect (b.status().contains ("refused"), b.status());

                beginTest ("Follow refuses during a session operation" + where);
                b.main.sessionIoBusy.store (true);
                fire (b, path, 3, "Verse");
                b.main.sessionIoBusy.store (false);
                expectEquals (b.player().getPositionSamples(), at (7.0));
                expect (b.status().contains ("refused"), b.status());

                beginTest ("Follow refuses under LOCK" + where);
                b.main.onLockToggled();
                fire (b, path, 3, "Verse");
                b.main.onLockToggled();
                expectEquals (b.player().getPositionSamples(), at (7.0));
                expectEquals (b.markers().getCount(), 3);

                beginTest ("A daemon take marks the recall at the capture position, Follow or not" + where);
                {
                    EngineStatus daemon;
                    daemon.recording = true;
                    daemon.source = "daemon";
                    daemon.positionSamples = at (12.5);
                    b.main.engine.setExternalCaptureStatus (daemon);
                    b.main.engine.setExternalRecording (true);
                    fire (b, path, 5, "Bridge");
                    b.main.engine.setExternalRecording (false);
                    expectEquals (b.markers().getCount(), 4, "no marker during a daemon take");
                    const auto& all = b.markers().getAll();
                    expectEquals (all[2].name, juce::String ("Bridge"));
                    expectEquals (all[2].sampleOffset, at (12.5));
                    expectEquals (b.player().getPositionSamples(), at (7.0), "Follow moved the playhead during a take");
                    b.markers().removeMarker (2);
                }

                beginTest ("A local take marks the recall at the capture position, Follow or not" + where);
                {
                    auto take = tempDir ("zf-scene-take");
                    auto& rec = b.main.engine.getRecorder();
                    rec.prepare ((double) kRate, 256, 1);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (take));
                    std::array<float, 256> samples {};
                    const float* input = samples.data();
                    for (int block = 0; block < 40; ++block)
                        rec.processBlock (&input, 1, 256);
                    const auto capturePosition = b.main.engine.getRecordTimelineSamples();
                    expect (capturePosition > 0);
                    fire (b, path, 5, "Outro");
                    rec.stopRecording();
                    take.deleteRecursively();
                    expectEquals (b.markers().getCount(), 4, "no marker during a local take");
                    const auto all = b.markers().getAll();
                    int found = -1;
                    for (int i = 0; i < (int) all.size(); ++i)
                        if (all[(size_t) i].name == "Outro") found = i;
                    expect (found >= 0);
                    if (found >= 0)
                    {
                        expectEquals (all[(size_t) found].sampleOffset, capturePosition);
                        b.markers().removeMarker (found);
                    }
                }
            }

            beginTest ("LOCK during a take: the console link marks it, the OSC listener stays closed");
            {
                // The listener's own LOCK gate sits in front of every message
                // and was not weakened; the link never had one. Pinned so the
                // difference is a decision rather than a surprise.
                b.markers().clear();
                EngineStatus daemon;
                daemon.recording = true;
                daemon.source = "daemon";
                daemon.positionSamples = at (3.0);
                b.main.engine.setExternalCaptureStatus (daemon);
                b.main.engine.setExternalRecording (true);
                b.main.onLockToggled();
                fire (b, Path::ConsoleLink, 1, "Locked Link");
                fire (b, Path::OscListener, 2, "Locked Osc");
                b.main.onLockToggled();
                b.main.engine.setExternalRecording (false);
                expectEquals (b.markers().getCount(), 1);
                expectEquals (b.markers().getAll()[0].name, juce::String ("Locked Link"));
            }

            beginTest ("A recall with only a number finds the marker a take dropped for it");
            {
                b.markers().clear();
                b.markers().drop (at (6.0), "Scene 12");
                b.player().setPositionSamples (at (0.0));
                b.osc.dispatchForTest (juce::OSCMessage ("/Console/Snapshots/recall", (juce::int32) 12));
                expectEquals (b.player().getPositionSamples(), at (6.0));
            }

            beginTest ("Every OSC dialect's recall reaches the same rule");
            {
                const std::array<std::pair<OscRemote::Dialect, const char*>, 4> recalls {{
                    { OscRemote::Dialect::DiGiCo,     "/Console/Snapshots/recall" },
                    { OscRemote::Dialect::AllenHeath, "/sq/scene/recall" },
                    { OscRemote::Dialect::SSL,        "/sslnet/snapshot/recall" },
                    { OscRemote::Dialect::Yamaha,     "/RIVAGE/Scene/recall" },
                }};
                b.main.engine.setSceneFollowEnabled (true);
                for (const auto& [dialect, address] : recalls)
                {
                    b.markers().clear();
                    b.markers().drop (at (4.0), "Scene 2");
                    b.player().setPositionSamples (at (1.0));
                    b.osc.setDialect (dialect);
                    b.osc.dispatchForTest (juce::OSCMessage (address, (juce::int32) 2));
                    expectEquals (b.player().getPositionSamples(), at (4.0), address);
                    expectEquals (b.markers().getCount(), 1, address);
                }
                b.osc.setDialect (OscRemote::Dialect::Generic);
                b.markers().clear();
                b.markers().drop (at (9.0), "Named");
                b.osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", (juce::int32) 2, juce::String ("Named")));
                expectEquals (b.player().getPositionSamples(), at (9.0), "generic scene name");
                b.osc.setDialect (OscRemote::Dialect::DiGiCo);
            }

            beginTest ("An explicit marker command is not a scene recall");
            {
                b.markers().clear();
                b.main.engine.setSceneFollowEnabled (false);
                b.osc.dispatchForTest (juce::OSCMessage ("/Console/Marker", juce::String ("Note")));
                expectEquals (b.markers().getCount(), 1);
            }

            beginTest ("Follow defaults off, persists, and the menu toggles it");
            {
                auto* props = b.main.engine.getAppProps();
                props->removeValue ("sceneFollow");
                props->saveIfNeeded();
                {
                    AudioEngine fresh;
                    expect (! fresh.isSceneFollowEnabled(), "Follow must default off");
                }
                const auto signature = [&]
                {
                    b.main.refreshMenuStateIfChanged();
                    return b.main.lastMenuStateSig;
                };
                const auto before = signature();
                b.main.menuItemSelected (963, 3);
                expect (b.main.engine.isSceneFollowEnabled());
                expect (signature() != before, "the menu tick must be in the state signature");
                {
                    AudioEngine fresh;
                    expect (fresh.isSceneFollowEnabled(), "Follow did not persist");
                }
                b.main.onLockToggled();
                b.main.menuItemSelected (963, 3);
                b.main.onLockToggled();
                expect (b.main.engine.isSceneFollowEnabled(), "LOCK must refuse the toggle");
                b.main.menuItemSelected (963, 3);
                expect (! b.main.engine.isSceneFollowEnabled());
            }
        }

        void testMacroParsing()
        {
            using namespace deskmacro;
            const auto parsed = [] (const juce::OSCMessage& m) { return parse (m).command; };
            const auto i = [] (const char* address, int v) { return juce::OSCMessage (address, (juce::int32) v); };

            beginTest ("Macro: only the listed addresses, address-only or integer");
            {
                const std::array<std::pair<const char*, Command>, 7> commands {{
                    { "/zynforge/macro/record",      Command::Record },
                    { "/zynforge/macro/stop",        Command::Stop },
                    { "/zynforge/macro/play",        Command::Play },
                    { "/zynforge/macro/pause",       Command::Pause },
                    { "/zynforge/macro/marker",      Command::Marker },
                    { "/zynforge/macro/marker/next", Command::MarkerNext },
                    { "/zynforge/macro/marker/prev", Command::MarkerPrev },
                }};
                for (const auto& [address, command] : commands)
                {
                    expect (parsed (juce::OSCMessage (address)) == command, address);
                    expect (parsed (i (address, 1)) == command, address);
                    expect (parsed (i (address, 127)) == command, address);
                    expect (parsed (i (address, -1)) == command, address);
                    expect (parsed (i (address, 0)) == Command::None, "integer 0 is a button release");
                    expect (parsed (juce::OSCMessage (address, 1.0f)) == Command::None, "floats are refused");
                    expect (parsed (juce::OSCMessage (address, juce::String ("1"))) == Command::None,
                            "strings are refused");
                    expect (parsed (juce::OSCMessage (address, (juce::int32) 1, (juce::int32) 1)) == Command::None,
                            "extra arguments are refused");
                }
                expect (kAddresses.size() == commands.size() + 1, "goto is tested below");
            }

            beginTest ("Macro: nothing but those addresses");
            {
                for (const char* address : { "/zynforge/record", "/zynforge/macro",
                                             "/zynforge/macro/arm", "/zynforge/macro/mute",
                                             "/zynforge/macro/recordx", "/zynforge/macro/marker/goto/1",
                                             "/ZYNFORGE/macro/record", "/Console/Snapshots/recall" })
                {
                    expect (parsed (juce::OSCMessage (address)) == Command::None, address);
                    expect (parsed (i (address, 1)) == Command::None, address);
                }
                expect (parsed (juce::OSCMessage ("/zynforge/macro/*")) == Command::None, "a wildcard must not match");
                expect (parsed (juce::OSCMessage ("/zynforge/macro/rec?rd")) == Command::None);
            }

            beginTest ("Macro: goto needs a 1-based integer");
            {
                const auto go = parse (i ("/zynforge/macro/marker/goto", 3));
                expect (go.command == Command::MarkerGoto);
                expectEquals (go.value, 3);
                expect (parsed (juce::OSCMessage ("/zynforge/macro/marker/goto")) == Command::None);
                expect (parsed (i ("/zynforge/macro/marker/goto", 0)) == Command::None);
                expect (parsed (i ("/zynforge/macro/marker/goto", -2)) == Command::None);
            }

            beginTest ("Macro: the desk address is one usable IPv4 address");
            {
                expectEquals (normaliseSourceIp (" 192.168.1.20 "), juce::String ("192.168.1.20"));
                expectEquals (normaliseSourceIp ("192.168.001.020"), juce::String ("192.168.1.20"));
                for (const char* bad : { "", "desk.local", "192.168.1", "192.168.1.20.1", "192.168.1.256",
                                         "0.0.0.0", "255.255.255.255", "224.0.0.1", "1.2.3.x", "1.2.3.-4" })
                    expect (normaliseSourceIp (bad).isEmpty(), bad);
            }

            beginTest ("Macro: identical commands inside the debounce window count once");
            {
                Debouncer d;
                const Parsed stop { Command::Stop, 0 };
                expect (d.admit (stop, 1000.0));
                expect (! d.admit (stop, 1100.0));
                expect (! d.admit (stop, 1249.0));
                expect (d.admit (stop, 1600.0));

                // A key repeating every 200 ms must stay one press, or holding
                // STOP would complete the two-tap guard.
                Debouncer held;
                expect (held.admit (stop, 0.0));
                for (double t = 200.0; t <= 1000.0; t += 200.0)
                    expect (! held.admit (stop, t), "a held key fired again at " + juce::String (t));
                expect (held.admit (stop, 1300.0));

                Debouncer different;
                expect (different.admit ({ Command::Play, 0 }, 0.0));
                expect (different.admit ({ Command::Stop, 0 }, 10.0));
                expect (different.admit ({ Command::MarkerGoto, 3 }, 20.0));
                expect (different.admit ({ Command::MarkerGoto, 4 }, 30.0));
                expect (! different.admit ({ Command::MarkerGoto, 4 }, 40.0));
            }
        }

        void testMacroCommands()
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Macro: transport goes through the host boundary, and only as named");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                DeskMacroListener macro (engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/record"), 0.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop", (juce::int32) 1), 1000.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play"), 2000.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/pause"), 3000.0);
                using A = AudioEngine::RemoteTransportAction;
                expect (spy.actions == std::vector<A> ({ A::StartRecord, A::StopAll, A::StartPlay, A::StopPlay }));
                expect (! engine.getRecorder().isRecording(), "the macro bypassed the host");
            }

            beginTest ("Macro: integer 0 and a repeat do nothing");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                DeskMacroListener macro (engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play", (juce::int32) 0), 0.0);
                expect (spy.actions.empty(), "integer 0 fired");
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play", (juce::int32) 1), 10.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play", (juce::int32) 1), 100.0);
                expectEquals ((int) spy.actions.size(), 1, "a double-fired macro acted twice");
            }

            beginTest ("Macro: without a host RECORD fails closed");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.setStripCount (1);
                engine.prepareForTests ((double) kRate, 256);
                engine.getRecorder().getTrack (0).armed.store (true);
                auto dir = tempDir ("zf-macro-nohost");
                engine.setActiveSessionDir (dir);
                DeskMacroListener macro (engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/record"), 0.0);
                expect (! engine.getRecorder().isRecording(), "a take started with no host to run pre-flight");
                engine.setActiveSessionDir ({});
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }

            beginTest ("Macro: LOCK refuses every command, with a session it could have acted on");
            {
                SceneBench b;
                b.markers().drop (at (6.0), "a");
                b.markers().drop (at (9.0), "b");
                b.player().setPositionSamples (at (2.0));
                TransportSpy spy;
                spy.attach (b.main.engine);
                DeskMacroListener macro (b.main.engine);
                b.main.onLockToggled();
                double t = 0.0;
                for (const auto& entry : deskmacro::kAddresses)
                    macro.dispatchForTest (juce::OSCMessage (entry.address, (juce::int32) 1), t += 1000.0);
                expect (b.status().containsIgnoreCase ("locked"), b.status());
                b.main.onLockToggled();
                expect (spy.actions.empty(), "a locked session took a transport command");
                expectEquals (b.markers().getCount(), 2, "a locked session took a marker");
                expectEquals (b.player().getPositionSamples(), at (2.0), "a locked session moved the playhead");
                b.main.engine.setRemoteTransportHandler ({});
            }

            beginTest ("Macro: no command arms, mutes, patches or renames anything");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.setStripCount (2);
                engine.setTrackName (0, "Kick");
                auto dir = tempDir ("zf-macro-state");
                engine.getMarkers().setContext (dir, kRate);
                auto& track = engine.getRecorder().getTrack (0);
                track.armed.store (false);
                track.muted.store (false);
                const auto routing = track.inputRouting.load();
                TransportSpy spy;
                spy.attach (engine);
                DeskMacroListener macro (engine);
                double t = 0.0;
                for (const auto& entry : deskmacro::kAddresses)
                    macro.dispatchForTest (juce::OSCMessage (entry.address, (juce::int32) 1), t += 1000.0);
                expect (! track.armed.load());
                expect (! track.muted.load());
                expectEquals (track.inputRouting.load(), routing);
                expectEquals (track.getNameThreadSafe(), juce::String ("Kick"));
                engine.getMarkers().clearContext();
                dir.deleteRecursively();
                engine.clearAllStripOverrides();
            }

            {
                SceneBench b;
                DeskMacroListener macro (b.main.engine);
                double t = 10000.0;
                auto send = [&] (const char* address) { macro.dispatchForTest (juce::OSCMessage (address), t += 1000.0); };
                auto sendGoto = [&] (int n)
                { macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/goto", (juce::int32) n), t += 1000.0); };

                beginTest ("Macro: markers drop, and next / prev / goto locate without touching the transport");
                b.player().setPositionSamples (at (2.0));
                b.main.engine.clearEditCursor();
                send ("/zynforge/macro/marker");
                expectEquals (b.markers().getCount(), 1);
                expectEquals (b.markers().getAll()[0].sampleOffset, at (2.0));
                b.markers().drop (at (6.0), "b");
                b.markers().drop (at (9.0), "c");

                b.main.engine.setEditCursorSample (at (1.0));
                send ("/zynforge/macro/marker/next");
                expectEquals (b.player().getPositionSamples(), at (6.0));
                expect (! b.player().isPlaying());
                expectEquals (b.main.engine.getEditCursorSample(), (juce::int64) -1);

                b.player().start();
                send ("/zynforge/macro/marker/next");
                expectEquals (b.player().getPositionSamples(), at (9.0));
                expect (b.player().isPlaying(), "next stopped the transport");
                b.player().setPositionSamples (at (9.2));
                send ("/zynforge/macro/marker/prev");
                expectEquals (b.player().getPositionSamples(), at (6.0),
                              "a rolling PREV just after a marker must step past it");
                b.player().stop();

                sendGoto (3);
                expectEquals (b.player().getPositionSamples(), at (9.0));
                sendGoto (1);
                expectEquals (b.player().getPositionSamples(), at (2.0));
                sendGoto (9);
                expectEquals (b.player().getPositionSamples(), at (2.0), "goto past the list must not move");
                expect (b.status().contains ("no marker 9"), b.status());
                send ("/zynforge/macro/marker/prev");
                expectEquals (b.player().getPositionSamples(), at (2.0));
                expect (b.status().contains ("no marker"), b.status());

                beginTest ("Macro: a marker jump refuses during a take, a punch and a session operation");
                b.player().setPositionSamples (at (3.0));
                b.player().setSeekLocked (true);
                sendGoto (3);
                b.player().setSeekLocked (false);
                expectEquals (b.player().getPositionSamples(), at (3.0));
                expect (b.status().contains ("refused"), b.status());

                b.main.sessionIoBusy.store (true);
                sendGoto (3);
                send ("/zynforge/macro/marker");
                b.main.sessionIoBusy.store (false);
                expectEquals (b.player().getPositionSamples(), at (3.0));
                expectEquals (b.markers().getCount(), 3, "a marker was dropped during a session operation");

                EngineStatus daemon;
                daemon.recording = true;
                daemon.source = "daemon";
                daemon.positionSamples = at (4.0);
                b.main.engine.setExternalCaptureStatus (daemon);
                b.main.engine.setExternalRecording (true);
                sendGoto (3);
                expectEquals (b.player().getPositionSamples(), at (3.0), "a marker jump moved the playhead in a take");
                send ("/zynforge/macro/marker");
                expectEquals (b.markers().getCount(), 4, "a macro marker belongs in a take");

                beginTest ("Macro: PAUSE refuses during a take");
                b.main.engine.getPlayer().start();
                send ("/zynforge/macro/pause");
                expect (b.player().isPlaying(), "PAUSE reached the player during a take");
                b.main.engine.getPlayer().stop();
                b.main.engine.setExternalRecording (false);
            }

            beginTest ("Macro: PLAY waits out a session operation, STOP does not");
            {
                SceneBench b;
                DeskMacroListener macro (b.main.engine);
                b.main.sessionIoBusy.store (true);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play"), 0.0);
                expect (! b.player().isPlaying(), "PLAY started during a session operation");
                expect (b.status().contains ("refused"), b.status());
                b.main.sessionIoBusy.store (false);
            }

            beginTest ("Macro: RECORD runs the host's pre-flight and shows its reason");
            {
                MainComponent main;
                main.engine.clearAllStripOverrides();
                main.engine.setStripCount (1);
                main.useCaptureDaemon = false;
                DeskMacroListener macro (main.engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/record"), 0.0);
                expect (! main.engine.isRecording(), "RECORD started without pre-flight");
                expect (main.statusLabel.getText().contains ("audio device"),
                        "pre-flight's reason was hidden: " + main.statusLabel.getText());
                main.engine.clearAllStripOverrides();
            }

            beginTest ("Macro: STOP keeps the two-tap guard, and a double fire is not two taps");
            {
                auto session = tempDir ("zf-macro-stop");
                {
                    MainComponent main;
                    main.engine.clearAllStripOverrides();
                    auto& rec = main.engine.getRecorder();
                    rec.prepare ((double) kRate, 256, 1);
                    rec.getTrack (0).armed.store (true);
                    main.engine.setActiveSessionDir (session);
                    expect (rec.startRecording (session));
                    std::array<float, 256> samples {};
                    const float* input = samples.data();
                    for (int block = 0; block < 20; ++block)
                        rec.processBlock (&input, 1, 256);

                    DeskMacroListener macro (main.engine);
                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop"), 0.0);
                    expect (main.engine.isRecording(), "the first STOP ended the take");
                    expect (main.statusLabel.getText().containsIgnoreCase ("armed"), main.statusLabel.getText());
                    expect (main.stopArmedAtMs != 0, "the first STOP must arm the guard");

                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop"), 100.0);
                    expect (main.engine.isRecording(), "a double-fired STOP ended the take");

                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop"), 600.0);
                    expect (pump ([&] { return ! main.engine.isRecording() && ! main.captureMetadataPending
                                               && main.pendingMetadataSaves == 0; }, 5000),
                            "a deliberate second STOP did not end the take");
                    expect (session.getChildFile ("Audio Files/Track_01.wav").existsAsFile());
                    main.engine.clearAllStripOverrides();
                }
                session.deleteRecursively();
            }

            beginTest ("Macro: only the pinned desk reaches the listener, over a real socket");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                auto& remotes = engine.getRemotes();

                expect (! remotes.startDeskMacro (47950, ""), "no desk address must mean no listener");
                expect (! remotes.startDeskMacro (47950, "desk.local"));
                expect (! remotes.isDeskMacroListening());
                engine.setControlsLocked (true);
                expect (! remotes.startDeskMacro (47950, "127.0.0.1"), "LOCK must refuse to start it");
                engine.setControlsLocked (false);

                int port = 0;
                for (int p = 47950; p < 47970 && port == 0; ++p)
                    if (remotes.startDeskMacro (p, "127.0.0.1")) port = p;
                expect (port != 0, "could not bind a test port");
                expect (remotes.isDeskMacroListening());

                const juce::OSCMessage play ("/zynforge/macro/play", (juce::int32) 1);
                {
                    juce::OSCSender sender;
                    expect (sender.connect ("127.0.0.1", port));
                    expect (sender.send (play));
                    expect (pump ([&] { return ! spy.actions.empty(); }, 2000), "the pinned desk was not heard");
                }
                expectEquals (remotes.getDeskMacroDropped(), (juce::int64) 0);

                remotes.stopDeskMacro();
                expect (! remotes.isDeskMacroListening());
                spy.actions.clear();
                expect (remotes.startDeskMacro (port, "10.9.8.7"));
                {
                    juce::OSCSender sender;
                    expect (sender.connect ("127.0.0.1", port));
                    expect (sender.send (play));
                    expect (sender.send (play));
                    expect (pump ([&] { return remotes.getDeskMacroDropped() >= 2; }, 2000),
                            "datagrams from another source were not counted");
                    pump ([] { return false; }, 100);
                }
                expect (spy.actions.empty(), "a datagram from another source reached the transport");
                expectEquals (remotes.getDeskMacroLastDropped(), juce::String ("127.0.0.1"));
                remotes.stopDeskMacro();
            }
        }

        void testMacroPanel()
        {
            beginTest ("Macro panel: refuses a bad address, keeps a good one, follows the engine");
            AudioEngine engine;
            auto* props = engine.getAppProps();
            props->removeValue ("deskMacroIp");
            props->removeValue ("deskMacroPort");
            props->saveIfNeeded();

            deskmacro_detail::Panel panel (engine);
            juce::ToggleButton* enable = nullptr;
            std::vector<juce::TextEditor*> editors;
            for (auto* child : panel.getChildren())
            {
                if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child)) enable = toggle;
                if (auto* editor = dynamic_cast<juce::TextEditor*> (child))   editors.push_back (editor);
            }
            const auto shown = [&]
            {
                juce::StringArray text;
                for (auto* child : panel.getChildren())
                    if (auto* label = dynamic_cast<juce::Label*> (child)) text.add (label->getText());
                return text.joinIntoString ("\n");
            };
            expect (enable != nullptr && editors.size() == 3);
            if (enable == nullptr || editors.size() != 3) return;
            auto& portEd = *editors[0];
            auto& ipEd   = *editors[1];
            auto& addressView = *editors[2];

            expectEquals (portEd.getText(), juce::String (deskmacro::kDefaultPort), "the default port");
            expect (! enable->getToggleState(), "it must start off");
            expect (shown().contains ("Off."), shown());
            for (const auto& entry : deskmacro::kAddresses)
                expect (addressView.getText().contains (entry.address), entry.address);
            expect (shown().contains ("most reliable"), "the note about DiGiCo integers is missing");

            ipEd.setText ("not an address");
            enable->setToggleState (true, juce::sendNotificationSync);
            expect (! engine.getRemotes().isDeskMacroListening());
            expect (! enable->getToggleState(), "a refused start must not stay ticked");
            expect (shown().contains ("IPv4"), shown());

            ipEd.setText ("127.0.0.1");
            portEd.setText ("");
            enable->setToggleState (true, juce::sendNotificationSync);
            expect (! engine.getRemotes().isDeskMacroListening(), "an empty port field must not bind anything");
            expect (shown().contains ("UDP port"), shown());

            ipEd.setText ("127.000.000.001");
            int port = 0;
            for (int p = 47950; p < 47970 && port == 0; ++p)
            {
                portEd.setText (juce::String (p));
                enable->setToggleState (true, juce::sendNotificationSync);
                if (engine.getRemotes().isDeskMacroListening()) port = p;
            }
            expect (port != 0, "could not bind a test port");
            expect (enable->getToggleState());
            expect (! ipEd.isEnabled() && ! portEd.isEnabled(), "a live listener's settings must not look editable");
            expect (shown().contains ("127.0.0.1"), shown());
            expectEquals (props->getIntValue ("deskMacroPort"), port);
            expectEquals (props->getValue ("deskMacroIp"), juce::String ("127.0.0.1"));

            // The listener stopped somewhere else while the panel was open.
            engine.getRemotes().stopDeskMacro();
            expect (pump ([&] { return ! enable->getToggleState(); }, 1000), "the panel showed a dead listener as on");

            engine.setControlsLocked (true);
            expect (pump ([&] { return ! enable->isEnabled(); }, 1000), "LOCK must grey the panel");
            engine.setControlsLocked (false);

            {
                deskmacro_detail::Panel reopened (engine);
                for (auto* child : reopened.getChildren())
                    if (auto* editor = dynamic_cast<juce::TextEditor*> (child); editor != nullptr && editor->getText() == "127.0.0.1")
                        port = 0;
                expectEquals (port, 0, "the panel lost the saved desk address");
            }
        }

        void testMenuItems()
        {
            beginTest ("Console menu carries Follow, desk macro and Save Console Log");
            MainComponent main;
            std::function<const juce::PopupMenu::Item* (const juce::PopupMenu&, int)> find;
            find = [&] (const juce::PopupMenu& menu, int id) -> const juce::PopupMenu::Item*
            {
                for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                {
                    const auto& item = it.getItem();
                    if (item.itemID == id) return &item;
                    if (item.subMenu != nullptr)
                        if (const auto* found = find (*item.subMenu, id)) return found;
                }
                return nullptr;
            };

            auto session = main.getMenuForIndex (3, {});
            for (const int id : { 963, 964, 969 })
                expect (find (session, id) != nullptr, "missing menu item " + juce::String (id));

            main.engine.setSceneFollowEnabled (true);
            auto ticked = main.getMenuForIndex (3, {});
            const auto* follow = find (ticked, 963);
            expect (follow != nullptr && follow->isTicked, "Follow must show its tick");
            main.engine.setSceneFollowEnabled (false);

            EngineStatus daemon;
            daemon.recording = true;
            daemon.source = "daemon";
            main.engine.setExternalCaptureStatus (daemon);
            main.engine.setExternalRecording (true);
            auto rolling = main.getMenuForIndex (3, {});
            const auto* save = find (rolling, 969);
            expect (save != nullptr && ! save->isEnabled, "the save must grey during a take");
            main.engine.setExternalRecording (false);
        }

        void testStereoPairs()
        {
            beginTest ("X32 stereo links: encode");
            {
                const auto d = x32Dialect();
                expectEquals (d.queryStereoLink (1).address, juce::String ("/config/chlink/1-2"));
                expectEquals (d.queryStereoLink (9).address, juce::String ("/config/chlink/9-10"));
                expectEquals (d.queryStereoLink (31).address, juce::String ("/config/chlink/31-32"));
                expect (! d.queryStereoLink (1).hasArgs(), "a query carries no value");
                for (const int bad : { 0, -1, 2, 30, 33 })
                    expect (d.queryStereoLink (bad).isEmpty(), juce::String (bad));
            }

            beginTest ("X32 stereo links: parse");
            {
                const auto d = x32Dialect();
                const auto on = d.parse (ConsoleMessage ("/config/chlink/1-2", { 1 }));
                expect (on.type == ConsoleEvent::Type::StereoLink);
                expectEquals (on.index, 1);
                expectEquals (on.intValue, 1);
                const auto off = d.parse (ConsoleMessage ("/config/chlink/31-32", { 0 }));
                expect (off.type == ConsoleEvent::Type::StereoLink);
                expectEquals (off.index, 31);
                expectEquals (off.intValue, 0);
                for (const char* bad : { "/config/chlink/2-3", "/config/chlink/1-3", "/config/chlink/33-34",
                                         "/config/chlink/1-2x", "/config/chlink/0-1", "/config/chlink/-1-2",
                                         "/config/chlink/1", "/config/chlink/" })
                    expect (d.parse (ConsoleMessage (bad, { 1 })).type == ConsoleEvent::Type::None, bad);
                expect (d.parse (ConsoleMessage ("/config/chlink/1-2")).type == ConsoleEvent::Type::None,
                        "no value is no answer");
            }

            beginTest ("Only a dialect with a public stereo message claims one");
            {
                for (const auto& p : consoleProfiles())
                {
                    const bool isX32 = p.kind == ConsoleProfile::Kind::BehringerX32;
                    expect (p.canReadStereoLinks == isX32, p.displayName);
                    expect ((p.dialect.queryStereoLink != nullptr) == isX32,
                            p.displayName + ": an encoder without the capability, or the reverse");
                    expect (p.dialect.parse (ConsoleMessage ("/config/chlink/1-2", { 1 })).type
                                != ConsoleEvent::Type::StereoLink || isX32, p.displayName);
                }
            }

            beginTest ("The link asks for every X32 pair, and other desks for none");
            {
                ConsoleLink link;
                std::vector<juce::String> sent;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m.address); });
                expectEquals (link.requestStereoLinks(), 16);
                expectEquals ((int) sent.size(), 16);
                if (sent.size() == 16)
                {
                    expectEquals (sent.front(), juce::String ("/config/chlink/1-2"));
                    expectEquals (sent.back(), juce::String ("/config/chlink/31-32"));
                }

                ConsoleLink other;
                other.setProfile (ConsoleProfile::Kind::DiGiCo);
                sent.clear();
                other.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m.address); });
                expectEquals (other.requestStereoLinks(), 0);
                expect (sent.empty());
            }

            beginTest ("A stereo reply reaches the link's callback");
            {
                ConsoleLink link;
                link.setMessageHook ([] (const ConsoleMessage&) {});
                int left = 0;
                bool linked = false;
                link.onStereoLink = [&] (int l, bool on) { left = l; linked = on; };
                link.injectReply (ConsoleMessage ("/config/chlink/5-6", { 1 }));
                expectEquals (left, 5);
                expect (linked);
            }

            beginTest ("Stereo links are kept only during a capture");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                remotes.onConsoleStereoLink (1, true);
                expect (remotes.takeCapturedStereoLinks().empty(),
                        "a link change on the desk must never restructure a session later");
                remotes.setConsoleNameCapture (true);
                remotes.onConsoleStereoLink (3, true);
                remotes.onConsoleStereoLink (5, false);
                remotes.setConsoleNameCapture (false);
                remotes.onConsoleStereoLink (7, true);
                const auto links = remotes.takeCapturedStereoLinks();
                expectEquals ((int) links.size(), 2);
                expect (links.at (3) && ! links.at (5));
                expect (remotes.takeCapturedStereoLinks().empty(), "taking clears");
            }

            {
                auto root = tempDir ("zf-console-sessions");
                MainComponent main;
                main.engine.clearAllStripOverrides();
                auto* props = main.engine.getAppProps();
                props->setValue ("sessionsRoot", root.getFullPathName());
                props->saveIfNeeded();

                beginTest ("Pairs from the desk link through the normal stereo path");
                const std::map<int, juce::String> names { { 1, "OH L" }, { 2, "OH R" }, { 3, "Kick" }, { 5, "Keys L" } };
                const std::map<int, bool> links { { 1, true }, { 3, false }, { 5, true } };
                const auto generation = main.engine.getRecorder().getTrackGeneration();
                main.buildSessionFromConsole (names, links, 3, "Console ");
                auto& rec = main.engine.getRecorder();
                expectEquals (rec.getNumTracks(), 6, "a pair's unnamed right half still needs its strip");
                expect (rec.getTrack (0).isStereo.load());
                expect (! rec.getTrack (1).isStereo.load());
                expect (! rec.getTrack (2).isStereo.load(), "a channel the desk reports unlinked was linked");
                expect (rec.getTrack (4).isStereo.load());
                expect (rec.getTrackGeneration() != generation, "the views were not told to rebuild");
                expectEquals (main.lastTrackCount, -1, "the strips must rebuild");
                expectEquals (main.engine.physicalFromLogical (1), 2, "a pair must collapse to one logical strip");
                expectEquals (main.engine.physicalFromLogical (2), 3);
                expectWithinAbsoluteError (rec.getTrack (0).pan.load(), -1.0f, 0.001f);
                expectWithinAbsoluteError (rec.getTrack (1).pan.load(),  1.0f, 0.001f);
                expectEquals (rec.getTrack (0).getNameThreadSafe(), juce::String ("OH L"));
                expectEquals (rec.getTrack (1).getNameThreadSafe(), juce::String ("OH R"));
                expect (main.statusLabel.getText().contains ("2 stereo pair(s) linked"), main.statusLabel.getText());

                beginTest ("Linked pairs round-trip through the session file");
                const auto session = main.engine.getActiveSessionDir();
                expect (session.getParentDirectory() == root, "the test escaped its temp root");
                {
                    AudioEngine reopened;
                    expect (reopened.loadSessionMixFrom (session));
                    expect (reopened.getRecorder().getTrack (0).isStereo.load());
                    expect (! reopened.getRecorder().getTrack (2).isStereo.load());
                    reopened.clearAllStripOverrides();
                }

                beginTest ("A desk with no stereo data keeps its names and says to link by hand");
                main.engine.clearAllStripOverrides();
                main.buildSessionFromConsole ({ { 1, "Vox" }, { 2, "Gtr" } }, {}, 0, "Console ");
                expectEquals (main.engine.getRecorder().getNumTracks(), 2);
                expect (! main.engine.getRecorder().getTrack (0).isStereo.load());
                expectEquals (main.engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("Vox"));
                expect (main.statusLabel.getText().contains ("by hand"), main.statusLabel.getText());

                beginTest ("A lost link reply is reported, not taken for mono");
                main.engine.clearAllStripOverrides();
                main.buildSessionFromConsole ({ { 1, "A" }, { 2, "B" } }, { { 1, true } }, 16, "Console ");
                expect (main.statusLabel.getText().contains ("1 of 16"), main.statusLabel.getText());

                beginTest ("Create session from a connected X32: names and pairs arrive together");
                {
                    main.consoleLink.setMessageHook ([&] (const ConsoleMessage& m)
                    {
                        if (m.address == "/ch/01/config/name")
                            main.consoleLink.injectReply (ConsoleMessage (m.address, { juce::String ("Bass L") }));
                        else if (m.address == "/ch/02/config/name")
                            main.consoleLink.injectReply (ConsoleMessage (m.address, { juce::String ("Bass R") }));
                        else if (m.address == "/ch/03/config/name")
                            main.consoleLink.injectReply (ConsoleMessage (m.address, { juce::String ("Lead") }));
                        else if (m.address.startsWith ("/config/chlink/"))
                            main.consoleLink.injectReply (ConsoleMessage (m.address, { m.address.endsWith ("/1-2") ? 1 : 0 }));
                    });
                    main.engine.clearAllStripOverrides();
                    main.createSessionFromConsole();
                    const auto started = juce::Time::getMillisecondCounterHiRes();
                    expect (pump ([&] { return main.statusLabel.getText().startsWith ("Session created"); }, 5000),
                            "the session was never built: " + main.statusLabel.getText());
                    expect (juce::Time::getMillisecondCounterHiRes() - started < 5000.0);
                    expectEquals (main.engine.getRecorder().getNumTracks(), 3);
                    expect (main.engine.getRecorder().getTrack (0).isStereo.load());
                    expect (main.statusLabel.getText().contains ("1 stereo pair(s) linked"), main.statusLabel.getText());
                    main.consoleLink.setMessageHook (nullptr);
                }

                props->removeValue ("sessionsRoot");
                props->saveIfNeeded();
                main.engine.setActiveSessionDir ({});
                main.engine.clearAllStripOverrides();
                root.deleteRecursively();
            }
        }

        void testConsoleLog()
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Log: nothing is stored while debug is off, and what was captured stays");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                remotes.logOsc ("ignored");
                expect (remotes.getOscLog().isEmpty());
                remotes.setOscDebug (true);
                remotes.logOsc ("kept");
                remotes.setOscDebug (false);
                remotes.logOsc ("also ignored");
                expectEquals (remotes.getOscLog().size(), 1);
                expect (remotes.getOscLog()[0].endsWith ("kept"));
            }

            beginTest ("Log: lines carry a date and milliseconds");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                remotes.setOscDebug (true);
                remotes.logOsc ("x");
                const auto line = remotes.getOscLog()[0];
                expect (line.substring (0, 4) == juce::Time::getCurrentTime().formatted ("%Y"), line);
                expect (line.upToFirstOccurrenceOf ("  ", false, false).fromLastOccurrenceOf (".", false, false).length() == 3, line);
            }

            beginTest ("Log: the cap holds a whole session and stays bounded");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                remotes.setOscDebug (true);
                expect (RemoteControl::kOscLogMaxLines >= 50000, "the old 200-line cap lost a session in seconds");
                for (int i = 0; i < RemoteControl::kOscLogMaxLines + 10; ++i)
                    remotes.logOsc ("line " + juce::String (i));
                const auto log = remotes.getOscLog();
                expectEquals (log.size(), RemoteControl::kOscLogMaxLines);
                expectEquals (remotes.getOscLogEvicted(), (juce::int64) 10);
                expect (log[log.size() - 1].endsWith ("line " + juce::String (RemoteControl::kOscLogMaxLines + 9)),
                        "the newest line must survive");

                const auto tail = remotes.getOscLogTail (3);
                expectEquals (tail.size(), 3);
                expect (tail[2] == log[log.size() - 1]);

                const auto revision = remotes.getOscLogRevision();
                remotes.logOsc (juce::String::repeatedString ("x", 5000));
                expect (remotes.getOscLogRevision() != revision);
                expect (remotes.getOscLogTail (1)[0].length() < RemoteControl::kOscLogMaxChars + 40,
                        "one line must not defeat the memory bound");
                expect (remotes.formatConsoleLog ("Console link: test").contains ("Oldest 11 lines"));

                remotes.clearOscLog();
                expect (remotes.getOscLog().isEmpty());
                expectEquals (remotes.getOscLogEvicted(), (juce::int64) 0);
            }

            beginTest ("Log: the formatter shows types and values, and never a credential");
            {
                const juce::OSCMessage console ("/Console/Snapshots/recall", (juce::int32) 5, juce::String ("Verse"), 0.5f);
                expectEquals (consolelog::describe (console),
                              juce::String ("/Console/Snapshots/recall ,isf 5 \"Verse\" 0.500000"));
                expectEquals (consolelog::describe (juce::OSCMessage ("/info")), juce::String ("/info ,"));

                juce::MemoryBlock blob ("abc", 3);
                juce::OSCMessage withBlob ("/x");
                withBlob.addBlob (blob);
                expect (consolelog::describe (withBlob).contains (",b"), "a blob must not vanish from the log");

                const auto generic = consolelog::describe (juce::OSCMessage ("/zynforge/record", (juce::int32) 1, juce::String ("s3cret")));
                expect (! generic.contains ("s3cret"), generic);
                expect (generic.contains ("/zynforge/record"), generic);

                const auto failed = consolelog::describe (juce::OSCMessage ("/zynforge/record", juce::String ("wrong-guess")));
                expect (! failed.contains ("wrong-guess"), "a failed attempt is still a credential: " + failed);

                expect (consolelog::describe (juce::OSCMessage ("/zynforge/macro/marker/goto", (juce::int32) 4)).endsWith ("4"),
                        "a macro's number is what the desk test needs to see");
                expect (! consolelog::describe (juce::OSCMessage ("/zynforge/macro/play", juce::String ("s3cret"))).contains ("s3cret"));

                const auto out = consolelog::describe (ConsoleMessage ("/ch/01/config/name", { 3, 0.25, juce::String ("x") }));
                expect (out.contains (",ifs"), out);
                expect (out.contains ("\"x\""), out);
            }

            beginTest ("Log: the OSC listener records sender, types and values, under LOCK too");
            {
                AudioEngine engine;
                engine.setStripCount (1);
                auto& remotes = engine.getRemotes();
                remotes.setOscDebug (true);
                int port = 0;
                for (int p = 47970; p < 47990 && port == 0; ++p)
                    if (remotes.startOsc (p, 1)) port = p;
                expect (port != 0, "could not bind a test port");

                juce::OSCSender sender;
                expect (sender.connect ("127.0.0.1", port));
                engine.setControlsLocked (true);
                expect (sender.send (juce::OSCMessage ("/Console/Whatever", (juce::int32) 7)));
                expect (pump ([&] { return remotes.getOscLog().joinIntoString ("\n").contains ("/Console/Whatever"); }, 2000),
                        "a desk's message was lost to LOCK");
                engine.setControlsLocked (false);
                const auto log = remotes.getOscLog().joinIntoString ("\n");
                expect (log.contains ("osc <- 127.0.0.1:"), log);
                expect (log.contains (",i 7"), log);

                beginTest ("Log: a bundle and a refused datagram are visible, not silent");
                juce::OSCBundle bundle;
                bundle.addElement (juce::OSCMessage ("/Console/InABundle", (juce::int32) 2));
                expect (sender.send (bundle));
                expect (pump ([&] { return remotes.getOscLog().joinIntoString ("\n").contains ("/Console/InABundle"); }, 2000),
                        "a bundle vanished from the log");

                juce::DatagramSocket raw (false);
                juce::MemoryOutputStream refused;
                refused.write ("/Console/Odd\0\0\0\0", 16);
                refused.write (",T\0\0", 4);       // a boolean: the parser refuses the packet
                expectEquals (raw.write ("127.0.0.1", port, refused.getData(), (int) refused.getDataSize()),
                              (int) refused.getDataSize());
                expect (pump ([&] { return remotes.getOscLog().joinIntoString ("\n").contains ("refused by the parser"); }, 2000),
                        "a datagram the parser refuses must not look like silence");
                const auto after = remotes.getOscLog().joinIntoString ("\n");
                expect (after.contains ("/Console/Odd"), after);
                expect (after.contains (",T"), "the type tag is the point of the line: " + after);

                beginTest ("Log: the live Generic token never reaches the log or the saved file");
                remotes.stopOsc();
                port = 0;
                for (int p = 47970; p < 47990 && port == 0; ++p)
                    if (remotes.startOsc (p, 0)) port = p;
                const auto token = remotes.getOscAccessToken();
                expect (token.isNotEmpty());
                juce::OSCSender generic;
                expect (generic.connect ("127.0.0.1", port));
                expect (generic.send (juce::OSCMessage ("/zynforge/marker", juce::String ("note"), token)));
                expect (generic.send (juce::OSCMessage ("/Console/Leak", token, (juce::int32) 1)));
                expect (generic.send (juce::OSCMessage (juce::OSCAddressPattern ("/" + token), (juce::int32) 1)));
                juce::OSCBundle tokenBundle;
                tokenBundle.addElement (juce::OSCMessage ("/zynforge/record", (juce::int32) 1, token));
                expect (generic.send (tokenBundle));
                expect (pump ([&] { return remotes.getOscLog().joinIntoString ("\n").contains ("[bundle, not acted on] /zynforge/record"); }, 2000));
                const auto saved = remotes.formatConsoleLog ("Console link: test");
                expect (saved.contains ("/zynforge/marker"), "the fixture never logged the attempt");
                expect (saved.contains ("/Console/Leak"));
                expect (! saved.contains (token), "the token reached a copyable log");
                remotes.logOsc ("link <- 10.0.0.1:1  /ch/01/config/name ,s \"" + token + "\"");
                expect (! remotes.getOscLog().joinIntoString ("\n").contains (token),
                        "a token on the console path reached the log");
                remotes.logOsc (juce::String::repeatedString ("x", RemoteControl::kOscLogMaxChars - 8) + token);
                expect (! remotes.getOscLogTail (1)[0].contains (token.substring (0, 6)),
                        "cutting a line to length must not keep part of a token");
                remotes.stopOsc();
            }

            beginTest ("Log: the desk macro listener logs what it heard and what it did");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                remotes.setOscDebug (true);
                TransportSpy spy;
                spy.attach (engine);
                DeskMacroListener macro (engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play", (juce::int32) 1), 0.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play", (juce::int32) 1), 10.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/arm", (juce::int32) 1), 20.0);
                const auto log = remotes.getOscLog().joinIntoString ("\n");
                expect (log.contains ("macro <- "), log);
                expect (log.contains ("/zynforge/macro/play ,i 1"), log);
                expect (log.contains ("macro: PLAY"), log);
                expect (log.contains ("debounce"), log);
                expect (log.contains ("/zynforge/macro/arm"), "an address it refuses is the one a test needs to see");
            }

            beginTest ("Log: the console link records both directions with the sender");
            {
                juce::DatagramSocket desk (false);
                int deskPort = 0;
                for (int p = 47990; p < 48000 && deskPort == 0; ++p)
                    if (desk.bindToPort (p, "127.0.0.1")) deskPort = p;
                expect (deskPort != 0, "test desk failed to bind");

                juce::StringArray lines;
                ConsoleLink link;
                link.onTraffic = [&] (const juce::String& line) { lines.add (line); };
                expect (link.connect ("127.0.0.1", deskPort));

                char probe[512];
                juce::String fromIp;
                int linkPort = 0;
                expect (desk.waitUntilReady (true, 2000) == 1, "no probe from the link");
                expect (desk.read (probe, sizeof (probe), false, fromIp, linkPort) > 0);
                expect (lines.joinIntoString ("\n").contains ("link -> 127.0.0.1:" + juce::String (deskPort) + "  /info ,"),
                        lines.joinIntoString ("\n"));

                juce::OSCSender reply;
                expect (reply.connectToSocket (desk, "127.0.0.1", linkPort));
                juce::Thread::sleep (50);
                expect (reply.send (juce::OSCMessage ("/ch/01/config/name", juce::String ("KICK"))));
                juce::OSCBundle bundle;
                bundle.addElement (juce::OSCMessage ("/in/a/bundle", (juce::int32) 1));
                expect (reply.send (bundle));
                expect (pump ([&] { return lines.joinIntoString ("\n").contains ("/in/a/bundle"); }, 3000),
                        "traffic the link doesn't act on must still be logged: " + lines.joinIntoString ("\n"));
                const auto all = lines.joinIntoString ("\n");
                expect (all.contains ("link <- 127.0.0.1:" + juce::String (deskPort)
                                      + "  /ch/01/config/name ,s \"KICK\""), all);
                link.disconnect();
            }

            beginTest ("Log: a real host wires the console link into the log");
            {
                MainComponent main;
                main.engine.getRemotes().setOscDebug (true);
                main.consoleLink.setMessageHook ([] (const ConsoleMessage&) {});
                main.consoleLink.requestChannelNames (1);
                main.consoleLink.injectReply (ConsoleMessage ("/ch/01/config/name", { juce::String ("Snare") }));
                const auto log = main.engine.getRemotes().getOscLog().joinIntoString ("\n");
                expect (log.contains ("link -> "), log);
                expect (log.contains ("link <- "), log);
                main.consoleLink.setMessageHook (nullptr);
                main.engine.clearAllStripOverrides();

                beginTest ("Save Console Log writes the header and every line, off the message thread");
                auto folder = tempDir ("zf-console-log");
                const auto file = folder.getChildFile ("log.txt");
                main.writeConsoleLog (file);
                expect (pump ([&] { return file.existsAsFile() && ! main.sessionIoBusy.load(); }, 5000),
                        "the log was never written");
                const auto text = file.loadFileAsString();
                expect (text.contains ("Console link:"), text);
                expect (text.contains ("/ch/01/config/name"), text);
                expect (main.statusLabel.getText().contains ("saved"), main.statusLabel.getText());

                beginTest ("Save Console Log refuses during a take");
                const auto during = folder.getChildFile ("during.txt");
                EngineStatus daemon;
                daemon.recording = true;
                daemon.source = "daemon";
                main.engine.setExternalCaptureStatus (daemon);
                main.engine.setExternalRecording (true);
                main.writeConsoleLog (during);
                main.engine.setExternalRecording (false);
                pump ([] { return false; }, 100);
                expect (! during.existsAsFile(), "the log was written during a take");
                folder.deleteRecursively();
            }

            beginTest ("A log request to a console goes out logged, message by message");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                remotes.setOscDebug (true);
                expect (remotes.requestConsoleChannelNames ("127.0.0.1", 47999, 1));
                const auto log = remotes.getOscLog().joinIntoString ("\n");
                expect (log.contains ("osc -> 127.0.0.1:47999  /Console/Channels/? ,"), log);
                expect (log.contains ("osc -> 127.0.0.1:47999  /Console/Channels ,i 1"), log);
            }
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            testSceneRule();
            testSceneFollowOnBothPaths();
            testMacroParsing();
            testMacroCommands();
            testMacroPanel();
            testMenuItems();
            testStereoPairs();
            testConsoleLog();
        }
    };

    static ConsoleVscTests consoleVscTests;
}
