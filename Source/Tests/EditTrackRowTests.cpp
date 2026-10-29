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
#include <cmath>
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

            //@@MORE_TESTS@@

            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static EditViewAuditTests editViewAuditTests;
}

