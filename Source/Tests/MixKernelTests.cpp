// The mix rules in Source/Audio/MixKernel.h, on their own. The audio callback
// and the offline bounce both call these; AudioCallbackTests' "Live playback
// and the offline bounce mix the same session identically" proves the two
// paths agree end to end.

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "../Audio/MixKernel.h"

#include <map>
#include <utility>

namespace zynforge
{
    class MixKernelTests final : public juce::UnitTest
    {
    public:
        MixKernelTests() : UnitTest ("Mix kernel", "zynforge") {}

        void runTest() override
        {
            using mix::VcaGate;

            beginTest ("Solo / mute / VCA gate follows console rules");
            {
                const VcaGate none;
                const VcaGate vca      { true, false, false };
                const VcaGate vcaMuted { true, true,  false };
                const VcaGate vcaSolo  { true, false, true  };

                expect (  mix::audible (false, false, none, false, false));
                expect (! mix::audible (true,  false, none, false, false), "strip mute");
                expect (! mix::audible (false, false, none, true,  false), "someone else soloed");
                expect (  mix::audible (true,  true,  none, true,  false), "solo beats the strip's own mute");
                expect (! mix::audible (false, true,  vcaMuted, true, false), "a muted VCA beats a strip solo");
                expect (! mix::audible (false, false, vca,  false, true), "another VCA is soloed");
                expect (  mix::audible (false, false, vcaSolo, false, true));
                expect (! mix::audible (false, false, none, false, true), "ungrouped strips drop under a VCA solo");
                expect (! mix::audible (true,  false, vcaSolo, false, true), "VCA solo keeps the strip's mute");
            }

            beginTest ("A stereo pair's right half reads its left partner's lanes");
            {
                expectEquals (mix::automationChannel (0, false), 0);
                expectEquals (mix::automationChannel (3, false), 3);
                expectEquals (mix::automationChannel (3, true),  2);
                expectEquals (mix::automationChannel (0, true),  0);   // no track -1
            }

            beginTest ("resolveLanes: static values, lanes, and the monitor mute rule");
            {
                // Fake lanes: channel -> (lane -> value). Missing = fallback.
                std::map<std::pair<int, int>, float> lanes;
                auto read = [&lanes] (int ch, mix::Lane lane, float fallback) noexcept
                {
                    const auto it = lanes.find ({ ch, (int) lane });
                    return it == lanes.end() ? fallback : it->second;
                };

                // Stopped, or the lock lost: static values; the monitor mute
                // applies only while rolling.
                auto m = mix::resolveLanes (1, 0, -6.0f, 0.25f, true, false, false, read);
                expectEquals (m.volDb, -6.0f);
                expectEquals (m.pan, 0.25f);
                expect (! m.muteLane && ! m.monitorMuted);
                m = mix::resolveLanes (1, 0, -6.0f, 0.25f, true, true, false, read);
                expect (! m.muteLane && m.monitorMuted);

                // Rolling with lanes: Volume + Mute come from autoCh, Pan
                // from the strip itself; a Mute lane overrides the static mute
                // for the monitor.
                lanes[{ 0, (int) mix::Lane::Volume }] = -20.0f;
                lanes[{ 1, (int) mix::Lane::Volume }] = +6.0f;    // ignored: pair reads ch 0
                lanes[{ 1, (int) mix::Lane::Pan }]    = -1.0f;
                lanes[{ 0, (int) mix::Lane::Mute }]   = 0.0f;
                m = mix::resolveLanes (1, 0, -6.0f, 0.25f, true, true, true, read);
                expectEquals (m.volDb, -20.0f);
                expectEquals (m.pan, -1.0f);
                expect (! m.muteLane && ! m.monitorMuted, "a Mute lane at 0 unmutes the monitor");

                lanes[{ 0, (int) mix::Lane::Mute }] = 1.0f;
                m = mix::resolveLanes (1, 0, -6.0f, 0.25f, false, true, true, read);
                expect (m.muteLane && m.monitorMuted);
            }

            beginTest ("Post-fader sends follow every mute; pre-fader sends do not");
            {
                expect (! mix::postFaderMuted (false, false, false));
                expect (mix::postFaderMuted (true,  false, false));
                expect (mix::postFaderMuted (false, true,  false));
                expect (mix::postFaderMuted (false, false, true));

                expectWithinAbsoluteError (mix::sendGain (0.0f, true, 0.5f), 0.5f, 1.0e-6f);
                expectWithinAbsoluteError (mix::sendGain (0.0f, true, 0.0f), 0.0f, 1.0e-6f);
                expectWithinAbsoluteError (mix::sendGain (0.0f, false, 0.0f), 1.0f, 1.0e-6f);
                expectWithinAbsoluteError (mix::sendGain (-6.0f, false, 0.0f),
                                           juce::Decibels::decibelsToGain (-6.0f), 1.0e-6f);
                expectEquals (mix::sendGain (-60.0f, false, 1.0f), 0.0f);
            }

            beginTest ("Constant-power pan law and master gain");
            {
                const auto c = mix::constantPowerPan (1.0, 0.0f);
                expectWithinAbsoluteError (c.left,  std::sqrt (0.5), 1.0e-9);
                expectWithinAbsoluteError (c.right, std::sqrt (0.5), 1.0e-9);
                const auto l = mix::constantPowerPan (2.0, -1.0f);
                expectWithinAbsoluteError (l.left, 2.0, 1.0e-9);
                expectWithinAbsoluteError (l.right, 0.0, 1.0e-9);
                const auto r = mix::constantPowerPan (1.0, 7.0f);   // clamped to hard right
                expectWithinAbsoluteError (r.left, 0.0, 1.0e-9);
                expectWithinAbsoluteError (r.right, 1.0, 1.0e-9);

                expectEquals (mix::masterGain (true, 0.0f), 0.0);
                expectWithinAbsoluteError (mix::masterGain (false, -6.0f),
                                           juce::Decibels::decibelsToGain (-6.0), 1.0e-12);
            }
        }
    };

    static MixKernelTests mixKernelTests;
}
