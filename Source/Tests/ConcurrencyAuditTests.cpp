#include "../Audio/AudioEngine.h"
#include "../Network/CaptureLink.h"
#include "../Network/CompanionServer.h"
#include "../UI/MainComponent.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif
#if JUCE_MAC || JUCE_LINUX
 #include <poll.h>
 #include <sys/socket.h>
#endif

namespace zynforge
{
    // Private access is restricted to scheduling existing paths, never replacing
    // the behavior under test. Every asynchronous closure owns its test state.
    struct ConcurrencyAuditAccess
    {
        static AudioEngine& engine (MainComponent& main) { return main.engine; }
        static bool deleteSession (MainComponent& main, const juce::File& directory)
        { return main.deleteCaptureSession (directory); }
        static void stopCapture (MainComponent& main, std::function<void (bool)> completion)
        { main.stopActiveCaptureAsync (true, std::move (completion)); }
        static void finalizationHook (MultitrackRecorder& recorder, std::function<void()> hook)
        { recorder.beforeFinalizationForTests = std::move (hook); }

        static void reportHooks (MultitrackRecorder& recorder,
                                 std::function<void()> beforeScan,
                                 std::function<void()> beforePublish,
                                 std::function<void()> afterPublish,
                                 std::function<void()> finished)
        {
            auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
            hooks->beforeScan = std::move (beforeScan);
            hooks->beforePublish = std::move (beforePublish);
            hooks->afterPublish = std::move (afterPublish);
            hooks->finished = std::move (finished);
            recorder.reportTestHooks = std::move (hooks);
        }

        static int companionPort (const CompanionServer& server)
        { return server.listener != nullptr ? server.listener->getBoundPort() : -1; }

        static void streamFixture (CompanionServer& server, std::function<void()> hook)
        {
            server.streamRing.allocate (64);
            server.streamReadTestHook = std::move (hook);
        }

        static bool streamActive (const CompanionServer& server)
        { return server.streamRing.active.load() > 0; }

        static void constrainSocketBuffers (capture::CaptureClient& client,
                                            juce::StreamingSocket& peer)
        {
            // Isolate the send-deadline layer from handshake negotiation. The
            // disposable peer deliberately never reads, including Hello.
            client.authenticated.store (true);
           #if JUCE_MAC || JUCE_LINUX
            const int bytes = 4096;
            if (client.socket != nullptr)
                setsockopt (client.socket->getRawSocketHandle(), SOL_SOCKET, SO_SNDBUF,
                            &bytes, sizeof (bytes));
            setsockopt (peer.getRawSocketHandle(), SOL_SOCKET, SO_RCVBUF, &bytes, sizeof (bytes));
           #else
            juce::ignoreUnused (client, peer);
           #endif
        }

        static void sendRawFrame (capture::CaptureClient& client, const juce::String& line)
        {
           #if JUCE_MAC || JUCE_LINUX
            // Bypass outgoing validation to test the peer's input boundary.
            // Socket ownership stays with client; all sends are nonblocking.
            const std::lock_guard<std::timed_mutex> lock (client.writeLock);
            if (client.socket == nullptr) return;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds (3);
            const char* bytes = line.toRawUTF8();
            size_t remaining = line.getNumBytesAsUTF8();
            while (remaining > 0 && std::chrono::steady_clock::now() < end)
            {
                pollfd descriptor { client.socket->getRawSocketHandle(), POLLOUT, 0 };
                if (::poll (&descriptor, 1, 20) <= 0) continue;
                int flags = MSG_DONTWAIT;
               #ifdef MSG_NOSIGNAL
                flags |= MSG_NOSIGNAL;
               #endif
                const auto sent = ::send (descriptor.fd, bytes, remaining, flags);
                if (sent <= 0) return;
                bytes += sent; remaining -= (size_t) sent;
            }
           #else
            juce::ignoreUnused (client, line);
           #endif
        }
    };

    namespace
    {
        struct AuditDirectory
        {
            juce::File path = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-concurrency-audit-" + juce::Uuid().toString());
            AuditDirectory() { path.createDirectory(); }
            ~AuditDirectory() { path.deleteRecursively(); }
            juce::var report() const
            { return juce::JSON::parse (path.getChildFile ("session.report.json")); }
        };

