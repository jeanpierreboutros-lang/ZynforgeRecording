// Direct tests against EditPage::TrackRow.
//
// This file only exists because TrackRow was made a PUBLIC nested declaration.
// It used to be private, with its ~4,000-line definition in EditTrackRow.h
// (included solely by EditPage.cpp) — so the three mouse handlers, every
// hit-test, the routing combos and the meter lifetime were structurally
// unreachable from the suite. Two consecutive audits found real defects in
// there that could only ever be smoke-tested. These are the regression guards
// for the ones that mattered.
//
// TrackRow is a juce::Component; constructing it headlessly is fine (its
// timers never fire without a running message loop) — same approach as
// EditPageAccessibilityTests.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    class EditTrackRowTests final : public juce::UnitTest
    {
    public:
        EditTrackRowTests() : juce::UnitTest ("EditPage TrackRow", "zynforge") {}

        // Everything a TrackRow needs to exist, in one place.
        struct Host
        {
            AudioEngine                engine;
            juce::AudioFormatManager   formats;
            juce::AudioThumbnailCache  cache { 8 };

            explicit Host (int strips)
            {
                formats.registerBasicFormats();
                // TEST ISOLATION. Every test-mode AudioEngine shares ONE
                // throwaway zynforge-test.settings, so per-strip writes
                // (setTrackStereo -> strip_stereo_N, routing -> strip_in_N)
                // persist into the NEXT suite that constructs an engine and
                // calls applyPersistedStripState(). This suite genuinely broke
                // "Player maps files by Track_NN index" that way. Wipe before
                // AND after so neither direction leaks.
                engine.clearAllStripOverrides();
                engine.prepareForTests (48000.0, 512);
                engine.setStripCount (strips);
            }

            ~Host() { engine.clearAllStripOverrides(); }
        };

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            // ── The meter-condemn UAF guard ──────────────────────────────────
            // Each TrackRow owns a LedMeter holding a TrackState& on its own
            // timer, and the rows only rebuild on EditPage's next 24 Hz tick --
            // so every recorder-vector shrink left EDIT reading freed memory
            // until then. condemnAllStrips() now calls TrackRow::invalidate().
            // Observable contract: once condemned, the row STOPS reading its
            // TrackState. (The same hazard was fixed three times on the MIXER
            // while this sat open, which is why it gets a real test.)
            beginTest ("invalidate() stops the row reading its TrackState");
            {
                Host h (4);
                EditPage::TrackRow row (0, false, h.engine, h.formats, h.cache);

                h.engine.setTrackName (0, "KICK");
                row.updatePollState();
                expectEquals (row.getNameTextForTests(), juce::String ("KICK"),
                              "a live row must track its TrackState");
                expect (! row.isCondemnedForTests());

                row.invalidate();
                expect (row.isCondemnedForTests());

                // After condemning, a change to the (about-to-be-freed) state
                // must NOT be picked up -- that read is the use-after-free.
                h.engine.setTrackName (0, "SNARE");
                row.updatePollState();
                expectEquals (row.getNameTextForTests(), juce::String ("KICK"),
                              "a condemned row must not dereference its TrackState");
            }

            // ── The stale-index guard ────────────────────────────────────────
            // Even without an explicit condemn, a row whose index is past the
            // end of a shrunken recorder must not index the vector. paint() has
            // always guarded this; updatePollState() did not.
            beginTest ("A row past the end of a shrunken recorder polls nothing");
            {
                Host h (4);
                EditPage::TrackRow row (3, false, h.engine, h.formats, h.cache);
                h.engine.setTrackName (3, "TOM");
                row.updatePollState();
                expectEquals (row.getNameTextForTests(), juce::String ("TOM"));

                h.engine.setStripCount (2);          // row 3 no longer exists
                row.updatePollState();               // must not index out of range
                expectEquals (row.getNameTextForTests(), juce::String ("TOM"),
                              "the poll must no-op rather than read past the end");
            }

            // ── Odd-index stereo routing must still be displayed ─────────────
            // A stereo row lists only EVEN start channels (ids 2, 4, 6 ...) but
            // the selection is routing+2, so a stereo L at an odd physical index
            // selected an id that wasn't in the list and the combo went BLANK --
            // the engineer had no way to see what the pair was patched to.
            beginTest ("A stereo pair at an odd index shows its routing, not a blank");
            {
                Host h (4);
                // Mono strip 0, stereo pair at 1-2 => L sits at an ODD index.
                h.engine.setTrackStereo (1, true);
                h.engine.setTrackInputRouting  (1, 1);   // odd
                h.engine.setTrackOutputRouting (1, 1);

                EditPage::TrackRow row (1, true, h.engine, h.formats, h.cache);
                row.refreshRoutingSelection();

                const auto in  = row.getInputRoutingTextForTests();
                const auto out = row.getOutputRoutingTextForTests();
                expect (in .isNotEmpty(), "input combo blanked on an odd stereo routing");
                expect (out.isNotEmpty(), "output combo blanked on an odd stereo routing");
                // Channels are 1-based on screen: device index 1 => "In 2-3".
                // startsWith, not equals: entries for channels the (absent, in
                // test mode) device doesn't have are suffixed "(off)". The
                // contract under test is that the routing is SHOWN at all.
                expect (in .startsWith ("In 2-3"),  "expected In 2-3..., got: "  + in);
                expect (out.startsWith ("Out 2-3"), "expected Out 2-3..., got: " + out);
            }

            beginTest ("An even-index stereo pair is unchanged");
            {
                Host h (4);
                h.engine.setTrackStereo (0, true);
                h.engine.setTrackInputRouting (0, 0);
                EditPage::TrackRow row (0, true, h.engine, h.formats, h.cache);
                row.refreshRoutingSelection();
                const auto txt = row.getInputRoutingTextForTests();
                expect (txt.startsWith ("In 1-2"),
                        "the normal even case must not regress; got: " + txt);
            }

            // ── Unrouted stays unrouted ──────────────────────────────────────
            beginTest ("An unrouted strip reads (unrouted)");
            {
                Host h (2);
                h.engine.setTrackInputRouting (0, -1);
                EditPage::TrackRow row (0, false, h.engine, h.formats, h.cache);
                row.refreshRoutingSelection();
                expectEquals (row.getInputRoutingTextForTests(), juce::String ("(unrouted)"));
            }
        }
    };

    static EditTrackRowTests editTrackRowTests;

    // ── EDIT view audit 2026-10-29 ──────────────────────────────────────────
    // Regression guards for the October EDIT-view review, driven through the
    // row's real mouse handlers (same idiom as TrackReorderTests) and through
    // EditPage's public surface.
    class EditViewAuditTests final : public juce::UnitTest
    {
    public:
        EditViewAuditTests() : juce::UnitTest ("EDIT view audit 2026-10-29", "zynforge") {}

        using AP   = AudioEngine::AutomationParam;
        using Lane = EditPage::TrackRow::LaneMode;

        // A session folder with real takes, loaded into a test-mode engine.
        struct Session
        {
            juce::File                 dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                 .getChildFile ("zf-editaudit-" + juce::Uuid().toString());
            AudioEngine                engine;
            juce::AudioFormatManager   formats;
            juce::AudioThumbnailCache  cache { 8 };

            // lengths[i] = samples in Track_(i+1).wav (0 = no file for that strip).
            Session (int strips, std::vector<int> lengths)
            {
                formats.registerBasicFormats();
                engine.clearAllStripOverrides();
                engine.prepareForTests (48000.0, 512);
                engine.setStripCount (strips);
                dir.getChildFile ("Audio Files").createDirectory();
                for (size_t i = 0; i < lengths.size(); ++i)
                    if (lengths[i] > 0)
                        writeWav (take ((int) i + 1), lengths[i]);
                engine.setActiveSessionDir (dir);
                engine.loadSession (dir);
                engine.setStripCount (strips);
            }
            ~Session()
            {
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
            juce::File take (int n) const
            {
                return dir.getChildFile ("Audio Files/Track_" + juce::String::formatted ("%02d.wav", n));
            }
            std::vector<Clip>& clips (int track) { return engine.clipsFor (track); }
        };

        static bool writeWav (const juce::File& file, int samples, int channels = 1)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
                file.createOutputStream().release(), 48000.0, (unsigned) channels, 24, {}, 0));
            if (writer == nullptr) return false;
            juce::AudioBuffer<float> audio (channels, samples);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < samples; ++i)
                    audio.setSample (c, i, 0.8f * std::sin ((float) i * 0.05f));
            return writer->writeFromAudioSampleBuffer (audio, 0, samples);
        }

        // An in-memory sine take that counts how often JUCE reads it.
        struct CountingReader final : public juce::AudioFormatReader
        {
            std::atomic<int>& reads;
            CountingReader (std::atomic<int>& counter, juce::int64 length)
                : juce::AudioFormatReader (nullptr, "counting"), reads (counter)
            {
                sampleRate = 48000.0;
                bitsPerSample = 32;
                usesFloatingPointData = true;
                numChannels = 1;
                lengthInSamples = length;
            }
            bool readSamples (int* const* dest, int numDestChannels, int startOffsetInDestBuffer,
                              juce::int64 startSampleInFile, int numSamples) override
            {
                ++reads;
                for (int c = 0; c < numDestChannels; ++c)
                    if (auto* d = reinterpret_cast<float*> (dest[c]))
                        for (int i = 0; i < numSamples; ++i)
                            d[startOffsetInDestBuffer + i] = 0.8f * std::sin ((float) (startSampleInFile + i) * 0.05f);
                return true;
            }
        };

        static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<int> p,
                                         juce::Point<int> down, bool dragged,
                                         juce::ModifierKeys mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier))
        {
            const auto now = juce::Time::getCurrentTime();
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), mods,
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, down.toFloat(), now, 1, dragged };
        }

        // Down -> drag (in ~4 px steps) -> up, like a real gesture.
        static void drag (juce::Component& c, juce::Point<int> from, juce::Point<int> to)
        {
            c.mouseDown (mouseAt (c, from, from, false));
            const int steps = juce::jmax (1, (int) (from.getDistanceFrom (to) / 4));
            for (int s = 1; s <= steps; ++s)
                c.mouseDrag (mouseAt (c, from + (to - from) * s / steps, from, true));
            c.mouseUp (mouseAt (c, to, from, true));
        }

        static void click (juce::Component& c, juce::Point<int> p)
        {
            c.mouseDown (mouseAt (c, p, p, false));
            c.mouseUp   (mouseAt (c, p, p, false));
        }

        static void pressButton (juce::Component& parent, const juce::String& text)
        {
            for (auto* child : parent.getChildren())
                if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getButtonText() == text)
                    if (b->onClick) b->onClick();
        }

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
               #elif JUCE_MODAL_LOOPS_PERMITTED
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
               #else
                juce::Thread::sleep (5);
               #endif
            }
            return predicate();
        }

        // The page's TrackRows, top to bottom (as TrackReorderTests walks them).
        static std::vector<EditPage::TrackRow*> rowsOf (EditPage& page)
        {
            std::vector<EditPage::TrackRow*> rows;
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                for (auto* child : c.getChildren())
                {
                    if (auto* row = dynamic_cast<EditPage::TrackRow*> (child)) rows.push_back (row);
                    walk (*child);
                }
            };
            walk (page);
            std::sort (rows.begin(), rows.end(), [] (auto* a, auto* b) { return a->getY() < b->getY(); });
            return rows;
        }

        // The component the page's viewport scrolls (the row list).
        static juce::Component* listOf (EditPage& page)
        {
            for (auto* child : page.getChildren())
                if (auto* vp = dynamic_cast<juce::Viewport*> (child))
                    return vp->getViewedComponent();
            return nullptr;
        }

        // Row geometry used throughout: a 1000 px inner lane (the wave pane
        // minus its 4 px insets) starting 4 px right of the header.
        static constexpr int kLaneW = 1000;
        static void sizeRow (EditPage::TrackRow& row, int height = 100)
        {
            row.setSize (row.getHeaderWidth() + kLaneW + 8, height);
        }
        static int laneX (const EditPage::TrackRow& row, double frac)
        {
            return row.getHeaderWidth() + 4 + (int) std::lround (frac * kLaneW);
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            // ── #1: a lane that does not SHOW automation must not edit it ────
            beginTest ("A Waveform or Markers lane never edits the automation it does not show");
            {
                EditTrackRowTests::Host h (2);
                h.engine.clearAutomation (AP::Volume);
                AutomationToolbar toolbar;
                EditPage::TrackRow row (0, false, h.engine, h.formats, h.cache);
                sizeRow (row);
                row.toolbar = &toolbar;

                pressButton (toolbar, "+ Point");
                for (auto mode : { Lane::Waveform, Lane::Markers })
                {
                    row.laneMode = mode;
                    click (row, { laneX (row, 0.3), 20 });
                    expect (h.engine.getAutomation (0, AP::Volume).empty(),
                            "+ Point on a waveform/markers lane wrote a hidden Volume point");
                }

                const auto tempoBefore = h.engine.getTempoMap().size();
                toolbar.setParamSilently (AutomationToolbar::Param::Tempo);
                row.laneMode = Lane::Waveform;
                click (row, { laneX (row, 0.5), 20 });
                expectEquals ((int) h.engine.getTempoMap().size(), (int) tempoBefore,
                              "a waveform lane must not write the session tempo map");

                // Select grabbing a hidden point and dragging rewrites its value.
                toolbar.setParamSilently (AutomationToolbar::Param::Volume);
                pressButton (toolbar, "Select");
                row.laneMode = Lane::Waveform;
                const auto hiddenAt = (juce::int64) (0.6 * (double) notionalEmptyLaneSamples (48000.0));
                h.engine.addAutomationPoint (0, AP::Volume, hiddenAt, -6.0f);
                drag (row, { laneX (row, 0.6), 50 }, { laneX (row, 0.6), 90 });
                const auto& lane = h.engine.getAutomation (0, AP::Volume);
                expectEquals ((int) lane.size(), 1);
                if (! lane.empty())
                {
                    expectEquals (lane.front().samplePos, hiddenAt, "hidden point moved");
                    expectWithinAbsoluteError (lane.front().value, -6.0f, 0.001f, "hidden point re-valued");
                }
                h.engine.clearAutomation (AP::Volume);
            }

            // ── #9: the row edits the lane it draws and labels ───────────────
            beginTest ("A row edits the lane its VIEW shows, not the toolbar's last pick");
            {
                EditTrackRowTests::Host h (2);
                h.engine.clearAutomation (AP::Volume);
                h.engine.clearAutomation (AP::Pan);
                AutomationToolbar toolbar;
                EditPage::TrackRow row (0, false, h.engine, h.formats, h.cache);
                sizeRow (row);
                row.toolbar = &toolbar;
                pressButton (toolbar, "+ Point");
                // Another row's VIEW pick set the toolbar to Volume silently;
                // this row still shows (and labels) Pan.
                toolbar.setParamSilently (AutomationToolbar::Param::Volume);
                row.laneMode = Lane::Pan;
                click (row, { laneX (row, 0.4), 20 });
                expectEquals ((int) h.engine.getAutomation (0, AP::Pan).size(), 1,
                              "the point must land on the Pan lane the row shows");
                expect (h.engine.getAutomation (0, AP::Volume).empty(),
                        "the hidden Volume lane must not change");
                h.engine.clearAutomation (AP::Pan);
            }

            // ── #5: dragging a point over a neighbour keeps the neighbour ────
            beginTest ("Dragging an automation point onto a neighbour does not erase it");
            {
                EditTrackRowTests::Host h (2);
                h.engine.clearAutomation (AP::Volume);
                AutomationToolbar toolbar;
                EditPage::TrackRow row (0, false, h.engine, h.formats, h.cache);
                sizeRow (row);
                row.toolbar = &toolbar;
                row.laneMode = Lane::Volume;
                pressButton (toolbar, "+ Point");
                click (row, { laneX (row, 0.3), 20 });
                click (row, { laneX (row, 0.4), 70 });
                expectEquals ((int) h.engine.getAutomation (0, AP::Volume).size(), 2, "setup");
                const float neighbourValue = h.engine.getAutomation (0, AP::Volume).back().value;

                pressButton (toolbar, "Select");
                drag (row, { laneX (row, 0.3), 20 }, { laneX (row, 0.4), 40 });
                const auto& lane = h.engine.getAutomation (0, AP::Volume);
                expectEquals ((int) lane.size(), 2, "the drag erased the neighbour it landed on");
                const bool neighbourKept = std::any_of (lane.begin(), lane.end(), [&] (const auto& p)
                    { return std::abs (p.value - neighbourValue) < 0.001f; });
                expect (neighbourKept, "the neighbour's value must survive");
                h.engine.clearAutomation (AP::Volume);
            }

            // ── #2: clip menu items on a stereo row edit BOTH halves ─────────
            beginTest ("Clip menu mute, fades and curve apply to both halves of a stereo row");
            {
                Session s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                EditPage::TrackRow row (0, true, s.engine, s.formats, s.cache);
                sizeRow (row);
                expect (! s.clips (0).empty() && ! s.clips (1).empty(), "setup: both halves hold a clip");

                row.applyClipMenuChoice (0, 400);   // Mute clip
                row.applyClipMenuChoice (0, 105);   // Fade in 500 ms
                row.applyClipMenuChoice (0, 320);   // Equal-power fades
                for (int t : { 0, 1 })
                {
                    if (s.clips (t).empty()) continue;
                    const auto& c = s.clips (t)[0];
                    const auto half = juce::String (t == 0 ? "L" : "R");
                    expect (c.muted, half + " clip not muted");
                    expectEquals (c.fadeInSamples, (juce::int64) 24000, half + " fade-in");
                    expectEquals (c.fadeCurve, 1, half + " fade curve");
                }
            }

            // ── 4757: menu items resolve each group peer's OWN clip ──────────
            beginTest ("Clip menu delete, lock and normalize act on each group peer's own clip");
            {
                Session s (2, { 48000, 48000 });
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                // A = [0,24k)[24k,48k); B = [0,6k)[6k,12k)[12k,48k).
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.splitTrackAtSample (1, 6000));
                expect (s.engine.splitTrackAtSample (1, 12000));
                expectEquals ((int) s.clips (0).size(), 2, "setup A");
                expectEquals ((int) s.clips (1).size(), 3, "setup B");
                EditPage::TrackRow row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);

                // Lock A's first clip [0,24k): B's clip under it (midpoint 12k) is #2.
                row.applyClipMenuChoice (0, 401);
                expect (s.clips (0)[0].locked);
                expect (! s.clips (1)[0].locked, "Lock hit B's clip #0 by raw index");
                expect (s.clips (1)[2].locked, "Lock missed the peer clip under the source");
                for (int t : { 0, 1 })
                    for (int i = 0; i < (int) s.clips (t).size(); ++i)
                        s.engine.setClipLocked (t, i, false);

                // Normalize A's second clip [24k,48k): B's clip under it is #2.
                row.applyClipMenuChoice (1, 414);
                expect (pumpUntil ([&] { return row.isEnabled(); }, 5000), "normalize never completed");
                expect (s.clips (0)[1].gainDb > 1.0f, "source clip normalized");
                expectWithinAbsoluteError (s.clips (1)[1].gainDb, 0.0f, 0.001f,
                                           "Normalize changed B's clip #1 by raw index");
                expect (s.clips (1)[2].gainDb > 1.0f, "Normalize missed the peer clip under the source");

                // Delete A's second clip [24k,48k): B must lose #2 [12k,48k), not #1.
                row.applyClipMenuChoice (1, 411);
                expectEquals ((int) s.clips (0).size(), 1);
                expectEquals ((int) s.clips (1).size(), 2);
                if (s.clips (1).size() == 2)
                {
                    expectEquals (s.clips (1)[0].timelineStartSamples, (juce::int64) 0);
                    expectEquals (s.clips (1)[1].timelineStartSamples, (juce::int64) 6000,
                                  "Delete removed B's clip #1 by raw index");
                }
            }

            // ── #3: takes and comps on a stereo row move BOTH halves ─────────
            beginTest ("Take and comp operations on a stereo row keep L and R on one arrangement");
            {
                Session s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                EditPage::TrackRow row (0, true, s.engine, s.formats, s.cache);
                sizeRow (row);
                const int takesBefore = s.engine.getTakeCount (0);
                expectEquals (s.engine.getTakeCount (1), takesBefore, "setup");

                row.applyTakeMenuChoice (850);   // New take from current
                expectEquals (s.engine.getTakeCount (0), takesBefore + 1);
                expectEquals (s.engine.getTakeCount (1), takesBefore + 1, "R missed the new take");

                // Edit the new take on both halves (as a stereo clip edit does).
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.splitTrackAtSample (1, 24000));
                row.applyTakeMenuChoice (800);   // back to the first take
                expectEquals ((int) s.clips (0).size(), 1);
                expectEquals ((int) s.clips (1).size(), 1, "R kept the edited take");

                // Comp a range of the edited take into the first take.
                s.engine.getPlayer().setLoopRegion (30000, 40000);
                row.applyTakeMenuChoice (870 + takesBefore);
                expectEquals ((int) s.clips (0).size(), (int) s.clips (1).size(),
                              "comp selection spliced only one half");
                s.engine.getPlayer().clearLoopRegion();

                // Comp-lane swipe pulls the swiped range into BOTH halves.
                row.applyTakeMenuChoice (860);   // show take lanes
                const int before0 = (int) s.clips (0).size();
                drag (row, { laneX (row, 0.05), 70 }, { laneX (row, 0.3), 70 });   // the second take's lane
                expect ((int) s.clips (0).size() != before0, "setup: the swipe comped L");
                expectEquals ((int) s.clips (0).size(), (int) s.clips (1).size(),
                              "the swipe comped only L");

                row.applyTakeMenuChoice (852);   // delete the active take
                expectEquals (s.engine.getTakeCount (0), s.engine.getTakeCount (1),
                              "delete take left L and R with different take lists");
            }

            // ── #4: deep zoom draws only the visible columns ────────────────
            beginTest ("Deep zoom reads only the visible part of a waveform, and normal zoom draws the same pixels");
            {
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                juce::AudioThumbnailCache thumbCache (4);
                juce::AudioThumbnail thumb (128, fm, thumbCache);
                std::atomic<int> reads { 0 };
                thumb.setReader (new CountingReader (reads, 20 * 48000), 0x5eed);
                expect (pumpUntil ([&] { return thumb.isFullyLoaded(); }, 10000), "thumbnail scan");

                // 2 s across 200,000 px (0.48 samples/px) seen through a 400 px
                // window scrolled into the middle: the work must scale with the
                // window, not the clip.
                {
                    juce::Image img (juce::Image::ARGB, 400, 60, true);
                    juce::Graphics g (img);
                    g.setOrigin ({ -100000, 0 });
                    reads = 0;
                    EditPage::TrackRow::drawThumbnailVisible (g, thumb, { 0, 0, 200000, 60 }, 0.0, 2.0, 0, 1.0f);
                    expect (reads.load() < 4000,
                            "the draw read the take once per lane pixel: " + juce::String (reads.load()) + " reads");
                }

                // Same pixels as JUCE's own whole-area draw: fully visible
                // (normal zoom) and scrolled at a detailed and a zoomed-out scale.
                auto render = [&] (bool viaRow, int areaW, int originX, double t1)
                {
                    juce::Image img (juce::Image::ARGB, 400, 60, true);
                    juce::Graphics g (img);
                    g.setOrigin ({ -originX, 0 });
                    g.setColour (juce::Colours::black);
                    const juce::Rectangle<int> area (0, 0, areaW, 60);
                    if (viaRow) EditPage::TrackRow::drawThumbnailVisible (g, thumb, area, 0.0, t1, 0, 1.0f);
                    else        thumb.drawChannel (g, area, 0.0, t1, 0, 1.0f);
                    return img;
                };
                struct Case { int areaW, originX; double t1; const char* what; };
                for (const auto& c : { Case { 400,  0,    20.0, "fully visible" },
                                       Case { 4000, 1600, 2.0,  "scrolled, detailed" },
                                       Case { 4000, 1600, 20.0, "scrolled, zoomed out" } })
                {
                    const auto a = render (true, c.areaW, c.originX, c.t1);
                    const auto b = render (false, c.areaW, c.originX, c.t1);
                    int differing = 0, inked = 0;
                    for (int y = 0; y < 60; ++y)
                        for (int x = 0; x < 400; ++x)
                        {
                            if (a.getPixelAt (x, y) != b.getPixelAt (x, y)) ++differing;
                            if (b.getPixelAt (x, y).getAlpha() > 0) ++inked;
                        }
                    expect (inked > 400, juce::String ("setup: nothing drawn, ") + c.what);
                    logMessage (juce::String (c.what) + ": " + juce::String (differing) + " of "
                                + juce::String (inked) + " inked pixels differ from JUCE's whole-area draw");
                    expect (differing <= inked / 100,
                            juce::String (c.what) + ": " + juce::String (differing) + " pixels differ");
                }
            }

            // ── #6: the host's EDIT lock is not overwritten by the page timer ─
            beginTest ("The host's EDIT lock holds through the page timer after recording stops");
            {
                Session s (2, { 48000, 48000 });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto* list = listOf (page);
                expect (list != nullptr, "setup: row list");
                // STOP has cleared `recording`, but finalization / the metadata
                // save is still pending, so the host keeps the timeline locked.
                page.setReadOnlyWhileRecording (true);
                pumpUntil ([] { return false; }, 250);   // ~6 page-timer ticks
                bool clicks = true, childClicks = true;
                if (list != nullptr) list->getInterceptsMouseClicks (clicks, childClicks);
                expect (! clicks && ! childClicks, "the page timer unlocked rows the host had locked");
                page.setReadOnlyWhileRecording (false);
                if (list != nullptr) list->getInterceptsMouseClicks (clicks, childClicks);
                expect (clicks && childClicks, "the host's unlock must take effect");
            }

            beginTest ("A drag already in flight stops editing when the timeline locks");
            {
                Session s (2, { 48000, 48000 });
                int reorders = 0;
                s.engine.onTracksReordered = [&reorders] (const std::vector<int>&) { ++reorders; };
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                expect (rows.size() == 2, "setup: two rows");
                if (rows.size() == 2)
                {
                    auto& row = *rows[0];
                    const int lane0 = row.getHeaderWidth() + 4;
                    const int laneW = row.getWidth() - row.getHeaderWidth() - 8;
                    const juce::Point<int> down (lane0 + laneW / 2, row.getHeight() / 2);
                    row.mouseDown (mouseAt (row, down, down, false));      // grab the clip body (Move)
                    page.setReadOnlyWhileRecording (true);                  // capture starts mid-drag
                    for (int dx = 4; dx <= 120; dx += 4)
                        row.mouseDrag (mouseAt (row, down + juce::Point<int> (dx, 0), down, true));
                    row.mouseUp (mouseAt (row, down + juce::Point<int> (120, 0), down, true));
                    expectEquals (s.clips (0)[0].timelineStartSamples, (juce::int64) 0,
                                  "the in-flight drag kept moving the clip under the lock");
                    page.setReadOnlyWhileRecording (false);

                    // A swatch reorder released under the lock must not commit.
                    const juce::Point<int> swatch (4, 10);
                    row.mouseDown (mouseAt (row, swatch, swatch, false));
                    row.mouseDrag (mouseAt (row, { 4, 200 }, swatch, true));
                    page.setReadOnlyWhileRecording (true);
                    row.mouseUp (mouseAt (row, { 4, 200 }, swatch, true));
                    expectEquals (reorders, 0, "a reorder committed under the lock");
                    page.setReadOnlyWhileRecording (false);
                }
            }

            // ── #7: the post-STOP rescan waits for async finalization ────────
            beginTest ("After an async STOP the EDIT waveform shows the finalized take");
            {
                const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("zf-editaudit-stop-" + juce::Uuid().toString());
                expect (dir.createDirectory().wasOk());
                {
                    AudioEngine engine;
                    engine.clearAllStripOverrides();
                    constexpr int block = 256, blocks = 188;   // ~1 s
                    engine.setStripCount (1);
                    engine.prepareForTests (48000, block);
                    auto& recorder = engine.getRecorder();
                    recorder.getTrack (0).armed.store (true);
                    engine.setActiveSessionDir (dir);   // a session is open before the take, as in the app

                    EditPage page (engine);
                    page.setBounds (0, 0, 1400, 300);
                    page.refresh();

                    expect (engine.startRecording (dir));
                    pumpUntil ([] { return false; }, 120);    // the page sees the take roll
                    std::vector<float> audio ((size_t) block, 0.5f);
                    const float* input[] { audio.data() };
                    for (int b = 0; b < blocks; ++b) recorder.processBlock (input, 1, block);

                    juce::WaitableEvent entered, release;
                    std::atomic<bool> completed { false };
                    recorder.beforeFinalizationForTests = [&] { entered.signal(); release.wait (5000); };
                    const juce::ScopeGuard releaseWorker { [&] { release.signal(); } };
                    expect (engine.stopRecordingAsync ([&] (bool) { completed = true; }));
                    expect (entered.wait (2000), "STOP never reached finalization");
                    pumpUntil ([] { return false; }, 200);    // page ticks while the files are still open
                    release.signal();
                    expect (pumpUntil ([&] { return completed.load(); }, 10000), "STOP did not complete");

                    auto rows = rowsOf (page);
                    expect (rows.size() == 1, "setup: one row");
                    const double takeSec = (double) (block * blocks) / 48000.0;
                    if (rows.size() == 1)
                    {
                        pumpUntil ([&] { return rows[0]->waveformsLoaded()
                                             && std::abs (rows[0]->getWaveformSecondsForTests() - takeSec) < 0.01; },
                                   5000);
                        expectWithinAbsoluteError (rows[0]->getWaveformSecondsForTests(), takeSec, 0.01,
                                                   "the lane kept the waveform scanned while the take was still open");
                    }
                    engine.clearAllStripOverrides();
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
                dir.deleteRecursively();
            }

            //@@MORE_TESTS@@

            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static EditViewAuditTests editViewAuditTests;
}

