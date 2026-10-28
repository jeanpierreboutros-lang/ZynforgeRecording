// Strip reorder: whole logical strips, ONE on-disk transaction per drag, and
// views that rebuild when the stereo layout moves.
//
// The EDIT swatch drag used to call AudioEngine::swapTracks for every row the
// pointer crossed. Each call is a full reorderTracks() transaction (stop the
// player, detach the audio callback, rename every take file, rewrite the
// session documents) run on the message thread, and a stereo strip's second
// swap ignored the first one's failure. The drag now only previews; release
// commits one reorder built by Source/Audio/TrackOrder.h.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/TrackOrder.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"

#include <algorithm>

namespace zynforge
{
    class TrackReorderTests final : public juce::UnitTest
    {
    public:
        TrackReorderTests() : juce::UnitTest ("Strip reorder", "zynforge") {}

        struct Host
        {
            AudioEngine engine;
            int reorders { 0 };

            Host (int strips, std::initializer_list<const char*> names)
            {
                engine.clearAllStripOverrides();
                engine.prepareForTests (48000.0, 512);
                engine.setStripCount (strips);
                int i = 0;
                for (auto* n : names) engine.setTrackName (i++, n);
                engine.onTracksReordered = [this] (const std::vector<int>&) { ++reorders; };
            }
            ~Host() { engine.clearAllStripOverrides(); }

            juce::String nameOf (int i) { return engine.getRecorder().getTrack (i).getNameThreadSafe(); }
            bool stereoAt (int i)       { return engine.getRecorder().getTrack (i).isStereo.load(); }
        };

        // The page's TrackRows, top to bottom.
        static std::vector<EditPage::TrackRow*> rowsOf (EditPage& page)
        {
            std::vector<EditPage::TrackRow*> rows;
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                for (int i = 0; i < c.getNumChildComponents(); ++i)
                {
                    auto* child = c.getChildComponent (i);
                    if (auto* row = dynamic_cast<EditPage::TrackRow*> (child)) rows.push_back (row);
                    walk (*child);
                }
            };
            walk (page);
            std::sort (rows.begin(), rows.end(), [&page] (auto* a, auto* b)
            { return page.getLocalPoint (a, juce::Point<int>()).y < page.getLocalPoint (b, juce::Point<int>()).y; });
            return rows;
        }

        static juce::Rectangle<int> areaInPage (EditPage& page, juce::Component& row)
        {
            return page.getLocalArea (&row, row.getLocalBounds());
        }

