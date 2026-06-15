// Integration test: the live-wave overview ring (the data the EDIT lane drains
// to draw the waveform AS it records) must receive data during a CONTINUE pass,
// not only during a fresh take. Drives the REAL MultitrackRecorder through
// fresh-record -> stop -> armContinue -> record, draining track 0's live-wave
// ring on BOTH passes. If the continue pass drains zero columns, the "no live
// waveform while continuing" bug is in the recorder; if it drains data, the bug
// is purely in the EDIT UI. Locks the recorder side either way.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/MultitrackRecorder.h"

namespace zynforge
{
    class LiveWaveContinueTests final : public juce::UnitTest
    {
    public:
        LiveWaveContinueTests() : juce::UnitTest ("Live wave continue", "zynforge") {}

        // Push `numBlocks` of DC `value` through an armed 1-track recorder and
        // return how many live-wave columns track 0 emitted across the pass.
        static int countLiveColumns (MultitrackRecorder& rec, float value,
                                     int numBlocks, int block)
        {
            std::vector<float> buf ((size_t) block, value);
            const float* ptr = buf.data();
            int columns = 0;
            for (int b = 0; b < numBlocks; ++b)
            {
                rec.processBlock (&ptr, 1, block);
                // Drain like the EDIT timer does, once per "tick" (== block here).
                rec.getTrack (0).liveWaveDrain ([&] (float, float) { ++columns; });
            }
            return columns;
        }

        void runTest() override
        {
            const double sr = 48000.0;
            const int    block = 512;
            // ~5 s base so the continue's prefill / base is realistic.
            const int baseBlocks = 470;     // 470*512 = 240640 ~ 5.01 s
            const int contBlocks = 200;     // 200*512 = 102400 ~ 2.13 s

            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-livecont-" + juce::Uuid().toString());
            dir.deleteRecursively();
            auto track = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");

            int freshColumns = 0, contColumns = 0;
            juce::int64 contBase = 0;

            beginTest ("fresh take emits live-wave columns");
            {
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
                rec.startRecording (dir);
                freshColumns = countLiveColumns (rec, 0.5f, baseBlocks, block);
                rec.stopRecording();
                expect (track.existsAsFile(), "fresh take not written");
                // ~ baseBlocks*block / kLiveBinSamples columns expected.
                expect (freshColumns > 0, "fresh take emitted NO live-wave columns");
            }

            beginTest ("CONTINUE pass also emits live-wave columns (same as fresh)");
            {
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
                rec.armContinue (0);   // base is recomputed from files on start
                rec.startRecording (dir);
                contBase = rec.getRecordBaseSamples();
                contColumns = countLiveColumns (rec, -0.3f, contBlocks, block);
                rec.stopRecording();

                expect (contBase > 0, "continue base is 0 -- recorder didn't see the existing take");
                expect (contColumns > 0,
                        "CONTINUE emitted NO live-wave columns -- the live waveform "
                        "can't build while continuing (recorder-side bug)");
                // The column count should scale with samples captured, like fresh.
                const double perBlockFresh = (double) freshColumns / (double) baseBlocks;
                const double perBlockCont  = (double) contColumns  / (double) contBlocks;
                expectWithinAbsoluteError (perBlockCont, perBlockFresh, 0.25,
                    "continue emits live columns at a different rate than fresh");
            }

            dir.deleteRecursively();
        }
    };

    static LiveWaveContinueTests liveWaveContinueTests;
}
