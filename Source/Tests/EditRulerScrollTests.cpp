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
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <CoreFoundation/CoreFoundation.h>

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
            denseMarkersRenameTheOneClicked();
            AudioEngine::setTestModeSkipAudioInit (false);
        }

        static juce::Component* topModal() { return juce::ModalComponentManager::getInstance()->getModalComponent (0); }
        static int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }
        static void pump (int ms)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + ms;
            while (juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
        }

        // Which marker a double-click at x on the marker strip offers to rename.
        static juce::String renamedAt (EditTimeRuler& ruler, int x)
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent e { juce::Desktop::getInstance().getMainMouseSource(), juce::Point<float> ((float) x, 10.0f),
                                       juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                       juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                       juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                       juce::MouseInputSource::defaultTiltY, &ruler, &ruler, now,
                                       juce::Point<float> ((float) x, 10.0f), now, 2, false };
            ruler.mouseDoubleClick (e);
            juce::String name;
            if (auto* aw = dynamic_cast<juce::AlertWindow*> (topModal()))
            {
                name = aw->getTextEditorContents ("n");
                aw->exitModalState (0);
            }
            for (int i = 0; i < 400 && modalCount() > 0; ++i) pump (5);
            return name;
        }

        void denseMarkersRenameTheOneClicked()
        {
            beginTest ("Double-clicking a marker flag renames that marker, however close the others sit");
            {
                const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("zf-ruler-" + juce::Uuid().toString());
                const auto take = dir.getChildFile ("Audio Files/Track_01.wav");
                take.getParentDirectory().createDirectory();
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
                        take.createOutputStream().release(), 48000.0, 1, 16, {}, 0));
                    juce::AudioBuffer<float> silence (1, 48000 * 10);
                    silence.clear();
                    expect (writer != nullptr && writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples()),
                            "setup: the take");
                }
                {
                    AudioEngine engine;
                    engine.clearAllStripOverrides();
                    engine.prepareForTests (48000.0, 512);
                    engine.setStripCount (1);
                    engine.setActiveSessionDir (dir);
                    engine.claimMixerFor (dir);
                    engine.loadSession (dir);
                    engine.setStripCount (1);
                    expect (engine.getPlayer().getTotalLengthSamples() == 48000 * 10, "setup: a ten-second session");

                    // Twelve markers 20 px apart: 100 px per second on a 1000 px lane.
                    EditTimeRuler ruler (engine);
                    ruler.setHeaderWidth (380);
                    ruler.setContentWidth (380 + 1000 + 2 * EditTimeRuler::kWaveInset);
                    ruler.setBounds (0, 0, 1400, 46);
                    auto& markers = engine.getMarkers();
                    markers.replaceAll ({});
                    for (int i = 0; i < 12; ++i)
                        markers.drop ((juce::int64) (48000.0 * (1.0 + 0.2 * i)), "M" + juce::String (i + 1));
                    const double pps = ruler.rulerPxPerSec (10.0);

                    int right = 0;
                    for (int i = 0; i < 12; ++i)
                    {
                        const int x = ruler.rulerTimeToX (1.0 + 0.2 * i, pps);
                        if (renamedAt (ruler, x) == "M" + juce::String (i + 1)) ++right;
                    }
                    expectEquals (right, 12, "a flag's double-click renamed another marker");

                    // Between two flags, on the name the later marker paints on top.
                    const int third = ruler.rulerTimeToX (1.4, pps);
                    expectEquals (renamedAt (ruler, third + 10), juce::String ("M3"),
                                  "a click on a name renamed a marker painted under it");
                    const int last = ruler.rulerTimeToX (1.0 + 0.2 * 11, pps);
                    expectEquals (renamedAt (ruler, last + 60), juce::String ("M12"),
                                  "a click on the last marker's name renamed an earlier one");
                    expectEquals (renamedAt (ruler, last + 200), juce::String(),
                                  "a click past every name renamed something");
                    markers.replaceAll ({});
                }
                dir.deleteRecursively();
            }
        }
    };

    static EditRulerScrollTests editRulerScrollTestsInstance;
}
