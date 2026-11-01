#include "../Audio/AudioEngine.h"
#include "../Audio/OscRemote.h"

#include <cstring>

namespace zynforge
{
class AuditSecurityTests final : public juce::UnitTest
{
public:
    AuditSecurityTests() : UnitTest ("Audit capability security", "zynforge") {}

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);

        beginTest ("OSC traffic log redacts the authentication argument");
        {
            AudioEngine engine;
            engine.clearAllStripOverrides();
            const juce::ScopeGuard cleanup { [&] { engine.clearAllStripOverrides(); } };
            engine.setStripCount (1);
            engine.getRemotes().setOscDebug (true);
            OscRemote osc (engine);
            const juce::String canary = "audit-only-capability-canary";
            osc.accessToken = canary;
            // Invoke the actual authenticated path, not dispatchForTest's
            // intentional authentication bypass.
            osc.oscMessageReceived (juce::OSCMessage (
                "/zynforge/channel/1/mute", (juce::int32) 1, canary));
            expect (engine.getRecorder().getTrack (0).muted.load(),
                    "fixture did not exercise an authenticated command");
            const auto log = engine.getRemotes().getOscLog().joinIntoString ("\n");
            expect (log.contains ("/zynforge/channel/1/mute"), "diagnostics lost the command address");
            expect (! log.contains (canary), "diagnostic log exposed the authorization token");
        }

        beginTest ("OSC channel commands obey LOCK and the token, and a bad channel changes nothing");
        {
            AudioEngine engine;
            engine.clearAllStripOverrides();
            const juce::ScopeGuard cleanup { [&] { engine.clearAllStripOverrides(); } };
            engine.setStripCount (2);
            OscRemote osc (engine);
            const juce::String token = "audit-only-capability";
            osc.accessToken = token;
            const auto muted = [&] (int track) { return engine.getRecorder().getTrack (track).muted.load(); };

            engine.setControlsLocked (true);
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/channel/1/mute", (juce::int32) 1, token));
            expect (! muted (0), "LOCK let a remote mute through");
            engine.setControlsLocked (false);

            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/channel/1/mute", (juce::int32) 1));
            expect (! muted (0), "a command with no token changed the mix");
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/channel/999/mute", (juce::int32) 1, token));
            expect (! muted (0) && ! muted (1), "a bad channel changed the mix");
            osc.oscMessageReceived (juce::OSCMessage ("/zynforge/channel/1/mute", (juce::int32) 1, token));
            expect (muted (0), "the unlocked command did not arrive");
        }

        beginTest ("OSC capability consumes exactly 32 bytes from injected OS entropy");
        {
            AudioEngine engine;
            OscRemote osc (engine);
            int calls = 0;
            size_t requested = 0;
            osc.entropyProviderForTests = [&] (void* output, size_t size)
            {
                ++calls; requested = size;
                std::memset (output, 0xa5, size);
                return true;
            };
            expect (osc.start (0));
            expectEquals (calls, 1, "capability generation bypassed the entropy provider");
            expectEquals ((int) requested, 32);
            expect (osc.getAccessToken() == juce::String::repeatedString ("a5", 32));
            osc.stop();
        }

        beginTest ("OSC entropy failure leaves no listener or fallback token");
        {
            AudioEngine engine;
            OscRemote osc (engine);
            int calls = 0;
            osc.entropyProviderForTests = [&] (void*, size_t) { ++calls; return false; };
            expect (! osc.start (0), "service opened despite entropy failure");
            expectEquals (calls, 1);
            expect (! osc.isListening());
            expect (osc.getAccessToken().isEmpty());
            osc.stop();
        }
    }
};
static AuditSecurityTests auditSecurityTests;
}
