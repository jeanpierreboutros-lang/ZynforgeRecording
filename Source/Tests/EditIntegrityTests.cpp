// EDIT view integrity: every clip command reaches the whole logical strip and
// its edit group, a selection names the clip it names, and undo covers what the
// lanes change. Driven through MainComponent's real commands and the row's real
// mouse handlers.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../UI/ClipEditTargets.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"
#include "../UI/MainComponent.h"
#include "../UI/WaveCacheFile.h"

#include <cmath>
#include <functional>
#include <set>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class EditIntegrityTests final : public juce::UnitTest
    {
    public:
        EditIntegrityTests() : juce::UnitTest ("EDIT integrity", "zynforge") {}

        using Lane = EditPage::TrackRow::LaneMode;
        using Ref  = EditPage::ClipRef;

        static bool writeWav (const juce::File& file, int samples, int channels = 1, float level = 0.8f)
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
                    audio.setSample (c, i, level * std::sin ((float) i * 0.05f));
            return writer->writeFromAudioSampleBuffer (audio, 0, samples);
        }

        // A real MainComponent on a real session folder, showing EDIT.
        struct Show
        {
            juce::File    dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("zf-editintegrity-" + juce::Uuid().toString());
            MainComponent main;
            AudioEngine&  engine { main.engine };
            juce::AudioFormatManager  formats;
            juce::AudioThumbnailCache cache { 8 };
            std::vector<std::function<void()>> prompts;   // delete prompts awaiting an answer

            Show (int strips, std::vector<int> lengths, float level = 0.8f)
            {
                formats.registerBasicFormats();
                engine.clearAllStripOverrides();
                engine.prepareForTests (48000.0, 512);
                engine.setStripCount (strips);
                dir.getChildFile ("Audio Files").createDirectory();
                for (size_t i = 0; i < lengths.size(); ++i)
                    if (lengths[i] > 0) writeWav (take ((int) i + 1), lengths[i], 1, level);
                engine.setActiveSessionDir (dir);
                engine.claimMixerFor (dir);
                engine.loadSession (dir);
                engine.setStripCount (strips);
                main.currentView = MainComponent::View::Edit;
                main.confirmForTests = [this] (std::function<void()> confirm)
                { prompts.push_back (std::move (confirm)); };
                page().setBounds (0, 0, 1400, 500);
                page().refresh();
            }
            ~Show()
            {
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
            juce::File take (int n) const
            {
                return dir.getChildFile ("Audio Files/Track_" + juce::String::formatted ("%02d.wav", n));
            }
            EditPage& page() { return *main.editPage; }
            std::vector<Clip>& clips (int track) { return engine.clipsFor (track); }
            void splitBoth (int a, int b, juce::int64 at)
            {
                engine.splitTrackAtSample (a, at);
                engine.splitTrackAtSample (b, at);
            }
        };

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
            return predicate();
        }

        static juce::MouseEvent mouseAt (juce::Component& c, juce::Point<int> p, juce::Point<int> down,
                                         bool dragged, bool shift = false)
        {
            const auto now = juce::Time::getCurrentTime();
            const int flags = juce::ModifierKeys::leftButtonModifier
                            | (shift ? (int) juce::ModifierKeys::shiftModifier : 0);
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), juce::ModifierKeys (flags),
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, down.toFloat(), now, 1, dragged };
        }

        static void click (juce::Component& c, juce::Point<int> p, bool shift = false)
        {
            c.mouseDown (mouseAt (c, p, p, false, shift));
            c.mouseUp   (mouseAt (c, p, p, false, shift));
        }

        static void drag (juce::Component& c, juce::Point<int> from, juce::Point<int> to)
        {
            c.mouseDown (mouseAt (c, from, from, false));
            const int steps = juce::jmax (1, (int) (from.getDistanceFrom (to) / 4));
            for (int s = 1; s <= steps; ++s)
                c.mouseDrag (mouseAt (c, from + (to - from) * s / steps, from, true));
            c.mouseUp (mouseAt (c, to, from, true));
        }

        static void pressButton (juce::Component& parent, const juce::String& text)
        {
            for (auto* child : parent.getChildren())
                if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getButtonText() == text)
                    if (b->onClick) b->onClick();
        }

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

        // A 1000 px inner lane, so one pixel is a known number of samples.
        static constexpr int kLaneW = 1000;
        static void sizeRow (EditPage::TrackRow& row, int height = 100)
        {
            row.setSize (row.getHeaderWidth() + kLaneW + 8, height);
        }
        static int laneX (const EditPage::TrackRow& row, double frac)
        {
            return row.getHeaderWidth() + 4 + (int) std::lround (frac * kLaneW);
        }

        static juce::String spans (const std::vector<Clip>& list)
        {
            juce::StringArray out;
            for (const auto& c : list)
                out.add (juce::String (c.timelineStartSamples) + "+" + juce::String (c.fileLengthSamples));
            return out.joinIntoString (" ");
        }

        static Clip clipAt (juce::int64 start, juce::int64 length)
        {
            Clip c;
            c.timelineStartSamples = start;
            c.fileLengthSamples    = length;
            return c;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;

            beginTest ("Peer clips match by span, then parallel index, then overlap");
            {
                const std::vector<Clip> longAndShort { clipAt (0, 48000), clipAt (10000, 2000) };
                expectEquals (matchingClipIndex (longAndShort, 1, longAndShort, true), 1,
                              "a short clip inside a long one found the long one");
                const std::vector<Clip> stacked { clipAt (0, 1000), clipAt (0, 1000) };
                expectEquals (matchingClipIndex (stacked, 1, stacked, true), 1, "stacked twins pair in order");
                const std::vector<Clip> left { clipAt (5000, 1000) }, right { clipAt (0, 1000) };
                expectEquals (matchingClipIndex (left, 0, right, true), 0, "a drifted pair keeps its index");
                expectEquals (matchingClipIndex (left, 0, right, false), -1, "a peer with nothing there is skipped");
                const std::vector<Clip> overlapped { clipAt (0, 10000), clipAt (9000, 2000) };
                expectEquals (clipIndexAtMidpoint (overlapped, 9500, 1000), 1,
                              "of two clips under the midpoint the one sharing the span wins");
            }

            beginTest ("Ripple delete moves both halves of a stereo clip together");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.splitBoth (0, 1, 16000);
                s.splitBoth (0, 1, 32000);
                s.page().setSelectedClip (0, 1);
                s.main.editRippleDelete();
                expectEquals (spans (s.clips (0)), juce::String ("0+16000 16000+16000"));
                expectEquals (spans (s.clips (1)), spans (s.clips (0)), "R did not follow L");
            }

            beginTest ("Ripple delete closes the same gap on every edit-group member");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                s.engine.splitTrackAtSample (0, 24000);
                s.engine.splitTrackAtSample (1, 12000);
                s.page().setSelectedClips ({ { 0, 0 }, { 1, 1 } }, { 0, 0 });
                s.main.editRippleDelete();
                expectEquals (spans (s.clips (0)), juce::String ("0+24000"));
                expectEquals (spans (s.clips (1)), juce::String ("0+24000"),
                              "the peer must lose the clicked clip's span, whatever its own splits");
            }

            beginTest ("A ripple of several clips closes every gap as one undo step");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.splitBoth (0, 1, 16000);
                s.splitBoth (0, 1, 32000);
                const auto before = spans (s.clips (0));
                s.main.undoManager.clearUndoHistory();
                s.page().setSelectedClips ({ { 0, 0 }, { 0, 2 } }, { 0, 0 });
                s.main.editRippleDelete();
                expectEquals (spans (s.clips (0)), juce::String ("0+16000"), "both gaps must close");
                expectEquals (spans (s.clips (1)), spans (s.clips (0)), "R did not follow L");
                expect (pumpUntil ([&] { return ! s.main.sessionIoBusy.load(); }, 5000), "setup: the save lands");
                s.main.editUndo();
                expectEquals (spans (s.clips (0)), before, "one Cmd+Z must bring every clip back");
                expectEquals (spans (s.clips (1)), before);
                expect (! s.main.undoManager.canUndo(), "the ripple left a second step");
            }

            beginTest ("Duplicate copies both halves and follows both copies");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.splitBoth (0, 1, 24000);
                s.page().setSelectedClip (0, 0);
                s.main.editDuplicateSelectedClip();
                expectEquals ((int) s.clips (0).size(), 3);
                expectEquals (spans (s.clips (1)), spans (s.clips (0)), "the copy played on L only");
                const std::set<Ref> copies { { 0, 1 }, { 1, 1 } };
                expect (s.page().getSelectedClips() == copies, "the selection must follow both copies");

                s.main.editDeleteSelectedClip();
                expectEquals ((int) s.clips (0).size(), 2);
                expectEquals ((int) s.clips (1).size(), 2, "Delete on the copy left R behind");
            }

            beginTest ("Duplicate of a multi-selection keeps every copy's index right");
            {
                Show s (1, { 48000 });
                s.engine.splitTrackAtSample (0, 16000);
                s.engine.splitTrackAtSample (0, 32000);
                s.page().setSelectedClips ({ { 0, 0 }, { 0, 2 } }, { 0, 2 });
                s.main.editDuplicateSelectedClip();
                expectEquals ((int) s.clips (0).size(), 5);
                const std::set<Ref> copies { { 0, 1 }, { 0, 4 } };
                expect (s.page().getSelectedClips() == copies);
                expectEquals (s.page().getSelectedClipIndex(), 4, "the primary follows its own copy");
            }

            beginTest ("Shift+click takes the R half and edit-group peers in and out together");
            {
                Show s (4, { 48000, 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                s.engine.setTrackEditGroup (2, 0);
                s.page().refresh();
                auto rows = rowsOf (s.page());
                expect (rows.size() == 3, "setup: three rows");
                if (rows.size() == 3)
                {
                    auto& row = *rows[0];
                    const juce::Point<int> body (row.getHeaderWidth() + (row.getWidth() - row.getHeaderWidth()) / 2,
                                                 row.getHeight() / 2);
                    click (row, body, true);
                    const std::set<Ref> whole { { 0, 0 }, { 1, 0 }, { 2, 0 } };
                    expect (s.page().getSelectedClips() == whole, "Shift+click selected one half");

                    s.main.editDeleteSelectedClip();
                    for (int t : { 0, 1, 2 })
                        expect (s.clips (t).empty(), "Delete left a half playing on track " + juce::String (t));
                    s.main.editUndo();

                    click (row, body, true);
                    click (row, body, true);
                    expect (s.page().getSelectedClips().empty(), "Shift+click must take all of it out again");
                    expectEquals (s.page().getSelectedClipTrack(), -1);
                }
            }

            beginTest ("Removing the primary clip from a selection hands the primary on");
            {
                Show s (2, { 48000, 48000 });
                s.page().setSelectedClips ({ { 0, 0 }, { 1, 0 } }, { 1, 0 });
                s.page().toggleSelectedClips ({ { 1, 0 } }, { 1, 0 });
                expectEquals (s.page().getSelectedClipTrack(), 0, "the primary named a clip no longer selected");
            }

            beginTest ("Nudge moves a stereo clip and its peers by one delta");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                s.splitBoth (0, 1, 24000);
                expect (s.engine.editClip (0, 0, AudioEngine::ClipEdit::Move, 1000));
                const auto moved = spans (s.clips (0));
                s.page().setSelectedClip (0, 0);
                s.main.editNudgeSelectedClip (-1);   // the peer is already at zero
                expectEquals (spans (s.clips (0)), moved, "one peer at zero must hold the other");

                s.engine.setClipLocked (1, 1, true);
                s.page().setSelectedClip (0, 1);
                s.main.editNudgeSelectedClip (+1);
                expectEquals (spans (s.clips (0)), moved, "a locked peer must hold the clip it follows");
            }

            beginTest ("A locked half refuses the whole delete, duplicate and ripple");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.splitBoth (0, 1, 24000);
                s.engine.setClipLocked (1, 0, true);
                const auto before = spans (s.clips (0));
                s.page().setSelectedClip (0, 0);
                s.main.editDeleteSelectedClip();
                s.main.editDuplicateSelectedClip();
                s.main.editRippleDelete();
                expectEquals (spans (s.clips (0)), before);
                expectEquals (spans (s.clips (1)), before);
            }

            beginTest ("A clip dragged over its neighbour keeps its own twin on the R half");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.splitBoth (0, 1, 16000);
                s.splitBoth (0, 1, 32000);
                EditPage::TrackRow row (0, true, s.engine, s.formats, s.cache);
                sizeRow (row);
                drag (row, { laneX (row, 0.9), 50 }, { laneX (row, 0.65), 50 });
                expect (s.clips (0)[2].timelineStartSamples < 24000, "setup: most of C now overlaps B");
                expectEquals (spans (s.clips (1)), spans (s.clips (0)), "R's neighbour moved instead of its C");
            }

            beginTest ("Deleting a short clip pasted inside a long one takes only its own twin");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                for (int t : { 0, 1 })
                    expectEquals (s.engine.pasteClip (t, 10000, 0, 2000, 0, 0, 0.0f, "short", s.take (t + 1), 0, 0), 1);
                EditPage::TrackRow row (0, true, s.engine, s.formats, s.cache);
                const std::vector<std::pair<int, int>> twins { { 0, 1 }, { 1, 1 } };
                expect (row.peerClipTargets (1) == twins, "the R half's long clip was picked");
                s.page().setSelectedClip (0, 1);
                s.main.editDeleteSelectedClip();
                expectEquals (spans (s.clips (1)), juce::String ("0+48000"), "R lost the long clip");
            }

            beginTest ("Cmd+X never removes a channel unasked, and a range outranks the strip");
            {
                Show s (3, { 48000, 48000, 48000 });
                s.main.selectedLogical = { 1 };
                s.main.editClipboardCut (true);
                expectEquals (s.engine.getRecorder().getNumTracks(), 3, "the channel went with no prompt");
                expectEquals ((int) s.prompts.size(), 1, "the cut must ask first");
                s.prompts.clear();

                s.engine.getPlayer().setLoopRegion (12000, 24000);
                s.main.editClipboardCut (true);
                expectEquals ((int) s.prompts.size(), 1);
                if (! s.prompts.empty()) s.prompts.back()();
                s.prompts.clear();
                expectEquals (s.engine.getRecorder().getNumTracks(), 3, "the range cut took the channel");
                expectEquals (spans (s.clips (1)), juce::String ("0+12000 24000+24000"), "the range was not cleared");
                s.engine.getPlayer().clearLoopRegion();

                s.main.editCutSelected (true);       // Track menu > Cut
                expectEquals (s.engine.getRecorder().getNumTracks(), 3);
                expectEquals ((int) s.prompts.size(), 1, "the menu cut must ask too");
                if (! s.prompts.empty()) s.prompts.back()();
                expectEquals (s.engine.getRecorder().getNumTracks(), 2, "a confirmed cut removes the channel");
            }

            beginTest ("A selection stays on its clip when the list shifts");
            {
                Show s (1, { 48000 });
                s.engine.splitTrackAtSample (0, 16000);
                s.engine.splitTrackAtSample (0, 32000);
                auto rows = rowsOf (s.page());
                expect (rows.size() == 1, "setup: one row");
                s.page().setSelectedClip (0, 2);
                if (! rows.empty()) rows[0]->applyClipMenuChoice (0, 411);   // right-click Delete, earlier clip
                expectEquals (s.page().getSelectedClipIndex(), 1, "the selection named another clip");
                expect (s.page().isClipSelected (0, s.clips (0)[1]), "the highlight must stay on the clip");
                expect (! s.page().isClipSelected (0, s.clips (0)[0]));

                s.main.editUndo();
                expect (s.page().getSelectedClips().empty(), "undo replaced the clip, so nothing is selected");
            }

            beginTest ("Delete after an undone duplicate takes nothing");
            {
                Show s (1, { 48000 });
                s.engine.splitTrackAtSample (0, 24000);
                s.page().setSelectedClip (0, 0);
                s.main.editDuplicateSelectedClip();
                s.main.editUndo();
                expect (s.page().getSelectedClips().empty());
                s.main.editDeleteSelectedClip();
                expectEquals ((int) s.clips (0).size(), 2, "Delete removed the clip after the original");
            }

            beginTest ("A clip healed away or split leaves the selection, and its pieces can be selected");
            {
                Show s (1, { 48000 });
                s.engine.splitTrackAtSample (0, 24000);
                s.page().setSelectedClip (0, 1);
                expect (s.engine.healSeparationAt (0, 24000));
                expect (s.page().getSelectedClips().empty(), "the healed clip is gone");

                s.page().setSelectedClip (0, 0);
                expect (s.engine.splitTrackAtSample (0, 12000));
                expect (s.page().getSelectedClips().empty(), "a split clip is two clips");
                s.page().setSelectedClip (0, 1);
                expectEquals ((int) s.page().getSelectedClips().size(), 1, "a piece must select on its own");
                expectEquals (s.page().getSelectedClipIndex(), 1);
                expect (! s.page().isClipSelected (0, s.clips (0)[0]));
            }

            beginTest ("A clip with its own file draws that file, not the strip's take");
            {
                Show s (2, { 96000 }, 0.0f);    // the take itself is silent
                const auto loud  = s.dir.getChildFile ("Audio Files/Track_01_consolidated_1.wav");
                const auto quiet = s.dir.getChildFile ("Audio Files/Track_01_consolidated_2.wav");
                expect (writeWav (loud, 96000, 1, 0.8f) && writeWav (quiet, 96000, 1, 0.0f));
                expect (s.engine.deleteClip (0, 0), "setup: empty the take");

                auto render = [&] (EditPage::TrackRow& row)
                {
                    const auto draw = [&row]
                    {
                        juce::Image img (juce::Image::ARGB, row.getWidth(), row.getHeight(), true);
                        juce::Graphics g (img);
                        row.paint (g);
                        return img;
                    };
                    draw();    // the first paint opens the clip's file
                    expect (pumpUntil ([&] { return row.waveformsLoaded(); }, 5000), "thumbnail scan");
                    return draw();
                };
                auto differs = [] (const juce::Image& a, const juce::Image& b, int x0, int x1)
                {
                    int count = 0;
                    for (int x = x0; x < x1; ++x)
                        for (int y = 20; y < a.getHeight() - 8; ++y)
                            if (a.getPixelAt (x, y) != b.getPixelAt (x, y)) ++count;
                    return count;
                };

                EditPage::TrackRow row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                row.setWaveformFiles (s.take (1), {});
                expect (s.engine.pasteClip (0, 0, 0, 96000, 0, 0, 0.0f, "c", quiet, -1, 0) == 0);
                const auto silent = render (row);
                expect (s.engine.deleteClip (0, 0));
                expect (s.engine.pasteClip (0, 0, 0, 96000, 0, 0, 0.0f, "c", loud, -1, 0) == 0);
                const auto drawn = render (row);
                expect (differs (drawn, silent, laneX (row, 0.2), laneX (row, 0.8)) > 1000,
                        "the block drew the silent take instead of the clip's file");

                // A clip pasted onto a strip with no take of its own still draws.
                EditPage::TrackRow empty (1, false, s.engine, s.formats, s.cache);
                sizeRow (empty);
                expect (s.engine.pasteClip (1, 0, 0, 96000, 0, 0, 0.0f, "c", quiet, -1, 0) == 0);
                const auto emptySilent = render (empty);
                expect (s.engine.deleteClip (1, 0));
                expect (s.engine.pasteClip (1, 0, 0, 96000, 0, 0, 0.0f, "c", loud, -1, 0) == 0);
                const auto emptyDrawn = render (empty);
                expect (differs (emptyDrawn, emptySilent, laneX (empty, 0.2), laneX (empty, 0.8)) > 1000,
                        "a strip with no take drew nothing for a pasted clip");
            }

            beginTest ("A strip move replaces the saved peaks");
            {
                Show s (2, { 48000 });
                expect (writeWav (s.take (2), 48000, 1, 0.1f));
                s.engine.loadSession (s.dir);
                s.page().refresh();
                const auto cacheFile = wavecache::fileIn (s.dir);
                expect (pumpUntil ([&] { return cacheFile.existsAsFile(); }, 10000), "the cache was never saved");
                juce::MemoryBlock before;
                expect (cacheFile.loadFileAsData (before));

                expect (s.engine.reorderTracks ({ 1, 0 }));
                const auto replaced = [&]
                {
                    juce::MemoryBlock now;
                    return ! cacheFile.existsAsFile()
                        || (cacheFile.loadFileAsData (now) && now != before);
                };
                expect (pumpUntil (replaced, 10000), "reopening would draw each channel's peaks on the other");
                expect (pumpUntil ([&] { return cacheFile.existsAsFile(); }, 10000),
                        "the rescan must save the new peaks");
                juce::MemoryBlock after;
                expect (cacheFile.loadFileAsData (after) && after != before, "the saved peaks are still the old ones");
            }

            beginTest ("A failed cache write leaves the good cache alone");
            {
                struct ShortStream final : public juce::OutputStream
                {
                    juce::int64 room, written { 0 };
                    explicit ShortStream (juce::int64 capacity) : room (capacity) {}
                    void flush() override {}
                    bool setPosition (juce::int64) override { return false; }
                    juce::int64 getPosition() override { return written; }
                    bool write (const void*, size_t count) override
                    {
                        if (written + (juce::int64) count > room) return false;
                        written += (juce::int64) count;
                        return true;
                    }
                };

                Show s (1, { 48000 });
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                wavecache::Caches caches;
                caches.push_back (std::make_unique<juce::AudioThumbnailCache> (8));
                juce::AudioThumbnail thumb (wavecache::kThumbResolution, fm, *caches.front());
                thumb.setSource (new juce::FileInputSource (s.take (1)));
                expect (pumpUntil ([&] { return thumb.isFullyLoaded(); }, 5000), "thumbnail scan");

                const auto never = [] { return false; };
                ShortStream full (1 << 24), tooSmall (32);
                expect (wavecache::write (full, caches, never), "setup: the cache writes");
                expect (! wavecache::write (tooSmall, caches, never), "a short write must report failure");

                const auto cacheFile = wavecache::fileIn (s.dir);
                expect (cacheFile.replaceWithText ("the good cache"));
                expect (! wavecache::save (s.dir, caches, [] { return true; }), "a stopped save must fail");
                expectEquals (cacheFile.loadFileAsString(), juce::String ("the good cache"),
                              "a stopped save replaced the cache");
                expect (s.dir.findChildFiles (juce::File::findFiles, false, "*.tmp;.*.tmp").isEmpty(),
                        "a stopped save left its staging file");

                // A file cut short by an older build is thrown away, not loaded.
                expect (wavecache::save (s.dir, caches, never));
                juce::MemoryBlock whole;
                expect (cacheFile.loadFileAsData (whole));
                expect (cacheFile.replaceWithData (whole.getData(), whole.getSize() / 2));
                wavecache::load (s.dir, caches, never);
                expect (! cacheFile.existsAsFile(), "a truncated cache must be dropped");
            }

            beginTest ("A session with saved peaks opens without waiting on them");
            {
                Show s (1, { 48000 });
                const auto cacheFile = wavecache::fileIn (s.dir);
                expect (pumpUntil ([&] { return cacheFile.existsAsFile(); }, 10000), "setup: cache saved");
                expect (cacheFile.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::hours (1)));

                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                expect (rows.size() == 1, "setup: one row");
                if (rows.size() == 1)
                    expect (pumpUntil ([&] { return rows[0]->getWaveformSecondsForTests() > 0.0; }, 5000),
                            "the row never picked up its waveform after the cache loaded");
            }

            beginTest ("A clip action left open cannot change a rolling take's arrangement");
            {
                Show s (3, { 48000, 48000 });     // strip 3 has no take yet, so it can record
                s.engine.setTrackStereo (0, true);
                s.splitBoth (0, 1, 24000);
                s.page().refresh();
                auto rows = rowsOf (s.page());
                expect (rows.size() == 2, "setup: a stereo row and a mono row");
                if (rows.size() == 2)
                {
                    auto& row = *rows[0];
                    s.main.undoManager.clearUndoHistory();

                    s.engine.setTrackArmed (2, true);
                    expect (s.engine.startRecording (s.dir), "setup: the take rolls");
                    for (int choice : { 400, 401, 410, 411, 105, 505 })
                        row.applyClipMenuChoice (0, choice);
                    expect (! s.engine.setClipMuted (0, 0, true), "the engine setters must refuse too");
                    expect (! s.engine.deleteClip (0, 0));
                    expect (! s.engine.setClipGainDb (0, 0, -6.0f));
                    expect (! s.engine.setClipFades (0, 0, 100, 0));
                    expect (! s.engine.editClip (0, 0, AudioEngine::ClipEdit::Move, 100));
                    expectEquals (s.engine.pasteClip (0, 0, 0, 100, 0, 0, 0.0f, "p", {}, -1, 0), -1);
                    s.engine.stopRecording();

                    // The take grew the lists; the refused edits must not show.
                    expect (! s.clips (0).empty() && ! s.clips (0)[0].muted && ! s.clips (0)[0].locked,
                            "the open menu changed a clip during the take");
                    expect (! s.main.undoManager.canUndo(), "a step was pushed during the take");
                }
            }

            beginTest ("EDIT R, I, M and S reach the edit group as they do on the MIXER");
            {
                Show s (4, { 48000, 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                s.engine.setTrackEditGroup (2, 0);
                s.page().refresh();
                auto rows = rowsOf (s.page());
                expect (rows.size() == 3, "setup: three rows");
                if (rows.size() == 3)
                {
                    auto& row = *rows[0];
                    auto press = [&row] (const juce::String& text, bool on)
                    {
                        for (auto* child : row.getChildren())
                            if (auto* b = dynamic_cast<juce::ToggleButton*> (child);
                                b != nullptr && b->getButtonText() == text)
                                b->setToggleState (on, juce::sendNotificationSync);
                    };
                    auto& rec = s.engine.getRecorder();
                    press ("M", true);
                    for (int t : { 0, 1, 2 }) expect (rec.getTrack (t).muted.load(), "M missed track " + juce::String (t));
                    expect (! rec.getTrack (3).muted.load(), "M reached a strip outside the group");
                    press ("M", false);
                    expect (! rec.getTrack (2).muted.load(), "the group must follow M off");

                    press ("S", true);
                    for (int t : { 0, 1, 2 }) expect (rec.getTrack (t).soloed.load(), "S missed track " + juce::String (t));
                    press ("S", false);
                    press ("I", true);
                    for (int t : { 0, 1, 2 }) expect (rec.getTrack (t).monitor.load(), "I missed track " + juce::String (t));
                    press ("I", false);
                    press ("R", true);
                    for (int t : { 0, 1, 2 }) expect (rec.getTrack (t).armed.load(), "R missed track " + juce::String (t));
                    press ("R", false);
                }
            }

            beginTest ("Consolidate that finishes after a strip move applies to nothing");
            {
                Show s (2, { 48000, 48000 });
                auto rows = rowsOf (s.page());
                expect (rows.size() == 2, "setup: two rows");
                if (rows.size() == 2)
                {
                    // Alike in every field, so only the track generation can tell.
                    for (int t : { 0, 1 })
                        expect (s.engine.setClipName (t, 0, "Same"));
                    s.main.undoManager.clearUndoHistory();
                    rows[0]->applyClipMenuChoice (0, 413);
                    expect (s.engine.reorderTracks ({ 1, 0 }));
                    expect (pumpUntil ([&] { return ! s.main.sessionIoBusy.load(); }, 5000),
                            "the render never finished");
                    for (int t : { 0, 1 })
                        expect (s.clips (t)[0].audioFile == juce::File(),
                                "the render landed on the strip now at that index");
                    expect (! s.main.undoManager.canUndo(), "a snapshot from before the move reached undo");
                }
            }

            beginTest ("A clip-menu Consolidate that cannot run says so");
            {
                Show s (1, { 48000 });
                auto rows = rowsOf (s.page());
                if (rows.size() == 1)
                {
                    expect (s.engine.setClipLocked (0, 0, true));
                    rows[0]->applyClipMenuChoice (0, 413);
                    expect (pumpUntil ([&] { return ! s.main.sessionIoBusy.load(); }, 5000), "the job never released");
                    expect (s.main.statusLabel.getText().containsIgnoreCase ("failed"),
                            "it failed with no word: " + s.main.statusLabel.getText());
                }
            }

            beginTest ("Strip selection and active row do not outlive their layout or session");
            {
                Show s (3, { 48000, 48000, 48000 });
                s.main.rebuildStrips();
                s.main.selectedLogical = { 2 };
                s.page().setActiveRowTrackIndex (2);
                s.main.rebuildStrips();
                expect (s.main.selectedLogical.count (2) > 0, "a rebuild with no change must keep it");

                expect (s.engine.reorderTracks ({ 2, 0, 1 }));
                s.page().refresh();
                s.main.rebuildStrips();
                expectEquals (s.page().getActiveRowTrackIndex(), -1, "the active row names another strip now");
                expect (s.main.selectedLogical.empty(), "the selection names another strip now");

                s.main.selectedLogical = { 0 };
                s.main.moveSelectedStrips (1);
                s.main.rebuildStrips();
                expect (s.main.selectedLogical == std::set<int> { 1 }, "strips moved on purpose stay selected");

                const auto other = s.dir.getSiblingFile (s.dir.getFileName() + "-other");
                expect (other.createDirectory().wasOk());
                s.page().setActiveRowTrackIndex (1);
                s.engine.setActiveSessionDir (other);
                s.page().refresh();
                s.main.rebuildStrips();
                expect (s.main.selectedLogical.empty(), "another session kept the selection");
                expectEquals (s.page().getActiveRowTrackIndex(), -1);
                other.deleteRecursively();
            }

            beginTest ("A Range drag or range key takes over from an older clip selection");
            {
                Show s (2, { 48000, 48000 });
                s.page().refresh();
                auto rows = rowsOf (s.page());
                auto* tools = s.page().getEditToolsBar();
                expect (rows.size() == 2 && tools != nullptr, "setup");
                if (rows.size() == 2 && tools != nullptr)
                {
                    auto& row = *rows[0];
                    s.page().setSelectedClip (1, 0);
                    tools->setTool (EditToolsBar::Tool::Selector);
                    const int laneLeft = row.getHeaderWidth() + 4;
                    drag (row, { laneLeft + 100, 40 }, { laneLeft + 400, 40 });
                    tools->setTool (EditToolsBar::Tool::Smart);
                    expect (s.engine.getPlayer().hasLoopRegion(), "setup: the drag made a range");
                    expect (s.page().getSelectedClips().empty(), "Delete would take the old clip, not the range");

                    s.page().setSelectedClip (1, 0);
                    s.main.editStartRange();
                    expect (s.page().getSelectedClips().empty(), "a range set from the keyboard must take over too");
                }
                s.engine.getPlayer().clearLoopRegion();
            }

            beginTest ("Tab walks keyboard focus onto no control");
            {
                Show s (1, { 48000 });
                expect (s.main.keyPressed (juce::KeyPress (juce::KeyPress::tabKey), nullptr),
                        "left to JUCE, Tab puts focus on a routing combo or transport button");
            }

            beginTest ("Ctrl+Cmd+C runs Crop and bare Cmd+C stays Copy");
            {
                Show s (1, { 48000 });
                s.engine.getPlayer().selectRange (12000, 36000);
                auto* modals = juce::ModalComponentManager::getInstance();
                const int modalsBefore = modals->getNumModalComponents();
                const juce::KeyPress copy ('c', juce::ModifierKeys::commandModifier, 0);
                const juce::KeyPress crop ('c', juce::ModifierKeys (juce::ModifierKeys::commandModifier
                                                                    | juce::ModifierKeys::ctrlModifier), 0);

                expect (s.main.keyPressed (copy, nullptr));
                expectEquals (modals->getNumModalComponents(), modalsBefore, "bare Cmd+C asked to crop");
                expect (s.engine.getPlayer().hasLoopRegion(), "bare Cmd+C ate the range");

                expect (s.main.keyPressed (crop, nullptr));
                auto* dialog = dynamic_cast<juce::AlertWindow*> (modals->getModalComponent (0));
                expect (dialog != nullptr && dialog->getName() == "Crop to loop range?",
                        "Ctrl+Cmd+C did not run Crop");
                if (dialog != nullptr)
                {
                    dialog->exitModalState (1);
                    expect (pumpUntil ([&] { return s.clips (0)[0].fileLengthSamples == 24000; }, 2000),
                            "the prompt's Crop did not crop");
                }
                s.engine.getPlayer().clearLoopRegion();
            }

            beginTest ("A clip pastes only onto a strip of its own stereo layout");
            {
                Show s (3, { 48000, 48000, 48000 });
                s.engine.setTrackStereo (0, true);
                s.page().refresh();
                s.page().setSelectedClip (0, 0);
                s.main.editClipboardCut (false);
                s.page().clearSelectedClip();

                s.page().setActiveRowTrackIndex (2);          // a mono strip
                s.engine.getPlayer().setPositionSamples (0);
                s.main.editClipboardPaste();
                expectEquals ((int) s.clips (2).size(), 1, "the stereo clip landed on a mono strip");

                s.page().setActiveRowTrackIndex (0);          // its own stereo strip
                s.main.editClipboardPaste();
                expectEquals ((int) s.clips (0).size(), 2);
                expectEquals ((int) s.clips (1).size(), 2, "both halves belong on the stereo strip");

                s.page().setSelectedClip (2, 0);
                s.main.editClipboardCut (false);
                s.page().clearSelectedClip();
                s.page().setActiveRowTrackIndex (0);
                s.main.editClipboardPaste();
                expectEquals ((int) s.clips (0).size(), 2, "a mono clip landed on one half of a pair");
            }

            beginTest ("A clip copied in one session does not paste into the next");
            {
                Show s (1, { 48000 });
                s.page().setSelectedClip (0, 0);
                s.main.editClipboardCut (false);
                s.page().clearSelectedClip();
                const auto other = s.dir.getSiblingFile (s.dir.getFileName() + "-other");
                expect (other.createDirectory().wasOk());
                s.engine.setActiveSessionDir (other);
                s.main.editClipboardPaste();
                expectEquals ((int) s.clips (0).size(), 1, "the paste kept a path into the other session");
                other.deleteRecursively();
            }

            beginTest ("A marker is found by identity after the list shifts");
            {
                Show s (1, { 48000 });
                auto& markers = s.engine.getMarkers();
                markers.setContext (s.dir, 48000.0);
                markers.drop (20000, "later");
                const auto id = markers.lastDroppedId;
                expectEquals (markers.indexOfId (id), 0);
                markers.drop (1000, "earlier");
                expectEquals (markers.indexOfId (id), 1, "a marker dropped ahead of it shifted its index");
                expectEquals (markers.indexOfId ("gone"), -1);
            }

            beginTest ("The ruler repaints when a marker is renamed");
            {
                Show s (1, { 48000 });
                auto& markers = s.engine.getMarkers();
                markers.setContext (s.dir, 48000.0);
                markers.drop (20000, "Marker 1");
                EditTimeRuler ruler (s.engine);
                const auto before = ruler.idleSignature();
                markers.renameMarker (0, "Chorus");
                expect (ruler.idleSignature() != before, "the idle ruler would keep the old name");
            }

            beginTest ("A marker jump or desk locate leaves FOLLOW on");
            {
                Show s (1, { 480000 });
                s.page().setBounds (0, 0, 1400, 500);
                s.page().refresh();
                s.page().setZoom (8.0f);
                juce::Button* follow = nullptr;
                for (auto* child : s.page().getChildren())
                    if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getButtonText() == "FOLLOW")
                        follow = b;
                expect (follow != nullptr, "setup: FOLLOW button");
                s.engine.startPlayback();
                expect (pumpUntil ([&] { return follow != nullptr && follow->isVisible(); }, 2000), "setup: the page saw it roll");
                s.page().scrollToSample (400000);
                if (follow != nullptr) expect (follow->getToggleState(), "a programmatic scroll paused FOLLOW");
                s.engine.stopPlayback();
            }

            beginTest ("Only the rows a take wrote rescan after STOP");
            {
                Show s (2, { 0, 48000 });
                s.engine.setTrackArmed (0, true);
                expect (s.engine.startRecording (s.dir), "setup: the take rolls");
                expect (pumpUntil ([&] { return s.page().isTimelineReadOnly(); }, 5000), "the page never saw the take");
                auto& recorder = s.engine.getRecorder();
                std::vector<float> audio (256, 0.5f);
                const float* input[] { audio.data(), audio.data() };
                for (int b = 0; b < 20; ++b) recorder.processBlock (input, 2, 256);
                s.engine.stopRecording();
                auto rows = rowsOf (s.page());
                expect (rows.size() == 2, "setup: two rows");
                if (rows.size() == 2)
                {
                    expect (pumpUntil ([&] { return rows[0]->getWaveformReloadsForTests() > 0; }, 5000),
                            "the captured row must rescan");
                    expectEquals (rows[1]->getWaveformReloadsForTests(), 0, "an un-armed row blanked and rescanned");
                }
            }

            beginTest ("Repaints reuse the zoomed-out overview of every clip on a row");
            {
                Show s (1, { 48000 * 60 });
                for (int i = 1; i < 6; ++i)
                    expect (s.engine.splitTrackAtSample (0, i * 48000 * 10));
                EditPage::TrackRow row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                row.setWaveformFiles (s.take (1), {});
                expect (pumpUntil ([&] { return row.waveformsLoaded() && row.getWaveformSecondsForTests() > 0.0; }, 20000),
                        "thumbnail scan");
                juce::Image image (juce::Image::ARGB, row.getWidth(), row.getHeight(), true);
                {
                    juce::Graphics g (image);
                    row.paint (g);
                }
                const auto built = row.getOverviewBuildsForTests();
                expect (built >= 6, "setup: each clip builds an overview, got " + juce::String (built));
                {
                    juce::Graphics g (image);
                    row.paint (g);
                }
                expectEquals (row.getOverviewBuildsForTests(), built, "a repaint rebuilt overviews it just made");
            }

            beginTest ("A row keeps to the ruler during a take");
            {
                Show s (2, { 0, 48000 });
                s.engine.setTrackArmed (0, true);
                EditPage::TrackRow row (1, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                row.setWaveformFiles (s.take (2), {});
                expect (pumpUntil ([&] { return row.waveformsLoaded(); }, 5000), "thumbnail scan");
                expect (s.engine.startRecording (s.dir), "setup: the take rolls");
                juce::Image image (juce::Image::ARGB, row.getWidth(), row.getHeight(), true);
                {
                    juce::Graphics g (image);
                    row.paint (g);
                }
                s.engine.stopRecording();
                // One second of audio on a five-minute recording span.
                int painted = 0;
                for (int x = laneX (row, 0.3); x < laneX (row, 0.9); ++x)
                    if (image.getPixelAt (x, 50) != brand::bgStrip) ++painted;
                expect (painted < (laneX (row, 0.9) - laneX (row, 0.3)) / 4,
                        "the clip block stretched across the recording span: " + juce::String (painted));
            }

            beginTest ("A take started past the live zoom limit relays out at once");
            {
                Show s (2, { 0, 480000 });
                s.page().setBounds (0, 0, 1400, 500);
                s.page().refresh();
                s.page().setZoom (64.0f);
                expect (pumpUntil ([&] { return ! s.main.sessionIoBusy.load(); }, 5000),
                        "setup: the zoom's layout save holds RECORD until it lands");
                s.engine.setTrackArmed (0, true);
                expect (s.engine.startRecording (s.dir), "setup: the take rolls: " + s.engine.getRecorder().getStartError());
                expect (pumpUntil ([&] { return s.page().getZoom() <= kMaxRecordingTimelineZoom; }, 2000), "zoom clamp");
                juce::Component* content = nullptr;
                for (auto* child : s.page().getChildren())
                    if (auto* viewport = dynamic_cast<juce::Viewport*> (child)) content = viewport->getViewedComponent();
                if (content != nullptr)
                    expect (content->getWidth() < 1400 * 20, "the lanes kept the deep zoom's width: "
                            + juce::String (content->getWidth()));
                s.engine.stopRecording();
            }

            beginTest ("After an unlink the R row draws its channel of the shared file");
            {
                Show s (2, {});
                expect (writeWav (s.take (1), 48000, 2));
                s.engine.loadSession (s.dir);
                s.engine.setStripCount (2);
                s.page().refresh();
                auto rows = rowsOf (s.page());
                expect (rows.size() == 2, "setup: two mono rows");
                if (rows.size() == 2)
                    expect (pumpUntil ([&] { return rows[1]->getWaveformSecondsForTests() > 0.0; }, 5000),
                            "the R row drew nothing though its clip plays");
            }

            beginTest ("The REC label follows a take, not every timeline lock");
            {
                Show s (1, { 48000 });
                juce::Label* label = nullptr;
                for (auto* child : s.page().getChildren())
                    if (auto* l = dynamic_cast<juce::Label*> (child); l != nullptr && l->getText().contains ("EDIT LOCKED"))
                        label = l;
                expect (label != nullptr, "setup: the label");
                s.page().setReadOnlyWhileRecording (true);    // a metadata save holds the lock
                pumpUntil ([] { return false; }, 120);
                if (label != nullptr) expect (! label->isVisible(), "REC showed with no take running");
                s.page().setReadOnlyWhileRecording (false);
            }

            beginTest ("Solo, Arm and strip paste are one undo step each");
            {
                Show s (2, { 48000, 48000 });
                s.main.rebuildStrips();
                s.main.selectedLogical = { 0 };
                auto settle = [&s] { for (int i = 0; i < 8; ++i) s.main.pollMixerUndo(); };
                auto& rec = s.engine.getRecorder();
                settle();
                s.main.undoManager.clearUndoHistory();
                s.main.rebaselineMixerUndo();

                s.main.editSoloSelection();
                settle();
                expect (rec.getTrack (0).soloed.load(), "setup: soloed");
                s.main.editUndo();
                expect (! rec.getTrack (0).soloed.load(), "undo must clear the solo");
                expect (! s.main.undoManager.canUndo(), "the solo left a second, empty step");
                s.main.editRedo();
                expect (rec.getTrack (0).soloed.load(), "redo did nothing");
                s.main.editUndo();
                settle();

                s.main.undoManager.clearUndoHistory();
                s.main.armSelection();
                settle();
                expect (rec.getTrack (0).armed.load(), "setup: armed");
                s.main.editUndo();
                expect (! rec.getTrack (0).armed.load());
                expect (! s.main.undoManager.canUndo(), "the arm left a second, empty step");
                s.main.editRedo();
                expect (rec.getTrack (0).armed.load(), "redo did nothing");
                s.engine.setTrackArmed (0, false);
            }

            beginTest ("A group fade drag leaves each peer's other fade alone");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                expect (s.engine.setClipFades (0, 0, 4800, 0) && s.engine.setClipFades (1, 0, 4800, 3000));
                EditPage::TrackRow row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const juce::Point<int> handle (laneX (row, 0.1), brand::space::sm + 2);
                drag (row, handle, handle + juce::Point<int> (40, 0));
                expect (s.clips (0)[0].fadeInSamples > 4800, "setup: the fade moved");
                expectEquals (s.clips (1)[0].fadeInSamples, s.clips (0)[0].fadeInSamples, "the peer follows the dragged edge");
                expectEquals (s.clips (1)[0].fadeOutSamples, (juce::int64) 3000, "the peer lost its own fade-out");
            }

            beginTest ("A group crossfade drag leaves each peer's outer fades alone");
            {
                Show s (2, { 48000, 48000 });
                s.engine.setTrackEditGroup (0, 0);
                s.engine.setTrackEditGroup (1, 0);
                s.splitBoth (0, 1, 24000);
                expect (s.engine.editClip (0, 1, AudioEngine::ClipEdit::Move, -4800));
                expect (s.engine.editClip (1, 1, AudioEngine::ClipEdit::Move, -4800));
                expect (s.engine.setClipFades (1, 0, 500, 0) && s.engine.setClipFades (1, 1, 0, 700));
                EditPage::TrackRow row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto total = s.engine.getPlayer().getTotalLengthSamples();
                auto floorX = [&] (juce::int64 sample)
                { return row.getHeaderWidth() + 4 + (int) ((double) sample / (double) total * kLaneW); };
                const juce::Point<int> handle ((floorX (19200) + floorX (24000)) / 2, 50);
                drag (row, handle, handle + juce::Point<int> (10, 0));
                expect (s.clips (0)[0].fadeOutSamples > 0, "setup: the crossfade moved");
                expectEquals (s.clips (1)[0].fadeInSamples, (juce::int64) 500, "the peer lost its fade-in");
                expectEquals (s.clips (1)[1].fadeOutSamples, (juce::int64) 700, "the peer lost its fade-out");
            }

            MainComponent::s_testConstruct = false;
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static EditIntegrityTests editIntegrityTests;
}
