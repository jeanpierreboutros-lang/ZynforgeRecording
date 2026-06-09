// Headless tests for post-show song detection (Source/Audio/SongDetector.h).
// Drives detect() with synthetic per-track envelopes (the unit the quorum
// logic actually consumes) plus one real synthesized WAV through
// envelopeFlags() to prove the streaming envelope end.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/SongDetector.h"

namespace zynforge
{
    class SongDetectorTests final : public juce::UnitTest
    {
    public:
        SongDetectorTests() : juce::UnitTest ("Song detection", "zynforge") {}

        // Build an envelope of `n` windows, active in [a, b).
        static std::vector<char> env (size_t n, size_t a, size_t b)
        {
            std::vector<char> e (n, 0);
            for (size_t i = a; i < b && i < n; ++i) e[i] = 1;
            return e;
        }

        void runTest() override
        {
            const double sr = 48000.0;
            songs::Params p;
            p.windowSec  = 0.100;   // 1 window = 4800 samples
            p.minGapSec  = 4.0;     // 40 windows
            p.minSongSec = 60.0;    // 600 windows
            p.quorumTracks = 2;

            const int win = (int) (sr * p.windowSec);

            beginTest ("two songs split by a real gap, with the quorum met");
            {
                // 3 tracks, 3000 windows (5 min). Songs at [100,900) and
                // [1400,2400) on two instrument tracks; an 'ambient mic'
                // track stays hot the whole time -- alone it must not
                // count as music (quorum 2).
                const size_t n = 3000;
                std::vector<std::vector<char>> flags;
                flags.push_back (env (n, 100, 900));
                flags.push_back (env (n, 100, 900));
                flags.push_back (env (n, 0, n));                  // crowd/ambient: always on
                for (size_t i = 1400; i < 2400; ++i) { flags[0][i] = 1; flags[1][i] = 1; }

                const auto found = songs::detect (flags, sr, p);
                expectEquals ((int) found.size(), 2);
                if (found.size() == 2)
                {
                    expectEquals (found[0].startSample, (juce::int64) 100 * win);
                    expectEquals (found[0].endSample,   (juce::int64) 900 * win);
                    expectEquals (found[1].startSample, (juce::int64) 1400 * win);
                }
            }

            beginTest ("ambient-only activity (below quorum) detects nothing");
            {
                const size_t n = 3000;
                std::vector<std::vector<char>> flags;
                flags.push_back (env (n, 0, n));      // one hot mic, nothing else
                flags.push_back (env (n, 0, 0));
                expect (songs::detect (flags, sr, p).empty());
            }

            beginTest ("a short dip inside a song does not split it");
            {
                // Song [100, 1000) with a 2 s (20-window) dip at 500 --
                // shorter than the 4 s gap, so still ONE song.
                const size_t n = 1500;
                std::vector<std::vector<char>> flags (2, env (n, 100, 1000));
                for (auto& f : flags)
                    for (size_t i = 500; i < 520; ++i) f[i] = 0;
                const auto found = songs::detect (flags, sr, p);
                expectEquals ((int) found.size(), 1);
            }

            beginTest ("a blip shorter than minSongSec is dropped");
            {
                const size_t n = 1500;
                std::vector<std::vector<char>> flags (2, env (n, 100, 200));   // 10 s
                expect (songs::detect (flags, sr, p).empty());
            }

            beginTest ("envelopeFlags streams a real file into window flags");
            {
                // 0-2 s silence, 2-4 s sine at 0.5: windows 0-19 quiet,
                // 20-39 loud at the default -40 dB threshold.
                const int total = (int) (4.0 * sr);
                std::vector<float> samples ((size_t) total, 0.0f);
                for (int i = (int) (2.0 * sr); i < total; ++i)
                    samples[(size_t) i] = 0.5f * (float) std::sin (
                        2.0 * juce::MathConstants<double>::pi * 200.0 * i / sr);

                auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("zf-songs-" + juce::Uuid().toString());
                dir.createDirectory();
                auto f = dir.getChildFile ("Track_01.wav");
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
                    std::unique_ptr<juce::AudioFormatWriter> w (
                        wav.createWriterFor (os.get(), sr, 1, 32, {}, 0));
                    expect (w != nullptr);
                    os.release();
                    const float* chans[1] = { samples.data() };
                    w->writeFromFloatArrays (chans, 1, total);
                }

                double fileSr = 0.0;
                const auto flags = songs::envelopeFlags (f, -40.0f, 0.100, fileSr);
                expectEquals (fileSr, sr);
                expectEquals ((int) flags.size(), 40);
                expect (! flags.empty() && flags[5] == 0,  "silent region flagged active");
                expect (flags.size() > 25 && flags[25] == 1, "sine region not flagged active");

                dir.deleteRecursively();
            }
        }
    };

    static SongDetectorTests songDetectorTests;
}
