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

#include <cmath>
#include <functional>
#include <set>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    class EditIntegrityTests final : public juce::UnitTest
    {
    public:
        EditIntegrityTests() : juce::UnitTest ("EDIT integrity", "zynforge") {}

        using AP   = AudioEngine::AutomationParam;
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

            Show (int strips, std::vector<int> lengths)
            {
                formats.registerBasicFormats();
                engine.clearAllStripOverrides();
                engine.prepareForTests (48000.0, 512);
                engine.setStripCount (strips);
                dir.getChildFile ("Audio Files").createDirectory();
                for (size_t i = 0; i < lengths.size(); ++i)
                    if (lengths[i] > 0) writeWav (take ((int) i + 1), lengths[i]);
                engine.setActiveSessionDir (dir);
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

        // The row geometry the EDIT audit tests use: a 1000 px inner lane.
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

            MainComponent::s_testConstruct = false;
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static EditIntegrityTests editIntegrityTests;
}
