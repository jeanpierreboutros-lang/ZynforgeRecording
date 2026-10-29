#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>
#include "../Audio/TimecodeChase.h"

namespace zynforge
{
    // Decoder-level tests for external timecode chase. The MTC path is the
    // newly-wired one (MIDI -> feedMtcQuarterFrame / feedMtcFullFrame). The LTC
    // biphase decoder is fed synthesized SMPTE 12M timecode at 24 / 25 / 29.97
    // DF / 30 fps, and LOCK is checked against audio that is NOT timecode.
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

            beginTest ("LTC: a square wave at the bit-clock rate never reads LOCK");
            {
                // A square wave crosses zero at a steady rate inside the old
                // "presence" window. That is not timecode: nothing decodes,
                // so the chase must stay idle.
                TimecodeChase tc;
                const double sr = 48000.0;
                std::vector<float> buf ((size_t) sr * 2);
                bool level = true;
                for (int i = 0; i < (int) buf.size(); ++i)
                {
                    if ((i % 10) == 0) level = ! level;
                    buf[(size_t) i] = level ? 0.8f : -0.8f;
                }
                feedBlocks (tc, buf, sr);
                expect (! tc.isRunning(), "a square wave read as LTC LOCK");
            }

            beginTest ("LTC presence: silence (no crossings) is not running");
            {
                TimecodeChase tc;
                std::vector<float> buf (48000, 0.0f);
                tc.feedLtc (buf.data(), (int) buf.size(), 48000.0);
                expect (! tc.isRunning());
            }

            beginTest ("LTC decodes 24 fps: timecode and frame rate");
            {
                TimecodeChase tc;
                const auto sig = ltcSignal (48000.0, 24.0, false, { 1, 2, 3, 0 }, 30);
                feedBlocks (tc, sig, 48000.0);
                expect (tc.isRunning(), "24 fps LTC did not LOCK");
                expectWithinAbsoluteError (tc.getFps(), 24.0f, 0.01f);
                expectTime (tc, { 1, 2, 4, 5 });   // 30 frames after 01:02:03:00
            }

            beginTest ("LTC decodes 25 fps across an hour boundary");
            {
                TimecodeChase tc;
                const auto sig = ltcSignal (48000.0, 25.0, false, { 10, 59, 59, 20 }, 40);
                feedBlocks (tc, sig, 48000.0);
                expect (tc.isRunning(), "25 fps LTC did not LOCK");
                expectWithinAbsoluteError (tc.getFps(), 25.0f, 0.01f);
                expect (! tc.isDropFrame());
                expectTime (tc, { 11, 0, 1, 9 });   // 10:59:59:20 + 39 frames
            }

            beginTest ("LTC decodes 30 fps up to 23:59:59:29");
            {
                TimecodeChase tc;
                const auto sig = ltcSignal (48000.0, 30.0, false, { 23, 59, 58, 0 }, 60);
                feedBlocks (tc, sig, 48000.0);
                expect (tc.isRunning(), "30 fps LTC did not LOCK");
                expectWithinAbsoluteError (tc.getFps(), 30.0f, 0.01f);
                expectTime (tc, { 23, 59, 59, 29 });
            }

            beginTest ("LTC decodes 29.97 drop-frame across the dropped labels");
            {
                TimecodeChase tc;
                // 00:00:59:25 .. 00:01:00:01 do not exist after :29 -> :02.
                const auto sig = ltcSignal (48000.0, 29.97, true, { 0, 0, 59, 25 }, 20);
                feedBlocks (tc, sig, 48000.0);
                expect (tc.isRunning(), "29.97 DF LTC did not LOCK");
                expectWithinAbsoluteError (tc.getFps(), 29.97f, 0.01f);
                expect (tc.isDropFrame());
                expectTime (tc, { 0, 1, 0, 16 });   // :25..:29 (5), then :02..:16 (15)
            }

