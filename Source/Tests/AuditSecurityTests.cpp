#include "../Audio/AudioEngine.h"
#include "../Audio/OscRemote.h"
#include "../Network/CompanionServer.h"

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
            engine.setOscDebug (true);
            OscRemote osc (engine);
            const juce::String canary = "audit-only-capability-canary";
            osc.accessToken = canary;
            // Invoke the actual authenticated path, not dispatchForTest's
            // intentional authentication bypass.
            osc.oscMessageReceived (juce::OSCMessage (
                "/zynforge/channel/1/mute", (juce::int32) 1, canary));
            expect (engine.getRecorder().getTrack (0).muted.load(),
                    "fixture did not exercise an authenticated command");
            const auto log = engine.getOscLog().joinIntoString ("\n");
            expect (log.contains ("/zynforge/channel/1/mute"), "diagnostics lost the command address");
            expect (! log.contains (canary), "diagnostic log exposed the authorization token");
        }

        beginTest ("Companion capability consumes exactly 32 bytes from injected OS entropy");
        {
            AudioEngine engine;
            CompanionServer server (engine);
            int calls = 0;
            size_t requested = 0;
            server.entropyProviderForTests = [&] (void* output, size_t size)
            {
                ++calls; requested = size;
                std::memset (output, 0x5a, size);
                return true;
            };
            expect (server.start (0));
            expectEquals (calls, 1, "capability generation bypassed the entropy provider");
            expectEquals ((int) requested, 32);
            expect (server.getAccessToken() == juce::String::repeatedString ("5a", 32),
                    "capability does not encode the provided entropy");
            server.stop();
        }

        beginTest ("Companion entropy failure leaves no listener or fallback token");
        {
            AudioEngine engine;
            CompanionServer server (engine);
            int calls = 0;
            server.entropyProviderForTests = [&] (void*, size_t) { ++calls; return false; };
            expect (! server.start (0), "service opened despite entropy failure");
            expectEquals (calls, 1);
            expect (! server.isRunning());
            expect (server.getAccessToken().isEmpty());
            server.stop();
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
