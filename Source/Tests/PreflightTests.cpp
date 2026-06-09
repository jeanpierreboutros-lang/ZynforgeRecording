// Headless tests for the measured Pre-Flight probes
// (Source/Audio/PreflightProbes.h). These run against real temp
// directories -- the point of the probes is that they measure the
// actual filesystem, so the tests do too.

#include <juce_core/juce_core.h>

#include "../Audio/PreflightProbes.h"

namespace zynforge
{
    class PreflightTests final : public juce::UnitTest
    {
    public:
        PreflightTests() : juce::UnitTest ("Preflight probes", "zynforge") {}

        void runTest() override
        {
            using namespace zynforge::preflight;

            beginTest ("requiredWriteMBps is sample rate x tracks x bytes");
            {
                // 48 kHz x 32 ch x 3 B = 4.608 MB/s
                expectWithinAbsoluteError (requiredWriteMBps (48000.0, 32, 3), 4.608, 1.0e-9);
                // 96 kHz x 64 ch x 4 B = 24.576 MB/s
                expectWithinAbsoluteError (requiredWriteMBps (96000.0, 64, 4), 24.576, 1.0e-9);
                // Degenerate inputs -> 0, never negative / NaN.
                expectEquals (requiredWriteMBps (0.0, 32, 3), 0.0);
                expectEquals (requiredWriteMBps (48000.0, 0, 3), 0.0);
                expectEquals (requiredWriteMBps (48000.0, 32, 0), 0.0);
            }

            beginTest ("minutesOfHeadroom converts free bytes at the demand rate");
            {
                // 1 GB free at 1 MB/s = 1000 s = 16.67 min.
                expectWithinAbsoluteError (minutesOfHeadroom (1000000000, 1.0), 16.6667, 0.001);
                expectEquals (minutesOfHeadroom (0, 1.0), 0.0);
                expectEquals (minutesOfHeadroom (1000, 0.0), 0.0);
            }

            beginTest ("volumeWritable: true on a temp dir, false on missing / file");
            {
                auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("zf-preflight-" + juce::Uuid().toString());
                expect (! volumeWritable (dir));            // doesn't exist yet
                expect (dir.createDirectory());
                expect (volumeWritable (dir));
                // No probe litter left behind.
                expect (dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
                dir.deleteRecursively();
            }

            beginTest ("measureWriteSpeedMBps measures, cleans up, fails closed");
            {
                auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("zf-preflight-spd-" + juce::Uuid().toString());
                expect (dir.createDirectory());
                const double mbps = measureWriteSpeedMBps (dir, 4);   // small probe: keep the suite fast
                expect (mbps > 0.0, "temp volume should measure a positive write speed");
                expect (dir.getNumberOfChildFiles (juce::File::findFiles) == 0,
                        "speed probe must delete its temp file");
                dir.deleteRecursively();

                expect (measureWriteSpeedMBps (dir, 4) <= 0.0, "missing dir fails closed");
                expect (measureWriteSpeedMBps (juce::File::getSpecialLocation (juce::File::tempDirectory), 0) <= 0.0,
                        "zero-size probe fails closed");
            }
        }
    };

    static PreflightTests preflightTests;
}