            beginTest ("LTC decodes at 44.1 kHz, inverted, with a noise floor");
            {
                TimecodeChase tc;
                auto sig = ltcSignal (44100.0, 25.0, false, { 2, 0, 0, 0 }, 50);
                juce::Random rnd (1234);
                for (auto& v : sig) v = -v + (rnd.nextFloat() - 0.5f) * 0.03f;
                feedBlocks (tc, sig, 44100.0, 333);
                expect (tc.isRunning(), "noisy, inverted 44.1 kHz LTC did not LOCK");
                expectWithinAbsoluteError (tc.getFps(), 25.0f, 0.01f);
                expectTime (tc, { 2, 0, 1, 24 });
            }

            beginTest ("LTC: a relocating source keeps LOCK and follows the new time");
            {
                TimecodeChase tc;
                auto sig = ltcSignal (48000.0, 25.0, false, { 1, 0, 0, 0 }, 20);
                const auto jump = ltcSignal (48000.0, 25.0, false, { 2, 30, 0, 0 }, 10);
                sig.insert (sig.end(), jump.begin(), jump.end());
                feedBlocks (tc, sig, 48000.0);
                expect (tc.isRunning(), "a timecode jump dropped LOCK");
                expectTime (tc, { 2, 30, 0, 9 });
            }

