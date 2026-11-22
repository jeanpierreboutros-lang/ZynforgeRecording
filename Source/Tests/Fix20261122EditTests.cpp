// The 2026-11-22 EDIT audit: a click that slid a clip, clips a press could hit
// but not see after STOP, fade presets that did nothing, a crossfade handle on
// a clip pasted inside another, and a minimap box a header's width off.
// Driven through TrackRow's real mouse handlers and paint, headlessly (the
// fixtures are EditRowFixTests').

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../UI/EditPage.h"
#include "../UI/EditTimeline.h"
#include "../UI/EditTrackRow.h"
#include "../UI/TimelineMinimap.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace zynforge
{
    class Fix20261122EditTests final : public juce::UnitTest
    {
    public:
        Fix20261122EditTests() : juce::UnitTest ("Fixes 2026-11-22 EDIT", "zynforge") {}

        static constexpr int kRate = 48000;
        using Row = EditPage::TrackRow;

        static juce::File tempDir (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-fix1122-" + tag + "-" + juce::Uuid().toString());
        }

        static bool writeWav (const juce::File& file, int samples, int channels = 1)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> os (file.createOutputStream().release());
            if (os == nullptr) return false;
            auto writer = format.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                          .withSampleRate ((double) kRate)
                                                          .withNumChannels (channels)
                                                          .withBitsPerSample (24));
            if (writer == nullptr) return false;
            juce::AudioBuffer<float> audio (channels, samples);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < samples; ++i)
                    audio.setSample (c, i, 0.8f * std::sin ((float) i * 0.05f));
            return writer->writeFromAudioSampleBuffer (audio, 0, samples);
        }

        static juce::File takeIn (const juce::File& dir, int n)
        {
            return dir.getChildFile ("Audio Files/Track_" + juce::String::formatted ("%02d.wav", n));
        }

        // A session folder with real takes, loaded into a test-mode engine.
        struct Session
        {
            juce::File                dir = tempDir ("lane");
            AudioEngine               engine;
            juce::AudioFormatManager  formats;
            juce::AudioThumbnailCache cache { 8 };

            Session (int strips, std::vector<int> lengths)
            {
                formats.registerBasicFormats();
                engine.clearAllStripOverrides();
                engine.prepareForTests ((double) kRate, 512);
                engine.setStripCount (strips);
                dir.getChildFile ("Audio Files").createDirectory();
                for (size_t i = 0; i < lengths.size(); ++i)
                    if (lengths[i] > 0) writeWav (takeIn (dir, (int) i + 1), lengths[i]);
                engine.setActiveSessionDir (dir);
                engine.claimMixerFor (dir);
                engine.loadSession (dir);
                engine.setStripCount (strips);
            }
            ~Session()
            {
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
            std::vector<Clip>& clips (int track) { return engine.clipsFor (track); }
        };

        // A 1000 px inner lane starting 4 px right of the header: 48 samples/px
        // over a one-second take.
        static constexpr int kLaneW = 1000;
        static void sizeRow (Row& row, int height = 100) { row.setSize (row.getHeaderWidth() + kLaneW + 8, height); }
        static int laneX (const Row& row, double frac) { return row.getHeaderWidth() + 4 + (int) std::lround (frac * kLaneW); }

        static Clip clipAt (juce::int64 start, juce::int64 length)
        {
            Clip c;
            c.timelineStartSamples = start;
            c.fileLengthSamples    = length;
            return c;
        }

        static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<int> p, juce::Point<int> down, bool dragged,
                                         int buttons = juce::ModifierKeys::leftButtonModifier)
        {
            const auto now = juce::Time::getCurrentTime();
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), juce::ModifierKeys (buttons),
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, down.toFloat(), now, 1, dragged };
        }

        // Press at `from`, move one pixel at a time to `to`, release there:
        // JUCE calls mouseDrag on every pixel of hand movement.
        static void dragByPixels (juce::Component& c, juce::Point<int> from, juce::Point<int> to)
        {
            c.mouseDown (mouseAt (c, from, from, false));
            const int steps = juce::jmax (std::abs (to.x - from.x), std::abs (to.y - from.y));
            for (int s = 1; s <= steps; ++s)
                c.mouseDrag (mouseAt (c, from + (to - from) * s / steps, from, true));
            c.mouseUp (mouseAt (c, to, from, steps > 0));
        }

        static std::vector<Row*> rowsOf (EditPage& page)
        {
            std::vector<Row*> rows;
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (auto* row = dynamic_cast<Row*> (child)) rows.push_back (row);
                    walk (*child);
                }
            };
            walk (page);
            std::sort (rows.begin(), rows.end(), [] (auto* a, auto* b) { return a->getY() < b->getY(); });
            return rows;
        }

        static juce::Image render (Row& row)
        {
            juce::Image image (juce::Image::ARGB, row.getWidth(), row.getHeight(), true);
            juce::Graphics g (image);
            row.paint (g);
            return image;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };

            aClickIsAClick();
            heldRowsShowWhatAPressHits();
            fadePresetsFitEveryPeer();
            enclosedClipsHaveNoCrossfade();
            minimapBoxIsTheVisibleLane();
        }

        // ── 1: a click with a little hand movement selects and edits nothing ──
        void aClickIsAClick()
        {
            beginTest ("A click that moves a few pixels selects the clip and moves, trims or rides nothing");
            {
                Session s (1, { kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                expectEquals ((int) rows.size(), 1, "setup: one row");
                if (rows.size() == 1)
                {
                    auto& row = *rows[0];
                    const int lane0 = row.getHeaderWidth() + 4;
                    const int laneW = row.getWidth() - row.getHeaderWidth() - 8;
                    expect (laneW > 200, "setup: a lane to click on");
                    const auto& clip = s.clips (0)[0];

                    const juce::Point<int> moves[] { { 1, 0 }, { 2, 0 }, { -3, 0 }, { 4, 0 }, { 3, 3 }, { 0, 4 } };
                    const juce::Point<int> body (lane0 + laneW / 2, row.getHeight() / 2 - 8);
                    for (const auto move : moves)
                    {
                        page.clearSelectedClip();
                        dragByPixels (row, body, body + move);
                        expectEquals (clip.timelineStartSamples, (juce::int64) 0,
                                      "a click moving " + move.toString() + " px slid the clip");
                        expectEquals (clip.fileLengthSamples, (juce::int64) kRate, "the click trimmed the clip");
                        expect (page.getSelectedClips().count ({ 0, 0 }) == 1,
                                "a click moving " + move.toString() + " px did not select the clip");
                    }

                    // Near the right edge the press arms a trim.
                    const juce::Point<int> edge (lane0 + laneW - 2, row.getHeight() / 2 - 8);
                    page.clearSelectedClip();
                    dragByPixels (row, edge, edge + juce::Point<int> (-3, 0));
                    expectEquals (clip.fileLengthSamples, (juce::int64) kRate, "a click at the edge trimmed the clip");
                    expect (page.getSelectedClips().count ({ 0, 0 }) == 1, "a click at the edge did not select the clip");

                    // On the clip-gain handle (bottom-left of the block) the press arms a ride.
                    const juce::Point<int> handle (lane0 + 30, row.getHeight() - 22);
                    dragByPixels (row, handle, handle + juce::Point<int> (0, -3));
                    expectWithinAbsoluteError (clip.gainDb, 0.0f, 0.0001f, "a click on the gain handle rode the gain");
                }
            }

            beginTest ("A real drag still moves and trims from the press point");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto& clip = s.clips (0)[0];

                const juce::Point<int> body (laneX (row, 0.5), 40);
                row.mouseDown (mouseAt (row, body, body, false));
                for (int dx = 1; dx < Row::kClickSlopPx; ++dx)
                    row.mouseDrag (mouseAt (row, body + juce::Point<int> (dx, 0), body, true));
                expectEquals (clip.timelineStartSamples, (juce::int64) 0, "the clip moved inside the click slop");
                row.mouseDrag (mouseAt (row, body + juce::Point<int> (Row::kClickSlopPx, 0), body, true));
                expectEquals (clip.timelineStartSamples, (juce::int64) (Row::kClickSlopPx * 48),
                              "leaving the slop did not move the clip by the pointer's distance from the press");
                for (int dx = Row::kClickSlopPx + 1; dx <= 200; ++dx)
                    row.mouseDrag (mouseAt (row, body + juce::Point<int> (dx, 0), body, true));
                row.mouseUp (mouseAt (row, body + juce::Point<int> (200, 0), body, true));
                expectEquals (clip.timelineStartSamples, (juce::int64) (200 * 48), "the drag did not follow the mouse");
            }
            {
                // A trim of the right edge, pixel by pixel.
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto& clip = s.clips (0)[0];
                const int endX = laneX (row, 1.0);
                dragByPixels (row, { endX - 2, 40 }, { endX - 102, 40 });
                expectEquals (clip.fileLengthSamples, (juce::int64) (kRate - 100 * 48),
                              "the trim did not follow the mouse from the press");
                expectEquals (clip.timelineStartSamples, (juce::int64) 0, "the trim moved the clip");
            }

            beginTest ("A Selector click that moves a few pixels places the playhead and selects no range");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                EditToolsBar tools;
                tools.setTool (EditToolsBar::Tool::Selector);
                row.toolsBar = &tools;
                auto& player = s.engine.getPlayer();
                const juce::Point<int> at (laneX (row, 0.25), 50);
                for (int dx = 1; dx < Row::kClickSlopPx; ++dx)
                {
                    dragByPixels (row, at, at + juce::Point<int> (dx, 0));
                    expect (! player.hasLoopRegion(), "a click moving " + juce::String (dx) + " px selected a range");
                    expectEquals (player.getPositionSamples(), (juce::int64) 12000, "the click did not place the playhead");
                }
                dragByPixels (row, at, at + juce::Point<int> (50, 0));
                expect (player.hasLoopRegion(), "a real Selector drag selected no range");
                expectEquals (player.getLoopStart(), (juce::int64) 12000, "the range does not start at the press");
                expectWithinAbsoluteError ((double) player.getLoopEnd(), 12000.0 + 50.0 * 48.0, 1.0,
                                           "the range does not end at the pointer");
                row.toolsBar = nullptr;
            }
        }

        // ── 2: after STOP a held row draws the clips a press can hit ─────────
        static void holdTake (Row& row, int bins, const std::function<float (int)>& level)
        {
            row.setLiveRecording (true);
            for (int i = 0; i < bins; ++i) row.appendLivePeakL (level (i));
            row.holdLiveEnvelopeUntilScanned();
        }

        void heldRowsShowWhatAPressHits()
        {
            constexpr int bins = kRate / TrackState::kLiveBinSamples;
            constexpr int midY = 50;   // the lane's centre on a 100 px row

            beginTest ("A row holding its take after STOP draws its clips' edges over the envelope");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                Row held  (0, false, s.engine, s.formats, s.cache);
                Row plain (0, false, s.engine, s.formats, s.cache);
                sizeRow (held);
                sizeRow (plain);
                holdTake (held, bins, [] (int) { return 0.9f; });
                const auto h = render (held);
                const auto p = render (plain);
                expect (held.isHoldingLiveEnvelope(), "setup: the row holds its take");
                expect (h.getPixelAt (laneX (held, 0.25), midY - 10) != p.getPixelAt (laneX (held, 0.25), midY - 10),
                        "setup: the held envelope is drawn");
                // The seam between the two clips: the second block's 1 px edge,
                // drawn on top of whatever is under it.
                for (int y : { midY - 20, midY, midY + 20 })
                    expect (h.getPixelAt (laneX (held, 0.5), y) == p.getPixelAt (laneX (held, 0.5), y),
                            "the split seam a press can grab is not drawn on the held row");
            }

            beginTest ("A row holding its take after STOP shows which clip is selected");
            {
                Session s (1, { kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                expectEquals ((int) rows.size(), 1, "setup: one row");
                if (rows.size() == 1)
                {
                    auto& row = *rows[0];
                    holdTake (row, bins, [] (int) { return 0.5f; });
                    page.clearSelectedClip();
                    const auto before = render (row);
                    page.setSelectedClip (0, 0);
                    const auto after = render (row);
                    expect (row.isHoldingLiveEnvelope(), "setup: the row holds its take");
                    int differing = 0;
                    for (int x = row.getHeaderWidth(); x < row.getWidth(); ++x)
                        for (int y = 0; y < row.getHeight(); ++y)
                            if (before.getPixelAt (x, y) != after.getPixelAt (x, y)) ++differing;
                    expect (differing > 100, "a selected clip looks unselected while its row holds the take: "
                                                 + juce::String (differing) + " pixels changed");
                }
            }

            beginTest ("An edit made while a row holds its take moves that clip's part of the envelope");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.deleteClip (0, 1), "setup: the second half deleted");
                expect (s.engine.editClip (0, 0, AudioEngine::ClipEdit::Move, 24000), "setup: the first half moved right");
                expectEquals (s.engine.getPlayer().getTotalLengthSamples(), (juce::int64) kRate, "setup: the timeline");
                Row held  (0, false, s.engine, s.formats, s.cache);
                Row plain (0, false, s.engine, s.formats, s.cache);
                sizeRow (held);
                sizeRow (plain);
                // Loud in the first quarter of the take only.
                holdTake (held, bins, [] (int i) { return i < bins / 4 ? 0.9f : 0.0f; });
                const auto h = render (held);
                const auto p = render (plain);
                expect (h.getPixelAt (laneX (held, 0.6), midY - 10) != p.getPixelAt (laneX (held, 0.6), midY - 10),
                        "the moved clip does not draw the take's first quarter where it now plays");
                expect (h.getPixelAt (laneX (held, 0.1), midY - 10) == p.getPixelAt (laneX (held, 0.1), midY - 10),
                        "the envelope still draws where no clip plays");
            }
        }

        // ── 3: a fade preset that does not fit is shortened, equally on peers ──
        void fadePresetsFitEveryPeer()
        {
            beginTest ("A fade preset that does not fit is shortened to fit every peer and says so");
            {
                Session s (2, { kRate, kRate });
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                expect (s.engine.setClipFades (0, 0, 0, 9600) && s.engine.setClipFades (1, 0, 0, 28800),
                        "setup: different fade-outs on the two peers");
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                juce::StringArray said;
                page.onStatus = [&said] (const juce::String& text) { said.add (text); };
                auto rows = rowsOf (page);
                expectEquals ((int) rows.size(), 2, "setup: two rows");
                if (rows.size() == 2)
                {
                    auto& row = *rows[0];
                    const auto fadesAre = [&] (juce::int64 in0, juce::int64 out0, juce::int64 in1, juce::int64 out1,
                                               const juce::String& what)
                    {
                        expectEquals (s.clips (0)[0].fadeInSamples,  in0,  what + ": first fade-in");
                        expectEquals (s.clips (0)[0].fadeOutSamples, out0, what + ": first fade-out");
                        expectEquals (s.clips (1)[0].fadeInSamples,  in1,  what + ": peer fade-in");
                        expectEquals (s.clips (1)[0].fadeOutSamples, out1, what + ": peer fade-out");
                    };

                    row.applyClipMenuChoice (0, 106);   // Fade in 1 s: fits neither
                    fadesAre (19200, 9600, 19200, 28800, "1 s fade in");
                    expect (said.size() == 1 && said[0].contains ("shortened") && said[0].contains ("400 ms"),
                            "the shortened fade was not reported: " + said.joinIntoString (" | "));

                    said.clear();
                    row.applyClipMenuChoice (0, 103);   // Fade in 50 ms: fits both
                    fadesAre (2400, 9600, 2400, 28800, "50 ms fade in");
                    expect (said.isEmpty(), "a fade that fits reported something: " + said.joinIntoString (" | "));

                    row.applyClipMenuChoice (0, 105);   // Fade in 500 ms: fits the first, not the peer
                    fadesAre (19200, 9600, 19200, 28800, "500 ms fade in");
                    expect (said.size() == 1 && said[0].contains ("shortened"), "the shortened fade was not reported");

                    said.clear();
                    expect (s.engine.setClipFades (0, 0, 0, kRate) && s.engine.setClipFades (1, 0, 0, kRate),
                            "setup: fade-outs that fill the clips");
                    row.applyClipMenuChoice (0, 104);   // Fade in 200 ms: no room at all
                    fadesAre (0, kRate, 0, kRate, "no room");
                    expect (said.size() == 1 && said[0].contains ("No room"), "a refused fade was not reported");

                    said.clear();
                    expect (s.engine.setClipFades (0, 0, 0, 0) && s.engine.setClipFades (1, 0, 0, 0), "setup: no fades");
                    expect (s.engine.setClipLocked (1, 0, true), "setup: the peer locked");
                    row.applyClipMenuChoice (0, 104);
                    expectEquals (s.clips (0)[0].fadeInSamples, (juce::int64) 0, "a fade landed beside a locked peer");
                    expect (said.size() == 1 && said[0].contains ("locked"), "the locked peer was not reported");
                }
            }
        }

        // ── 4: a clip enclosed in another is not a crossfade ─────────────────
        void enclosedClipsHaveNoCrossfade()
        {
            using Pairs = std::vector<std::pair<int, int>>;
            beginTest ("Only clips that meet at a boundary crossfade");
            {
                expect (timelineCrossfades ({ clipAt (0, 48000), clipAt (19200, 4800) }).empty(),
                        "a clip pasted inside a long one paired as a crossfade");
                expect (timelineCrossfades ({ clipAt (0, 24000), clipAt (21600, 24000) }) == Pairs { { 0, 1 } },
                        "a real overlap lost its crossfade");
                expect (timelineCrossfades ({ clipAt (21600, 24000), clipAt (0, 24000) }) == Pairs { { 1, 0 } },
                        "a real overlap out of list order lost its crossfade");
                expect (timelineCrossfades ({ clipAt (0, 48000), clipAt (10000, 2000), clipAt (40000, 20000) })
                            == Pairs { { 0, 2 } },
                        "the long clip lost its crossfade into the clip past its end");
                expect (timelineCrossfades ({ clipAt (0, 24000), clipAt (0, 48000) }).empty(),
                        "clips sharing a start paired as a crossfade");
                expect (timelineCrossfades ({ clipAt (50000, 24000), clipAt (24000, 24000) }).empty(),
                        "clips that do not overlap paired");
                expect (timelineCrossfades ({ clipAt (0, 100000), clipAt (10000, 10000), clipAt (15000, 10000) })
                            == Pairs { { 1, 2 } },
                        "two clips meeting inside a long one lost their crossfade");
            }

            beginTest ("Dragging over a clip pasted inside a long one leaves the long clip's end alone");
            {
                Session s (1, { kRate });
                s.clips (0).push_back (clipAt (19200, 4800));
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto total = (double) s.engine.getPlayer().getTotalLengthSamples();
                const auto floorX = [&] (juce::int64 sample)
                { return row.getHeaderWidth() + 4 + (int) ((double) sample / total * kLaneW); };
                // Where the fake X drew its handle: the middle of [short start, long end].
                const juce::Point<int> fakeHandle ((floorX (19200) + floorX (48000)) / 2, 50);
                dragByPixels (row, fakeHandle, fakeHandle + juce::Point<int> (40, 0));
                for (const auto& c : s.clips (0))
                {
                    expectEquals (c.fadeInSamples,  (juce::int64) 0, "a fake crossfade wrote a fade-in");
                    expectEquals (c.fadeOutSamples, (juce::int64) 0, "a fake crossfade faded out the long clip's end");
                }
            }

            beginTest ("A real crossfade's handle still drags");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.editClip (0, 1, AudioEngine::ClipEdit::Move, -4800));
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto total = (double) s.engine.getPlayer().getTotalLengthSamples();
                const auto floorX = [&] (juce::int64 sample)
                { return row.getHeaderWidth() + 4 + (int) ((double) sample / total * kLaneW); };
                const juce::Point<int> handle ((floorX (19200) + floorX (24000)) / 2, 50);
                dragByPixels (row, handle, handle + juce::Point<int> (10, 0));
                expect (s.clips (0)[0].fadeOutSamples > 0 && s.clips (0)[1].fadeInSamples > 0,
                        "the crossfade handle did not drag");
            }
        }

        // ── 5: the minimap's box is the lane on screen ───────────────────────
        void minimapBoxIsTheVisibleLane()
        {
            beginTest ("The minimap box is the lane right of the pinned header, and a click centres it there");
            {
                constexpr int header = brand::space::editHeaderW;
                constexpr int inset  = brand::space::editWaveInset;
                constexpr int lane   = 4000;                       // content lane width
                constexpr int contentW = header + lane + 2 * inset;
                constexpr int viewW  = 1400;
                TimelineMinimap minimap;
                minimap.setSize (1000, 16);                        // 4 lane px per minimap px
                const auto mm = [] (int contentX) { return (double) (contentX - header - inset) / (double) lane * 1000.0; };

                minimap.setView (contentW, header, 2000, viewW);
                auto box = minimap.viewBox();
                expectWithinAbsoluteError ((double) box.getX(), mm (2000 + header), 1.0,
                                           "the box does not start where the visible lane does");
                expectWithinAbsoluteError ((double) box.getRight(), mm (2000 + viewW), 1.0,
                                           "the box does not end at the view's right edge");

                minimap.setView (contentW, header, 0, viewW);
                box = minimap.viewBox();
                expectWithinAbsoluteError ((double) box.getX(), 0.0, 1.0, "unscrolled, the box starts at the timeline's start");
                expectWithinAbsoluteError ((double) box.getRight(), mm (viewW), 1.0, "unscrolled, the box is the lane on screen");

                int scrolledTo = -1;
                minimap.onScrollToContentX = [&scrolledTo] (int x) { scrolledTo = x; };
                const juce::Point<int> click (500, 8);
                minimap.mouseDown (mouseAt (minimap, click, click, false));
                const int clickedX = header + inset + lane / 2;   // the lane's middle time
                const int laneMiddle = scrolledTo + header + (viewW - header) / 2;
                expectEquals (laneMiddle, clickedX, "the clicked time is not in the middle of the visible lane");

                minimap.setView (contentW, header, scrolledTo, viewW);
                box = minimap.viewBox();
                expectWithinAbsoluteError ((double) box.getCentreX(), 500.0, 1.0,
                                           "after the click the box is not centred on it");
            }
        }
    };

    static Fix20261122EditTests fix20261122EditTests;
}
