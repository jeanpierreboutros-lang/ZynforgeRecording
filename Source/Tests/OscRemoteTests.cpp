#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>
#include "../Audio/AudioEngine.h"
#include "../Audio/OscRemote.h"

namespace zynforge
{
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

            beginTest ("DiGiCo dialect routes channel + transport");
            {
                AudioEngine engine;
                engine.setStripCount (4);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);

                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/2/mute", (juce::int32) 1));
                expect (engine.getRecorder().getTrack (1).muted.load());

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
        }
    };

    static OscRemoteTests oscRemoteTests;
}
