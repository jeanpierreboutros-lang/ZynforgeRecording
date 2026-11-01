#include "../Audio/AudioEngine.h"
#include "../Audio/OscRemote.h"
#include "../Network/ConsoleLink.h"

namespace zynforge
{
class ReviewNetworkRepairTests final : public juce::UnitTest
{
public:
    ReviewNetworkRepairTests() : UnitTest ("Review network repairs", "zynforge") {}

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        beginTest ("Gain capture completeness requires every requested headamp");
        {
            ConsoleLink link;
            std::vector<ConsoleMessage> sent;
            link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
            link.captureGains (2);
            link.injectReply (ConsoleMessage ("/headamp/000/gain", { 0.25 }));
            link.injectReply (ConsoleMessage ("/headamp/127/gain", { 0.75 }));
            expect (! link.isGainCaptureComplete(), "unrequested headamp completed capture");
            expectEquals ((int) link.getCapturedGains().count (127), 0);
            link.injectReply (ConsoleMessage ("/headamp/001/gain", { 0.5 }));
            expect (link.isGainCaptureComplete());
            sent.clear();
            link.restoreGains();
            expectEquals ((int) sent.size(), 2);
            if (sent.size() == 2)
            {
                expectEquals (sent[0].address, juce::String ("/headamp/000/gain"));
                expectEquals (sent[1].address, juce::String ("/headamp/001/gain"));
            }
        }

        beginTest ("Partial gain capture cannot be restored to a console");
        {
            ConsoleLink link;
            int writes = 0;
            juce::String status;
            link.setMessageHook ([&] (const ConsoleMessage& m) { if (m.hasArgs()) ++writes; });
            link.onStatus = [&] (const juce::String& s) { status = s; };
            link.captureGains (2);
            link.injectReply (ConsoleMessage ("/headamp/000/gain", { 0.25 }));
            expect (! link.isGainCaptureComplete());
            writes = 0; status.clear();
            link.restoreGains();
            expectEquals (writes, 0, "partial capture wrote to the console");
            expect (status.containsIgnoreCase ("partial") || status.containsIgnoreCase ("incomplete"), status);
        }

        beginTest ("Realtime MIDI inside program changes never becomes scene data");
        {
            juce::MemoryBlock buffer;
            juce::uint8 status = 0;
            std::vector<ConsoleMessage> messages;
            const juce::uint8 bytes[] { 0xc0, 0xf8, 5 };
            for (auto byte : bytes)
            {
                buffer.append (&byte, 1);
                MidiTcpTransport::frame (buffer, messages, status);
            }
            expectEquals ((int) messages.size(), 2);
            if (messages.size() == 2)
            {
                expectEquals (messages[0].intArg(), 0xf8);
                expectEquals (messages[1].intArg(), 0xc0);
                expectEquals (messages[1].intArg (1), 5);
            }
            int scenes = 0;
            for (const auto& m : messages)
            {
                const auto event = allenHeathMidiDialect().parse (m);
                if (event.type == ConsoleEvent::Type::SceneRecalled)
                { ++scenes; expectEquals (event.index, 6); }
            }
            expectEquals (scenes, 1);
            expectEquals ((int) buffer.getSize(), 0);
        }

        beginTest ("Realtime MIDI survives running-status and SysEx split reads");
        {
            juce::MemoryBlock buffer;
            juce::uint8 status = 0;
            std::vector<ConsoleMessage> messages;
            const juce::uint8 bytes[] { 0xb0, 1, 2, 3, 0xf8, 4, 0xf0, 0, 0xfa, 1, 0xf7 };
            for (auto byte : bytes)
            {
                buffer.append (&byte, 1);
                MidiTcpTransport::frame (buffer, messages, status);
            }
            expectEquals ((int) messages.size(), 5);
            if (messages.size() == 5)
            {
                expectEquals (messages[1].intArg(), 0xf8);
                expectEquals (messages[2].intArg(), 0xb0);
                expectEquals (messages[2].intArg (1), 3);
                expectEquals (messages[2].intArg (2), 4);
                expectEquals (messages[3].intArg(), 0xfa);
                expectEquals (messages[4].intArg(), 0xf0);
                expectEquals ((int) messages[4].args.size(), 4);
                expectEquals (messages[4].intArg (3), 0xf7);
            }
            expectEquals ((int) buffer.getSize(), 0);
        }

        beginTest ("Authenticated OSC excludes credentials from payload arity");
        {
            AudioEngine engine;
            engine.clearAllStripOverrides();
            const juce::ScopeGuard cleanup { [&] { engine.clearAllStripOverrides(); } };
            engine.setStripCount (1);
            engine.setTrackName (0, "original");
            int transportCalls = 0;
            engine.setRemoteTransportHandler ([&] (auto, juce::String&) -> std::optional<bool>
                { ++transportCalls; return true; });
            OscRemote osc (engine);
            const juce::String token = "audit-payload-token";
            osc.accessToken = token;
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/channel/1/name", token));
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/record", token));
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/play", token));
            expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("original"));
            expectEquals (transportCalls, 0);
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/channel/1/name", juce::String ("valid"), token));
            expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("valid"));
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-review-osc-" + juce::Uuid().toString());
            expect (dir.createDirectory().wasOk());
            const juce::ScopeGuard remove { [&] { dir.deleteRecursively(); } };
            engine.getMarkers().setContext (dir, 48000);
            engine.setEditCursorSample (0);
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/marker", token));
            expectEquals (engine.getMarkers().getCount(), 1);
            expect (! engine.getMarkers().getLast().name.contains (token), "credential became marker text");
        }

    }
};
static ReviewNetworkRepairTests reviewNetworkRepairTests;
}
