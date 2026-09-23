// Phase 1 of the multi-part take model: a take split across Track_NN.wav +
// Track_NN_partXX.wav must load as ONE track and play back SEAMLESSLY across
// the part boundary (today the player ignored the continuations and only
// played part 1). Read-only -- proves the ConcatReader stitching.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"

namespace zynforge
{
    class MultiPartPlaybackTests final : public juce::UnitTest
    {
    public:
        MultiPartPlaybackTests() : juce::UnitTest ("Multi-part playback", "zynforge") {}

        struct FailingReader final : juce::AudioFormatReader
        {
            FailingReader() : juce::AudioFormatReader (nullptr, "failing")
            { sampleRate = 48000.0; numChannels = 1; lengthInSamples = 8; bitsPerSample = 24; }
            bool readSamples (int* const*, int, int, juce::int64, int) override { return false; }
        };

        static void writeDc (const juce::File& f, float value, int len, double sr)
        {
            std::vector<float> s ((size_t) len, value);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, 1, 24, {}, 0));
            os.release();
            const float* ch[1] = { s.data() };
            w->writeFromFloatArrays (ch, 1, len);
        }

        // Mean of output channel 0 over one block at `pos`, polled until the
        // background BufferingAudioReader has filled (or ~0.6 s elapses).
        static float warmMeanAt (SessionPlayer& player, int numCh, int block, juce::int64 pos)
        {
            for (int t = 0; t < 120; ++t)
            {
                player.start();
                player.setPositionSamples (pos);
                std::vector<std::vector<float>> ob ((size_t) numCh, std::vector<float> ((size_t) block, 0.0f));
                std::vector<float*> op ((size_t) numCh);
                for (int c = 0; c < numCh; ++c) op[(size_t) c] = ob[(size_t) c].data();
                player.processBlock (op.data(), numCh, block);
                double sum = 0.0; float pk = 0.0f;
                for (int i = 0; i < block; ++i) { sum += ob[0][(size_t) i]; pk = juce::jmax (pk, std::abs (ob[0][(size_t) i])); }
                if (pk > 0.05f) return (float) (sum / block);
                juce::Thread::sleep (5);
            }
            return 0.0f;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            const double sr = 48000.0;
            const int    block = 512;
            const int    lenA = 48000;   // part 1 @ +0.50
            const int    lenB = 24000;   // part 2 @ -0.30

            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-multipart-" + juce::Uuid().toString());
            auto af = dir.getChildFile ("Audio Files");
            af.createDirectory();
            writeDc (af.getChildFile ("Track_01.wav"),         0.50f, lenA, sr);
            writeDc (af.getChildFile ("Track_01_part02.wav"), -0.30f, lenB, sr);

            beginTest ("split take loads as ONE track with the summed length");
            {
                AudioEngine eng;
                const int n = eng.loadSession (dir);
                expectEquals (n, 1, "parts should collapse to one track, not many");
                expect (eng.getActiveSessionDir() == dir,
                        "loading a session must make it the active save/export target");
                auto& player = eng.getPlayer();
                player.prepare (sr, block);
                expectEquals ((int) player.getTotalLengthSamples(), lenA + lenB,
                              "total length should span both parts");
            }

            beginTest ("playback is seamless across the part boundary");
            {
                AudioEngine eng;
                eng.loadSession (dir);
                auto& player = eng.getPlayer();
                player.prepare (sr, block);

                // Deep inside each part.
                expectWithinAbsoluteError (warmMeanAt (player, 1, block, lenA / 2),         0.50f, 0.05f);
                expectWithinAbsoluteError (warmMeanAt (player, 1, block, lenA + lenB / 2), -0.30f, 0.05f);

                // A block straddling the boundary must contain BOTH parts'
                // audio -- no silent gap, no dropped continuation.
                player.start();
                player.setPositionSamples (lenA - block / 2);
                // warm it
                bool sawBoundary = false;
                for (int t = 0; t < 120; ++t)
                {
                    std::vector<float> ob ((size_t) block, 0.0f);
                    float* op[1] = { ob.data() };
                    player.setPositionSamples (lenA - block / 2);
                    player.processBlock (op, 1, block);
                    float mn = 1.0f, mx = -1.0f;
                    for (float v : ob) { mn = juce::jmin (mn, v); mx = juce::jmax (mx, v); }
                    if (mx > 0.4f && mn < -0.2f)   // saw +0.5 (part1) AND -0.3 (part2)
                    {
                        sawBoundary = true;
                        break;
                    }
                    juce::Thread::sleep (5);
                }
                expect (sawBoundary, "boundary block did not contain both parts (gap at the seam)");
            }

            beginTest ("numeric part discovery does not place part100 before part11");
            {
                const auto main = af.getChildFile ("Track_02.wav");
                writeDc (main, 0.1f, 64, sr);
                writeDc (af.getChildFile ("Track_02_part100.wav"), 0.2f, 64, sr);
                const auto parts = findTakeParts (main);
                expectEquals ((int) parts.size(), 100);
                expectEquals (parts[10].getFileName(), juce::String ("Track_02_part11.wav"));
                expectEquals (parts[99].getFileName(), juce::String ("Track_02_part100.wav"));
                main.deleteFile();
                af.getChildFile ("Track_02_part100.wav").deleteFile();
            }

            beginTest ("missing middle part is rejected instead of time-shifting later audio");
            {
                auto broken = dir.getParentDirectory().getChildFile ("zf-broken-" + juce::Uuid().toString());
                auto baf = broken.getChildFile ("Audio Files");
                baf.createDirectory();
                writeDc (baf.getChildFile ("Track_01.wav"), 0.1f, 64, sr);
                writeDc (baf.getChildFile ("Track_01_part03.wav"), 0.3f, 64, sr);
                AudioEngine eng;
                expectEquals (eng.loadSession (broken), 0);
                broken.deleteRecursively();
            }

            beginTest ("absurd part and track indices cannot allocate unbounded readers");
            {
                const auto main = af.getChildFile ("Track_03.wav");
                writeDc (main, 0.1f, 64, sr);
                const auto hugePart = af.getChildFile ("Track_03_part999999999.wav");
                writeDc (hugePart, 0.2f, 64, sr);
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                const auto parts = findTakeParts (main);
                expectEquals ((int) parts.size(), 2, "huge part index expanded into millions of paths");
                expect (ConcatReader::create (fm, parts) == nullptr, "invalid take loaded as complete");
                main.deleteFile(); hugePart.deleteFile();

                const auto hugeTrack = af.getChildFile ("Track_999999999.wav");
                writeDc (hugeTrack, 0.2f, 64, sr);
                AudioEngine eng;
                expectEquals (eng.loadSession (dir), 1, "huge track index expanded player slots");
                hugeTrack.deleteFile();
            }

            beginTest ("a failing continuation reader propagates read failure");
            {
                std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
                readers.push_back (std::make_unique<FailingReader>());
                readers.push_back (std::make_unique<FailingReader>());
                ConcatReader joined (std::move (readers));
                int samples[8] {};
                int* channels[1] { samples };
                expect (! joined.readSamples (channels, 1, 0, 0, 8));
            }

            beginTest ("unload() fully empties the player (new session can't inherit it)");
            {
                AudioEngine eng;
                expect (eng.loadSession (dir) >= 1);
                auto& player = eng.getPlayer();
                expect (player.isLoaded(), "should be loaded after loadSession");
                expect (player.getTotalLengthSamples() > 0);

                player.unload();
                expect (! player.isLoaded(), "player still loaded after unload");
                expectEquals ((int) player.getTotalLengthSamples(), 0,
                              "length should reset to 0 on unload");
                // A reload of an EMPTY dir must report no tracks (the new-session
                // case): proves the old session didn't linger.
                auto empty = dir.getParentDirectory().getChildFile ("zf-empty-" + juce::Uuid().toString());
                empty.getChildFile ("Audio Files").createDirectory();
                expectEquals (eng.loadSession (empty), 0, "empty session should load 0 tracks");
                expect (! player.isLoaded());
                empty.deleteRecursively();
            }

            dir.deleteRecursively();
        }
    };

    static MultiPartPlaybackTests multiPartPlaybackTests;
}
