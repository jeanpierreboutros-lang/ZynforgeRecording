// Regression guards for the 2026-08-12 EDIT-view audit.
//
// The EDIT row is a private nested class inside EditPage, so the fixes that
// live in its mouse handlers (meter condemn, automation-point re-resolve,
// routing-combo fallback, wave-cache ordering) can't be driven headlessly.
// What IS testable is the shared logic those fixes were extracted onto --
// EditTimeline.h -- plus the engine invariant the point-drag fix depends on.
// The rest is smoke-test territory; see CHANGELOG.

#include <juce_core/juce_core.h>

#include "../Audio/AudioEngine.h"
#include "../UI/EditTimeline.h"

namespace zynforge
{
    class EditViewFixTests final : public juce::UnitTest
    {
    public:
        EditViewFixTests() : juce::UnitTest ("EDIT view fixes 2026-08-12", "zynforge") {}

        static Clip clipAt (juce::int64 start, juce::int64 len)
        {
            Clip c;
            c.timelineStartSamples = start;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = len;
            return c;
        }

        void runTest() override
        {
            // ── The empty-session lane span is ONE number ────────────────────
            // The ruler drew 300 s while laneTimelineSamples() returned 60 s and
            // the tempo lane hardcoded 48000*60, so on a session with no audio
            // an automation/tempo point placed under the ruler's 2:30 mark was
            // stored at 0:30. Both now route through this helper.
            beginTest ("Notional empty-lane span is 5 minutes at the DEVICE rate");
            {
                expectEquals (kNotionalEmptyLaneSec, 300.0);
                expectEquals (notionalEmptyLaneSamples (48000.0), (juce::int64) (48000 * 300));
                // Rate-dependent: the tempo lane's old hardcoded 48000 gave 30 s
                // of real time at 96 k, so its points landed at half position.
                expectEquals (notionalEmptyLaneSamples (96000.0), (juce::int64) (96000 * 300));
                expectEquals (notionalEmptyLaneSamples (44100.0), (juce::int64) (44100 * 300));
                // A zero / bogus device rate must not collapse the lane to 0
                // samples (which would make every hit-test map to sample 0).
                expect (notionalEmptyLaneSamples (0.0) > 0, "must not collapse to a zero-length lane");
            }

            // ── Edit-group broadcasts resolve the PEER's own clip ────────────
            // Clip indices are per-track. Broadcasting this row's index edited
            // whichever clip sat at that position on the peer.
            beginTest ("clipIndexAtMidpoint maps by timeline overlap, not index");
            {
                // Peer split into three; source is one clip covering [1000,2000).
                const std::vector<Clip> peer {
                    clipAt (0,    1000),
                    clipAt (1000, 1000),
                    clipAt (2000, 1000),
                };
                // Source clip 0 spans [1000,2000) -> peer clip 1, NOT peer clip 0.
                expectEquals (clipIndexAtMidpoint (peer, 1000, 1000), 1,
                              "must follow the timeline, not reuse the source index");
                expectEquals (clipIndexAtMidpoint (peer, 0,    1000), 0);
                expectEquals (clipIndexAtMidpoint (peer, 2000, 1000), 2);

                // Identical arrangements: index mapping is the identity, so the
                // normal grouped-pair case is unchanged.
                const std::vector<Clip> same { clipAt (0, 500), clipAt (500, 500) };
                expectEquals (clipIndexAtMidpoint (same, 0,   500), 0);
                expectEquals (clipIndexAtMidpoint (same, 500, 500), 1);

                // Peer has nothing under the source clip -> -1 so the caller
                // SKIPS that peer instead of editing an arbitrary clip.
                const std::vector<Clip> gap { clipAt (0, 100) };
                expectEquals (clipIndexAtMidpoint (gap, 5000, 1000), -1);
                expectEquals (clipIndexAtMidpoint ({},   0,    1000), -1);
                // Degenerate source clip.
                expectEquals (clipIndexAtMidpoint (gap, 0, 0), -1);
            }

            // ── The invariant behind the point-drag fix ──────────────────────
            // Dragging a point re-adds it at the new position; the lane is kept
            // SORTED, so the dragged point's index changes the moment it crosses
            // a neighbour. The handler used to keep the stale index and so
            // deleted/dragged the neighbour on the next event. This locks the
            // sort invariant that makes re-resolution necessary, and that
            // nearest-by-sample finds the moved point.
            beginTest ("Automation lane stays sorted, so a dragged point's index moves");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine eng;
                eng.prepareForTests (48000.0, 512);
                using AP = AudioEngine::AutomationParam;

                // Space the points WELL past addPointLocked's 4096-sample merge
                // window (it replaces rather than appends inside that), or all
                // three collapse into one and the test proves nothing.
                constexpr juce::int64 pA = 48000, pB = 96000, pC = 144000;
                constexpr juce::int64 dragTo = 120000;   // between pB and pC

                eng.clearAutomation (AP::Volume);
                eng.addAutomationPoint (0, AP::Volume, pA, -6.0f);
                eng.addAutomationPoint (0, AP::Volume, pB, -3.0f);
                eng.addAutomationPoint (0, AP::Volume, pC,  0.0f);

                {
                    const auto& lane = eng.getAutomation (0, AP::Volume);
                    expectEquals ((int) lane.size(), 3, "test setup: points must not merge");
                    expectEquals (lane[0].samplePos, pA);
                    expectEquals (lane[2].samplePos, pC);
                }

                // Drag point 0 (at pA) to dragTo -- past its neighbour at pB.
                eng.removeAutomationPointNear (0, AP::Volume, pA, 1);
                eng.addAutomationPoint        (0, AP::Volume, dragTo, -6.0f);

                const auto& after = eng.getAutomation (0, AP::Volume);
                expectEquals ((int) after.size(), 3, "the drag must not add or drop points");
                // Sorted => the dragged point is now index 1, not 0. Holding the
                // stale index 0 would have made the NEXT drag event delete the
                // point at pB instead -- the "dragging one point eats its
                // neighbours" bug.
                expectEquals (after[0].samplePos, pB);
                expectEquals (after[1].samplePos, dragTo);
                expectEquals (after[2].samplePos, pC);

                // Nearest-by-sample (what the handler now does) finds it.
                int nearest = -1;
                juce::int64 best = std::numeric_limits<juce::int64>::max();
                for (int i = 0; i < (int) after.size(); ++i)
                {
                    const auto d = std::abs (after[(size_t) i].samplePos - dragTo);
                    if (d < best) { best = d; nearest = i; }
                }
                expectEquals (nearest, 1, "re-resolution must land on the dragged point");
                expectWithinAbsoluteError (after[(size_t) nearest].value, -6.0f, 0.001f);
            }
        }
    };

    static EditViewFixTests editViewFixTests;
}
