// The Twin Recorder link wired into the real MainComponent: what a remote
// RECORD and STOP do here, how the two-tap STOP gates the link, and what the
// floating panel lets the operator change while a take rolls. The peer is a
// hand-driven socket, so the wire is asserted directly.

#include <juce_core/juce_core.h>

#include "../Network/CaptureIdentity.h"
#include "../Network/CaptureProtocol.h"
#include "../Network/LineSocket.h"
#include "../Network/SecureToken.h"
#include "../UI/MainComponent.h"
#include "../UI/TwinRecorderDialog.h"

#include <atomic>
#include <deque>
#include <thread>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    namespace
    {
        namespace identity = capture::identity;
        const juce::String kCode = "7K3M-Q2XD-9HVB-T4WN";
        const juce::String kKey  = "7K3MQ2XD9HVBT4WN";    // the code as both ends hash it

        // Plays the follower for a real leader, and records what it is told.
        struct FollowerSocket
        {
            juce::StreamingSocket listener;
            std::unique_ptr<juce::StreamingSocket> socket;
            juce::MemoryBlock scratch;
            std::deque<juce::String> lines;
            juce::StringArray messages;
            std::thread reader;
            std::atomic<bool> quit { false }, linked { false };
            std::mutex lock;

            int listen()
            {
                return listener.createListener (0, "127.0.0.1") ? listener.getBoundPort() : -1;
            }

            static juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
            {
                auto* o = new juce::DynamicObject();
                for (const auto& [name, value] : fields) o->setProperty (name, value);
                return juce::var (o);
            }

            juce::var readLine()
            {
                while (! quit.load())
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

            void serve()
            {
                reader = std::thread ([this]
                {
                    if (listener.waitUntilReady (true, 3000) <= 0) return;
                    socket.reset (listener.waitForNextConnection());
                    if (socket == nullptr) return;
                    const auto nonce = securetoken::generate();
                    linesocket::writeAll (*socket, capture::frame (object ({
                        { "type", "twin.hello" }, { "version", 1 }, { "role", "follower" }, { "nonce", nonce } })));
                    const auto auth = readLine();
                    const auto text = "zynforge-twin:1:" + nonce + ":" + auth.getProperty ("nonce", "").toString();
                    linesocket::writeAll (*socket, capture::frame (object ({
                        { "type", "twin.accept" }, { "proof", identity::authenticate (kKey, "follower:" + text) } })));
                    const auto key = identity::authenticate (kKey, "session:" + text);
                    juce::int64 sent = 0;
                    auto nextStatus = linesocket::Clock::now();
                    while (! quit.load())
                    {
                        if (linesocket::Clock::now() >= nextStatus)
                        {
                            const auto body = juce::JSON::toString (object ({ { "m", "status" }, { "name", "peer" } }), true);
                            const auto sequence = ++sent;
                            linesocket::writeAll (*socket, capture::frame (object ({
                                { "type", "twin.frame" }, { "seq", sequence }, { "body", body },
                                { "mac", identity::authenticate (key, "F:" + juce::String (sequence) + ":" + body) } })));
                            nextStatus = linesocket::Clock::now() + std::chrono::milliseconds (50);
                        }
                        if (! linesocket::pumpLines (*socket, scratch,
                                [this] (const juce::String& l) { lines.push_back (l); }, 10))
                            return;
                        while (! lines.empty())
                        {
                            const auto body = juce::JSON::parse (juce::JSON::parse (lines.front())
                                                                     .getProperty ("body", "").toString());
                            lines.pop_front();
                            const auto type = body.getProperty ("m", "").toString();
                            if (type == "status") { linked.store (true); continue; }
                            const std::lock_guard<std::mutex> guard (lock);
                            messages.add (type);
                        }
                    }
                });
            }

            int count (const juce::String& type)
            {
                const std::lock_guard<std::mutex> guard (lock);
                int n = 0;
                for (const auto& message : messages) if (message == type) ++n;
                return n;
            }

            ~FollowerSocket()
            {
                quit.store (true);
                if (reader.joinable()) reader.join();
            }
        };
    }

    class TwinLinkHostTests final : public juce::UnitTest
    {
    public:
        TwinLinkHostTests() : juce::UnitTest ("Twin recorder host", "zynforge") {}

        static bool settle (MainComponent& main, const std::function<bool()>& done, int timeoutMs = 8000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            for (;;)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
               #elif JUCE_MODAL_LOOPS_PERMITTED
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
               #else
                juce::Thread::sleep (5);
               #endif
                main.twinLink.tick();
                if (done()) return true;
                if (juce::Time::getMillisecondCounterHiRes() > end) return false;
            }
        }

        static juce::File tempSession()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-twin-host-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        bool linkAsLeader (MainComponent& main, FollowerSocket& peer)
        {
            const int port = peer.listen();
            expect (port > 0);
            peer.serve();
            twin::Config config;
            config.role = twin::Role::Leader;
            config.port = port;
            config.peerAddress = "127.0.0.1";
            config.pairingCode = kCode;
            config.reconnectMs = 50;
            juce::String error;
            expect (main.twinLink.start (config, error), error);
            const bool up = settle (main, [&]
            {
                return main.twinLink.snapshot().state == twin::LinkState::Linked && peer.linked.load();
            });
            expect (up, "the leader never linked");
            return up;
        }

        // A take rolling on the real recorder, known to the link as a relayed one.
        void rollTake (MainComponent& main, const juce::File& session)
        {
            auto& recorder = main.engine.getRecorder();
            recorder.prepare (48000.0, 256, 1);
            recorder.getTrack (0).armed.store (true, std::memory_order_relaxed);
            main.engine.setActiveSessionDir (session);
            expect (recorder.startRecording (session), "synthetic capture did not start");
            std::vector<float> audio (256, 0.2f);
            const float* input = audio.data();
            for (int block = 0; block < 16; ++block) recorder.processBlock (&input, 1, 256);
        }

        void settleStopped (MainComponent& main)
        {
            expect (settle (main, [&] { return ! main.captureMetadataPending
                                            && main.pendingMetadataSaves == 0
                                            && ! main.engine.isRecording(); }, 6000));
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };
            {
                MainComponent main;

                beginTest ("Off by default: nothing listens, and no status element shows");
                expect (! main.twinLink.isEnabled());
                expectEquals (main.twinLink.getBoundPort(), -1);
                main.updateTwinStatusLabel();
                expect (! main.twinStatusLabel.isVisible());

                beginTest ("Menu item 968 is built and dispatches");
                expect (main.getMenuCommands().handles (968), "968 has no handler");

                beginTest ("A follower take runs the host's own pre-flight; the reason travels back");
                {
                    juce::String refusal;
                    expect (! main.twinStartRecord (refusal));
                    expect (refusal.containsIgnoreCase ("channel") || refusal.containsIgnoreCase ("device"), refusal);
                    expect (! main.engine.isRecording());
                }

                beginTest ("LOCK refuses a link RECORD, as it refuses every remote RECORD");
                {
                    main.onLockToggled();
                    juce::String refusal;
                    expect (! main.twinStartRecord (refusal));
                    expect (refusal.containsIgnoreCase ("locked"), refusal);
                    main.onLockToggled();
                }

                beginTest ("A punch in progress is never ended by a link RECORD");
                {
                    main.punchSessionActive = true;
                    juce::String refusal;
                    expect (! main.twinStartRecord (refusal));
                    expect (refusal.containsIgnoreCase ("punch"), refusal);
                    expect (main.punchSessionActive, "the link ended a punch");
                    main.punchSessionActive = false;
                }

                beginTest ("A link RECORD refuses to punch; a local press may");
                {
                    const auto session = tempSession();
                    MainComponent::RecordStartPlan plan;
                    plan.dir = session;
                    plan.continueTake = true;
                    plan.punchAt = 4800;
                    plan.numTracks = 1;
                    plan.armed = 1;
                    main.twinStartInFlight = true;
                    main.runRecordPlan (plan);
                    main.twinStartInFlight = false;
                    expect (! main.engine.isRecording(), "a link start opened a punch");
                    expect (main.statusLabel.getText().containsIgnoreCase ("punch"), main.statusLabel.getText());
                    session.deleteRecursively();
                }

                beginTest ("The status view covers a daemon take");
                {
                    EngineStatus daemon;
                    daemon.recording = true;
                    daemon.elapsedSamples = 96000;
                    daemon.sampleRate = 96000.0;
                    daemon.armedTracks = 4;
                    daemon.backupFailed = true;
                    daemon.source = "daemon";
                    main.engine.setExternalCaptureStatus (daemon, juce::Time::currentTimeMillis());
                    main.engine.setExternalRecording (true);
                    const auto status = main.twinStatus();
                    main.engine.setExternalRecording (false);
                    expect (status.recording);
                    expectEquals (status.elapsedSamples, (juce::int64) 96000);
                    expectEquals (status.armedTracks, 4);
                    expectEquals (status.writeFailures, 1);
                    expectEquals (status.backupHealth, juce::String ("failed"));
                    main.engine.invalidateExternalCaptureStatus();
                }

                beginTest ("Leader RECORD: the take is running before the link hears of it");
                {
                    FollowerSocket peer;
                    if (! linkAsLeader (main, peer)) return;
                    const auto session = tempSession();
                    auto& recorder = main.engine.getRecorder();
                    recorder.prepare (48000.0, 256, 1);
                    recorder.getTrack (0).armed.store (true, std::memory_order_relaxed);
                    main.engine.setActiveSessionDir (session);
                    MainComponent::RecordStartPlan plan;
                    plan.dir = session;
                    plan.numTracks = 1;
                    plan.armed = 1;
                    main.runRecordPlan (plan);
                    expect (main.engine.isRecording(), main.statusLabel.getText());
                    expect (settle (main, [&] { return peer.count ("start") == 1; }), "no start reached the follower");

                    beginTest ("Two-tap STOP: the first tap sends nothing, the second ends the take and then sends");
                    main.onStopClicked();
                    expect (main.engine.isRecording(), "one tap ended the take");
                    expect (! settle (main, [&] { return peer.count ("stop") > 0; }, 250),
                            "the follower was told on the first tap");
                    main.onStopClicked();
                    expect (! main.engine.isRecording());
                    expect (settle (main, [&] { return peer.count ("stop") == 1; }), "stop never reached the follower");
                    settleStopped (main);
                    main.twinLink.stop();
                    session.deleteRecursively();
                }

                beginTest ("Two-tap STOP from the RECORD button sends STOP once as well");
                {
                    FollowerSocket peer;
                    if (! linkAsLeader (main, peer)) return;
                    const auto session = tempSession();
                    rollTake (main, session);
                    main.twinLink.noteLocalTakeStarted();
                    main.handleRecordPressDuringTake();
                    expect (main.engine.isRecording());
                    main.handleRecordPressDuringTake();
                    expect (! main.engine.isRecording());
                    expect (settle (main, [&] { return peer.count ("stop") == 1; }));
                    settleStopped (main);
                    main.twinLink.stop();
                    session.deleteRecursively();
                }

                beginTest ("A punch on the leader tells the follower nothing");
                {
                    FollowerSocket peer;
                    if (! linkAsLeader (main, peer)) return;
                    const auto session = tempSession();
                    rollTake (main, session);
                    main.stopActiveCaptureAsync (false);
                    settleStopped (main);

                    MainComponent::RecordStartPlan plan;
                    plan.dir = session;
                    plan.continueTake = true;
                    plan.punchAt = 512;
                    plan.numTracks = 1;
                    plan.armed = 1;
                    main.runRecordPlan (plan);
                    expect (main.engine.isRecording(), main.statusLabel.getText());
                    expect (main.manualPunchActive, "the fixture did not open a punch");
                    expect (! settle (main, [&] { return peer.count ("start") > 0; }, 150), "a punch started the follower");
                    std::vector<float> audio (256, 0.1f);
                    const float* input = audio.data();
                    for (int block = 0; block < 4; ++block)
                        main.engine.getRecorder().processBlock (&input, 1, 256);
                    main.onStopClicked();
                    settleStopped (main);
                    expect (! settle (main, [&] { return peer.count ("stop") > 0; }, 150), "a punch stopped the follower");
                    main.twinLink.stop();
                    session.deleteRecursively();
                }

                beginTest ("Stop & Quit's path sends no STOP: the follower must keep rolling");
                {
                    FollowerSocket peer;
                    if (! linkAsLeader (main, peer)) return;
                    const auto session = tempSession();
                    rollTake (main, session);
                    main.twinLink.noteLocalTakeStarted();
                    main.stopActiveCaptureAsync (false);
                    settleStopped (main);
                    expect (! settle (main, [&] { return peer.count ("stop") > 0; }, 150),
                            "an unconfirmed stop reached the follower");
                    main.twinLink.stop();
                    session.deleteRecursively();
                }

                beginTest ("A link STOP ends the take through finalization and reports back");
                {
                    const auto session = tempSession();
                    rollTake (main, session);
                    main.stopArmedAtMs = juce::Time::getMillisecondCounter();
                    bool finished = false, clean = false;
                    main.twinStopRecord ([&] (bool ok, juce::String) { finished = true; clean = ok; });
                    expect (! main.engine.isRecording());
                    expectEquals ((int) main.stopArmedAtMs, 0, "a stale armed tap outlived the take");
                    expect (settle (main, [&] { return finished; }, 6000), "the link STOP never completed");
                    expect (clean, main.lastCaptureFinalizationError);
                    expect (session.getChildFile ("session.report.json").existsAsFile());
                    session.deleteRecursively();
                }

                beginTest ("LOCK refuses a link STOP: the take keeps rolling and the leader hears why");
                {
                    const auto session = tempSession();
                    rollTake (main, session);
                    main.onLockToggled();
                    bool finished = false, clean = true;
                    juce::String why;
                    main.twinStopRecord ([&] (bool ok, juce::String text)
                    { finished = true; clean = ok; why = text; });
                    expect (finished && ! clean);
                    expect (why.containsIgnoreCase ("locked"), why);
                    expect (main.engine.isRecording(), "LOCK did not hold the take");
                    main.onLockToggled();
                    main.stopActiveCaptureAsync (false);
                    settleStopped (main);
                    session.deleteRecursively();
                }

                beginTest ("A marker dropped at the transport reaches the link with its final name");
                {
                    juce::String dropped;
                    const auto saved = main.engine.onMarkerDropped;
                    main.engine.onMarkerDropped = [&] (const juce::String& name) { dropped = name; };
                    const auto session = tempSession();
                    rollTake (main, session);
                    main.engine.getMarkers().setContext (session, 48000.0);
                    expect (main.engine.dropMarkerAtCurrentPosition ("Verse") >= 0);
                    expectEquals (dropped, juce::String ("Verse"));
                    expect (main.engine.dropMarkerAtCurrentPosition() >= 0);
                    expect (dropped.startsWith ("Marker"), dropped);
                    main.engine.onMarkerDropped = saved;
                    main.stopActiveCaptureAsync (false);
                    settleStopped (main);
                    session.deleteRecursively();
                }

                beginTest ("The header shows the link, loudly when it is lost");
                {
                    FollowerSocket* peer = new FollowerSocket();
                    if (! linkAsLeader (main, *peer)) { delete peer; return; }
                    main.updateTwinStatusLabel();
                    expect (main.twinStatusLabel.isVisible());
                    expect (main.twinStatusLabel.getText().startsWith ("TWIN"), main.twinStatusLabel.getText());
                    delete peer;
                    expect (settle (main, [&] { return main.twinLink.snapshot().state == twin::LinkState::Lost; }));
                    main.updateTwinStatusLabel();
                    expectEquals (main.twinStatusLabel.getText(), juce::String ("TWIN LOST"));
                    expect (main.twinStatusLabel.findColour (juce::Label::backgroundColourId) == brand::accentRecord,
                            "TWIN LOST is not a loud fill");
                    main.twinLink.stop();
                    main.updateTwinStatusLabel();
                    expect (! main.twinStatusLabel.isVisible());
                }

                beginTest ("The panel keeps its warning and re-derives what is editable");
                {
                    std::atomic<bool> rolling { false };
                    int settingsWrites = 0;
                    TwinRecorderDialog::Content content (main.twinLink, [&] { return rolling.load(); },
                                                         [] { return (juce::PropertiesFile*) nullptr; },
                                                         [&] { ++settingsWrites; });
                    expect (content.getWarning().containsIgnoreCase ("not an independent recorder"));
                    expect (content.getWarning().containsIgnoreCase ("app bug"));
                    expect (content.getWarning().containsIgnoreCase ("not encrypted"));
                    content.setRole (twin::Role::Follower);
                    expectEquals (content.getCode().length(), 19);
                    expect (content.canRegenerate());
                    expect (content.roleEditable());

                    rolling.store (true);
                    content.refresh();
                    expect (! content.roleEditable(), "the role stayed editable under a take");
                    expect (! content.canRegenerate(), "the code could change under a take");
                    const auto codeBefore = content.getCode();
                    content.pressRegenerate();
                    expectEquals (content.getCode(), codeBefore, "a new code landed under a take");
                    rolling.store (false);
                    content.refresh();
                    expect (content.roleEditable());

                    content.setPort (80);
                    content.pressEnable();
                    expect (! main.twinLink.isEnabled(), "a privileged port started the link");
                    expect (content.getDetails().containsIgnoreCase ("1024"), content.getDetails());

                    content.setRole (twin::Role::Leader);
                    content.setPort (49877);
                    content.setPeerAddress ("mini.local");
                    content.setCode (kCode);
                    content.pressEnable();
                    expect (! main.twinLink.isEnabled(), "a name was accepted as an address");
                    expect (content.getDetails().containsIgnoreCase ("IP address"), content.getDetails());

                    content.setPeerAddress ("127.0.0.1");
                    content.pressEnable();
                    expect (main.twinLink.isEnabled());
                    content.refresh();
                    expect (! content.roleEditable(), "the role stayed editable while linked");
                    expect (content.getStateLine().containsIgnoreCase ("connecting"), content.getStateLine());
                    content.pressEnable();
                    expect (! main.twinLink.isEnabled());
                }
            }
        }
    };

    static TwinLinkHostTests twinLinkHostTests;
}
