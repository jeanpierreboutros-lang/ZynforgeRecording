// Headless tests for the capture daemon <-> GUI wire protocol
// (Source/Network/CaptureProtocol.h). The contract must round-trip every
// message type through JSON, negotiate versions, and reject malformed input
// -- all verifiable without a socket or a daemon.

#include <juce_core/juce_core.h>

#include "../Network/CaptureProtocol.h"

namespace zynforge
{
    class CaptureProtocolTests final : public juce::UnitTest
    {
    public:
        CaptureProtocolTests() : juce::UnitTest ("Capture protocol", "zynforge") {}

        void runTest() override
        {
            using namespace zynforge::capture;

            beginTest ("every Action round-trips through string");
            {
                const Action all[] = {
                    Action::Hello, Action::StartRecording, Action::StopRecording,
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
                bool ok = false;
                const auto back = Command::fromJson (hello.toJson(), ok);
                expect (ok);
                expectEquals (back.version, kProtocolVersion);

                expect (versionsCompatible (kProtocolVersion, kProtocolVersion));
                expect (! versionsCompatible (kProtocolVersion, kProtocolVersion + 1),
                        "mismatched versions must be incompatible (fail loud)");

                Reply r; r.ok = true; r.version = kProtocolVersion;
                const auto rb = Reply::fromJson (r.toJson());
                expect (rb.ok);
                expectEquals (rb.version, kProtocolVersion);
                expectEquals (messageType (r.toJson()), juce::String ("reply"));

                Reply bad; bad.ok = false; bad.error = "version mismatch";
                const auto bb = Reply::fromJson (bad.toJson());
                expect (! bb.ok);
                expectEquals (bb.error, juce::String ("version mismatch"));
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
