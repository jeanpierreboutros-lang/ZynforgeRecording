#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>
#include "../Audio/TimecodeChase.h"
#include "LtcTestSignal.h"

namespace zynforge
{
    using ltctest::Tc;
    using ltctest::appendBit;
    using ltctest::ltcSignal;

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

            beginTest ("MTC full-frame message publishes the timecode immediately, as a cue, not a running clock");
            {
                // A master sends one while stopped to cue its slaves (a locate,
                // a scrub): chase must not take it for a clock that is running.
                TimecodeChase tc;
                tc.feedMtcFullFrame (9, 58, 7, 11);
                expect (! tc.isRunning(), "a cue from a stopped master read as running timecode");
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

            // One quarter-frame message of the group that labels hh:mm:ss:ff.
            const auto quarterFrame = [] (int type, int hr, int mn, int sc, int fr, int rate)
            {
                const int nib[8] = { fr & 0x0F, (fr >> 4) & 0x01, sc & 0x0F, (sc >> 4) & 0x03,
                                     mn & 0x0F, (mn >> 4) & 0x03, hr & 0x0F,
                                     ((hr >> 4) & 0x01) | (rate << 1) };
                return (juce::uint8) ((type << 4) | nib[type]);
            };

            beginTest ("MTC joined at any phase never publishes a time mixed from two groups");
            {
                // A boundary where every field changes: 05:28 frame 28 to 06:00
                // frame 0 in the top digits, the case that read an hour wrong.
                for (int phase = 0; phase < 8; ++phase)
                {
                    TimecodeChase tc;
                    for (int type = phase; type < 8; ++type)
                        tc.feedMtcQuarterFrame (quarterFrame (type, 5, 59, 59, 28, 3));
                    for (int type = 0; type < 8; ++type)
                    {
                        tc.feedMtcQuarterFrame (quarterFrame (type, 6, 0, 0, 0, 3));
                        if (type < 7 && phase != 0)
                            expect (! tc.isRunning() , "a part-run time was published");
                    }
                    expectEquals ((int) tc.getHours(),   6);
                    expectEquals ((int) tc.getMinutes(), 0);
                    expectEquals ((int) tc.getSeconds(), 0);
                    expectEquals ((int) tc.getFrames(),  0);
                }
            }

            beginTest ("A running MTC stream joined late reads every time in order");
            {
                for (int phase = 1; phase < 8; ++phase)
                {
                    TimecodeChase tc;
                    int published = -1, seconds = 9 * 30 + 24;   // 00:00:09:24 at 30 fps, frame count
                    for (int group = 0; group < 12; ++group, seconds += 2)
                    {
                        const int fr = seconds % 30, sc = seconds / 30;
                        for (int type = group == 0 ? phase : 0; type < 8; ++type)
                            tc.feedMtcQuarterFrame (quarterFrame (type, 0, 0, sc, fr, 3));
                        if (! tc.isRunning()) continue;
                        const int now = (int) tc.getSeconds() * 30 + (int) tc.getFrames();
                        expect (published < 0 || now == published + 2,
                                "phase " + juce::String (phase) + ": " + juce::String (published)
                                    + " jumped to " + juce::String (now));
                        published = now;
                    }
                    expect (published > 0, "nothing was read");
                }
            }

            beginTest ("A lost MTC quarter-frame drops that run instead of mixing groups");
            {
                TimecodeChase tc;
                for (int type = 0; type < 8; ++type)
                    tc.feedMtcQuarterFrame (quarterFrame (type, 1, 2, 3, 4, 3));
                for (int type = 0; type < 8; ++type)
                    if (type != 3)   // lost on the wire
                        tc.feedMtcQuarterFrame (quarterFrame (type, 9, 9, 9, 8, 3));
                expectEquals ((int) tc.getHours(), 1, "the damaged run replaced a good time");
                for (int type = 0; type < 8; ++type)
                    tc.feedMtcQuarterFrame (quarterFrame (type, 2, 2, 3, 6, 3));
                expectEquals ((int) tc.getHours(), 2, "the next whole run was not read");
            }

            beginTest ("A full frame between quarter-frame runs never keeps a stopped master running");
            {
                TimecodeChase tc;
                for (int type = 0; type < 8; ++type)
                    tc.feedMtcQuarterFrame (quarterFrame (type, 1, 0, 0, 0, 3));
                expect (tc.isRunning());
                juce::Thread::sleep (300);   // the master stopped
                expect (! tc.isRunning());
                tc.feedMtcFullFrame (1, 0, 30, 0, 30.0f);   // and cued a new place
                expect (! tc.isRunning(), "a cue restarted a stopped master's clock");
                expectEquals ((int) tc.getSeconds(), 30);
            }

            // Quarter-frames for `frames` frames from `start`, timed as a master
            // running at `realFps` sends them; labels count at the nominal rate.
            const auto feedMtcRun = [&quarterFrame] (TimecodeChase& tc, Tc start, int frames, int nominal,
                                                    int rateCode, double realFps)
            {
                int count = ((start.h * 60 + start.m) * 60 + start.s) * nominal + start.f;
                const double msPerQuarter = 1000.0 / (realFps * 4.0);
                double now = 1000.0;
                for (int f = 0; f + 1 < frames; f += 2, count += 2)
                {
                    const int fr = count % nominal, total = count / nominal;
                    const int sc = total % 60, mn = (total / 60) % 60, hr = (total / 3600) % 24;
                    for (int type = 0; type < 8; ++type, now += msPerQuarter)
                        tc.feedMtcQuarterFrame (quarterFrame (type, hr, mn, sc, fr, rateCode), now);
                }
            };

            beginTest ("MTC at 29.97 non-drop is told from 30 once the master has run a while");
            {
                TimecodeChase pulled;
                feedMtcRun (pulled, { 1, 0, 0, 0 }, 30 * 12, 30, 3, 30000.0 / 1001.0);
                expectWithinAbsoluteError (pulled.getFps(), 29.97f, 0.01f);
                expect (! pulled.isDropFrame());

                TimecodeChase real;
                feedMtcRun (real, { 1, 0, 0, 0 }, 30 * 12, 30, 3, 30.0);
                expectWithinAbsoluteError (real.getFps(), 30.0f, 0.001f, "true 30 fps MTC read as pulled down");

                TimecodeChase film;
                feedMtcRun (film, { 1, 0, 0, 0 }, 24 * 12, 24, 0, 24000.0 / 1001.0);
                expectWithinAbsoluteError (film.getFps(), 23.976f, 0.01f);
            }

            beginTest ("MTC running backwards is read from its own direction");
            {
                TimecodeChase tc;
                for (int type = 7; type >= 0; --type)
                    tc.feedMtcQuarterFrame (quarterFrame (type, 1, 2, 3, 4, 1));
                expect (tc.isRunning());
                expectEquals ((int) tc.getMinutes(), 2);
                expectEquals ((int) tc.getFrames(),  4);
                expectWithinAbsoluteError (tc.getFps(), 25.0f, 0.01f);
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
                // 00:01:00;16 is frame 1814 of 1001/30000 s: the labels skipped
                // are not time.
                expectWithinAbsoluteError (tc.timeOfDaySeconds (0), 1814.0 * 1001.0 / 30000.0, 1.0e-9);
            }

            beginTest ("29.97 drop-frame labels convert by their frame count, both ways");
            {
                TimecodeChase tc;
                tc.feedMtcFullFrame (0, 0, 0, 0, 29.97f, true);
                expect (tc.isDropFrame());
                const auto label = [] (int h, int m, int s, int f)
                {
                    return ((juce::uint32) h << 24) | ((juce::uint32) m << 16) | ((juce::uint32) s << 8) | (juce::uint32) f;
                };
                // The standard drop-frame label of frame N (SMPTE 12M).
                const auto dfLabel = [&label] (juce::int64 n)
                {
                    const auto tens = n / 17982, rest = n % 17982;
                    n += 18 * tens + (rest < 2 ? 0 : 2 * ((rest - 2) / 1798));
                    return label ((int) ((n / 108000) % 24), (int) ((n / 1800) % 60), (int) ((n / 30) % 60), (int) (n % 30));
                };
                const auto real = [] (juce::int64 frames) { return (double) frames * 1001.0 / 30000.0; };

                // The last label of a minute is about 60 ms past what its digits say.
                expectWithinAbsoluteError (tc.secondsAt (label (0, 0, 59, 29), 0), real (1799), 1.0e-9,
                                           "a drop-frame label was read as label seconds");
                expectWithinAbsoluteError (tc.secondsAt (label (0, 1, 0, 2), 0), real (1800), 1.0e-9,
                                           "the frame after the dropped labels");
                expectWithinAbsoluteError (tc.secondsAt (label (0, 10, 0, 0), 0), real (17982), 1.0e-9,
                                           "a tenth minute drops nothing");
                expectWithinAbsoluteError (tc.secondsAt (label (1, 0, 0, 0), 0), real (107892), 1.0e-9);
                expectWithinAbsoluteError (tc.secondsAt (label (0, 0, 59, 29), 1), real (1800), 1.0e-9,
                                           "a frame late is one real frame, not a label");
                expectWithinAbsoluteError (tc.secondsAt (24u << 24, 0), real (2589408), 1.0e-6,
                                           "a drop-frame day is 2589408 frames");

                int wrongLabel = 0, wrongTime = 0;
                for (juce::int64 n = 0; n < 2589408; n += 97)
                {
                    const auto expected = dfLabel (n);
                    if (tc.labelAt (real (n)) != expected) ++wrongLabel;
                    if (std::abs (tc.secondsAt (expected, 0) - real (n)) > 1.0e-6) ++wrongTime;
                }
                for (const juce::int64 n : { (juce::int64) 1798, (juce::int64) 1799, (juce::int64) 1800,
                                             (juce::int64) 17981, (juce::int64) 17982, (juce::int64) 17983,
                                             (juce::int64) 19781, (juce::int64) 19782, (juce::int64) 2589407 })
                {
                    if (tc.labelAt (real (n)) != dfLabel (n)) ++wrongLabel;
                    if (std::abs (tc.secondsAt (dfLabel (n), 0) - real (n)) > 1.0e-6) ++wrongTime;
                }
                expectEquals (wrongLabel, 0, "a time of day read back as the wrong drop-frame label");
                expectEquals (wrongTime, 0, "a drop-frame label read as the wrong time");
            }

            beginTest ("The chase time counts how late a label was known and how long ago it came");
            {
                const double sr = 48000.0;
                // LTC: a label is known when its sync word ends the frame, one
                // frame after it began, and the playhead keeps moving after that.
                {
                    TimecodeChase tc;
                    const auto sig = ltcSignal (sr, 25.0, false, { 1, 0, 0, 0 }, 50);
                    const double base = 100000.0;
                    for (size_t pos = 0; pos < sig.size(); pos += 512)
                    {
                        const int n = (int) juce::jmin ((size_t) 512, sig.size() - pos);
                        tc.feedLtc (sig.data() + pos, n, sr, base + (double) (pos + (size_t) n) * 1000.0 / sr);
                    }
                    expect (tc.isRunning());
                    const double end = base + (double) sig.size() * 1000.0 / sr;
                    // Frame 0 began at the signal's first sample.
                    expectWithinAbsoluteError (tc.timeOfDayNow (end), 3600.0 + (double) sig.size() / sr, 0.002,
                                               "the LTC chase target runs behind the master");
                    expectWithinAbsoluteError (tc.timeOfDayNow (end + 25.0), 3600.0 + (double) sig.size() / sr + 0.025,
                                               0.002, "the label's age since it came is not counted");
                }
                // MTC: a quarter-frame run labels the frame of its first message;
                // the whole time is known two frames later.
                {
                    TimecodeChase tc;
                    const double base = 5000.0;
                    for (int type = 0; type < 8; ++type)
                        tc.feedMtcQuarterFrame (quarterFrame (type, 1, 0, 0, 10, 1), base + type * 10.0);
                    expect (tc.isRunning());
                    const double last = base + 70.0;
                    expectWithinAbsoluteError (tc.timeOfDayNow (last + 30.0), 3600.0 + 12.0 / 25.0 + 0.030, 1.0e-6,
                                               "the MTC chase target runs behind the master");
                }
                // A full frame is a cue from a parked master: where, not a clock.
                {
                    TimecodeChase tc;
                    tc.feedMtcFullFrame (1, 0, 0, 10, 25.0f);
                    expectWithinAbsoluteError (tc.timeOfDayNow (juce::Time::getMillisecondCounterHiRes() + 500.0),
                                               3600.0 + 10.0 / 25.0, 1.0e-9, "a cue was run forward");
                }
            }

            beginTest ("LTC at 29.97 non-drop is told from 30, and its time counts real seconds");
            {
                TimecodeChase tc;
                const auto sig = ltcSignal (48000.0, 30000.0 / 1001.0, false, { 1, 0, 0, 0 }, 40);
                feedBlocks (tc, sig, 48000.0);
                expect (tc.isRunning(), "29.97 non-drop LTC did not LOCK");
                expectWithinAbsoluteError (tc.getFps(), 29.97f, 0.01f);
                expect (! tc.isDropFrame());
                expectTime (tc, { 1, 0, 1, 9 });
                // Frames are what is real: 01:00:01:09 is 108039 frames of 1001/30000 s.
                expectWithinAbsoluteError (tc.timeOfDaySeconds (0), 108039.0 * 1001.0 / 30000.0, 1.0e-6);
                // So 80 s of labels are 80.08 s of a free-running player, not 80.
                const juce::uint32 a = (1u << 24), b = (1u << 24) | (1u << 16) | (20u << 8);
                expectWithinAbsoluteError (tc.secondsAt (b, 0) - tc.secondsAt (a, 0), 80.08, 1.0e-6);
                expectEquals ((int) tc.labelAt (tc.secondsAt (b, 0)), (int) b, "a stamp does not read back as its label");
            }

            beginTest ("LTC at 23.976 is told from 24");
            {
                TimecodeChase tc;
                const auto sig = ltcSignal (48000.0, 24000.0 / 1001.0, false, { 2, 0, 0, 0 }, 40);
                feedBlocks (tc, sig, 48000.0);
                expect (tc.isRunning(), "23.976 LTC did not LOCK");
                expectWithinAbsoluteError (tc.getFps(), 23.976f, 0.01f);
                expectTime (tc, { 2, 0, 1, 15 });
                expectWithinAbsoluteError (tc.timeOfDaySeconds (0), (2.0 * 3600.0 * 24.0 + 39.0) * 1001.0 / 24000.0, 1.0e-6);
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
    };

    static TimecodeTests timecodeTests;
}