        struct ReportGate
        {
            juce::WaitableEvent entered, release, finished;
            void pause() { entered.signal(); release.wait (5000); }
        };

        struct SlowJsonFixture final : juce::DynamicObject
        {
            bool entered = false; // serialized synchronously on the requesting thread
            void writeAsJSON (juce::OutputStream& out, const juce::JSON::FormatOptions& options) override
            {
                entered = true;
                juce::Thread::sleep (40); // bounded work crosses the request's 10 ms deadline
                juce::DynamicObject::writeAsJSON (out, options);
            }
        };

        struct OversizedJsonFixture final : juce::DynamicObject
        {
            juce::String json = "{\"fixture\":\"" + juce::String::repeatedString ("x", (1 << 20) + 1) + "\"}";
            void writeAsJSON (juce::OutputStream& out, const juce::JSON::FormatOptions&) override
            {
                // A single bulk write isolates the byte limit from formatting CPU time.
                out.write (json.toRawUTF8(), json.getNumBytesAsUTF8());
            }
        };

        bool auditWait (const std::function<bool()>& predicate, int timeoutMs = 1500)
        {
            const auto start = juce::Time::getMillisecondCounter();
            while (! predicate())
            {
                if (juce::Time::getMillisecondCounter() - start >= (juce::uint32) timeoutMs)
                    return false;
                juce::Thread::sleep (2);
            }
            return true;
        }

        void prepareAuditRecorder (MultitrackRecorder& recorder)
        {
            recorder.prepare (48000.0, 256, 1);
            recorder.getTrack (0).armed.store (true);
        }

        bool recordAuditBlock (MultitrackRecorder& recorder, const juce::File& directory)
        {
            if (! recorder.startRecording (directory)) return false;
            juce::AudioBuffer<float> block (1, 256);
            juce::FloatVectorOperations::fill (block.getWritePointer (0), 0.25f, 256);
            const float* input[] { block.getReadPointer (0) };
            recorder.processBlock (input, 1, 256);
            recorder.stopRecording();
            return true;
        }
    }