        // A left-button event at `p` (component-local), for driving a row's
        // own mouse handlers directly.
        static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<int> p,
                                         juce::Point<int> down, bool dragged)
        {
            const auto now = juce::Time::getCurrentTime();
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                     juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, down.toFloat(), now, 1, dragged };
        }

        void runTest() override
        {
            using namespace trackorder;

            beginTest ("Logical blocks keep stereo pairs whole");
            {
                // [0] [1,2] [3] [4] -- a trailing L flag with no partner is mono.
                const auto b = logicalBlocks ({ false, true, false, false, true });
                expectEquals ((int) b.size(), 4);
                expect (b[1] == std::vector<int> { 1, 2 });
                expect (b[3] == std::vector<int> { 4 });
                expectEquals (blockStartingAt (b, 2), -1, "an R half starts no block");
                expectEquals (blockStartingAt (b, 3), 2);
            }

            beginTest ("moveBlock moves whole strips and rejects no-ops");
            {
                const auto b = logicalBlocks ({ false, true, false, false });   // [0] [1,2] [3]
                expect (moveBlock (b, 0, 3) == std::vector<int> { 1, 2, 3, 0 }, "mono to the end");
                expect (moveBlock (b, 1, 0) == std::vector<int> { 1, 2, 0, 3 }, "pair to the top");
                expect (moveBlock (b, 2, 1) == std::vector<int> { 0, 3, 1, 2 }, "mono above the pair");
                expect (moveBlock (b, 1, 1).empty(), "in front of itself");
                expect (moveBlock (b, 1, 2).empty(), "in front of its own successor");
                expect (moveBlock (b, 3, 0).empty(), "source out of range");
                expect (moveBlock (b, 0, 4).empty(), "target out of range");
            }

            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("A drag previews without touching the engine, then commits ONE reorder");
            {
                Host h (4, { "A", "B", "C", "D" });
                h.engine.setTrackStereo (1, true);   // rows: A | B+C | D
                EditPage page (h.engine);
                page.setBounds (0, 0, 900, 600);
                page.refresh();
                auto rows = rowsOf (page);
                expectEquals ((int) rows.size(), 3);

                const auto pair = areaInPage (page, *rows[1]);
                expectEquals (page.updateReorderPreview ({ 40, pair.getY() + 2 }), 1,
                              "upper half of the pair lands in front of it");
                const auto last = areaInPage (page, *rows.back());
                expectEquals (page.updateReorderPreview ({ 40, last.getBottom() - 2 }), 4,
                              "lower half of the last row lands after it");
                expectEquals (h.reorders, 0, "previewing must not reorder");
                expectEquals (h.nameOf (0), juce::String ("A"));

                expect (page.commitReorder (0, 4));
                expectEquals (h.reorders, 1, "one drag is one transaction");
                expectEquals (h.nameOf (0), juce::String ("B"));
                expectEquals (h.nameOf (1), juce::String ("C"));
                expectEquals (h.nameOf (2), juce::String ("D"));
                expectEquals (h.nameOf (3), juce::String ("A"));
                expect (h.stereoAt (0), "the pair moved as a pair");
                expect (! h.stereoAt (2) && ! h.stereoAt (3));
            }

            beginTest ("The swatch drag reorders once, on release, however many rows it crosses");
            {
                Host h (4, { "A", "B", "C", "D" });
                EditPage page (h.engine);
                page.setBounds (0, 0, 900, 600);
                page.refresh();
                auto rows = rowsOf (page);
                expectEquals ((int) rows.size(), 4);
                auto& row = *rows[0];

                // Drag A's swatch down past every row to the lower half of D.
                const juce::Point<int> down { 4, 10 };
                const auto end = row.getLocalPoint (&page, juce::Point<int> (40, areaInPage (page, *rows.back()).getBottom() - 2));
                row.mouseDown (mouseAt (row, down, down, false));
                for (int y = down.y; y < end.y; y += 7)
                    row.mouseDrag (mouseAt (row, { 4, y }, down, true));
                row.mouseDrag (mouseAt (row, { 4, end.y }, down, true));
                expectEquals (h.reorders, 0, "crossing rows must not reorder before release");
                expectEquals (h.nameOf (0), juce::String ("A"));

                row.mouseUp (mouseAt (row, { 4, end.y }, down, true));
                expectEquals (h.reorders, 1, "release commits exactly one reorder");
                expectEquals (h.nameOf (0), juce::String ("B"));
                expectEquals (h.nameOf (3), juce::String ("A"));
            }

            beginTest ("A move that keeps both counts still rebuilds the EDIT rows");
            {
                // [A, B+C] -> [B+C, A]: three tracks and two rows before AND after.
                Host h (3, { "A", "B", "C" });
                h.engine.setTrackStereo (1, true);
                EditPage page (h.engine);
                page.setBounds (0, 0, 900, 600);
                page.refresh();
                auto before = rowsOf (page);
                expect (before.size() == 2 && before[0]->getTrackIndex() == 0 && ! before[0]->isStereoPair());

                expect (h.engine.reorderTracks ({ 1, 2, 0 }));
                page.refresh();
                auto after = rowsOf (page);
                expectEquals ((int) after.size(), 2);
                expect (after[0]->getTrackIndex() == 0 && after[0]->isStereoPair(),
                        "row 0 must now draw the stereo pair");
                expect (after[1]->getTrackIndex() == 2 && ! after[1]->isStereoPair(),
                        "row 1 must now draw the mono strip at track 2");
            }

            beginTest ("Dropping in place or on a stale layout changes nothing");
            {
                Host h (4, { "A", "B", "C", "D" });
                EditPage page (h.engine);
                page.setBounds (0, 0, 900, 600);
                page.refresh();
                expect (page.commitReorder (1, 1), "in front of itself is a no-op success");
                expect (page.commitReorder (1, 2), "in front of its successor is a no-op success");
                expectEquals (h.reorders, 0);

                // Another surface links 1+2 mid-drag: track 2 is now an R half.
                h.engine.setTrackStereo (1, true);
                expect (! page.commitReorder (2, 0), "a stale source must be refused");
                expectEquals (h.reorders, 0);
                expectEquals (h.nameOf (0), juce::String ("A"));
            }

            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static TrackReorderTests trackReorderTests;
}
