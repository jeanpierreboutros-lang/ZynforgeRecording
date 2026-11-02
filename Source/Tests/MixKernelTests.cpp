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
            beginTest ("Solo / mute gate follows console rules");
            {
                expect (mix::audible (false, false, false));
                expect (! mix::audible (true, false, false), "the strip's mute");
                expect (! mix::audible (false, false, true), "another strip is soloed");
                expect (mix::audible (false, true, true));
            }

            beginTest ("Solo gets a muted strip heard; every other strip stays out");
            {
                expect (mix::audible (true, true, true), "solo beats the strip's own mute");
                expect (! mix::audible (true, false, false));
                expect (! mix::audible (false, false, true));
            }

            beginTest ("The stream bus takes mutes and ignores solo");
            {
                expect (mix::streamAudible (false));
                expect (! mix::streamAudible (true));
            }

            beginTest ("Trim-Follow offset: a real move, limited; no reference, none");
            {
                expectEquals (mix::trimFollowDb (false, 36.0f, 0.0f), 0.0f);
                expectEquals (mix::trimFollowDb (true, 31.0f, 30.0f), 1.0f);
                expectEquals (mix::trimFollowDb (true, 72.0f, 0.0f), mix::kTrimFollowMaxUpDb);
                expectEquals (mix::trimFollowDb (true, -96.0f, 0.0f), mix::kTrimFollowMaxDownDb);
            }

            beginTest ("The master pair stays a pair on a smaller device, and off stays off");
            {
                const auto same = [this] (mix::OutputPair p, int l, int r)
                {
                    expectEquals (p.left, l);
                    expectEquals (p.right, r);
                };
                same (mix::masterOutputs (4, 5, 8), 4, 5);
                same (mix::masterOutputs (8, 9, 2), 0, 1);
                same (mix::masterOutputs (8, 9, 6), 4, 5);
                same (mix::masterOutputs (8, 9, 1), 0, 0);
                same (mix::masterOutputs (0, 1, 0), -1, -1);
                same (mix::masterOutputs (-1, -1, 8), -1, -1);
            }

            beginTest ("A mix bus stays off a patched output unless it has nowhere else");
            {
                expect (mix::busHeldOffOutputs (true, 8, false));
                expect (! mix::busHeldOffOutputs (false, 8, false));
                expect (! mix::busHeldOffOutputs (true, 2, false), "a two-output device is the monitor");
                expect (! mix::busHeldOffOutputs (true, 8, true), "the engineer said this rig may share");
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

            beginTest ("A fader at the -60 dB floor is off");
            {
                expectEquals (juce::Decibels::decibelsToGain (mix::stripDb (-60.0f), -60.0f), 0.0f);
                // Even a trim added afterwards (trim-follow) stays off.
                expect (mix::stripDb (-60.0f) + 24.0f < mix::kFaderFloorDb);
                expectWithinAbsoluteError (mix::stripDb (-6.0f), -6.0f, 1.0e-6f);
            }
        }
    };

    static MixKernelTests mixKernelTests;
}
