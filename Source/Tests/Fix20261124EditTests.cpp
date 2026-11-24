// The 2026-11-24 EDIT fixes: a crossfade handle that made a level bump, right
// and middle clicks that acted as left clicks, a trim drag that read the disk
// on every mouse event, a held take that followed its strip index into another
// session, locked peers that fade and gain edits went past, a pasted clip that
// drew the other channel, wheel zoom that only zoomed out, fit-to-window rows
// sized against the wrong heights, and a continue take's dim backdrop drawn as
// the whole file. Driven through the real rows, page, toolbar, minimap and
// main window, headlessly.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/ClipModel.h"
#include "../UI/EditPage.h"
#include "../UI/EditTimeline.h"
#include "../UI/EditToolsBar.h"
#include "../UI/EditTrackRow.h"
#include "../UI/MainComponent.h"
#include "../UI/TimelineMinimap.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class Fix20261124EditTests final : public juce::UnitTest
    {
    public:
        Fix20261124EditTests() : juce::UnitTest ("Fixes 2026-11-24 edit", "zynforge") {}

        static constexpr int kRate = 48000;
        using Row = EditPage::TrackRow;
        using Sample = std::function<float (int /*channel*/, int /*sample*/)>;

        static float sine (int, int i) { return 0.8f * std::sin ((float) i * 0.05f); }

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs = 3000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
            return predicate();
        }

        static juce::File tempDir (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-fix1124-" + tag + "-" + juce::Uuid().toString());
        }

        static bool writeWav (const juce::File& file, int samples, int channels = 1, const Sample& level = sine)
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
                    audio.setSample (c, i, level (c, i));
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
            int                       strips;

            Session (int n, std::vector<int> lengths, const Sample& level = sine) : strips (n)
            {
                formats.registerBasicFormats();
                engine.clearAllStripOverrides();
                engine.prepareForTests ((double) kRate, 512);
                engine.setStripCount (strips);
                write (dir, lengths, level);
                open (dir);
            }
            ~Session()
            {
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
            static void write (const juce::File& folder, const std::vector<int>& lengths, const Sample& level)
            {
                folder.getChildFile ("Audio Files").createDirectory();
                for (size_t i = 0; i < lengths.size(); ++i)
                    if (lengths[i] > 0) writeWav (takeIn (folder, (int) i + 1), lengths[i], 1, level);
            }
            void open (const juce::File& folder)
            {
                engine.setActiveSessionDir (folder);
                engine.claimMixerFor (folder);
                engine.loadSession (folder);
                engine.setStripCount (strips);
            }
            std::vector<Clip>& clips (int track) { return engine.clipsFor (track); }
            void group (std::initializer_list<int> tracks)
            {
                for (int t : tracks) engine.setTrackEditGroup (t, 0);
            }
        };

        // A 1000 px inner lane starting 4 px right of the header: 48 samples/px
        // over a one-second take.
        static constexpr int kLaneW = 1000;
        static void sizeRow (Row& row, int height = 100) { row.setSize (row.getHeaderWidth() + kLaneW + 8, height); }
        static int laneX (const Row& row, double frac)
        { return row.getHeaderWidth() + brand::space::xs + (int) std::lround (frac * kLaneW); }

        static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<int> p, juce::Point<int> down, bool dragged,
                                         int buttons = juce::ModifierKeys::leftButtonModifier)
        {
            const auto now = juce::Time::getCurrentTime();
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), juce::ModifierKeys (buttons),
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, down.toFloat(), now, 1, dragged };
        }

        // Press at `from`, move one pixel at a time to `to`, release there.
        static void dragByPixels (juce::Component& c, juce::Point<int> from, juce::Point<int> to,
                                  int buttons = juce::ModifierKeys::leftButtonModifier)
        {
            c.mouseDown (mouseAt (c, from, from, false, buttons));
            const int steps = juce::jmax (std::abs (to.x - from.x), std::abs (to.y - from.y));
            for (int s = 1; s <= steps; ++s)
                c.mouseDrag (mouseAt (c, from + (to - from) * s / steps, from, true, buttons));
            c.mouseUp (mouseAt (c, to, from, steps > 0, buttons));
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

        // Pixels that differ between two renders in the columns [x0, x1),
        // leaving out the rows within `skipAround` px of y = `centre` (where a
        // silent waveform still draws its flat line).
        static int differing (const juce::Image& a, const juce::Image& b, int x0, int x1,
                              int centre = -1000, int skipAround = 0)
        {
            int n = 0;
            for (int x = x0; x < x1; ++x)
                for (int y = 0; y < a.getHeight(); ++y)
                    if (std::abs (y - centre) > skipAround && a.getPixelAt (x, y) != b.getPixelAt (x, y)) ++n;
            return n;
        }

        static const Clip* clipStartingAt (Session& s, int track, juce::int64 start)
        {
            for (const auto& c : s.clips (track))
                if (c.timelineStartSamples == start) return &c;
            return nullptr;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };

            crossfadeHandleMakesACrossfade();     // EDIT-01
            menuPressesAreMenusOnly();            // EDIT-02
            deleteActsOnWhatItAsked();            // EDIT-02 (Delete key)
            trimDragReadsTheSourceOnce();         // EDIT-03
            heldTakeStaysInItsSession();          // EDIT-04
            lockedPeerStopsFadeDrags();           // EDIT-05
            lockProtectsClipGain();               // EDIT-06
            clipDrawsItsOwnChannel();             // EDIT-07
            wheelZoomFollowsTheWheel();           // EDIT-08
            fitToWindowFillsTheView();            // EDIT-09
            toolsAndMinimapIgnoreMenuPresses();   // EDIT-10
            continueDrawsTheArrangement();        // EDIT-11
        }

        // ── EDIT-01 ──────────────────────────────────────────────────────────
        // Split at 0.5 s and slide the second half 0.1 s left: the two overlap
        // on [19200, 24000), and the X's dot sits in the middle of that.
        static juce::Point<int> crossfadeDot (Row& row, Session& s)
        {
            const auto total = (double) s.engine.getPlayer().getTotalLengthSamples();
            const auto floorX = [&] (juce::int64 sample)
            { return row.getHeaderWidth() + brand::space::xs + (int) ((double) sample / total * kLaneW); };
            return { (floorX (19200) + floorX (24000)) / 2, 50 };
        }

        void overlapSumsToOne (Session& s, int track, const juce::String& what)
        {
            const auto* a = clipStartingAt (s, track, 0);
            const auto* b = clipStartingAt (s, track, 19200);
            expect (a != nullptr && b != nullptr, "setup: the two clips");
            if (a == nullptr || b == nullptr) return;
            expectEquals (a->fadeOutSamples, (juce::int64) 4800, what + ": the outgoing fade does not span the overlap");
            expectEquals (b->fadeInSamples,  (juce::int64) 4800, what + ": the incoming fade does not span the overlap");
            // Linear fades over the same span: the two gains sum to 1 at every
            // sample, so identical audio plays at its own level throughout.
            double worst = 0.0;
            juce::int64 worstAt = -1;
            for (juce::int64 t = 19200; t < 24000; t += 8)
            {
                const double sum = clipFadeGain (*a, t - a->timelineStartSamples)
                                 + clipFadeGain (*b, t - b->timelineStartSamples);
                if (std::abs (sum - 1.0) > worst) { worst = std::abs (sum - 1.0); worstAt = t; }
            }
            expect (worst < 0.01, what + ": the gains over the overlap miss unity by " + juce::String (worst, 3)
                                      + " at sample " + juce::String (worstAt) + " (a level bump, not a crossfade)");
        }

        void crossfadeHandleMakesACrossfade()
        {
            beginTest ("Dragging the crossfade dot fades each clip across the whole overlap");
            for (const int dx : { 10, -10, 40 })
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.editClip (0, 1, AudioEngine::ClipEdit::Move, -4800));
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto dot = crossfadeDot (row, s);
                dragByPixels (row, dot, dot + juce::Point<int> (dx, 0));
                overlapSumsToOne (s, 0, "a " + juce::String (dx) + " px drag");
            }

            beginTest ("Nudging the dot of a full crossfade leaves it whole");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.editClip (0, 1, AudioEngine::ClipEdit::Move, -4800));
                expect (s.engine.setClipFades (0, 0, 0, 4800) && s.engine.setClipFades (0, 1, 4800, 0),
                        "setup: a crossfade across the overlap");
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto dot = crossfadeDot (row, s);
                dragByPixels (row, dot, dot + juce::Point<int> (7, 0));
                overlapSumsToOne (s, 0, "a nudge");
            }

            beginTest ("The crossfade dot reaches every edit-group peer the same way");
            {
                Session s (2, { kRate, kRate });
                s.group ({ 0, 1 });
                for (int t : { 0, 1 })
                {
                    expect (s.engine.splitTrackAtSample (t, 24000));
                    expect (s.engine.editClip (t, 1, AudioEngine::ClipEdit::Move, -4800));
                }
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto dot = crossfadeDot (row, s);
                dragByPixels (row, dot, dot + juce::Point<int> (25, 0));
                overlapSumsToOne (s, 0, "the dragged row");
                overlapSumsToOne (s, 1, "its group peer");
            }
        }

        // ── EDIT-02 ──────────────────────────────────────────────────────────
        void menuPressesAreMenusOnly()
        {
            beginTest ("A right-click or Ctrl-click on a clip keeps the clip selection");
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
                    int menus = 0;
                    row.clipMenuForTests = [&menus] (int) { ++menus; };
                    const int lane0 = row.getHeaderWidth() + brand::space::xs;
                    const int laneW = row.getWidth() - row.getHeaderWidth() - 2 * brand::space::xs;
                    const juce::Point<int> body (lane0 + laneW / 2, row.getHeight() / 2 - 8);
                    for (const int buttons : { (int) juce::ModifierKeys::rightButtonModifier,
                                               juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::leftButtonModifier })
                    {
                        page.setSelectedClip (0, 0);
                        expect (page.getSelectedClips().count ({ 0, 0 }) == 1, "setup: the clip selected");
                        dragByPixels (row, body, body, buttons);
                        expect (page.getSelectedClips().count ({ 0, 0 }) == 1,
                                "a menu press cleared the clip selection (buttons " + juce::String (buttons) + ")");
                    }
                    expectEquals (menus, 2, "the clip menu did not open for each menu press");
                    row.clipMenuForTests = nullptr;
                }
            }

            beginTest ("A middle-button drag moves and trims nothing");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto& clip = s.clips (0)[0];
                const juce::Point<int> body (laneX (row, 0.5), 40);
                dragByPixels (row, body, body + juce::Point<int> (200, 0), juce::ModifierKeys::middleButtonModifier);
                expectEquals (clip.timelineStartSamples, (juce::int64) 0, "a middle-button drag moved the clip");
                const juce::Point<int> edge (laneX (row, 1.0) - 2, 40);
                dragByPixels (row, edge, edge + juce::Point<int> (-200, 0), juce::ModifierKeys::middleButtonModifier);
                expectEquals (clip.fileLengthSamples, (juce::int64) kRate, "a middle-button drag trimmed the clip");
            }
        }

        // The Delete prompt over a real main window. The answer runs later, and
        // the selection may change while the question waits.
        struct Show
        {
            juce::File    dir = tempDir ("delete");
            MainComponent main;
            AudioEngine&  engine { main.engine };
            std::vector<std::function<void()>> prompts;

            explicit Show (int samples)
            {
                engine.clearAllStripOverrides();
                engine.prepareForTests ((double) kRate, 512);
                engine.setStripCount (1);
                writeWav (takeIn (dir, 1), samples);
                engine.setActiveSessionDir (dir);
                engine.claimMixerFor (dir);
                engine.loadSession (dir);
                engine.setStripCount (1);
                main.currentView = MainComponent::View::Edit;
                main.confirmForTests = [this] (std::function<void()> confirm) { prompts.push_back (std::move (confirm)); };
                page().setBounds (0, 0, 1400, 500);
                page().refresh();
            }
            ~Show()
            {
                main.confirmForTests = nullptr;
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
            EditPage& page() { return *main.editPage; }
            juce::int64 clipSpan()
            {
                juce::int64 n = 0;
                for (const auto& c : engine.clipsFor (0)) n += c.fileLengthSamples;
                return n;
            }
        };

        void deleteActsOnWhatItAsked()
        {
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restore { [] { MainComponent::s_testConstruct = false; } };
            const juce::KeyPress shiftDelete (juce::KeyPress::deleteKey, juce::ModifierKeys::shiftModifier, 0);

            beginTest ("Shift+Delete asked about a clip deletes no range when the clip selection is gone");
            {
                Show s (kRate);
                s.page().setSelectedClip (0, 0);
                s.engine.getPlayer().selectRange (12000, 24000);
                expect (s.main.keyPressed (shiftDelete, nullptr), "setup: Delete handled");
                expectEquals ((int) s.prompts.size(), 1, "setup: the delete question");
                s.page().clearSelectedClip();   // the clip the question named is no longer selected
                if (! s.prompts.empty()) s.prompts.back()();
                expectEquals (s.clipSpan(), (juce::int64) kRate, "the answer cut the range instead of the clip it asked about");
            }

            beginTest ("Shift+Delete asked about a range deletes no clip selected while it waited");
            {
                Show s (kRate);
                s.page().clearSelectedClip();
                s.engine.getPlayer().selectRange (12000, 24000);
                expect (s.main.keyPressed (shiftDelete, nullptr), "setup: Delete handled");
                expectEquals ((int) s.prompts.size(), 1, "setup: the delete question");
                s.page().setSelectedClip (0, 0);
                if (! s.prompts.empty()) s.prompts.back()();
                expectEquals ((int) s.engine.clipsFor (0).size(), 1, "the answer deleted a clip it never asked about");
                expectEquals (s.clipSpan(), (juce::int64) kRate, "the answer changed the arrangement");
            }

            beginTest ("Delete still acts on the clip it asked about");
            {
                Show s (kRate);
                s.page().setSelectedClip (0, 0);
                expect (s.main.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey), nullptr));
                if (! s.prompts.empty()) s.prompts.back()();
                expectEquals ((int) s.engine.clipsFor (0).size(), 0, "the confirmed delete did not delete the clip");
            }
        }

        // ── EDIT-03 ──────────────────────────────────────────────────────────
        void trimDragReadsTheSourceOnce()
        {
            beginTest ("A trim drag reads the clip's own file once, not on every mouse event");
            {
                Session s (1, { kRate });
                const auto own = s.dir.getChildFile ("Audio Files/Bounce.wav");
                expect (writeWav (own, 2 * kRate), "setup: the clip's own file");
                auto& clip = s.clips (0)[0];
                clip.audioFile     = own;
                clip.sourceChannel = 0;
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);

                const juce::Point<int> grab (laneX (row, 1.0) - 2, 40);
                row.mouseDown (mouseAt (row, grab, grab, false));
                int dx = 1;
                for (; dx <= 10; ++dx)
                    row.mouseDrag (mouseAt (row, grab + juce::Point<int> (dx, 0), grab, true));
                expect (clip.fileLengthSamples > kRate, "setup: the trim extends the clip");
                // The drag measured the file when it began. Reading it again on
                // every event would find nothing now and stop the trim.
                expect (own.deleteFile(), "setup: the file goes away mid-drag");
                for (; dx <= 200; ++dx)
                    row.mouseDrag (mouseAt (row, grab + juce::Point<int> (dx, 0), grab, true));
                row.mouseUp (mouseAt (row, grab + juce::Point<int> (200, 0), grab, true));
                expectEquals (clip.fileLengthSamples, (juce::int64) (kRate + 200 * 48),
                              "the drag re-read the source file on a later mouse event");
            }

            beginTest ("A trim drag still stops at the end of the clip's own file");
            {
                Session s (1, { kRate });
                const auto own = s.dir.getChildFile ("Audio Files/Bounce.wav");
                expect (writeWav (own, kRate + 4800), "setup: the clip's own file, 0.1 s longer");
                auto& clip = s.clips (0)[0];
                clip.audioFile     = own;
                clip.sourceChannel = 0;
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const juce::Point<int> grab (laneX (row, 1.0) - 2, 40);
                dragByPixels (row, grab, grab + juce::Point<int> (300, 0));
                expectEquals (clip.fileLengthSamples, (juce::int64) (kRate + 4800), "the trim ran past the file's end");
            }
        }

        // ── EDIT-04 ──────────────────────────────────────────────────────────
        static void holdTake (Row& row, float level)
        {
            constexpr int bins = kRate / TrackState::kLiveBinSamples;
            row.setLiveRecording (true);
            for (int i = 0; i < bins; ++i) row.appendLivePeakL (level);
            row.holdLiveEnvelopeUntilScanned();
        }

        void heldTakeStaysInItsSession()
        {
            for (const bool condemned : { true, false })
            {
                beginTest (condemned ? "A held take does not follow its strip id into another session's new rows"
                                     : "A held take does not stay on a row reused by another session");
                Session s (1, { kRate });
                const auto other = tempDir ("other");
                Session::write (other, { kRate }, sine);
                {
                    EditPage page (s.engine);
                    page.setBounds (0, 0, 1400, 500);
                    page.refresh();
                    auto rows = rowsOf (page);
                    expectEquals ((int) rows.size(), 1, "setup: one row");
                    if (rows.size() == 1)
                    {
                        holdTake (*rows[0], 0.9f);
                        expect (rows[0]->isHoldingLiveEnvelope(), "setup: the row holds the take");
                        const auto stripKey = rows[0]->getStripKey();

                        s.open (other);
                        if (condemned) page.condemnAllRows();
                        page.refresh();
                        rows = rowsOf (page);
                        expectEquals ((int) rows.size(), 1, "setup: one row in the other session");
                        if (rows.size() == 1)
                        {
                            expectEquals (rows[0]->getStripKey(), stripKey, "setup: the strip id repeats across sessions");
                            expect (! rows[0]->isHoldingLiveEnvelope(),
                                    "the other session's row shows the last session's take");
                        }
                    }
                }
                other.deleteRecursively();
            }

            beginTest ("A held take still follows its strip into a new row of its own session");
            {
                Session s (1, { kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                if (rows.size() == 1)
                {
                    holdTake (*rows[0], 0.9f);
                    page.condemnAllRows();
                    page.refresh();
                    rows = rowsOf (page);
                    expect (rows.size() == 1 && rows[0]->isHoldingLiveEnvelope(),
                            "a row rebuilt in the same session dropped the held take");
                }
                else expect (false, "setup: one row");
            }
        }

        // ── EDIT-05 ──────────────────────────────────────────────────────────
        void lockedPeerStopsFadeDrags()
        {
            beginTest ("A fade-handle drag changes no clip of the group when one peer clip is locked");
            for (const bool fadeIn : { true, false })
            {
                Session s (2, { kRate, kRate });
                s.group ({ 0, 1 });
                for (int t : { 0, 1 })
                    expect (s.engine.setClipFades (t, 0, fadeIn ? 4800 : 0, fadeIn ? 0 : 4800), "setup: a fade on each");
                expect (s.engine.setClipLocked (1, 0, true), "setup: the peer locked");
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const juce::Point<int> grip (laneX (row, fadeIn ? 0.1 : 0.9), brand::space::sm + 3);
                dragByPixels (row, grip, grip + juce::Point<int> (fadeIn ? 60 : -60, 0));
                for (int t : { 0, 1 })
                {
                    const auto& c = s.clips (t)[0];
                    expectEquals (fadeIn ? c.fadeInSamples : c.fadeOutSamples, (juce::int64) 4800,
                                  juce::String (fadeIn ? "fade-in" : "fade-out") + " drag changed track "
                                      + juce::String (t) + " beside a locked peer");
                }
            }

            beginTest ("A crossfade drag changes no clip of the group when one peer clip is locked");
            {
                Session s (2, { kRate, kRate });
                s.group ({ 0, 1 });
                for (int t : { 0, 1 })
                {
                    expect (s.engine.splitTrackAtSample (t, 24000));
                    expect (s.engine.editClip (t, 1, AudioEngine::ClipEdit::Move, -4800));
                }
                expect (s.engine.setClipLocked (1, 1, true), "setup: the peer's incoming clip locked");
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto dot = crossfadeDot (row, s);
                dragByPixels (row, dot, dot + juce::Point<int> (20, 0));
                for (int t : { 0, 1 })
                    for (const auto& c : s.clips (t))
                    {
                        expectEquals (c.fadeInSamples,  (juce::int64) 0, "a crossfade landed on track " + juce::String (t));
                        expectEquals (c.fadeOutSamples, (juce::int64) 0, "a crossfade landed on track " + juce::String (t));
                    }
            }
        }

        // ── EDIT-06 ──────────────────────────────────────────────────────────
        // The clip-gain handle: the bottom-left corner of the block.
        static juce::Point<int> gainHandle (const Row& row)
        {
            return { laneX (row, 0.0) + 30, row.getHeight() - brand::space::sm - 12 };
        }

        void lockProtectsClipGain()
        {
            beginTest ("The engine refuses clip gain on a locked clip");
            {
                Session s (1, { kRate });
                expect (s.engine.setClipLocked (0, 0, true));
                expect (! s.engine.setClipGainDb (0, 0, -6.0f), "a locked clip took a new gain");
                expectWithinAbsoluteError (s.clips (0)[0].gainDb, 0.0f, 0.0001f, "a locked clip's gain changed");
                expect (s.engine.setClipLocked (0, 0, false));
                expect (s.engine.setClipGainDb (0, 0, -6.0f), "an unlocked clip refused its gain");
            }

            beginTest ("A gain preset, ride or Alt reset changes no clip of the group when one peer is locked");
            {
                Session s (2, { kRate, kRate });
                s.group ({ 0, 1 });
                for (int t : { 0, 1 }) expect (s.engine.setClipGainDb (t, 0, -3.0f), "setup: a gain on each");
                expect (s.engine.setClipLocked (1, 0, true), "setup: the peer locked");
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto gainsStay = [&] (const juce::String& what)
                {
                    for (int t : { 0, 1 })
                        expectWithinAbsoluteError (s.clips (t)[0].gainDb, -3.0f, 0.0001f,
                                                   what + " changed track " + juce::String (t) + "'s gain");
                };

                row.applyClipMenuChoice (0, 502);   // -6 dB
                gainsStay ("the -6 dB preset");

                const auto handle = gainHandle (row);
                dragByPixels (row, handle, handle + juce::Point<int> (0, -40));
                gainsStay ("a gain ride");

                const int alt = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier;
                row.mouseDown (mouseAt (row, handle, handle, false, alt));
                row.mouseUp (mouseAt (row, handle, handle, false, alt));
                gainsStay ("an Alt reset");

                expect (s.engine.setClipLocked (1, 0, false), "setup: the peer unlocked");
                row.applyClipMenuChoice (0, 502);
                for (int t : { 0, 1 })
                    expectWithinAbsoluteError (s.clips (t)[0].gainDb, -6.0f, 0.0001f, "an unlocked group lost its preset");
            }
        }

        // ── EDIT-07 ──────────────────────────────────────────────────────────
        void clipDrawsItsOwnChannel()
        {
            beginTest ("A clip on the row's own stereo file draws the channel it plays");
            {
                Session s (1, { kRate });
                // Left loud, right silent. The row is the right half of an
                // unlinked pair: its own take is channel 1 of this file.
                const auto shared = s.dir.getChildFile ("Audio Files/Pair.wav");
                expect (writeWav (shared, kRate, 2, [] (int ch, int i) { return ch == 0 ? sine (ch, i) : 0.0f; }),
                        "setup: the stereo file");
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                row.setWaveformFiles (shared, {}, 1);
                expect (pumpUntil ([&] { return row.waveformsLoaded() && row.getWaveformSecondsForTests() > 0.5; }, 5000),
                        "setup: the file never scanned");
                auto& clip = s.clips (0)[0];
                clip.audioFile = shared;
                clip.sourceChannel = 1;
                const auto right = render (row);
                clip.sourceChannel = 0;   // pasted from the left row: plays channel 0
                const auto left = render (row);
                expect (differing (left, right, laneX (row, 0.1), laneX (row, 0.9)) > 200,
                        "a clip that plays the left channel draws the row's right channel");
            }
        }

        // ── EDIT-08 ──────────────────────────────────────────────────────────
        void wheelZoomFollowsTheWheel()
        {
            beginTest ("Cmd-wheel zoom moves with the axis that carries the wheel, and a zero step zooms nothing");
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
                    const auto wheel = [&] (int mods, float dx, float dy)
                    {
                        juce::MouseWheelDetails w;
                        w.deltaX = dx; w.deltaY = dy; w.isReversed = false; w.isSmooth = false; w.isInertial = false;
                        const juce::Point<int> at (laneX (row, 0.5), 40);
                        row.mouseWheelMove (mouseAt (row, at, at, false, mods), w);
                    };
                    const int cmd = juce::ModifierKeys::commandModifier;
                    const int cmdShift = cmd | juce::ModifierKeys::shiftModifier;

                    page.setVerticalZoom (2.0f);
                    page.wheelZoomVertical (0.0f);
                    expectWithinAbsoluteError (page.getVerticalZoom(), 2.0f, 0.0001f, "a zero step zoomed the waveforms");
                    wheel (cmdShift, 0.25f, 0.0f);   // macOS turns Shift+wheel into a horizontal scroll
                    expect (page.getVerticalZoom() > 2.0f, "Cmd+Shift+wheel up did not zoom the waveforms in: "
                                                               + juce::String (page.getVerticalZoom()));
                    page.setVerticalZoom (2.0f);
                    wheel (cmdShift, -0.25f, 0.0f);
                    expect (page.getVerticalZoom() < 2.0f, "Cmd+Shift+wheel down did not zoom the waveforms out");
                    page.setVerticalZoom (2.0f);
                    wheel (cmdShift, 0.0f, 0.0f);
                    expectWithinAbsoluteError (page.getVerticalZoom(), 2.0f, 0.0001f, "a still wheel zoomed the waveforms");

                    page.setZoom (4.0f);
                    page.wheelZoomHorizontal (0.0f);
                    expectWithinAbsoluteError (page.getZoom(), 4.0f, 0.0001f, "a zero step zoomed the timeline");
                    wheel (cmd, 0.25f, 0.0f);         // Cmd + a sideways swipe
                    expect (page.getZoom() > 4.0f, "Cmd + a sideways swipe zoomed the timeline out: "
                                                       + juce::String (page.getZoom()));
                    page.setZoom (4.0f);
                    wheel (cmd, 0.0f, 0.25f);
                    expect (page.getZoom() > 4.0f, "Cmd+wheel up no longer zooms the timeline in");
                }
            }
        }

        // ── EDIT-09 ──────────────────────────────────────────────────────────
        void fitToWindowFillsTheView()
        {
            beginTest ("Fit-to-window rows fill the view beside a custom-height row, and hidden rows take no room");
            {
                Session s (3, { kRate, kRate, kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 900);
                page.refresh();
                auto rows = rowsOf (page);
                expectEquals ((int) rows.size(), 3, "setup: three rows");
                if (rows.size() == 3)
                {
                    auto* viewport = rows[0]->findParentComponentOfClass<juce::Viewport>();
                    expect (viewport != nullptr, "setup: the rows' viewport");
                    if (viewport == nullptr) return;
                    const int viewH = viewport->getHeight();
                    constexpr int gap = brand::space::sm;
                    rows[0]->setRowSize (Row::Size::Custom);
                    rows[0]->setCustomHeight (300);
                    for (int i : { 1, 2 }) rows[(size_t) i]->setRowSize (Row::Size::FitToWindow);
                    rows[0]->onSizeChosen (*rows[0], Row::Size::Custom);   // the list lays the rows out again

                    expectEquals (rows[0]->getHeight(), 300, "setup: the custom row");
                    const int used = rows[0]->getHeight() + rows[1]->getHeight() + rows[2]->getHeight() + 2 * gap;
                    expect (std::abs (used - viewH) <= 2, "the rows use " + juce::String (used) + " px of a "
                                                              + juce::String (viewH) + " px view");

                    page.setLogicalRowsVisible ({ 0, 1 });
                    expect (! rows[2]->isVisible(), "setup: the third row hidden");
                    const int shown = rows[0]->getHeight() + rows[1]->getHeight() + gap;
                    expect (std::abs (shown - viewH) <= 2, "with a row hidden the shown rows use " + juce::String (shown)
                                                               + " px of a " + juce::String (viewH) + " px view");
                    expect (rows[2]->getHeight() == 0 || rows[2]->getY() >= rows[1]->getBottom(),
                            "the hidden row sits between shown rows");
                    page.setLogicalRowsVisible ({});
                }
            }
        }

        // ── EDIT-10 ──────────────────────────────────────────────────────────
        void toolsAndMinimapIgnoreMenuPresses()
        {
            const int menuButtons[] { juce::ModifierKeys::rightButtonModifier, juce::ModifierKeys::middleButtonModifier,
                                      juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::leftButtonModifier };

            beginTest ("A right or middle click on a tool button picks no tool");
            {
                EditToolsBar tools;
                tools.setBounds (0, 0, 400, 40);
                tools.setTool (EditToolsBar::Tool::Smart);
                int buttons = 0;
                for (auto* child : tools.getChildren())
                {
                    ++buttons;
                    const juce::Point<int> at (child->getWidth() / 2, child->getHeight() / 2);
                    for (const int b : menuButtons)
                    {
                        child->mouseDown (mouseAt (*child, at, at, false, b));
                        child->mouseUp (mouseAt (*child, at, at, false, b));
                        expect (tools.getTool() == EditToolsBar::Tool::Smart,
                                "a menu press on a tool button picked a tool (buttons " + juce::String (b) + ")");
                        tools.setTool (EditToolsBar::Tool::Smart);
                    }
                }
                expect (buttons >= 5, "setup: the tool buttons");
                // A left click still picks.
                if (auto* last = tools.getChildren().getLast())
                {
                    const juce::Point<int> at (last->getWidth() / 2, last->getHeight() / 2);
                    last->mouseDown (mouseAt (*last, at, at, false));
                    last->mouseUp (mouseAt (*last, at, at, false));
                    expect (tools.getTool() != EditToolsBar::Tool::Smart, "a left click picks no tool");
                }
            }

            beginTest ("A right or middle press on the minimap scrolls nothing");
            {
                TimelineMinimap minimap;
                minimap.setSize (1000, 16);
                minimap.setView (5000, brand::space::editHeaderW, 0, 1400);
                int scrolls = 0;
                minimap.onScrollToContentX = [&scrolls] (int) { ++scrolls; };
                const juce::Point<int> at (700, 8);
                for (const int b : menuButtons)
                {
                    minimap.mouseDown (mouseAt (minimap, at, at, false, b));
                    minimap.mouseDrag (mouseAt (minimap, at + juce::Point<int> (50, 0), at, true, b));
                    minimap.mouseUp (mouseAt (minimap, at + juce::Point<int> (50, 0), at, true, b));
                }
                expectEquals (scrolls, 0, "a menu press scrolled the timeline");
                minimap.mouseDown (mouseAt (minimap, at, at, false));
                expectEquals (scrolls, 1, "a left press no longer scrolls");
            }
        }

        // ── EDIT-11 ──────────────────────────────────────────────────────────
        void continueDrawsTheArrangement()
        {
            beginTest ("During a continue take the dim take is drawn clip by clip where it plays");
            {
                // Loud first half, silent second half. Keep only the loud half
                // and move it to 0.5 s: the arrangement is silent, then loud.
                Session s (1, { kRate }, [] (int ch, int i) { return i < kRate / 2 ? sine (ch, i) : 0.0f; });
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.deleteClip (0, 1), "setup: the silent half deleted");
                expect (s.engine.editClip (0, 0, AudioEngine::ClipEdit::Move, 24000), "setup: the loud half moved right");
                expectEquals (s.engine.getPlayer().getTotalLengthSamples(), (juce::int64) kRate, "setup: the timeline");

                Row live (0, false, s.engine, s.formats, s.cache);
                Row bare (0, false, s.engine, s.formats, s.cache);   // no take scanned: no dim backdrop
                sizeRow (live);
                sizeRow (bare);
                live.setWaveformFiles (takeIn (s.dir, 1), {});
                expect (pumpUntil ([&] { return live.waveformsLoaded() && live.getWaveformSecondsForTests() > 0.5; }, 5000),
                        "setup: the take never scanned");
                // A continue: the capture starts after the arrangement's end.
                constexpr juce::int64 prefill = kRate / TrackState::kLiveBinSamples;
                for (Row* r : { &live, &bare })
                {
                    r->setLiveRecording (true, prefill);
                    for (int i = 0; i < 40; ++i) r->appendLivePeakL (0.0f);
                }
                const auto withTake = render (live);
                const auto without  = render (bare);
                const double total = (double) (prefill + 40) * TrackState::kLiveBinSamples;
                const auto xAt = [&] (juce::int64 sample)
                { return live.getHeaderWidth() + brand::space::xs + (int) ((double) sample / total * kLaneW); };
                expectEquals (differing (withTake, without, xAt (4000), xAt (20000)), 0,
                              "the dim take draws audio where no clip plays");
                // Loud audio reaches well away from the lane's centre line.
                expect (differing (withTake, without, xAt (28000), xAt (44000), 50, 8) > 200,
                        "the dim take does not draw the moved clip where it plays");
            }
        }
    };

    static Fix20261124EditTests fix20261124EditTests;
}
