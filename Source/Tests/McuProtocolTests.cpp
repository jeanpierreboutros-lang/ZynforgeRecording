#include <juce_audio_basics/juce_audio_basics.h>
#include "../Audio/McuProtocol.h"

namespace zynforge
{
    class McuProtocolTests final : public juce::UnitTest
    {
    public:
        McuProtocolTests() : UnitTest ("MCU protocol", "zynforge") {}

        void runTest() override
        {
            using namespace mcu;

            beginTest ("Fader dB <-> 14-bit round-trips and clamps");
            {
                for (float dB : { -65.0f, -20.0f, 0.0f, 6.0f, 12.0f })
                    expectWithinAbsoluteError (faderToDb (dbToFader (dB)), dB, 0.02f);
                expectEquals (dbToFader (-100.0f), 0);        // clamps low
                expectEquals (dbToFader (100.0f), 16383);     // clamps high
                expect (faderToDb (0) <= kFaderMinDb + 0.01f);
                expect (faderToDb (16383) >= kFaderMaxDb - 0.01f);
            }

            beginTest ("Button note decode maps per-strip + transport");
            {
                expect (decodeButton (0x10).action == Action::Mute   && decodeButton (0x10).strip == 0);
                expect (decodeButton (0x17).action == Action::Mute   && decodeButton (0x17).strip == 7);
                expect (decodeButton (0x08).action == Action::Solo   && decodeButton (0x08).strip == 0);
                expect (decodeButton (0x00).action == Action::Arm    && decodeButton (0x00).strip == 0);
                expect (decodeButton (0x18).action == Action::Select && decodeButton (0x18).strip == 0);
                expect (decodeButton (Play).action  == Action::Play);
                expect (decodeButton (Stop).action  == Action::Stop);
                expect (decodeButton (Record).action == Action::Record);
                expect (decodeButton (0x40).action  == Action::None);   // unmapped
            }

            beginTest ("buttonNote is the inverse of decodeButton");
            {
                expectEquals (buttonNote (Action::Mute, 3), 0x10 + 3);
                expectEquals (buttonNote (Action::Solo, 5), 0x08 + 5);
                expectEquals (buttonNote (Action::Play, 0), (int) Play);
            }

            beginTest ("Fader message is a pitch-wheel on the strip's channel");
            {
                const auto m = fader (2, 0.0f);
                expect (m.isPitchWheel());
                expectEquals (m.getChannel(), 3);   // strip 2 -> MIDI channel 3
            }

            beginTest ("Scribble strip is a 10-byte+ SysEx at the strip's offset");
            {
                const auto m = scribble (1, "Kick");
                expect (m.isSysEx());
                const auto* d = m.getSysExData();
                expectEquals ((int) d[0], 0x00); expectEquals ((int) d[1], 0x00);
                expectEquals ((int) d[2], 0x66); expectEquals ((int) d[3], 0x14);
                expectEquals ((int) d[4], 0x12);
                expectEquals ((int) d[5], 7);     // strip 1 -> offset 7
            }
        }
    };

    static McuProtocolTests mcuProtocolTests;
}