    class ConcurrencyAuditTests final : public juce::UnitTest
    {
    public:
        ConcurrencyAuditTests() : UnitTest ("October concurrency audit", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

           #if JUCE_MAC
            for (const bool overlapDeviceChange : { false, true })
            {
            beginTest (overlapDeviceChange
                ? "Device stop and reconfiguration defer safely while STOP finalization owns the writers"
                : "STOP services UI messages while captured media finalization is still pending");
            {
                AuditDirectory session;
                const auto previousTestConstruct = MainComponent::s_testConstruct;
                MainComponent::s_testConstruct = true;
                const juce::ScopeGuard resetTestConstruct { [previousTestConstruct]
                { MainComponent::s_testConstruct = previousTestConstruct; } };
                MainComponent main;
                auto& engine = ConcurrencyAuditAccess::engine (main);
                engine.setStripCount (1);
                engine.prepareForTests (48000, 256);
                engine.setTrackArmed (0, true);
                struct FinalizationGate
                {
                    juce::WaitableEvent entered, release, sentinelHandled, hashFinished;
                    std::atomic<bool> blocked { false }, enteredObserved { false };
                    std::atomic<bool> sentinelQueued { false }, sentinelRan { false };
                    std::atomic<bool> sentinelDuringFinalization { false }, capturedStopped { false };
                    std::atomic<bool> prematureCompletion { false }, completed { false }, ok { false };
                    std::atomic<bool> deviceOverlap { false }, mutationsRefused { false };
                };
                auto gate = std::make_shared<FinalizationGate>();
                ConcurrencyAuditAccess::finalizationHook (engine.getRecorder(), [gate]
                {
                    gate->blocked.store (true);
                    gate->entered.signal();
                    gate->release.wait (5000);
                    gate->blocked.store (false);
                });
                ConcurrencyAuditAccess::reportHooks (engine.getRecorder(), {}, {}, {},
                    [gate] { gate->hashFinished.signal(); });
                expect (engine.startRecording (session.path));
                float samples[256] {};
                const float* input[] { samples };
                engine.getRecorder().processBlock (input, 1, 256);
                juce::Component::SafePointer<MainComponent> safeMain (&main);
                std::thread watchdog ([gate, safeMain, overlapDeviceChange]
                {
                    if (! gate->entered.wait (5000)) { gate->release.signal(); return; }
                    gate->enteredObserved.store (true);
                    gate->sentinelQueued.store (juce::MessageManager::callAsync ([gate, safeMain, overlapDeviceChange]
                    {
                        gate->sentinelDuringFinalization.store (gate->blocked.load());
                        if (safeMain != nullptr)
                        {
                            gate->capturedStopped.store (! ConcurrencyAuditAccess::engine (*safeMain)
                                                            .getRecorder().isRecording());
                            if (overlapDeviceChange && gate->blocked.load())
                            {
                                auto& engine = ConcurrencyAuditAccess::engine (*safeMain);
                                const auto count = engine.getRecorder().getNumTracks();
                                engine.setStripCount (count + 1);
                                const bool refused = ! engine.startRecording (engine.getRecorder().getActiveSessionDir());
                                gate->mutationsRefused.store (refused && engine.getRecorder().getNumTracks() == count);
                                engine.audioDeviceStopped();
                                engine.prepareForTests (44100.0, 128);
                                gate->deviceOverlap.store (true);
                            }
                        }
                        gate->prematureCompletion.store (gate->blocked.load() && gate->completed.load());
                        gate->sentinelRan.store (true);
                        gate->release.signal();
                        gate->sentinelHandled.signal();
                    }));
                    if (! gate->sentinelHandled.wait (2000)) gate->release.signal();
                });
                expect (juce::MessageManager::callAsync ([gate, safeMain]
                {
                    if (safeMain != nullptr)
                        ConcurrencyAuditAccess::stopCapture (*safeMain, [gate] (bool ok)
                        {
                            gate->prematureCompletion.store (gate->prematureCompletion.load() || gate->blocked.load());
                            gate->ok.store (ok);
                            gate->completed.store (true);
                        });
                }));
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
                while (juce::Time::getMillisecondCounterHiRes() < deadline)
                {
                    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
                    if (gate->completed.load() && gate->sentinelRan.load()) break;
                }
                gate->release.signal();
                watchdog.join();
                expect (gate->enteredObserved.load(), "media finalization seam was never exercised");
                expect (gate->sentinelQueued.load() && gate->sentinelRan.load());
                expect (gate->sentinelDuringFinalization.load(), "STOP media finalization blocked the message thread");
                expect (gate->capturedStopped.load(), "STOP must stop capturing before slow finalization");
                expect (! gate->prematureCompletion.load(), "STOP reported complete before finalization finished");
                expect (gate->completed.load() && gate->ok.load(), "STOP did not complete successfully after release");
                expect (gate->hashFinished.wait (6000), "report job did not complete");
                expect (session.path.getChildFile ("Audio Files/Track_01.wav").existsAsFile());
                expect (! session.path.getChildFile ("recording.session").exists());
                expect (session.path.getChildFile ("Session File Backups").isDirectory());
                if (overlapDeviceChange)
                {
                    expect (gate->deviceOverlap.load(), "device change did not overlap finalization");
                    expect (gate->mutationsRefused.load(), "finalization allowed a new capture or track replacement");
                    expectEquals (engine.getDeviceSampleRate(), 44100.0);
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (
                        session.path.getChildFile ("Audio Files/Track_01.wav")));
                    expect (reader != nullptr);
                    if (reader)
                    {
                        expectEquals (reader->lengthInSamples, (juce::int64) 256);
                        expectEquals (reader->sampleRate, 48000.0);
                    }
                }
            }
            }
           #endif

