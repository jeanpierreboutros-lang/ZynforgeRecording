// Headless tests for the capture daemon <-> GUI wire protocol
// (Source/Network/CaptureProtocol.h). The contract must round-trip every
// message type through JSON, negotiate versions, and reject malformed input
// -- all verifiable without a socket or a daemon.

#include <juce_core/juce_core.h>

#include "../Network/CaptureProtocol.h"
#include "../Network/CaptureIdentity.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace zynforge
{
    class CaptureProtocolTests final : public juce::UnitTest
    {
    public:
        CaptureProtocolTests() : juce::UnitTest ("Capture protocol", "zynforge") {}

        void runTest() override
        {
            using namespace zynforge::capture;

            beginTest ("Endpoint proof matches RFC 4231 HMAC-SHA256 test case 2");
            {
                const auto proof = identity::authenticate ("Jefe", "what do ya want for nothing?");
                expectEquals (proof, juce::String ("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
                expect (identity::equalProof (proof, proof));
                expect (! identity::equalProof (proof, proof.dropLastCharacters (1) + "4"));
                expect (! identity::equalProof ({}, {}));
            }

           #if JUCE_MAC || JUCE_LINUX
            beginTest ("Endpoint identity rejects FIFO and linked key files without blocking or changing targets");
            {
                // Reserve a currently unused endpoint number and require that
                // its identity path is absent before creating any test artifact.
                juce::StreamingSocket reservation;
                const bool reserved = reservation.createListener (0, "127.0.0.1");
                expect (reserved);
                if (reserved)
                {
                    const int port = reservation.getBoundPort();
                    const auto directory = juce::File ("/tmp/zynforge-capture-" + juce::String ((int) ::geteuid()));
                    const auto keyFile = directory.getChildFile (juce::String (port) + ".key");
                    struct stat prior {};
                    const bool absent = ::lstat (keyFile.getFullPathName().toRawUTF8(), &prior) != 0 && errno == ENOENT;
                    expect (absent, "reserved test endpoint unexpectedly has an identity file");
                    if (absent)
                    {
                        identity::Endpoint seed;
                        const bool created = seed.create (port);
                        expect (created, "could not initialize the private identity directory");
                        seed.reset();
                        if (created)
                        {
                            const auto target = directory.getChildFile ("audit-target-" + juce::Uuid().toString());
                            const juce::ScopeGuard cleanup { [&]
                            {
                                ::unlink (keyFile.getFullPathName().toRawUTF8());
                                target.deleteFile();
                            } };
                            const bool fifoCreated = ::mkfifo (keyFile.getFullPathName().toRawUTF8(), 0600) == 0;
                            expect (fifoCreated);
                            if (fifoCreated)
                            {
                                std::mutex mutex;
                                std::condition_variable finished;
                                bool done = false;
                                // Release a pre-fix blocking FIFO open after a
                                // full second so a failing regression cannot hang
                                // the test runner indefinitely.
                                std::thread watchdog ([&]
                                {
                                    std::unique_lock<std::mutex> lock (mutex);
                                    if (! finished.wait_for (lock, std::chrono::seconds (1), [&] { return done; }))
                                    {
                                        const int writer = ::open (keyFile.getFullPathName().toRawUTF8(), O_WRONLY | O_NONBLOCK);
                                        if (writer >= 0) ::close (writer);
                                    }
                                });
                                const auto started = std::chrono::steady_clock::now();
                                const auto loaded = identity::Endpoint::load (port);
                                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::steady_clock::now() - started).count();
                                { const std::lock_guard<std::mutex> lock (mutex); done = true; }
                                finished.notify_all();
                                watchdog.join();
                                expect (loaded.isEmpty());
                                expect (elapsed < 700, "FIFO identity waited for the watchdog instead of failing promptly");
                                identity::Endpoint endpoint;
                                expect (! endpoint.create (port), "FIFO was accepted as an endpoint key");
                                ::unlink (keyFile.getFullPathName().toRawUTF8());
                            }

                            const auto canary = juce::String::repeatedString ("ab", 32);
                            expect (target.replaceWithText (canary));
                            expect (::chmod (target.getFullPathName().toRawUTF8(), 0600) == 0);
                            for (const bool symbolic : { true, false })
                            {
                                const int linked = symbolic
                                    ? ::symlink (target.getFullPathName().toRawUTF8(), keyFile.getFullPathName().toRawUTF8())
                                    : ::link (target.getFullPathName().toRawUTF8(), keyFile.getFullPathName().toRawUTF8());
                                expectEquals (linked, 0);
                                if (linked != 0) continue;
                                expect (identity::Endpoint::load (port).isEmpty(),
                                        symbolic ? "symlink key was followed" : "multiply-linked key was accepted");
                                identity::Endpoint endpoint;
                                expect (! endpoint.create (port), "linked identity was overwritten");
                                expectEquals (target.loadFileAsString(), canary);
                                struct stat existing {};
                                expect (::lstat (keyFile.getFullPathName().toRawUTF8(), &existing) == 0,
                                        "rejected identity path was removed");
                                ::unlink (keyFile.getFullPathName().toRawUTF8());
                            }
                        }
                    }
                }
            }
           #endif

            beginTest ("Hello without an explicit version cannot negotiate by default");
            {
                const auto json = juce::JSON::parse ("{\"type\":\"cmd\",\"action\":\"hello\"}");
                bool parsed = false;
                const auto command = Command::fromJson (json, parsed);
                expect (parsed);
                expect (! versionsCompatible (command.version, kProtocolVersion));
            }

            beginTest ("every Action round-trips through string");
            {
                const Action all[] = {
                    Action::Hello, Action::ConfigureCapture, Action::StartRecording, Action::StopRecording,
                    Action::StartPlayback, Action::StopPlayback, Action::ArmTrack,
                    Action::SetCaptureFormat, Action::SetTrackCount,
                    Action::SetSessionDir, Action::Ping, Action::Quit };
                for (auto a : all)
                {
                    bool ok = false;
                    const auto back = actionFromString (actionToString (a), ok);
                    expect (ok, "action string not recognised: " + actionToString (a));
                    expect (back == a, "action round-trip mismatch: " + actionToString (a));
                }
                bool ok = true;
                actionFromString ("bogusAction", ok);
                expect (! ok, "unknown action should report not-ok");
            }

            beginTest ("Command round-trips through JSON (incl. framing)");
            {
                Command c;
                c.action = Action::StartRecording;
                c.sessionDir = "/Users/x/Music/Zynforge Sessions/Show 1";
                Command d; d.action = Action::ArmTrack; d.trackIndex = 7; d.boolValue = true;
                Command e; e.action = Action::SetCaptureFormat; e.intValue = 6;

                for (const auto& src : { c, d, e })
                {
                    // Frame -> parse a line -> rebuild.
                    const auto line = frame (src.toJson());
                    expect (line.endsWithChar ('\n'));
                    const auto parsed = juce::JSON::parse (line.trim());
                    expectEquals (messageType (parsed), juce::String ("cmd"));
                    bool ok = false;
                    const auto back = Command::fromJson (parsed, ok);
                    expect (ok, "command did not parse");
                    expect (back.action == src.action);
                    expectEquals (back.sessionDir, src.sessionDir);
                    expectEquals (back.trackIndex, src.trackIndex);
                    expect (back.boolValue == src.boolValue);
                    expectEquals (back.intValue, src.intValue);
                }
            }

            beginTest ("Command::fromJson rejects wrong type / unknown action");
            {
                bool ok = true;
                // A status message is not a command.
                auto* o = new juce::DynamicObject();
                o->setProperty ("type", "status");
                Command::fromJson (juce::var (o), ok);
                expect (! ok, "parsed a status as a command");

                ok = true;
                auto* o2 = new juce::DynamicObject();
                o2->setProperty ("type", "cmd");
                o2->setProperty ("action", "frobnicate");
                Command::fromJson (juce::var (o2), ok);
                expect (! ok, "parsed an unknown action");
            }

            beginTest ("Hello handshake carries + negotiates the version");
            {
                Command hello; hello.action = Action::Hello; hello.version = kProtocolVersion;
                hello.authNonce = juce::String::repeatedString ("12", 32);
                hello.authProof = juce::String::repeatedString ("34", 32);
                bool ok = false;
                const auto back = Command::fromJson (hello.toJson(), ok);
                expect (ok);
                expectEquals (back.version, kProtocolVersion);
                expectEquals (back.authNonce, hello.authNonce);
                expectEquals (back.authProof, hello.authProof);

                expect (versionsCompatible (kProtocolVersion, kProtocolVersion));
                expect (! versionsCompatible (kProtocolVersion, kProtocolVersion + 1),
                        "mismatched versions must be incompatible (fail loud)");

                Reply r; r.ok = true; r.completed = true; r.version = kProtocolVersion;
                r.authProof = juce::String::repeatedString ("56", 32);
                const auto rb = Reply::fromJson (r.toJson());
                expect (rb.ok);
                expect (rb.completed);
                expectEquals (rb.version, kProtocolVersion);
                expectEquals (rb.authProof, r.authProof);
                expectEquals (messageType (r.toJson()), juce::String ("reply"));

                Reply bad; bad.ok = false; bad.error = "version mismatch";
                const auto bb = Reply::fromJson (bad.toJson());
                expect (! bb.ok);
                expectEquals (bb.error, juce::String ("version mismatch"));
                expect (! bb.completed);
            }

            beginTest ("status messages wrap EngineStatus and round-trip");
            {
                EngineStatus s;
                s.recording = true; s.numTracks = 3; s.armedTracks = 2;
                s.audioLoadPct = 22.0f; s.missedSamples = 0;
                TrackStatus t; t.name = "Snare"; t.peak = 0.7f; t.armed = true;
                s.tracks = { t };

                const auto v = encodeStatus (s);
                expectEquals (messageType (v), juce::String ("status"));
                const auto back = decodeStatus (v);
                expect (back.recording);
                expectEquals (back.numTracks, 3);
                expectEquals (back.armedTracks, 2);
                expectEquals ((int) back.tracks.size(), 1);
                if (! back.tracks.empty())
                    expectEquals (back.tracks[0].name, juce::String ("Snare"));
            }
        }
    };

    static CaptureProtocolTests captureProtocolTests;
}
