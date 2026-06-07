#include <juce_core/juce_core.h>
#include "../Audio/TimecodeChase.h"

namespace zynforge
{
    // Decoder-level tests for external timecode chase. The MTC path is the
    // newly-wired one (MIDI -> feedMtcQuarterFrame / feedMtcFullFrame); the LTC
    // biphase decoder is exercised at the presence-detection level here (a full
    // frame-decode is validated in the field against a real LTC generator).
    class TimecodeTests final : public juce::UnitTest
    {
    public:
        TimecodeTests() : UnitTest ("Timecode chase", "zynforge") {}

        void runTest() override
        {
            beginTest ("MTC quarter-frame run assembles HH:MM:SS:FF + frame rate");
            {
                TimecodeChase tc;
                // 01:02:03:04 at 30 fps (rate code 3).
                const int hr = 1, mn = 2, sc = 3, fr = 4, rate = 3;
                const int nib[8] = {
                    fr & 0x0F,            // 0: frame LSB
                    (fr >> 4) & 0x01,     // 1: frame MSB
                    sc & 0x0F,            // 2: sec LSB
                    (sc >> 4) & 0x03,     // 3: sec MSB
                    mn & 0x0F,            // 4: min LSB
                    (mn >> 4) & 0x03,     // 5: min MSB
                    hr & 0x0F,            // 6: hour LSB
                    ((hr >> 4) & 0x01) | (rate << 1)  // 7: hour MSB + rate
                };
                for (int type = 0; type < 8; ++type)
                    tc.feedMtcQuarterFrame ((juce::uint8) ((type << 4) | nib[type]));

                expect (tc.isRunning());
                expectEquals ((int) tc.getHours(),   hr);
                expectEquals ((int) tc.getMinutes(), mn);
                expectEquals ((int) tc.getSeconds(), sc);
                expectEquals ((int) tc.getFrames(),  fr);
                expectWithinAbsoluteError (tc.getFps(), 30.0f, 0.01f);
            }

            beginTest ("MTC quarter-frame decodes two-digit fields (12:34:56:29 @ 25fps)");
            {
                TimecodeChase tc;
                const int hr = 12, mn = 34, sc = 56, fr = 29, rate = 1; // 25 fps
                const int nib[8] = {
                    fr & 0x0F, (fr >> 4) & 0x01,
                    sc & 0x0F, (sc >> 4) & 0x03,
                    mn & 0x0F, (mn >> 4) & 0x03,
                    hr & 0x0F, ((hr >> 4) & 0x01) | (rate << 1)
                };
                for (int type = 0; type < 8; ++type)
                    tc.feedMtcQuarterFrame ((juce::uint8) ((type << 4) | nib[type]));

                expectEquals ((int) tc.getHours(),   hr);
                expectEquals ((int) tc.getMinutes(), mn);
                expectEquals ((int) tc.getSeconds(), sc);
                expectEquals ((int) tc.getFrames(),  fr);
                expectWithinAbsoluteError (tc.getFps(), 25.0f, 0.01f);
            }

            beginTest ("MTC full-frame message publishes the timecode immediately");
            {
                TimecodeChase tc;
                tc.feedMtcFullFrame (9, 58, 7, 11);
                expect (tc.isRunning());
                expectEquals ((int) tc.getHours(),   9);
                expectEquals ((int) tc.getMinutes(), 58);
                expectEquals ((int) tc.getSeconds(), 7);
                expectEquals ((int) tc.getFrames(),  11);
            }

            beginTest ("Partial MTC quarter-frame run does not publish a stale time");
            {
                TimecodeChase tc;
                // Only 5 of 8 pieces -> incomplete, must not flip running.
                for (int type = 0; type < 5; ++type)
                    tc.feedMtcQuarterFrame ((juce::uint8) ((type << 4) | 1));
                expect (! tc.isRunning());
            }

            beginTest ("LTC presence: a square wave at the bit-clock rate reads running");
            {
                TimecodeChase tc;
                const double sr = 48000.0;
                // 30 fps × 80 bits = 2400 bits/s; biphase has up to 2 edges/bit
                // -> ~4800 crossings/s. A square wave toggling every ~5 samples
                // (~4800 Hz fundamental, two crossings/cycle => ~9600 cross/s is
                // too high) -> use a period that lands in the 1500..5500 window.
                // Toggle every 10 samples => 2400 transitions/s within range.
                std::vector<float> buf (sr);   // 1 second
                bool level = true;
                for (int i = 0; i < (int) buf.size(); ++i)
                {
                    if ((i % 10) == 0) level = ! level;
                    buf[(size_t) i] = level ? 0.8f : -0.8f;
                }
                tc.feedLtc (buf.data(), (int) buf.size(), sr);
                expect (tc.isRunning());
                expect (tc.getCrossingsPerSecond() > 1500.0f);
            }

            beginTest ("LTC presence: silence (no crossings) is not running");
            {
                TimecodeChase tc;
                std::vector<float> buf (48000, 0.0f);
                tc.feedLtc (buf.data(), (int) buf.size(), 48000.0);
                expect (! tc.isRunning());
            }
        }
    };

    static TimecodeTests timecodeTests;
}
