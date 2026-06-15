// Unit tests for the live recording waveform overview (TrackState live-wave
// ring) -- the lock-free SPSC the audio thread fills with min/max bins so the
// EDIT lane can draw the detailed waveform AS it records. Verifies binning,
// drain order, emptiness after drain, and reset.

#include <juce_core/juce_core.h>

#include "../Audio/TrackState.h"

namespace zynforge
{
    class LiveWaveTests final : public juce::UnitTest
    {
    public:
        LiveWaveTests() : juce::UnitTest ("Live wave overview", "zynforge") {}

        void runTest() override
        {
            const int bin = TrackState::kLiveBinSamples;

            beginTest ("bins min/max per kLiveBinSamples and drains in order");
            {
                auto t = std::make_unique<TrackState>();
                // Bin 0: a ramp -0.5 .. +0.5. Bin 1: constant +0.2.
                std::vector<float> data ((size_t) bin * 2, 0.0f);
                for (int i = 0; i < bin; ++i) data[(size_t) i] = -0.5f + (float) i / (float) (bin - 1);
                for (int i = 0; i < bin; ++i) data[(size_t) (bin + i)] = 0.2f;

                t->liveWavePush (data.data(), bin * 2);

                std::vector<std::pair<float, float>> pairs;
                t->liveWaveDrain ([&] (float mn, float mx) { pairs.emplace_back (mn, mx); });

                expectEquals ((int) pairs.size(), 2, "should emit one pair per bin");
                expectWithinAbsoluteError (pairs[0].first,  -0.5f, 0.01f);
                expectWithinAbsoluteError (pairs[0].second,  0.5f, 0.01f);
                expectWithinAbsoluteError (pairs[1].first,   0.2f, 0.01f);
                expectWithinAbsoluteError (pairs[1].second,  0.2f, 0.01f);

                // A partial bin (bin-1 samples) emits nothing yet.
                int more = 0;
                t->liveWavePush (data.data(), bin - 1);
                t->liveWaveDrain ([&] (float, float) { ++more; });
                expectEquals (more, 0, "a partial bin should not emit");
            }

            beginTest ("drained ring is empty; reset clears a pending bin");
            {
                auto t = std::make_unique<TrackState>();
                std::vector<float> data ((size_t) bin, 0.3f);
                t->liveWavePush (data.data(), bin);          // one full bin queued
                int n = 0;
                t->liveWaveDrain ([&] (float, float) { ++n; });
                expectEquals (n, 1);
                int again = 0;
                t->liveWaveDrain ([&] (float, float) { ++again; });
                expectEquals (again, 0, "already drained");

                // reset() drops a queued bin + the partial accumulator.
                t->liveWavePush (data.data(), bin);
                t->liveWaveReset();
                int after = 0;
                t->liveWaveDrain ([&] (float, float) { ++after; });
                expectEquals (after, 0, "reset should empty the ring");
            }
        }
    };

    static LiveWaveTests liveWaveTests;
}
