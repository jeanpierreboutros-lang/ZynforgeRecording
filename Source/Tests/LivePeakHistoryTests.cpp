#include <juce_core/juce_core.h>

#include "../UI/LivePeakHistory.h"
#include "../Audio/TrackState.h"

namespace zynforge
{
    class LivePeakHistoryTests final : public juce::UnitTest
    {
    public:
        LivePeakHistoryTests() : juce::UnitTest ("Long-take live waveform", "zynforge") {}

        void runTest() override
        {
            constexpr int bin = TrackState::kLiveBinSamples;

            beginTest ("Three-hour take stays bounded and keeps real sample time");
            {
                LivePeakHistory history (bin);
                const juce::int64 binsInThreeHours = 3LL * 3600 * 48000 / bin;
                for (juce::int64 i = 0; i < binsInThreeHours; ++i)
                    history.append (i == binsInThreeHours - 1 ? 1.0f : 0.25f);

                expectEquals (history.spanSamples(), binsInThreeHours * bin);
                expect (history.size() <= LivePeakHistory::maxPoints);
                expect (history.getBinsPerPoint() < 1024,
                        "resolution must grow with duration, not with each UI overflow");
                expect (history.at (history.size() - 1) >= 0.25f);
            }

            beginTest ("Long continuation prefill is bounded and new peaks follow it");
            {
                LivePeakHistory history (bin);
                const juce::int64 prefill = 3LL * 3600 * 48000 / bin;
                history.reset (prefill);
                expect (history.size() <= LivePeakHistory::maxPoints / 2 + 1);
                expectEquals (history.spanSamples(), prefill * bin);
                for (juce::int64 i = 0; i < history.getBinsPerPoint(); ++i)
                    history.append (0.8f);
                expectEquals (history.spanSamples(), (prefill + history.getBinsPerPoint()) * bin);
                expect (history.at (history.size() - 1) >= 0.8f);
            }
        }
    };

    static LivePeakHistoryTests livePeakHistoryTests;
}
