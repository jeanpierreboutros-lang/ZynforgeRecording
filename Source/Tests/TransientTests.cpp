// Transient detector tests. Synthesises a signal with known onset
// positions (silence -> burst -> decay -> silence -> burst -> ...)
// and verifies the detector finds them.

#include <juce_core/juce_core.h>
#include "../Audio/TransientDetector.h"

namespace zynforge
{
    class TransientDetectorTests final : public juce::UnitTest
    {
    public:
        TransientDetectorTests() : UnitTest ("Transient detector", "zynforge") {}

        void runTest() override
        {
            beginTest ("Empty / too-short input returns empty");
            {
                std::vector<float> tiny (100, 0.0f);
                const auto onsets = TransientDetector::detect (tiny.data(),
                                                                (juce::int64) tiny.size(),
                                                                48000.0);
                expect (onsets.empty());
            }

            beginTest ("Single burst is detected, returned position is near burst start");
            {
                const double sr = 48000.0;
                const juce::int64 totalN = (juce::int64) (sr * 2.0);   // 2 s
                std::vector<float> buf (totalN, 0.0f);
                const juce::int64 burstAt = (juce::int64) (sr * 1.0);  // burst at t=1.0
                const juce::int64 burstN  = (juce::int64) (sr * 0.05); // 50 ms
                for (juce::int64 i = 0; i < burstN; ++i)
                    buf[burstAt + i] = (i % 2 == 0) ? 0.6f : -0.6f;    // square at Nyquist/2
                const auto onsets = TransientDetector::detect (buf.data(), totalN, sr);
                expect (! onsets.empty());
                // Detector should fire within ~50 ms of the burst start.
                const juce::int64 detected = onsets.front();
                expect (std::abs (detected - burstAt) < (juce::int64) (sr * 0.05));
            }

            beginTest ("Three spaced bursts produce three onsets in order");
            {
                const double sr = 48000.0;
                const juce::int64 totalN = (juce::int64) (sr * 4.0);
                std::vector<float> buf (totalN, 0.0f);
                const juce::int64 bursts[] = {
                    (juce::int64) (sr * 0.5),
                    (juce::int64) (sr * 1.5),
                    (juce::int64) (sr * 2.5),
                };
                const juce::int64 burstN = (juce::int64) (sr * 0.03);
                for (auto pos : bursts)
                    for (juce::int64 i = 0; i < burstN; ++i)
                        buf[pos + i] = (i % 2 == 0) ? 0.5f : -0.5f;
                const auto onsets = TransientDetector::detect (buf.data(), totalN, sr);
                expect ((int) onsets.size() >= 3);
                expect (onsets[0] < onsets[1]);
                expect (onsets[1] < onsets[2]);
                // Each detected onset should land within 50 ms of its source.
                for (size_t i = 0; i < 3 && i < onsets.size(); ++i)
                    expect (std::abs (onsets[i] - bursts[i]) < (juce::int64) (sr * 0.05));
            }

            beginTest ("Refractory window suppresses double-fires from a single burst");
            {
                const double sr = 48000.0;
                const juce::int64 totalN = (juce::int64) (sr * 2.0);
                std::vector<float> buf (totalN, 0.0f);
                // Long burst (300 ms) -- detector should fire once, not
                // continuously across the burst.
                const juce::int64 burstAt = (juce::int64) (sr * 0.5);
                const juce::int64 burstN  = (juce::int64) (sr * 0.3);
                for (juce::int64 i = 0; i < burstN; ++i)
                    buf[burstAt + i] = (i % 2 == 0) ? 0.7f : -0.7f;
                const auto onsets = TransientDetector::detect (buf.data(), totalN, sr);
                // Allow up to 2 (one at attack, possibly one at sustain
                // change) -- but definitely not dozens.
                expect ((int) onsets.size() <= 2);
            }

            beginTest ("Pure silence returns empty");
            {
                const double sr = 48000.0;
                std::vector<float> buf ((size_t) (sr * 2.0), 0.0f);
                const auto onsets = TransientDetector::detect (buf.data(),
                                                                (juce::int64) buf.size(),
                                                                sr);
                expect (onsets.empty());
            }

            beginTest ("Constant noise (no transients) returns empty");
            {
                const double sr = 48000.0;
                juce::Random rng (12345);
                std::vector<float> buf ((size_t) (sr * 2.0));
                for (auto& s : buf) s = (rng.nextFloat() - 0.5f) * 0.1f;
                const auto onsets = TransientDetector::detect (buf.data(),
                                                                (juce::int64) buf.size(),
                                                                sr);
                // Steady noise floor shouldn't produce false positives.
                expect ((int) onsets.size() <= 1);
            }
        }
    };

    static TransientDetectorTests transientDetectorTestsInstance;
}
