// Unit-level tests for the automation lane surface. Run by setting
// the ZYNFORGE_RUN_TESTS env var before launch -- the app instantiates
// an AudioEngine in test-mode (no audio device init) and exercises the
// per-track lane API. Quits with exit code 0 on pass, 1 on failure.
//
// Adds a real safety net for refactors -- one failed unit test beats
// "the app launched at 49 MB for 9 seconds without crashing."

#include <juce_core/juce_core.h>
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class AutomationLaneTests final : public juce::UnitTest
    {
    public:
        AutomationLaneTests() : UnitTest ("Automation lanes", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("addAutomationPoint inserts + sorts");
            {
                AudioEngine eng;
                eng.setStripCount (4);
                using P = AudioEngine::AutomationParam;
                eng.addAutomationPoint (0, P::Volume, 4096 * 10, -3.0f);
                eng.addAutomationPoint (0, P::Volume, 4096 * 2,  -6.0f);
                eng.addAutomationPoint (0, P::Volume, 4096 * 5,  -9.0f);
                const auto& lane = eng.getAutomation (0, P::Volume);
                expectEquals ((int) lane.size(), 3);
                expect (lane[0].samplePos < lane[1].samplePos);
                expect (lane[1].samplePos < lane[2].samplePos);
            }

            beginTest ("addAutomationPoint kSnap-replaces nearby points");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                using P = AudioEngine::AutomationParam;
                eng.addAutomationPoint (0, P::Volume, 100000, -3.0f);
                eng.addAutomationPoint (0, P::Volume, 100050, -9.0f);
                const auto& lane = eng.getAutomation (0, P::Volume);
                expectEquals ((int) lane.size(), 1);
                expectEquals (lane[0].value, -9.0f);
            }

            beginTest ("Mute lane snaps to 0/1 and forces Hold curve");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                using P = AudioEngine::AutomationParam;
                eng.addAutomationPoint (0, P::Mute, 1000,  0.3f);
                eng.addAutomationPoint (0, P::Mute, 50000, 0.8f);
                const auto& lane = eng.getAutomation (0, P::Mute);
                expectEquals ((int) lane.size(), 2);
                expectEquals (lane[0].value, 0.0f);
                expectEquals (lane[1].value, 1.0f);
                expect (lane[0].curve == AudioEngine::AutomationCurve::Hold);
                expect (lane[1].curve == AudioEngine::AutomationCurve::Hold);
            }

            beginTest ("automationValueAt interpolates and honours tension");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                using P = AudioEngine::AutomationParam;
                eng.addAutomationPoint (0, P::Volume, 0,       0.0f);
                eng.addAutomationPoint (0, P::Volume, 1000000, 12.0f);
                const auto midLinear = eng.automationValueAt (0, P::Volume, 500000, 0.0f);
                expectWithinAbsoluteError (midLinear, 6.0f, 0.1f);

                eng.setAutomationTensionAt (0, P::Volume, 0, 4096, +1.0f);
                const auto midEaseOut = eng.automationValueAt (0, P::Volume, 500000, 0.0f);
                expect (midEaseOut > 6.0f);
            }

            beginTest ("before the first point holds the fader level, not the point value");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                using P = AudioEngine::AutomationParam;
                // Single point at 1,000,000 with -12 dB. The fader (fallback)
                // is 0 dB. Anything BEFORE the point must read the fader level
                // (0), not -12 -- so the move starts at the point, not before.
                eng.addAutomationPoint (0, P::Volume, 1000000, -12.0f);
                expectWithinAbsoluteError (eng.automationValueAt (0, P::Volume, 0,       0.0f), 0.0f,   0.01f);
                expectWithinAbsoluteError (eng.automationValueAt (0, P::Volume, 500000,  0.0f), 0.0f,   0.01f);
                expectWithinAbsoluteError (eng.automationValueAt (0, P::Volume, 1000000, 0.0f), -12.0f, 0.01f);
                // After the last point the value sticks (holds -12).
                expectWithinAbsoluteError (eng.automationValueAt (0, P::Volume, 2000000, 0.0f), -12.0f, 0.01f);
            }

            beginTest ("Safe lock blocks every write path");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                using P = AudioEngine::AutomationParam;
                eng.addAutomationPoint (0, P::Volume, 0, 0.0f);
                eng.setTrackAutomationSafe (0, true);

                eng.addAutomationPoint (0, P::Volume, 50000, -6.0f);
                eng.removeAutomationPointNear (0, P::Volume, 0, 4096);
                std::vector<AudioEngine::AutomationPoint> paste { { 1, 1.0f } };
                eng.pasteAutomationRange (0, P::Volume, 100000, paste);
                eng.clearAutomationRange (0, P::Volume, 0, 1000000);

                const auto& lane = eng.getAutomation (0, P::Volume);
                expectEquals ((int) lane.size(), 1);
                expectEquals (lane[0].value, 0.0f);
            }

            beginTest ("Thinned write skips drops within minSamplesBetween");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                using P = AudioEngine::AutomationParam;
                eng.writeAutomationPointThinned (0, P::Volume, 100000, -3.0f, 50000);
                eng.writeAutomationPointThinned (0, P::Volume, 110000, -6.0f, 50000);
                eng.writeAutomationPointThinned (0, P::Volume, 200000, -9.0f, 50000);
                const auto& lane = eng.getAutomation (0, P::Volume);
                expectEquals ((int) lane.size(), 2);
            }

            beginTest ("Punch gate blocks writes outside [in, out)");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                using P = AudioEngine::AutomationParam;
                eng.setAutomationPunchEnabled (true);
                eng.setAutomationPunchRange (100000, 200000);
                eng.writeAutomationPointThinned (0, P::Volume,  50000, -3.0f, 1);
                eng.writeAutomationPointThinned (0, P::Volume, 150000, -6.0f, 1);
                eng.writeAutomationPointThinned (0, P::Volume, 250000, -9.0f, 1);
                const auto& lane = eng.getAutomation (0, P::Volume);
                expectEquals ((int) lane.size(), 1);
                expectEquals (lane[0].value, -6.0f);
            }

            beginTest ("JSON round-trip preserves curve, tension, safe");
            {
                AudioEngine eng;
                eng.setStripCount (2);
                using P = AudioEngine::AutomationParam;
                using C = AudioEngine::AutomationCurve;
                eng.addAutomationPoint (0, P::Volume, 1000,  -3.0f);
                eng.addAutomationPoint (0, P::Volume, 50000, +6.0f);
                eng.setAutomationTensionAt (0, P::Volume, 1000, 4096, +0.7f);
                eng.setTrackAutomationSafe (1, true);

                const auto v = eng.automationToJson();

                AudioEngine eng2;
                eng2.setStripCount (2);
                eng2.loadAutomationFromJson (v);
                const auto& lane = eng2.getAutomation (0, P::Volume);
                expectEquals ((int) lane.size(), 2);
                expect (lane[0].curve == C::Linear);
                expectWithinAbsoluteError (lane[0].tension, 0.7f, 0.01f);
                expect (eng2.isTrackAutomationSafe (1));
            }

            beginTest ("Legacy ExpUp/ExpDown migrates to Linear+tension");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                juce::Array<juce::var> tracks;
                juce::DynamicObject::Ptr trk (new juce::DynamicObject());
                trk->setProperty ("track", 0);
                juce::Array<juce::var> volPts;
                juce::DynamicObject::Ptr pt (new juce::DynamicObject());
                pt->setProperty ("s", (juce::int64) 1000);
                pt->setProperty ("v", -3.0);
                pt->setProperty ("c", 3);  // ExpUp
                volPts.add (juce::var (pt.get()));
                trk->setProperty ("volume", juce::var (volPts));
                tracks.add (juce::var (trk.get()));
                eng.loadAutomationFromJson (juce::var (tracks));

                using C = AudioEngine::AutomationCurve;
                const auto& lane = eng.getAutomation (0, AudioEngine::AutomationParam::Volume);
                expectEquals ((int) lane.size(), 1);
                expect (lane[0].curve == C::Linear);
                expectWithinAbsoluteError (lane[0].tension, -0.5f, 0.01f);
            }
        }
    };

    static AutomationLaneTests automationLaneTestsInstance;
}
