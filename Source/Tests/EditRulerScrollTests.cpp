// Regression lock for the EDIT time-ruler ↔ wave-lane alignment under
// horizontal scroll. The ruler is a FIXED-width sibling above the scrolling
// wave lanes; when zoomed in and scrolled it must subtract the viewport's
// horizontal scroll (viewport.getViewPositionX()) from its time→x mapping, or
// every tick / label / playhead / marker drifts right of the audio it labels by
// exactly the scroll amount. That drift shipped once ("the ruler bug") — this
// test pins the maths so it can't come back:
//   1. a non-zero scroll shifts the forward map left by exactly that many px,
//   2. rulerXToTime stays the inverse of rulerTimeToX at any scroll (so marker
//      hit-testing + the shift-drag punch range stay correct),
//   3. the ruler playhead lands on the SAME screen-x as the lane playhead
//      (which EditPage computes as headerW + (int)(frac*(waveW-8)) + 4, drawn
//      inside the scrolled row, i.e. minus scrollX).
//
// EditTimeRuler needs an AudioEngine; built in test mode (no device). Its idle
// timer never ticks without a running message loop, so construction is cheap.

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

#include "../UI/EditTimeRuler.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class EditRulerScrollTests final : public juce::UnitTest
    {
    public:
        EditRulerScrollTests() : juce::UnitTest ("EDIT ruler scroll align", "zynforge") {}

        // Mirror EXACTLY what EditPage::refresh computes for the lane playhead,
        // then where it lands on screen once the row is scrolled left.
        static int lanePlayheadScreenX (juce::int64 pos, juce::int64 total,
                                        int contentW, int headerW, int scrollX)
        {
            const int    wavePaneWidth = juce::jmax (1, contentW - headerW);
            const double frac          = (double) pos / (double) total;
            const int    playheadX     = (int) (frac * (wavePaneWidth - 8)) + 4;
            return headerW + playheadX - scrollX;   // row is scrolled, header pinned
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            {
                AudioEngine engine;
                EditTimeRuler ruler (engine);

                const int    headerW  = 380;
                const int    contentW = 4000;   // zoomed: wider than any viewport
                const double totalSec = 100.0;
                ruler.setHeaderWidth (headerW);
                ruler.setContentWidth (contentW);
                const double pps = ruler.rulerPxPerSec (totalSec);

                beginTest ("scroll shifts the forward map left by exactly the offset");
                {
                    ruler.setScrollOffsetX (0);
                    const int x0 = ruler.rulerTimeToX (50.0, pps);
                    ruler.setScrollOffsetX (537);
                    const int xS = ruler.rulerTimeToX (50.0, pps);
                    expectEquals (x0 - xS, 537,
                                  "a 537 px scroll must move the tick 537 px left");
                }

                beginTest ("rulerXToTime is the inverse of rulerTimeToX at any scroll");
                {
                    for (int scroll : { 0, 200, 1500, 3000 })
                    {
                        ruler.setScrollOffsetX (scroll);
                        for (double sec : { 0.0, 12.5, 49.0, 73.2, 99.0 })
                        {
                            const int    x    = ruler.rulerTimeToX (sec, pps);
                            const double back = ruler.rulerXToTime (x, pps);
                            // Within one pixel's worth of time (the forward map
                            // truncates to int px).
                            expect (std::abs (back - sec) <= (1.0 / pps) + 1.0e-6,
                                    "round-trip drifted at scroll " + juce::String (scroll)
                                        + ", sec " + juce::String (sec));
                        }
                    }
                }

                beginTest ("ruler playhead lands on the same screen-x as the lane playhead");
                {
                    const double sr    = 48000.0;
                    const auto   total = (juce::int64) (totalSec * sr);
                    for (int scrollX : { 0, 250, 612, 1800 })
                    {
                        ruler.setScrollOffsetX (scrollX);
                        for (juce::int64 pos : { (juce::int64) 0, total / 4,
                                                 total / 2, total * 9 / 10, total })
                        {
                            const int rulerX = ruler.rulerTimeToX ((double) pos / sr, pps);
                            const int laneX  = lanePlayheadScreenX (pos, total, contentW,
                                                                    headerW, scrollX);
                            expect (std::abs (rulerX - laneX) <= 1,
                                    "ruler/lane playhead misaligned by "
                                        + juce::String (rulerX - laneX)
                                        + " px at scroll " + juce::String (scrollX));
                        }
                    }
                }
            }
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static EditRulerScrollTests editRulerScrollTestsInstance;
}