            beginTest ("LTC: noise, tones and random biphase data never read LOCK");
            {
                const double sr = 48000.0;
                juce::Random rnd (99);
                std::vector<std::pair<juce::String, std::vector<float>>> cases;
                {
                    std::vector<float> noise ((size_t) sr * 3);
                    for (auto& v : noise) v = rnd.nextFloat() * 1.6f - 0.8f;
                    cases.emplace_back ("white noise", std::move (noise));
                }
                for (const double hz : { 440.0, 1000.0, 1200.0, 2400.0 })
                {
                    std::vector<float> tone ((size_t) sr * 3);
                    for (size_t i = 0; i < tone.size(); ++i)
                        tone[i] = 0.7f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / sr);
                    cases.emplace_back (juce::String (hz) + " Hz tone", std::move (tone));
                }
                {
                    // Program-like: two detuned tones plus low noise.
                    std::vector<float> prog ((size_t) sr * 3);
                    for (size_t i = 0; i < prog.size(); ++i)
                        prog[i] = 0.4f * (float) std::sin (0.0731 * (double) i)
                                + 0.3f * (float) std::sin (0.2117 * (double) i)
                                + 0.05f * (rnd.nextFloat() - 0.5f);
                    cases.emplace_back ("program audio", std::move (prog));
                }
                {
                    // LTC-shaped modulation carrying random bits: right edge
                    // timing, but no consistent sync words.
                    std::vector<float> bmc;
                    double clock = 0.0; float level = 0.5f;
                    const double bitLen = sr / 2000.0;
                    for (int b = 0; b < 2000 * 3; ++b)
                        appendBit (bmc, clock, level, bitLen, rnd.nextBool() ? 1 : 0);
                    cases.emplace_back ("random biphase bits", std::move (bmc));
                }
                for (auto& [name, sig] : cases)
                {
                    TimecodeChase tc;
                    bool everLocked = false;
                    for (size_t pos = 0; pos < sig.size(); pos += 512)
                    {
                        const int n = (int) juce::jmin ((size_t) 512, sig.size() - pos);
                        tc.feedLtc (sig.data() + pos, n, sr);
                        everLocked = everLocked || tc.isRunning();
                    }
                    expect (! everLocked, name + " read as LTC LOCK");
                }
            }

            beginTest ("LTC: LOCK drops when the timecode stops (silence, noise, or no feed)");
            {
                const double sr = 48000.0;
                {
                    TimecodeChase tc;
                    feedBlocks (tc, ltcSignal (sr, 25.0, false, { 1, 0, 0, 0 }, 25), sr);
                    expect (tc.isRunning());
                    feedBlocks (tc, std::vector<float> ((size_t) (sr * 0.5), 0.0f), sr);
                    expect (! tc.isRunning(), "LOCK latched through half a second of silence");
                }
                {
                    TimecodeChase tc;
                    feedBlocks (tc, ltcSignal (sr, 25.0, false, { 1, 0, 0, 0 }, 25), sr);
                    expect (tc.isRunning());
                    juce::Random rnd (7);
                    std::vector<float> noise ((size_t) (sr * 0.5));
                    for (auto& v : noise) v = rnd.nextFloat() - 0.5f;
                    feedBlocks (tc, noise, sr);
                    expect (! tc.isRunning(), "LOCK latched through half a second of noise");
                }
                {
                    // The source strip is unrouted: feedLtc is simply never
                    // called again, so the audio thread cannot clear anything.
                    TimecodeChase tc;
                    feedBlocks (tc, ltcSignal (sr, 25.0, false, { 1, 0, 0, 0 }, 25), sr);
                    expect (tc.isRunning());
                    juce::Thread::sleep (450);
                    expect (! tc.isRunning(), "LOCK latched after the LTC feed stopped");
                }
            }
        }

    private:
        struct Tc { int h, m, s, f; };

        void expectTime (TimecodeChase& tc, Tc want)
        {
            const auto got = juce::String::formatted ("%02d:%02d:%02d:%02d", (int) tc.getHours(),
                                                      (int) tc.getMinutes(), (int) tc.getSeconds(),
                                                      (int) tc.getFrames());
            expectEquals (got, juce::String::formatted ("%02d:%02d:%02d:%02d", want.h, want.m, want.s, want.f));
        }

        static void feedBlocks (TimecodeChase& tc, const std::vector<float>& sig, double sr, int block = 512)
        {
            for (size_t pos = 0; pos < sig.size(); pos += (size_t) block)
                tc.feedLtc (sig.data() + pos, (int) juce::jmin ((size_t) block, sig.size() - pos), sr);
        }

        // Biphase-mark: a transition at every bit start, plus one mid-cell
        // for a '1'. `clock` is the running time in (fractional) samples.
        static void appendBit (std::vector<float>& out, double& clock, float& level, double bitLen, int bit)
        {
            auto emitUntil = [&] (double t) { while ((double) out.size() < t) out.push_back (level); };
            level = -level;
            emitUntil (clock + bitLen * 0.5);
            if (bit != 0) level = -level;
            emitUntil (clock + bitLen);
            clock += bitLen;
        }

        // SMPTE 12M LTC: `count` consecutive frames from `start`, with the
        // drop-frame label skip when `df`.
        static std::vector<float> ltcSignal (double sr, double fps, bool df, Tc start, int count)
        {
            std::vector<float> out;
            double clock = 0.0; float level = 0.5f;
            const int nominal = (int) std::lround (fps);
            const double bitLen = sr / (80.0 * fps);
            Tc t = start;
            for (int n = 0; n < count; ++n)
            {
                int bits[80] {};
                auto put = [&] (int at, int width, int value)
                { for (int i = 0; i < width; ++i) bits[at + i] = (value >> i) & 1; };
                put (0, 4, t.f % 10); put (8, 2, t.f / 10); bits[10] = df ? 1 : 0;
                put (16, 4, t.s % 10); put (24, 3, t.s / 10);
                put (32, 4, t.m % 10); put (40, 3, t.m / 10);
                put (48, 4, t.h % 10); put (56, 2, t.h / 10);
                const int sync[16] { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
                for (int i = 0; i < 16; ++i) bits[64 + i] = sync[i];
                for (int b = 0; b < 80; ++b) appendBit (out, clock, level, bitLen, bits[b]);

                if (++t.f >= nominal)
                {
                    t.f = 0;
                    if (++t.s >= 60) { t.s = 0; if (++t.m >= 60) { t.m = 0; if (++t.h >= 24) t.h = 0; } }
                    if (df && t.s == 0 && (t.m % 10) != 0) t.f = 2;
                }
            }
            // A real generator keeps going: the next frame's first edge is
            // what completes this frame's final bit cell.
            level = -level;
            while ((double) out.size() < clock + bitLen * 0.5) out.push_back (level);
            return out;
        }
    };

    static TimecodeTests timecodeTests;
}