           #if JUCE_MAC
            beginTest ("Async punch STOP refreshes explicit cross-track readers without changing clip edits");
            {
                AuditDirectory session;
                const auto audio = session.path.getChildFile ("Audio Files");
                expect (audio.createDirectory().wasOk());
                const auto source = audio.getChildFile ("Track_01.wav");
                const auto writeConstant = [] (const juce::File& file, float value)
                {
                    juce::WavAudioFormat format;
                    std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
                        file.createOutputStream().release(), 48000.0, 1, 24, {}, 0));
                    if (! writer) return false;
                    juce::AudioBuffer<float> samples (1, 1024);
                    juce::FloatVectorOperations::fill (samples.getWritePointer (0), value, 1024);
                    return writer->writeFromAudioSampleBuffer (samples, 0, 1024);
                };
                expect (writeConstant (source, 0.25f));
                expect (writeConstant (audio.getChildFile ("Track_02.wav"), 0.1f));
                AudioEngine engine;
                engine.setStripCount (2);
                engine.prepareForTests (48000.0, 256);
                engine.getRecorder().setBackupDirectory ({});
                engine.getRecorder().setMirrors ({});
                engine.getRecorder().setCaptureFormat (CaptureFormat::Wav24);
                expectEquals (engine.loadSession (session.path), 2);
                Clip cross;
                cross.name = "Copied source retained after punch";
                cross.audioFile = source;
                cross.fileLengthSamples = 1024;
                cross.sourceChannel = 0;
                engine.clipsFor (1) = { cross };
                engine.getPlayer().setTrackClips (1, { cross });
                engine.setTrackArmed (0, true);
                engine.setTrackArmed (1, false);
                engine.armPunchIn (128);
                expect (engine.startRecording (session.path));
                float captured[256];
                juce::FloatVectorOperations::fill (captured, 0.75f, 256);
                const float* inputs[] { captured, captured };
                engine.getRecorder().processBlock (inputs, 2, 256);
                auto hashFinished = std::make_shared<juce::WaitableEvent>();
                ConcurrencyAuditAccess::reportHooks (engine.getRecorder(), {}, {}, {},
                    [hashFinished] { hashFinished->signal(); });
                auto completed = std::make_shared<std::atomic<bool>> (false);
                auto clean = std::make_shared<std::atomic<bool>> (false);
                expect (engine.stopRecordingAsync ([completed, clean] (bool ok)
                { clean->store (ok); completed->store (true); }));
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 8000.0;
                while (! completed->load() && juce::Time::getMillisecondCounterHiRes() < deadline)
                    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
                expect (completed->load() && clean->load(), "punch finalization did not complete cleanly");
                if (completed->load())
                {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> disk (formats.createReaderFor (source));
                    expect (disk != nullptr);
                    if (disk)
                    {
                        juce::AudioBuffer<float> sample (1, 1);
                        expect (disk->read (&sample, 0, 1, 192, true, false));
                        expectWithinAbsoluteError (sample.getSample (0, 0), 0.75f, 0.001f);
                    }
                    expectEquals ((int) engine.clipsFor (1).size(), 1);
                    expect (engine.clipsFor (1)[0].audioFile == source);
                    expectEquals (engine.clipsFor (1)[0].fileLengthSamples, (juce::int64) 1024);
                    juce::AudioBuffer<float> output (2, 128);
                    engine.getPlayer().setPositionSamples (192);
                    engine.getPlayer().start();
                    engine.getPlayer().processBlock (output.getArrayOfWritePointers(), 2, 128);
                    engine.getPlayer().stop();
                    expectWithinAbsoluteError (output.getSample (0, 0), 0.75f, 0.001f,
                        "default track did not play the committed punch");
                    expectWithinAbsoluteError (output.getSample (1, 0), 0.75f, 0.001f,
                        "explicit cross-track reader still plays the pre-punch inode");
                }
                expect (hashFinished->wait (6000));
            }
           #endif

            beginTest ("A refused recording start preserves the previous pending integrity job");
            {
                AuditDirectory session, invalid;
                MultitrackRecorder recorder;
                prepareAuditRecorder (recorder);
                auto gate = std::make_shared<ReportGate>();
                ConcurrencyAuditAccess::reportHooks (recorder, [gate] { gate->pause(); }, {}, {},
                                                     [gate] { gate->finished.signal(); });
                expect (recordAuditBlock (recorder, session.path));
                expect (gate->entered.wait (2000), "hash worker never reached its gate");
                const auto notDirectory = invalid.path.getChildFile ("ordinary-file");
                expect (notDirectory.replaceWithText ("a destination cannot be created here"));
                expect (! recorder.startRecording (notDirectory));
                gate->release.signal();
                expect (gate->finished.wait (6000), "hash worker did not finish after release");
                const auto report = session.report();
                expect (report.isObject());
                expect (! (bool) report.getProperty ("sha256Pending", true),
                        "refused START abandoned the previous take's hashes");
            }

            beginTest ("Starting another session does not abandon the previous session's hashes");
            {
                AuditDirectory first, second;
                MultitrackRecorder recorder;
                prepareAuditRecorder (recorder);
                auto gate = std::make_shared<ReportGate>();
                ConcurrencyAuditAccess::reportHooks (recorder, [gate] { gate->pause(); }, {}, {},
                                                     [gate] { gate->finished.signal(); });
                expect (recordAuditBlock (recorder, first.path));
                expect (gate->entered.wait (2000));
                auto secondDone = std::make_shared<juce::WaitableEvent>();
                ConcurrencyAuditAccess::reportHooks (recorder, {}, {}, {},
                                                     [secondDone] { secondDone->signal(); });
                expect (recordAuditBlock (recorder, second.path));
                gate->release.signal();
                expect (gate->finished.wait (6000));
                expect (secondDone->wait (6000));
                expect (! (bool) first.report().getProperty ("sha256Pending", true),
                        "session B cancelled independent session A verification");
                expect (! (bool) second.report().getProperty ("sha256Pending", true));
            }

            beginTest ("An old final report cannot overwrite a newer STOP report");
            {
                AuditDirectory session;
                MultitrackRecorder recorder;
                prepareAuditRecorder (recorder);
                auto before = std::make_shared<ReportGate>();
                ConcurrencyAuditAccess::reportHooks (recorder, {}, [before] { before->pause(); },
                    {}, [before] { before->finished.signal(); });
                expect (recordAuditBlock (recorder, session.path));
                expect (before->entered.wait (2000));
                auto next = std::make_shared<ReportGate>();
                ConcurrencyAuditAccess::reportHooks (recorder, [next] { next->pause(); }, {}, {},
                                                     [next] { next->finished.signal(); });
                recorder.armContinue (256);
                expect (recordAuditBlock (recorder, session.path));
                expectEquals ((juce::int64) session.report().getProperty ("totalSamples", 0),
                              (juce::int64) 512, "new pending report must describe both passes");
                before->release.signal();
                // Hold the newer scan before it can mask an obsolete write.
                // This also works when the repaired old job refuses to publish.
                expect (next->entered.wait (2000));
                expectEquals ((juce::int64) session.report().getProperty ("totalSamples", 0),
                              (juce::int64) 512, "obsolete hash completion replaced newer metadata");
                next->release.signal();
                expect (before->finished.wait (6000));
                expect (next->finished.wait (6000));
                expectEquals ((juce::int64) session.report().getProperty ("totalSamples", 0),
                              (juce::int64) 512);
            }

            beginTest ("Deleting a capture cancels pending report publication without a false integrity error");
            {
                AuditDirectory session, other;
                const auto previousTestConstruct = MainComponent::s_testConstruct;
                MainComponent::s_testConstruct = true;
                const juce::ScopeGuard resetTestConstruct { [previousTestConstruct]
                { MainComponent::s_testConstruct = previousTestConstruct; } };
                MainComponent main;
                auto& engine = ConcurrencyAuditAccess::engine (main);
                engine.setStripCount (1);
                engine.prepareForTests (48000, 256);
                auto& recorder = engine.getRecorder();
                recorder.getTrack (0).armed.store (true);
                auto gate = std::make_shared<ReportGate>();
                ConcurrencyAuditAccess::reportHooks (recorder, {}, [gate] { gate->pause(); }, {},
                                                     [gate] { gate->finished.signal(); });
                expect (recordAuditBlock (recorder, session.path));
                expect (gate->entered.wait (2000));
                engine.setActiveSessionDir (other.path);
                expect (ConcurrencyAuditAccess::deleteSession (main, session.path));
                gate->release.signal();
                expect (gate->finished.wait (6000));
                expect (! session.path.exists(), "obsolete report job recreated a deleted capture");
                expect (! recorder.hasAsyncReportFailure(),
                        "intentional capture removal was reported as failed integrity finalization");
                expect (engine.getActiveSessionDir() == other.path);
            }

            beginTest ("A capture request deadline includes a peer that does not read its socket");
            {
                juce::StreamingSocket listener;
                expect (listener.createListener (0, "127.0.0.1"));
                capture::CaptureClient client;
                const bool connected = client.connect ("127.0.0.1", listener.getBoundPort());
                expect (connected);
                if (connected)
                {
                    std::unique_ptr<juce::StreamingSocket> peer (listener.waitForNextConnection());
                    expect (peer != nullptr);
                    if (peer != nullptr)
                    {
                        ConcurrencyAuditAccess::constrainSocketBuffers (client, *peer);
                        auto closeNow = std::make_shared<juce::WaitableEvent>();
                        // A watchdog only closes this disposable peer. Even the
                        // broken implementation cannot leave the suite hung.
                        std::thread watchdog ([closeNow, socket = peer.get()]
                        { closeNow->wait (1200); socket->close(); });
                        capture::Command command;
                        command.action = capture::Action::ConfigureCapture;
                        juce::DynamicObject::Ptr configuration (new juce::DynamicObject());
                        // Below the transport's 1 MiB frame limit; rejecting an
                        // oversized message must not accidentally satisfy this.
                        configuration->setProperty ("fixture", juce::String::repeatedString ("x", 512 * 1024));
                        command.configuration = juce::var (configuration.get());
                        const auto start = juce::Time::getMillisecondCounter();
                        const auto reply = client.request (command, 100);
                        const auto elapsed = juce::Time::getMillisecondCounter() - start;
                        closeNow->signal();
                        watchdog.join();
                        expect (! reply.ok);
                        expect (elapsed < 700, "100 ms request waited for watchdog instead of its deadline: "
                                             + juce::String (elapsed) + " ms");
                    }
                }
                client.disconnect(); listener.close();
            }

            beginTest ("Expired JSON serialization leaves the authenticated connection reusable");
            {
                capture::CaptureServer server;
                server.onCommand = [&server] (const capture::Command& command)
                {
                    if (command.action == capture::Action::Hello) return;
                    capture::Reply reply; reply.id = command.id; reply.ok = true;
                    server.sendReply (reply);
                };
                expect (server.listen (0));
                capture::CaptureClient client;
                expect (client.connect ("127.0.0.1", server.getPort()));
                expect (client.hello (1000).ok);
                auto* fixture = new SlowJsonFixture();
                fixture->setProperty ("fixture", "deadline inside serialization");
                capture::Command command; command.action = capture::Action::ConfigureCapture;
                command.configuration = juce::var (fixture);
                expect (! client.request (command, 10).ok);
                expect (fixture->entered, "fixture did not reach JSON serialization");
                expect (client.isConnected(), "unsent serialization timeout closed a healthy socket");
                capture::Command ping; ping.action = capture::Action::Ping;
                expect (client.request (ping, 1000).ok, "subsequent command could not reuse untouched connection");
                client.disconnect(); server.stop();
            }

            beginTest ("Oversized outgoing JSON is rejected before socket mutation and ordinary wire values survive");
            {
                const juce::String expected = "quote \" slash \\ newline\n Unicode Ω 🎛";
                std::atomic<int> oversizedReceived { 0 };
                capture::CaptureServer server;
                server.onCommand = [&server, &oversizedReceived, expected] (const capture::Command& command)
                {
                    if (command.action == capture::Action::Hello) return;
                    if (command.configuration.getProperty ("fixture", "").toString().getNumBytesAsUTF8() > (1u << 20))
                        oversizedReceived.fetch_add (1);
                    capture::Reply reply; reply.id = command.id;
                    reply.ok = command.action == capture::Action::Ping
                            || command.configuration.getProperty ("fixture", "").toString() == expected;
                    server.sendReply (reply);
                };
                expect (server.listen (0));
                capture::CaptureClient client;
                expect (client.connect ("127.0.0.1", server.getPort()));
                expect (client.hello (1000).ok);
                capture::Command oversized; oversized.action = capture::Action::ConfigureCapture;
                oversized.configuration = juce::var (new OversizedJsonFixture());
                expect (! client.request (oversized, 100).ok);
                expect (client.isConnected(), "oversized unsent frame closed a healthy socket");
                capture::Command ordinary; ordinary.action = capture::Action::ConfigureCapture;
                auto* object = new juce::DynamicObject(); object->setProperty ("fixture", expected);
                ordinary.configuration = juce::var (object);
                expect (client.request (ordinary, 1000).ok, "valid escaped/Unicode frame did not survive rejection");
                expectEquals (oversizedReceived.load(), 0, "oversized configuration reached the server handler");
                client.disconnect(); server.stop();
            }

           #if JUCE_MAC || JUCE_LINUX
            beginTest ("A completed incoming JSON line over one MiB is rejected before command dispatch");
            {
                std::atomic<int> commandsReceived { 0 };
                capture::CaptureServer server;
                server.onCommand = [&commandsReceived] (const capture::Command& command)
                {
                    if (command.action == capture::Action::ConfigureCapture)
                        commandsReceived.fetch_add (1);
                };
                expect (server.listen (0));
                capture::CaptureClient client;
                expect (client.connect ("127.0.0.1", server.getPort()));
                expect (client.hello (1000).ok);
                const juce::String prefix = "{\"type\":\"cmd\",\"action\":\"configureCapture\",\"configuration\":{\"fixture\":\"";
                const juce::String suffix = "\"}}";
                const int filler = (1 << 20) + 1 - prefix.length() - suffix.length();
                const auto line = prefix + juce::String::repeatedString ("x", filler) + suffix + "\n";
                ConcurrencyAuditAccess::sendRawFrame (client, line);
                expect (auditWait ([&client] { return ! client.isConnected(); }, 4000),
                        "oversized complete line left the authenticated transport open");
                client.disconnect(); server.stop();
                expectEquals (commandsReceived.load(), 0, "oversized complete line reached command handler");
            }
           #endif

            beginTest ("A connection without Hello cannot evict the authenticated capture client");
            {
                capture::CaptureServer server;
                expect (server.listen (0));
                capture::CaptureClient owner;
                expect (owner.connect ("127.0.0.1", server.getPort()));
                expect (owner.hello (1000).ok);
                juce::StreamingSocket stranger;
                expect (stranger.connect ("127.0.0.1", server.getPort(), 500));
                juce::Thread::sleep (350);
                expect (owner.isConnected(), "unauthenticated accept evicted the established controller");
                expect (! owner.wasSuperseded());
                stranger.close(); owner.disconnect(); server.stop();
            }

            beginTest ("Matching protocol version alone does not authenticate a fake capture daemon");
            {
                // A raw peer knows the public framing/version but has no
                // CaptureServer identity. Do not use the real server here:
                // the ownership test above must trust that actual endpoint.
                juce::StreamingSocket listener;
                expect (listener.createListener (0, "127.0.0.1"));
                capture::CaptureClient client;
                const bool connected = client.connect ("127.0.0.1", listener.getBoundPort());
                expect (connected);
                if (connected)
                {
                    std::unique_ptr<juce::StreamingSocket> peer (listener.waitForNextConnection());
                    expect (peer != nullptr);
                    if (peer != nullptr)
                    {
                        std::atomic<bool> replied { false };
                        std::thread fake ([&]
                        {
                            juce::String request;
                            const auto started = juce::Time::getMillisecondCounter();
                            while (! request.containsChar ('\n')
                                   && request.length() < 8192
                                   && juce::Time::getMillisecondCounter() - started < 1500)
                            {
                                if (peer->waitUntilReady (true, 50) <= 0) continue;
                                char bytes[512];
                                const auto count = peer->read (bytes, sizeof (bytes), false);
                                if (count <= 0) return;
                                request += juce::String::fromUTF8 (bytes, count);
                            }
                            const auto command = juce::JSON::parse (request.upToFirstOccurrenceOf ("\n", false, false));
                            if (! command.isObject()) return;
                            juce::DynamicObject::Ptr reply (new juce::DynamicObject());
                            reply->setProperty ("type", "reply");
                            reply->setProperty ("ok", true);
                            reply->setProperty ("completed", true);
                            reply->setProperty ("version", capture::kProtocolVersion);
                            reply->setProperty ("id", command.getProperty ("id", 0));
                            const auto frame = juce::JSON::toString (juce::var (reply.get()), true) + "\n";
                            replied.store (peer->write (frame.toRawUTF8(), (int) frame.getNumBytesAsUTF8())
                                           == (int) frame.getNumBytesAsUTF8());
                        });
                        const auto reply = client.hello (1000);
                        fake.join();
                        expect (replied.load(), "fake fixture did not send its version-only reply");
                        expect (! reply.ok,
                                "client accepted a version-only service without daemon identity");
                        peer->close();
                    }
                }
                client.disconnect(); listener.close();
            }

            beginTest ("A daemon timestamp in the future is unavailable rather than indefinitely fresh");
            {
                AudioEngine engine;
                EngineStatus status;
                status.recording = true;
                status.sampleRate = 48000;
                engine.setExternalRecording (true);
                engine.setExternalCaptureStatus (status, juce::Time::currentTimeMillis() + 60000);
                expectEquals (engine.captureStatus().source, juce::String ("daemon-unavailable"));
            }

            beginTest ("TSan: first companion start safely publishes to a running callback");
            {
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (48000, 128);
                std::atomic<bool> run { true };
                std::thread audio ([&]
                {
                    juce::AudioBuffer<float> input (1, 128), output (2, 128);
                    input.clear();
                    while (run.load (std::memory_order_relaxed))
                        engine.audioDeviceIOCallbackWithContext (input.getArrayOfReadPointers(), 1,
                            output.getArrayOfWritePointers(), 2, 128, {});
                });
                expect (engine.startCompanionServer (0));
                juce::Thread::sleep (30);
                run.store (false, std::memory_order_relaxed);
                audio.join();
                engine.stopCompanionServer();
            }

            beginTest ("TSan: capture status and structural channel changes share one lock");
            {
                AudioEngine engine;
                engine.setStripCount (1);
                std::atomic<bool> run { true };
                std::thread poller ([&]
                {
                    while (run.load (std::memory_order_relaxed))
                        (void) engine.captureStatus();
                });
                for (int i = 0; i < 100; ++i)
                    engine.getRecorder().setTrackCount ((i % 2) + 1);
                run.store (false, std::memory_order_relaxed);
                poller.join();
                expectEquals (engine.getRecorder().getNumTracks(), 2);
            }

            beginTest ("TSan: a lagged streaming reader remains safe while the audio ring wraps");
            {
                AudioEngine engine;
                engine.prepareForTests (48000, 64);
                CompanionServer server (engine);
                auto entered = std::make_shared<std::atomic<bool>> (false);
                ConcurrencyAuditAccess::streamFixture (server, [entered]
                { entered->store (true, std::memory_order_relaxed); });
                expect (server.start (0));
                juce::StreamingSocket browser;
                const bool connected = browser.connect ("127.0.0.1",
                    ConcurrencyAuditAccess::companionPort (server), 500);
                expect (connected);
                if (connected)
                {
                    const auto request = "GET /stream.wav?t=" + server.getAccessToken()
                        + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
                    browser.write (request.toRawUTF8(), (int) request.getNumBytesAsUTF8());
                    expect (auditWait ([&] { return ConcurrencyAuditAccess::streamActive (server); }));
                    juce::AudioBuffer<float> input (2, 64);
                    juce::FloatVectorOperations::fill (input.getWritePointer (0), 0.1f, 64);
                    juce::FloatVectorOperations::fill (input.getWritePointer (1), 0.2f, 64);
                    std::atomic<bool> run { true };
                    std::thread producer ([&]
                    {
                        while (run.load (std::memory_order_relaxed))
                            server.feedStreamSamples (input.getReadPointer (0), input.getReadPointer (1), 64);
                    });
                    expect (auditWait ([entered] { return entered->load (std::memory_order_relaxed); }));
                    juce::Thread::sleep (100);
                    run.store (false, std::memory_order_relaxed);
                    producer.join();
                    browser.close();
                }
                server.stop();
            }
        }
    };

    static ConcurrencyAuditTests concurrencyAuditTests;
}
