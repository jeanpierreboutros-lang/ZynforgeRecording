// EDIT rows against the rest of the window: whole-session swaps, the three
// views' routing, and the row's gestures. Driven through a real MainComponent
// and the row's real mouse handlers.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../UI/ChannelStrip.h"
#include "../UI/EditPage.h"
#include "../UI/EditTimeRuler.h"
#include "../UI/EditTrackRow.h"
#include "../UI/FloatingLaunch.h"
#include "../UI/MainComponent.h"
#include "../UI/MarkerListDialog.h"
#include "../UI/PatchPage.h"
#include "../UI/RenameLabel.h"
#include "../UI/SessionProjPath.h"
#include "../UI/WaveCacheFile.h"

#include <cmath>
#include <functional>
#include <set>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class EditRowFixTests final : public juce::UnitTest
    {
    public:
        EditRowFixTests() : juce::UnitTest ("EDIT row fixes", "zynforge") {}

        static constexpr int kRate = 48000;
        using Row  = EditPage::TrackRow;

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
                .getChildFile ("zf-editrow-" + tag + "-" + juce::Uuid().toString());
        }

        static bool writeWav (const juce::File& file, int samples, int channels = 1)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
                file.createOutputStream().release(), (double) kRate, (unsigned) channels, 24, {}, 0));
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

        // A real main window with saved sessions on disk, showing EDIT.
        struct Show
        {
            MainComponent& main;
            AudioEngine&   engine { main.engine };
            juce::Array<juce::File> dirs;

            explicit Show (MainComponent& window) : main (window)
            {
                main.switchView (MainComponent::View::Edit);
                main.editPage->setBounds (0, 0, 1400, 500);
            }

            ~Show()
            {
                engine.stopPlayback();
                engine.clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine.setStripCount (0);
                engine.clearAllStripOverrides();
                main.lastTrackCount = -1;
                main.confirmForTests = nullptr;
                for (const auto& d : dirs) d.deleteRecursively();
            }

            EditPage& page() { return *main.editPage; }

            // Saved and reopened, so its mixer really comes off disk. Each
            // name is a logical strip; stereoFirst makes the first of them a pair.
            juce::File session (const juce::StringArray& names, bool stereoFirst, bool armFirst)
            {
                const auto dir = tempDir ("session");
                dirs.add (dir);
                int file = 1;
                for (int i = 0; i < names.size(); ++i)
                {
                    const int channels = stereoFirst && i == 0 ? 2 : 1;
                    writeWav (takeIn (dir, file), kRate, channels);
                    file += channels;
                }
                main.openSessionFolder (dir, true);
                engine.prepareForTests ((double) kRate, 512);
                if (stereoFirst) main.setStereoLink (0, true);
                int track = 0;
                for (int i = 0; i < names.size(); ++i)
                {
                    engine.setTrackName (track, names[i]);
                    track += stereoFirst && i == 0 ? 2 : 1;
                }
                engine.setTrackArmed (0, armFirst);
                main.saveSessionStateTo (dir);
                return dir;
            }

            // The rows the page holds once a tick has rebuilt whatever was condemned.
            bool rowsReadLive()
            {
                const auto rows = rowsOf (page());
                return ! rows.empty()
                    && std::none_of (rows.begin(), rows.end(), [] (Row* r) { return r->isCondemnedForTests(); });
            }
        };

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };
            MainComponent main;
            main.setSize (1320, 820);

            rowsFollowTheSession (main);
            theMasterIsNotAChannel (main);
            theMouseTakesWhatItCanSee();
            aSessionJobOwnsTheEditor (main);
            theRowMenuIsReachable();
            rowsKeepTheirView (main);
            rulerSpeaksTheSessionRate();
            markersLaneShowsTheTake();
            verticalMovesKeepTheTimeline();
            resizingARowMovesNothing();
            noFirstLookDuringATake();
            newMediaKeepsTheCache (main);
            shiftRangeAdds();
            rangeBandSitsOnTheLane();
            liveEnvelopeMeetsThePlayhead();
            scanRepaintsOnceMore();
            fadeToolGrabs();

            // 2026-11-11 audit
            promptsLetATakeThrough (main);
            cueEditsSurviveTimelineUndo (main);
            continuedTakesKeepOneWaveform();
            clipPromptsFollowTheirClip();
        }

        void rowsFollowTheSession (MainComponent& main)
        {
            beginTest ("Opening a session of the same size gives EDIT that session's strips");
            {
                Show show (main);
                const auto last = show.session ({ "OVERHEADS", "KICK" }, true, true);
                const auto tonight = show.session ({ "PIANO", "VOX" }, true, false);

                main.openSessionFolder (last, true);
                expect (pumpUntil ([&] { return show.rowsReadLive(); }), "setup: rows for the first session");
                auto rows = rowsOf (show.page());
                expectEquals ((int) rows.size(), 2, "setup: a stereo pair and a mono strip");
                if (rows.size() == 2)
                {
                    expectEquals (rows[0]->getNameTextForTests(), juce::String ("OVERHEADS"));
                    expect (pumpUntil ([&] { return rowsOf (show.page())[0]->isArmShownForTests(); }),
                            "setup: the first session's strip is armed");
                }

                main.openSessionFolder (tonight, true);
                expect (pumpUntil ([&] { return show.rowsReadLive(); }),
                        "the rows stayed condemned after a session of the same size opened");
                rows = rowsOf (show.page());
                expectEquals ((int) rows.size(), 2, "the layout did not change");
                if (rows.size() == 2)
                {
                    expectEquals (rows[0]->getNameTextForTests(), juce::String ("PIANO"));
                    expectEquals (rows[1]->getNameTextForTests(), juce::String ("VOX"));
                    expect (! rows[0]->isArmShownForTests(), "R stayed lit on a strip that is not armed");

                    // The row follows the engine from here on, as a live row does.
                    show.engine.setTrackArmed (0, true);
                    expect (pumpUntil ([&] { return rowsOf (show.page())[0]->isArmShownForTests(); }),
                            "the new row does not poll its strip");
                }
            }

            beginTest ("Recover or Restore of the open session revives the rows");
            {
                Show show (main);
                const auto dir = show.session ({ "KICK", "SNARE" }, false, false);
                expect (pumpUntil ([&] { return show.rowsReadLive(); }), "setup: rows");
                main.openSessionFolder (dir, true);   // same folder, same strips
                expect (pumpUntil ([&] { return show.rowsReadLive(); }),
                        "re-reading the open session left its rows condemned");
            }

            beginTest ("A delete the engine refuses leaves EDIT rows alive");
            {
                Show show (main);
                main.confirmForTests = [] (std::function<void()> confirm) { confirm(); };
                show.session ({ "ONLY" }, false, false);
                main.rebuildStrips();
                expect (pumpUntil ([&] { return show.rowsReadLive(); }), "setup: rows");
                expectEquals ((int) main.strips.size(), 1, "setup: one strip");

                show.page().onRowDelete (0);
                expectEquals (show.engine.getRecorder().getNumTracks(), 1, "a saved session keeps its last strip");
                expect (pumpUntil ([&] { return show.rowsReadLive(); }),
                        "a refused delete left the row condemned for good");

                show.engine.setTrackName (0, "RENAMED");
                expect (pumpUntil ([&] { return rowsOf (show.page())[0]->getNameTextForTests() == "RENAMED"; }),
                        "the row stopped polling after a refused delete");
            }
        }

        static juce::String patchOf (AudioEngine& engine, int track)
        {
            const auto& state = engine.getRecorder().getTrack (track);
            return juce::String (state.inputRouting.load()) + "/" + juce::String (state.outputRouting.load());
        }

        // The strip's output combo: the one that offers the master.
        static juce::ComboBox* outputComboOf (juce::Component& strip)
        {
            for (auto* child : strip.getChildren())
                if (auto* combo = dynamic_cast<juce::ComboBox*> (child))
                    if (combo->getItemText (0).endsWith ("Master")) return combo;
            return nullptr;
        }

        static juce::MouseEvent leftAt (juce::Component& c, juce::Point<int> p)
        {
            const auto now = juce::Time::getCurrentTime();
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                     juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, p.toFloat(), now, 1, false };
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

        static void drag (juce::Component& c, juce::Point<int> from, juce::Point<int> to,
                          int buttons = juce::ModifierKeys::leftButtonModifier)
        {
            c.mouseDown (mouseAt (c, from, from, false, buttons));
            const int steps = juce::jmax (1, (int) (from.getDistanceFrom (to) / 4));
            for (int s = 1; s <= steps; ++s)
                c.mouseDrag (mouseAt (c, from + (to - from) * s / steps, from, true, buttons));
            c.mouseUp (mouseAt (c, to, from, true, buttons));
        }

        void theMouseTakesWhatItCanSee()
        {
            // The body clear of the gain handle in the bottom-left corner.
            const auto bodyY = 40;

            beginTest ("A clip pasted inside a long one takes the click, and the long one keeps the rest");
            {
                Session s (1, { kRate });
                s.clips (0).push_back (clipAt (19200, 4800));   // 100 px wide, on top
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);

                const int inside = laneX (row, 0.45);
                drag (row, { inside, bodyY }, { inside + 100, bodyY });
                expectEquals (s.clips (0)[0].timelineStartSamples, (juce::int64) 0, "the clip underneath moved");
                expect (s.clips (0)[1].timelineStartSamples > 19200, "the clip on top could not be reached");

                const auto shortStart = s.clips (0)[1].timelineStartSamples;
                drag (row, { laneX (row, 0.9), bodyY }, { laneX (row, 0.9) - 20, bodyY });
                expectEquals (s.clips (0)[1].timelineStartSamples, shortStart, "a click off it moved the top clip");
            }

            beginTest ("Right-click and the Fade tool open the clip on top");
            {
                Session s (1, { kRate });
                s.clips (0).push_back (clipAt (19200, 4800));
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                int opened = -1;
                row.clipMenuForTests = [&opened] (int clip) { opened = clip; };

                const juce::Point<int> inside (laneX (row, 0.45), bodyY);
                row.mouseDown (mouseAt (row, inside, inside, false, juce::ModifierKeys::popupMenuClickModifier));
                expectEquals (opened, 1, "right-click opened the clip underneath");

                opened = -1;
                EditToolsBar tools;
                tools.setTool (EditToolsBar::Tool::Fade);
                row.toolsBar = &tools;
                row.mouseDown (mouseAt (row, inside, inside, false));
                row.mouseUp   (mouseAt (row, inside, inside, false));
                expectEquals (opened, 1, "the Fade tool opened the clip underneath");
                row.toolsBar = nullptr;
            }

            beginTest ("A split seam gives each clip its own edge");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const int seam = laneX (row, 0.5);

                drag (row, { seam + 3, bodyY }, { seam + 53, bodyY });
                expect (s.clips (0)[1].timelineStartSamples > 24000, "the right clip's start could not be reached");
                expectEquals (s.clips (0)[0].fileLengthSamples, (juce::int64) 24000, "the left clip's end moved");

                const auto rightStart = s.clips (0)[1].timelineStartSamples;
                drag (row, { seam - 3, bodyY }, { seam - 53, bodyY });
                expect (s.clips (0)[0].fileLengthSamples < 24000, "the left clip's end could not be reached");
                expectEquals (s.clips (0)[1].timelineStartSamples, rightStart, "the right clip moved");
            }

            beginTest ("A short fade-in at a seam can be grabbed");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.setClipFades (0, 1, 240, 0));   // 5 px
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const int handle = laneX (row, 0.5) + 5;
                const int top = brand::space::sm + 2;
                drag (row, { handle, top }, { handle + 50, top });
                expect (s.clips (0)[1].fadeInSamples > 240, "the fade-in at the seam could not be grabbed");
                expectEquals (s.clips (0)[0].fileLengthSamples, (juce::int64) 24000, "the left clip's end moved");
            }

            beginTest ("Both ends of a crossfade stay reachable under the clip that covers them");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                expect (s.engine.editClip (0, 1, AudioEngine::ClipEdit::Move, -2400));   // 50 px overlap
                expectEquals (s.clips (0)[1].timelineStartSamples, (juce::int64) 21600, "setup: an overlap");
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);

                // The session now ends where the moved clip ends.
                const auto total = (double) s.engine.getPlayer().getTotalLengthSamples();
                const int aEnd = laneX (row, 24000.0 / total);
                drag (row, { aEnd - 2, bodyY }, { aEnd - 22, bodyY });
                expect (s.clips (0)[0].fileLengthSamples < 24000, "the outgoing clip's end was out of reach");

                const int bStart = laneX (row, 21600.0 / total);
                drag (row, { bStart + 2, bodyY }, { bStart - 18, bodyY });
                expect (s.clips (0)[1].timelineStartSamples < 21600, "the incoming clip's start was out of reach");
            }
        }

        static juce::StringArray itemsOf (const juce::PopupMenu& menu)
        {
            juce::StringArray items;
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                items.add (it.getItem().text);
            return items;
        }

        void fadeToolGrabs()
        {
            beginTest ("The Fade tool's squares drag, and a click on the clip opens its menu");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                EditToolsBar tools;
                tools.setTool (EditToolsBar::Tool::Fade);
                row.toolsBar = &tools;
                int opened = -1;
                row.clipMenuForTests = [&opened] (int clip) { opened = clip; };
                const int top = brand::space::sm + 2;

                drag (row, { laneX (row, 0.0) + 2, top }, { laneX (row, 0.0) + 52, top });
                expect (s.clips (0)[0].fadeInSamples > 0, "the fade-in square did not drag");
                drag (row, { laneX (row, 1.0) - 2, top }, { laneX (row, 1.0) - 52, top });
                expect (s.clips (0)[0].fadeOutSamples > 0, "the fade-out square did not drag");
                expectEquals (opened, -1, "a press on a square opened the menu");

                const auto fadeIn = s.clips (0)[0].fadeInSamples;
                drag (row, { laneX (row, 0.0) + 52, top }, { laneX (row, 0.0) + 102, top });
                expect (s.clips (0)[0].fadeInSamples > fadeIn, "an existing fade's square did not drag");

                const juce::Point<int> body (laneX (row, 0.5), 50);
                row.mouseDown (mouseAt (row, body, body, false));
                row.mouseUp   (mouseAt (row, body, body, false));
                expectEquals (opened, 0, "a click on the clip did not open its menu");
                expectEquals (s.clips (0)[0].timelineStartSamples, (juce::int64) 0, "the Fade tool moved a clip");
                row.toolsBar = nullptr;
            }
        }

        void scanRepaintsOnceMore()
        {
            beginTest ("The lanes repaint once more after a waveform scan ends");
            {
                ScanRepaint repaint;
                expect (! repaint.tick (false), "an idle page repaints nothing");
                expect (repaint.tick (true), "a running scan repaints");
                expect (repaint.tick (false), "the tick a scan ended on is the one that matters");
                expect (! repaint.tick (false), "the repaint is bounded");
            }
        }

        void liveEnvelopeMeetsThePlayhead()
        {
            beginTest ("With pre-roll on, the live waveform ends at the record head");
            {
                const auto dir = tempDir ("preroll");
                expect (dir.createDirectory().wasOk());
                {
                    constexpr int block = 256;
                    AudioEngine engine;
                    engine.clearAllStripOverrides();
                    engine.setStripCount (1);
                    engine.prepareForTests ((double) kRate, block);
                    auto& rec = engine.getRecorder();
                    rec.getTrack (0).armed.store (true);
                    rec.setPreRollSeconds (1);
                    engine.setActiveSessionDir (dir);
                    engine.claimMixerFor (dir);

                    std::vector<float> audio ((size_t) block, 0.5f);
                    const float* input[] { audio.data() };
                    for (int b = 0; b < 200; ++b) rec.processBlock (input, 1, block);

                    EditPage page (engine);
                    page.setBounds (0, 0, 1400, 300);
                    page.refresh();
                    expect (engine.startRecording (dir));
                    expect (rec.getPreRollHistorySamples() > 0, "setup: the take has pre-roll");
                    for (int b = 0; b < 40; ++b) rec.processBlock (input, 1, block);

                    const auto rows = rowsOf (page);
                    expect (rows.size() == 1, "setup: one row");
                    if (rows.size() == 1)
                    {
                        expect (pumpUntil ([&] { return rows[0]->getLiveEnvelopeSamplesForTests()
                                                     >= rec.getPreRollHistorySamples(); }, 2000),
                                "the live waveform started at zero, behind the record head");
                        expect (std::abs (rec.getRecordTimelineSamples() - rows[0]->getLiveEnvelopeSamplesForTests())
                                    <= 2 * TrackState::kLiveBinSamples,
                                "the live waveform and the record head disagree");
                    }
                    engine.stopRecording();
                    rec.setPreRollSeconds (0);
                    engine.clearAllStripOverrides();
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
                dir.deleteRecursively();
            }
        }

        void rangeBandSitsOnTheLane()
        {
            beginTest ("The range band sits where the clips and the ruler put those samples");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                auto render = [&row]
                {
                    juce::Image img (juce::Image::ARGB, row.getWidth(), row.getHeight(), true);
                    juce::Graphics g (img);
                    row.paint (g);
                    return img;
                };
                const auto plain = render();
                s.engine.getPlayer().setLoopRegion (43200, 45600);   // 0.90 to 0.95 of the lane
                const auto shaded = render();
                int first = -1, last = -1;
                for (int x = row.getHeaderWidth(); x < row.getWidth(); ++x)
                    if (shaded.getPixelAt (x, 2) != plain.getPixelAt (x, 2))
                    {
                        if (first < 0) first = x;
                        last = x;
                    }
                expectEquals (first, laneX (row, 0.90), "the band's left edge is off the lane's mapping");
                expectEquals (last,  laneX (row, 0.95), "the band's right edge is off the lane's mapping");
            }
        }

        void shiftRangeAdds()
        {
            beginTest ("Shift+range on a selected track keeps it selected");
            {
                Session s (2, { kRate, kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                std::set<int> selected;
                page.onRowSelect = [&selected] (int track, bool additive)
                {
                    if (! additive) { selected = { track }; return; }
                    if (! selected.erase (track)) selected.insert (track);
                };
                page.isTrackSelected = [&selected] (int track) { return selected.count (track) > 0; };
                page.getEditToolsBar()->setTool (EditToolsBar::Tool::Selector);
                const auto rows = rowsOf (page);
                expect (rows.size() == 2, "setup: two rows");
                if (rows.size() == 2)
                {
                    const juce::Point<int> a (rows[0]->getHeaderWidth() + 200, 30), b = a + juce::Point<int> (100, 0);
                    const int shiftDrag = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier;
                    drag (*rows[0], a, b);
                    expect (selected == std::set<int> { 0 }, "setup: the range selects its track");
                    drag (*rows[0], a, b, shiftDrag);
                    expect (selected == std::set<int> { 0 }, "Shift+range turned the track off: the range now covers every track");
                    drag (*rows[1], a, b, shiftDrag);
                    expect (selected == std::set<int> ({ 0, 1 }), "Shift+range no longer adds a track");
                }
                page.getEditToolsBar()->setTool (EditToolsBar::Tool::Smart);
            }
        }

        void newMediaKeepsTheCache (MainComponent& main)
        {
            beginTest ("After a Consolidate the saved waveforms still load on reopen");
            {
                Show show (main);
                const auto dir = show.session ({ "ONE" }, false, false);
                const auto cache = wavecache::fileIn (dir);
                expect (pumpUntil ([&] { return cache.existsAsFile() && ! main.sessionIoBusy.load(); }, 10000),
                        "setup: the session's waveforms were never saved");
                pumpUntil ([] { return false; }, 50);   // a file written now is newer than the cache

                bool finished = false;
                const auto before = dir.getChildFile ("Audio Files").getNumberOfChildFiles (juce::File::findFiles);
                main.consolidateTracks ({ 0 }, 0, 24000);
                finished = pumpUntil ([&] { return dir.getChildFile ("Audio Files").getNumberOfChildFiles (juce::File::findFiles) > before
                                                && ! main.sessionIoBusy.load(); }, 10000);
                expect (finished, "setup: the consolidate never finished");
                expect (pumpUntil ([&] { return ! waveCachePredatesAudio (cache, dir); }, 10000),
                        "the saved waveforms predate the new file, so reopening rescans every track");
            }

            beginTest ("After an import the saved waveforms still load on reopen");
            {
                Show show (main);
                const auto dir = show.session ({ "ONE" }, false, false);
                const auto cache = wavecache::fileIn (dir);
                expect (pumpUntil ([&] { return cache.existsAsFile() && ! main.sessionIoBusy.load(); }, 10000),
                        "setup: the session's waveforms were never saved");
                pumpUntil ([] { return false; }, 50);   // a file written now is newer than the cache

                const auto folder = tempDir ("source");
                show.dirs.add (folder);
                const auto source = folder.getChildFile ("Guitar.wav");
                expect (writeWav (source, kRate));
                juce::Array<juce::File> picks;
                picks.add (source);
                main.completeAudioImport (picks);
                expect (pumpUntil ([&] { return show.engine.getRecorder().getNumTracks() == 2
                                             && ! main.sessionIoBusy.load(); }, 10000),
                        "setup: the import never finished");
                expect (pumpUntil ([&] { return ! waveCachePredatesAudio (cache, dir); }, 10000),
                        "the saved waveforms predate the imported file, so reopening rescans every track");
            }
        }

        void noFirstLookDuringATake()
        {
            beginTest ("A row touches no new clip file during a take");
            {
                const auto fresh = tempDir ("take");
                expect (fresh.createDirectory().wasOk());
                {
                    Session s (1, { kRate });
                    const auto own = s.dir.getChildFile ("Audio Files/Track_01_consolidated_1.wav");
                    expect (writeWav (own, kRate));
                    Row row (0, false, s.engine, s.formats, s.cache);
                    sizeRow (row);
                    auto clip = clipAt (0, 4800);
                    clip.audioFile = own;

                    s.engine.getRecorder().getTrack (0).armed.store (true);
                    s.engine.setActiveSessionDir (fresh);
                    s.engine.claimMixerFor (fresh);
                    expect (s.engine.startRecording (fresh));
                    expect (row.laneWaveFor (clip, false).thumb == nullptr, "paint opened a file during a take");
                    expectEquals (row.getOwnThumbnailCountForTests(), 0, "a blank lane was cached for good");
                    s.engine.stopRecording();
                    expect (row.laneWaveFor (clip, false).thumb != nullptr, "the file's waveform never came back");
                    expectEquals (row.getOwnThumbnailCountForTests(), 1);
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (fresh);
                fresh.deleteRecursively();
            }
        }

        void resizingARowMovesNothing()
        {
            beginTest ("Grabbing a row's bottom edge in the lane resizes it and places no cursor");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                s.engine.getPlayer().setPositionSamples (1000);
                const juce::Point<int> edge (laneX (row, 0.5), row.getHeight() - 2);
                drag (row, edge, edge + juce::Point<int> (0, 30));
                expectEquals (s.engine.getEditCursorSample(), (juce::int64) -1, "the grab placed an edit cursor");
                expectEquals (s.engine.getPlayer().getPositionSamples(), (juce::int64) 1000, "the grab moved the playhead");
                expect (row.getRowSize() == Row::Size::Custom, "the drag did not resize the row");
                expectEquals (row.getCustomHeight(), 130);
            }
        }

        void verticalMovesKeepTheTimeline()
        {
            const auto viewportOf = [] (EditPage& page) -> juce::Viewport*
            {
                for (auto* child : page.getChildren())
                    if (auto* vp = dynamic_cast<juce::Viewport*> (child)) return vp;
                return nullptr;
            };

            beginTest ("Tab to rename keeps the timeline where it is");
            {
                Session s (12, { kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                page.setZoom (8.0f);
                auto* vp = viewportOf (page);
                const auto rows = rowsOf (page);
                expect (vp != nullptr && rows.size() == 12, "setup: twelve rows in a viewport");
                if (vp != nullptr && rows.size() == 12)
                {
                    vp->setViewPosition (600, 0);
                    rows.back()->beginRename();
                    expectEquals (vp->getViewPositionX(), 600, "the hop reset the timeline to its start");
                    expect (vp->getViewPositionY() > 0, "setup: the hop has to scroll");
                }
            }

            beginTest ("A swatch drag scrolls the rows, not the timeline");
            {
                Session s (12, { kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                page.setZoom (8.0f);
                auto* vp = viewportOf (page);
                expect (vp != nullptr, "setup: a viewport");
                if (vp != nullptr)
                {
                    vp->setViewPosition (600, 0);
                    for (int step = 0; step < 8; ++step)
                        page.updateReorderPreview ({ 4, vp->getBottom() - 4 });
                    page.clearReorderPreview();
                    expectEquals (vp->getViewPositionX(), 600, "the drag slid the timeline back");
                    expect (vp->getViewPositionY() > 0, "the drag no longer scrolls the rows");
                }
            }
        }

        void markersLaneShowsTheTake()
        {
            beginTest ("A row on the Markers lane shows the take rolling on it");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                row.laneMode = Row::LaneMode::Markers;
                auto render = [&row]
                {
                    juce::Image img (juce::Image::ARGB, row.getWidth(), row.getHeight(), true);
                    juce::Graphics g (img);
                    row.paint (g);
                    return img;
                };
                const auto idle = render();
                row.setLiveRecording (true);
                for (int i = 0; i < 100; ++i) row.appendLivePeakL (0.9f);
                const auto live = render();
                auto changed = [&] (double from, double to)
                {
                    int differing = 0;
                    for (int x = laneX (row, from); x < laneX (row, to); ++x)
                        for (int y = 8; y < row.getHeight() - 8; ++y)
                            if (live.getPixelAt (x, y) != idle.getPixelAt (x, y)) ++differing;
                    return differing;
                };
                expect (changed (0.05, 0.45) > 0, "the lane showed no capture");
                expectEquals (changed (0.60, 0.95), 0, "the capture ran past its share of the timeline");
            }
        }

        void rulerSpeaksTheSessionRate()
        {
            beginTest ("The ruler reads a loaded session at its own rate");
            {
                const auto dir = tempDir ("rate");
                dir.getChildFile ("Audio Files").createDirectory();
                {
                    juce::WavAudioFormat format;
                    std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
                        takeIn (dir, 1).createOutputStream().release(), 44100.0, 1, 24, {}, 0));
                    juce::AudioBuffer<float> audio (1, 44100);
                    audio.clear();
                    expect (writer != nullptr && writer->writeFromAudioSampleBuffer (audio, 0, 44100));
                }
                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.prepareForTests ((double) kRate, 512);
                EditTimeRuler ruler (engine);
                expectEquals (ruler.timelineRate(), (double) kRate, "with nothing loaded the device sets the span");
                engine.setStripCount (1);
                engine.setActiveSessionDir (dir);
                engine.claimMixerFor (dir);
                engine.loadSession (dir);
                expectEquals (ruler.timelineRate(), 44100.0, "the labels read short of file time");
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
        }

        void rowsKeepTheirView (MainComponent& main)
        {
            const auto viewOf = [] (Row& r)
            {
                return juce::String ((int) r.getRowSize()) + (r.laneMode == Row::LaneMode::Markers ? " markers" : " waveform");
            };
            const auto large = juce::String ((int) Row::Size::Large) + " markers";
            const auto plain = juce::String ((int) Row::Size::Small) + " waveform";

            beginTest ("A row's height and lane outlive a rebuild");
            {
                Session s (2, { kRate, kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                expect (rows.size() == 2, "setup: two rows");
                rows[0]->applyRowMenuChoice (5);
                rows[0]->applyRowMenuChoice (Row::kMenuLaneMarkers);

                s.engine.addOneStrip();
                expect (pumpUntil ([&] { return rowsOf (page).size() == 3; }), "the new strip never got a row");
                rows = rowsOf (page);
                expectEquals (viewOf (*rows[0]), large, "adding a channel reset the row");
                expectEquals (viewOf (*rows[2]), plain, "a new row takes the default");
                expectEquals (rows[0]->getHeight(), Row::pixelsFor (Row::Size::Large), "the height did not reach the layout");
            }

            beginTest ("A row's height and lane follow the strip through a move that keeps the layout");
            {
                Session s (2, { kRate, kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                expect (rows.size() == 2, "setup: two rows");
                rows[0]->applyRowMenuChoice (5);
                rows[0]->applyRowMenuChoice (Row::kMenuLaneMarkers);

                expect (s.engine.reorderTracks ({ 1, 0 }));
                page.refresh();   // what the host's onTracksReordered handler does
                rows = rowsOf (page);
                expect (rows.size() == 2, "the layout did not change");
                if (rows.size() == 2)
                {
                    expectEquals (viewOf (*rows[0]), plain, "the view stayed at the position");
                    expectEquals (viewOf (*rows[1]), large, "the view did not follow the strip");
                }
            }

            beginTest ("A row's height and lane save with the session");
            {
                Show show (main);
                const auto first = show.session ({ "KICK", "SNARE" }, false, false);
                expect (pumpUntil ([&] { return show.rowsReadLive(); }), "setup: rows");
                auto rows = rowsOf (show.page());
                rows[1]->applyRowMenuChoice (5);
                rows[1]->applyRowMenuChoice (Row::kMenuLaneMarkers);
                expect (pumpUntil ([&] { return main.pendingMetadataSaves == 0; }), "the layout save never ended");
                const auto project = juce::JSON::parse (findSessionProj (first));
                const auto* views = project.getProperty ("ui", {}).getProperty ("editRows", {}).getArray();
                expect (views != nullptr && views->size() == 1, "the session's layout does not hold the row's view");

                show.session ({ "OTHER" }, false, false);
                expect (pumpUntil ([&] { return show.rowsReadLive() && rowsOf (show.page()).size() == 1; }), "setup: other session");
                expect (pumpUntil ([&] { return main.pendingMetadataSaves == 0; }), "the save never ended");

                main.openSessionFolder (first, true);
                expect (pumpUntil ([&] { return show.rowsReadLive() && rowsOf (show.page()).size() == 2; }), "reopen");
                rows = rowsOf (show.page());
                if (rows.size() == 2)
                {
                    expectEquals (viewOf (*rows[0]), plain);
                    expectEquals (viewOf (*rows[1]), large, "the row's view did not come back with the session");
                }
            }
        }

        void theRowMenuIsReachable()
        {
            beginTest ("A right-click on the channel name opens the row's menu, as the bare header does");
            {
                Session s (1, { kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                const auto rows = rowsOf (page);
                expect (rows.size() == 1, "setup: one row");
                if (rows.size() == 1)
                {
                    RenameLabel* name = nullptr;
                    for (auto* child : rows[0]->getChildren())
                        if (auto* label = dynamic_cast<RenameLabel*> (child)) name = label;
                    expect (name != nullptr && name->onPopupMenu != nullptr, "the name field swallows the right-click");
                }
            }

            beginTest ("The row's menu names lane, size and delete, and sends a Markers row back");
            {
                Session s (1, { kRate });
                EditPage::TrackRow row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto header = itemsOf (row.rowMenu (true));
                for (const auto& item : { "Lane Shows Waveform", "Lane Shows Markers", "small", "Set all tracks to", "Delete Channel" })
                    expect (header.contains (item), juce::String ("the header menu lost ") + item);
                const auto lane = itemsOf (row.rowMenu (false));
                expect (lane.contains ("Lane Shows Waveform"), "a lane with no clip offers no way back");
                expect (! lane.contains ("Delete Channel"), "a stray click on a lane must not offer Delete");

                row.applyRowMenuChoice (Row::kMenuLaneMarkers);
                expect (row.laneMode == Row::LaneMode::Markers);
                row.applyRowMenuChoice (Row::kMenuLaneWaveform);
                expect (row.laneMode == Row::LaneMode::Waveform);
                row.applyRowMenuChoice (5);
                expect (row.getRowSize() == Row::Size::Large);
            }

            beginTest ("Right-clicking a Markers lane opens the row's menu, not a clip's");
            {
                Session s (1, { kRate });
                EditPage::TrackRow row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                row.laneMode = Row::LaneMode::Markers;
                int opened = -1;
                row.clipMenuForTests = [&opened] (int clip) { opened = clip; };
                const juce::Point<int> p (laneX (row, 0.5), 40);
                row.mouseDown (mouseAt (row, p, p, false, juce::ModifierKeys::rightButtonModifier));
                expectEquals (opened, -1, "a Markers row opened a clip menu");
                expect (juce::PopupMenu::dismissAllActiveMenus(), "the row's menu did not open");
            }
        }

        void aSessionJobOwnsTheEditor (MainComponent& main)
        {
            beginTest ("A session job refuses clip edits and a new strip at the engine");
            {
                Session s (1, { kRate });
                expect (s.engine.splitTrackAtSample (0, 24000));
                s.engine.setSessionTransitionActive (true);
                expect (s.engine.isArrangementFrozen(), "a job leaves the arrangement open to edits");
                expect (! s.engine.editClip (0, 1, AudioEngine::ClipEdit::Move, 480), "a clip moved under a job");
                expect (! s.engine.setClipFades (0, 0, 480, 0), "a fade changed under a job");
                expect (! s.engine.setClipGainDb (0, 0, -3.0f), "a clip gain changed under a job");
                s.engine.addOneStrip();
                expectEquals (s.engine.getRecorder().getNumTracks(), 1, "a strip was added under a job");
                s.engine.setSessionTransitionActive (false);
                expect (s.engine.editClip (0, 1, AudioEngine::ClipEdit::Move, 480), "edits stay available afterwards");
            }

            beginTest ("A disabled EDIT page takes no drag and no double-click");
            {
                Session s (1, { kRate });
                EditPage page (s.engine);
                page.setBounds (0, 0, 1400, 500);
                page.refresh();
                auto rows = rowsOf (page);
                expect (rows.size() == 1, "setup: one row");
                if (rows.size() == 1)
                {
                    auto& row = *rows[0];
                    const auto lane0 = row.getHeaderWidth() + 4;
                    const auto laneW = row.getWidth() - row.getHeaderWidth() - 8;
                    const juce::Point<int> body (lane0 + laneW / 2, 30);

                    page.setEnabled (false);
                    drag (row, body, body + juce::Point<int> (120, 0));
                    expectEquals (s.clips (0)[0].timelineStartSamples, (juce::int64) 0,
                                  "a drag landed while a job held the editor");
                    expectEquals (s.engine.getEditCursorSample(), (juce::int64) -1,
                                  "the click moved the edit cursor under a job");

                    // A drag already in flight when the job takes the page.
                    page.setEnabled (true);
                    row.mouseDown (mouseAt (row, body, body, false));
                    page.setEnabled (false);
                    for (int dx = 4; dx <= 120; dx += 4)
                        row.mouseDrag (mouseAt (row, body + juce::Point<int> (dx, 0), body, true));
                    row.mouseUp (mouseAt (row, body + juce::Point<int> (120, 0), body, true));
                    expectEquals (s.clips (0)[0].timelineStartSamples, (juce::int64) 0,
                                  "a drag in flight kept editing under a job");

                    auto* list = row.getParentComponent();
                    const juce::Point<int> below (lane0, list->getHeight() - 4);
                    list->mouseDoubleClick (mouseAt (*list, below, below, false));
                    expectEquals (s.engine.getRecorder().getNumTracks(), 1, "the double-click added a strip");

                    page.setEnabled (true);
                    list->mouseDoubleClick (mouseAt (*list, below, below, false));
                    expectEquals (s.engine.getRecorder().getNumTracks(), 2, "setup: the double-click adds a strip");
                }
            }

            beginTest ("The host locks EDIT's rows while a job holds the session");
            {
                Show show (main);
                show.session ({ "ONE" }, false, false);
                expect (pumpUntil ([&] { return main.pendingMetadataSaves == 0; }), "the save never ended");
                main.updateHeaderFromStatus (show.engine.captureStatus(), false);
                expect (! show.page().isTimelineReadOnly(), "setup: rows open");
                main.sessionIoBusy.store (true);
                main.updateHeaderFromStatus (show.engine.captureStatus(), false);
                expect (show.page().isTimelineReadOnly(), "a job left EDIT's rows taking clicks");
                expect (! show.page().isEnabled(), "a job left the editor enabled");
                main.sessionIoBusy.store (false);
                main.updateHeaderFromStatus (show.engine.captureStatus(), false);
                expect (! show.page().isTimelineReadOnly());
                expect (show.page().isEnabled());
            }
        }

        void theMasterIsNotAChannel (MainComponent& main)
        {
            beginTest ("-> Master in the MIXER keeps the strip's input, on both halves of a pair");
            {
                Show show (main);
                show.session ({ "KEYS", "VOX" }, true, true);
                main.rebuildStrips();
                expectEquals ((int) main.strips.size(), 2, "setup: a pair and a mono strip");

                auto* mono = outputComboOf (*main.strips[1]);
                expect (mono != nullptr, "setup: the mono strip's output combo");
                if (mono != nullptr)
                {
                    mono->setSelectedId (8, juce::sendNotificationSync);    // Out 7
                    expectEquals (patchOf (show.engine, 2), juce::String ("6/6"), "a real channel stays linked");
                    mono->setSelectedId (1, juce::sendNotificationSync);
                    expectEquals (patchOf (show.engine, 2), juce::String ("6/-1"),
                                  "the master took the strip's input with it");
                }

                auto* pair = outputComboOf (*main.strips[0]);
                expect (pair != nullptr, "setup: the pair's output combo");
                if (pair != nullptr)
                {
                    pair->setSelectedId (6, juce::sendNotificationSync);    // Out 5-6
                    expectEquals (patchOf (show.engine, 0), juce::String ("4/4"), "a real channel stays linked on L");
                    expectEquals (patchOf (show.engine, 1), juce::String ("5/5"), "a real channel stays linked on R");
                    pair->setSelectedId (1, juce::sendNotificationSync);
                    expectEquals (patchOf (show.engine, 0), juce::String ("4/-1"), "L lost its input");
                    expectEquals (patchOf (show.engine, 1), juce::String ("5/-1"), "R lost its input");
                }
            }

            beginTest ("Clearing an output in PATCH keeps the input; clearing an input still clears both");
            {
                Show show (main);
                show.session ({ "KEYS", "VOX" }, true, true);
                show.engine.setDeviceInputCountForTests (8);
                show.engine.setDeviceOutputCountForTests (8);
                const juce::ScopeGuard noDevice { [&show]
                {
                    show.engine.setDeviceInputCountForTests (0);
                    show.engine.setDeviceOutputCountForTests (0);
                } };

                std::unique_ptr<juce::DialogWindow> window (PatchPage::launch (show.engine));
                juce::TabbedComponent* tabs = nullptr;
                std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
                {
                    if (auto* t = dynamic_cast<juce::TabbedComponent*> (&c)) tabs = t;
                    for (auto* child : c.getChildren()) walk (*child);
                };
                if (window != nullptr) walk (*window);
                expect (tabs != nullptr, "setup: the patch tabs");
                if (tabs != nullptr)
                {
                    // Matrix geometry: 90 px row header, 92 px column header, 56 px rows.
                    const auto dot = [] (int column, int row, int columnWidth)
                    { return juce::Point<int> (90 + column * columnWidth + 10, 92 + row * 56 + 10); };
                    auto* outputs = tabs->getTabContentComponent (1);
                    auto* inputs  = tabs->getTabContentComponent (0);
                    outputs->setBounds (0, 0, 1000, 700);
                    inputs->setBounds (0, 0, 1000, 700);
                    const int colW = (1000 - 90 - 12) / 2;

                    show.engine.setTrackLinkedRouting (2, 3);          // mono strip: column 1
                    outputs->mouseDown (leftAt (*outputs, dot (1, 3, colW)));
                    expectEquals (patchOf (show.engine, 2), juce::String ("3/-1"),
                                  "clearing the output took the input with it");

                    show.engine.setTrackLinkedRouting (0, 4);          // the pair: column 0
                    show.engine.setTrackLinkedRouting (1, 5);
                    outputs->mouseDown (leftAt (*outputs, dot (0, 4, colW)));
                    expectEquals (patchOf (show.engine, 0), juce::String ("4/-1"), "L lost its input");
                    expectEquals (patchOf (show.engine, 1), juce::String ("5/-1"), "R lost its input");

                    show.engine.setTrackLinkedRouting (2, 3);
                    outputs->mouseDown (leftAt (*outputs, dot (1, 5, colW)));
                    expectEquals (patchOf (show.engine, 2), juce::String ("5/5"), "a real channel stays linked");

                    inputs->mouseDown (leftAt (*inputs, dot (1, 5, colW)));
                    expectEquals (patchOf (show.engine, 2), juce::String ("-1/-1"),
                                  "an input cleared by hand clears both, as before");
                }
            }
        }

        // ── 2026-11-11 audit ────────────────────────────────────────────────

        static int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }
        static juce::Component* topModal() { return juce::ModalComponentManager::getInstance()->getModalComponent (0); }

        // Ends every open modal by its Cancel-style result and lets it clean up.
        static void dismissModals()
        {
            auto* modals = juce::ModalComponentManager::getInstance();
            for (int i = modals->getNumModalComponents(); --i >= 0;)
                if (auto* c = modals->getModalComponent (i)) c->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        template <class T>
        static T* findChild (juce::Component& root)
        {
            for (auto* child : root.getChildren())
            {
                if (auto* hit = dynamic_cast<T*> (child)) return hit;
                if (auto* deeper = findChild<T> (*child)) return deeper;
            }
            return nullptr;
        }

        static juce::Button* buttonTitled (juce::Component& root, const juce::String& title)
        {
            for (auto* child : root.getChildren())
            {
                if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getTitle() == title) return b;
                if (auto* deeper = buttonTitled (*child, title)) return deeper;
            }
            return nullptr;
        }

        // Nothing the window started is still writing the session: an edit or a
        // take waits for that, as it would in the app.
        static bool settle (Show& show)
        {
            return pumpUntil ([&]
            {
                return ! show.main.sessionIoBusy.load() && show.main.pendingMetadataSaves == 0
                    && ! show.engine.isSessionTransitionActive() && ! show.engine.isCaptureFinalizing();
            }, 10000);
        }

        // A continued take on the open session, as a desk or the twin would start it.
        bool startTakeIn (Show& show, const juce::File& dir)
        {
            expect (settle (show), "setup: the session never settled");
            show.engine.armContinue (show.engine.getCaptureLengthSamples());
            const bool started = show.engine.startRecording (dir);
            expect (started, "setup: " + show.engine.getRecorder().getStartError());
            return started;
        }

        bool stopTake (Show& show)
        {
            show.main.stopActiveCaptureAsync (false);
            return pumpUntil ([&]
            {
                return ! show.engine.isRecording() && ! show.engine.isCaptureFinalizing()
                    && ! show.main.captureMetadataPending && show.main.pendingMetadataSaves == 0;
            }, 10000);
        }

        // Where the ruler's double-click finds the marker at `seconds`, mapped the
        // way the ruler maps it at that moment.
        static int rulerFlagX (EditTimeRuler& ruler, AudioEngine& engine, double seconds)
        {
            const auto& player = engine.getPlayer();
            const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const auto total = player.getTotalLengthSamples();
            const double totalSec = total > 0 ? (double) total / sr : kNotionalEmptyLaneSec;
            return ruler.rulerTimeToX (seconds, ruler.rulerPxPerSec (totalSec));
        }

        void promptsLetATakeThrough (MainComponent& main)
        {
            beginTest ("Every prompt EDIT opens closes as Cancel when a take starts behind it");
            {
                Show show (main);
                main.confirmForTests = nullptr;
                show.session ({ "KICK", "SNARE" }, false, false);
                expect (pumpUntil ([&] { return show.rowsReadLive(); }), "setup: rows");
                dismissModals();
                auto& engine = show.engine;
                const auto marked = [&] (const juce::String& what, const std::function<void()>& open)
                {
                    open();
                    const bool opened = pumpUntil ([] { return modalCount() > 0; }, 2000);
                    expect (opened, what + ": setup: nothing opened");
                    if (opened)
                        expect ((bool) topModal()->getProperties()[kClosesForTake],
                                what + " would stand in front of STOP for a take started behind it");
                    dismissModals();
                };

                engine.getPlayer().setLoopRegion (4800, 9600);
                marked ("Crop to loop range", [&] { main.editCropToLoopRange(); });
                marked ("Delete recording",   [&] { main.confirmDeleteRecording (false, [] {}); });
                marked ("Delete channel",     [&] { main.confirmDeleteChannels (1, [] {}); });
                engine.getPlayer().clearLoopRegion();
                marked ("New marker name",    [&] { main.dropMarkerAndPromptName(); });
                marked ("Add cue",            [&] { main.addCueAtTransport(); });
                SetlistBar::Cue intro;
                intro.name = "Intro";
                main.cues = { intro };
                main.currentCueIndex = 0;
                marked ("Rename cue",         [&] { main.renameCurrentCue(); });
                marked ("Memory Locations",   [&] { MarkerListDialog::launch (engine); });

                const auto row = [&] (int i) { return rowsOf (show.page())[(size_t) i]; };
                expectEquals ((int) rowsOf (show.page()).size(), 2, "setup: two rows");
                if (rowsOf (show.page()).size() == 2)
                {
                    marked ("Rename clip",  [&] { row (0)->applyClipMenuChoice (0, 412); });
                    marked ("Clip gain",    [&] { row (0)->applyClipMenuChoice (0, 510); });
                    marked ("Strip colour", [&] { row (0)->openColourPicker(); });
                    marked ("Strip reorder notice", [&]
                    {
                        auto& r = *row (1);
                        const juce::Point<int> swatch (5, 30);
                        r.mouseDown (mouseAt (r, swatch, swatch, false));
                        engine.getPlayer().start();
                        r.mouseDrag (mouseAt (r, swatch + juce::Point<int> (0, 20), swatch, true));
                        engine.getPlayer().stop();
                        r.mouseUp (mouseAt (r, swatch + juce::Point<int> (0, 20), swatch, true));
                    });
                }
                if (auto* ruler = findChild<EditTimeRuler> (show.page()))
                {
                    engine.getMarkers().drop (kRate / 2, "Chorus");
                    const int x = rulerFlagX (*ruler, engine, 0.5);
                    marked ("Rename marker", [&] { ruler->mouseDoubleClick (mouseAt (*ruler, { x, 10 }, { x, 10 }, false)); });
                }
                else
                    expect (false, "setup: no ruler");
                main.cues.clear();
                main.currentCueIndex = -1;
            }

            beginTest ("A take closes the prompt it starts behind, and nothing renames or deletes a marker during it");
            {
                Show show (main);
                main.confirmForTests = nullptr;
                const auto dir = show.session ({ "KICK" }, false, true);
                expect (pumpUntil ([&] { return show.rowsReadLive(); }), "setup: rows");
                dismissModals();
                auto& engine = show.engine;
                engine.getMarkers().drop (kRate / 2, "Chorus");
                const auto markerId = engine.getMarkers().lastDroppedId;
                main.cues.clear();
                main.currentCueIndex = -1;

                main.addCueAtTransport();
                expect (pumpUntil ([] { return modalCount() > 0; }, 2000), "setup: the cue prompt");
                if (startTakeIn (show, dir))
                {
                    main.serviceModalsAroundTakes();   // the window's 10 Hz tick (no timers in a test window)
                    expect (pumpUntil ([] { return modalCount() == 0; }, 2000),
                            "the cue prompt still stands in front of STOP");
                    expect (pumpUntil ([&] { return main.cues.size() == 1; }, 1000),
                            "the cue asked for was dropped with its prompt");

                    MarkerListDialog list (engine);
                    list.applyMenuChoice (2, markerId);
                    expect (engine.getMarkers().indexOfId (markerId) >= 0,
                            "Memory Locations deleted a marker during the take");
                    list.applyMenuChoice (1, markerId);
                    expectEquals (modalCount(), 0, "Memory Locations opened Rename over the take");

                    if (auto* ruler = findChild<EditTimeRuler> (show.page()))
                    {
                        const int x = rulerFlagX (*ruler, engine, 0.5);
                        ruler->mouseDoubleClick (mouseAt (*ruler, { x, 10 }, { x, 10 }, false));
                        expectEquals (modalCount(), 0, "the ruler opened Rename over the take");
                    }
                    dismissModals();
                    expect (stopTake (show), "setup: the take never stopped");
                }
                dismissModals();
                main.cues.clear();
                main.currentCueIndex = -1;
            }
        }

        void cueEditsSurviveTimelineUndo (MainComponent& main)
        {
            const auto cue = [] (const juce::String& name, juce::int64 at)
            {
                SetlistBar::Cue c;
                c.name = name;
                c.samplePos = at;
                return c;
            };
            const auto names = [&main]
            {
                juce::StringArray out;
                for (const auto& c : main.cues) out.add (c.name + "@" + juce::String (c.samplePos));
                return out.joinIntoString (",");
            };

            beginTest ("Undo and redo of Ripple Delete keep a cue updated since, once, where it was updated");
            {
                Show show (main);
                show.session ({ "KICK" }, false, false);
                auto& engine = show.engine;
                main.cues = { cue ("Intro", 4800), cue ("Song 2", 36000) };
                main.currentCueIndex = -1;
                expect (settle (show), "setup: the session never settled");
                main.flushMixerUndo();   // the setup's mixer changes are not the step undone
                expect (main.rippleTimeline ("Ripple Delete", engine.allTimelineTracks(), 9600, 14400) > 0,
                        "setup: the ripple");
                expectEquals (names(), juce::String ("Intro@4800,Song 2@31200"), "setup: the cue moved with the audio");

                main.currentCueIndex = 1;
                engine.getPlayer().setPositionSamples (24000);
                main.updateCueAtTransport();
                settle (show);
                main.editUndo();
                expectEquals (names(), juce::String ("Intro@4800,Song 2@24000"),
                              "undo brought back the cue as it was before its Update");
                settle (show);
                main.editRedo();
                expectEquals (names(), juce::String ("Intro@4800,Song 2@24000"),
                              "redo kept the stale copy of the updated cue");
            }

            beginTest ("Undo of Ripple Delete keeps a cue's new name, and a cue deleted since stays deleted");
            {
                Show show (main);
                show.session ({ "KICK" }, false, false);
                auto& engine = show.engine;
                main.cues = { cue ("Intro", 4800), cue ("Song 2", 36000) };
                main.currentCueIndex = -1;
                expect (settle (show), "setup: the session never settled");
                main.flushMixerUndo();   // the setup's mixer changes are not the step undone
                expect (main.rippleTimeline ("Ripple Delete", engine.allTimelineTracks(), 9600, 14400) > 0,
                        "setup: the ripple");
                main.cues[1].name = "Encore";              // what Rename Cue does
                main.currentCueIndex = 0;
                main.setlistBar.onDeleteCue();             // Delete cue on "Intro"
                settle (show);
                main.editUndo();
                expectEquals (names(), juce::String ("Encore@36000"),
                              "undo revived a deleted cue or the old name of a renamed one");
                settle (show);
                main.editRedo();
                expectEquals (names(), juce::String ("Encore@31200"), "redo lost the rename");
            }

            beginTest ("Undo of Ripple Delete keeps a marker's new name and does not revive a deleted one");
            {
                Show show (main);
                show.session ({ "KICK" }, false, false);
                auto& engine = show.engine;
                main.cues.clear();
                main.currentCueIndex = -1;
                auto& markers = engine.getMarkers();
                markers.replaceAll ({});
                markers.drop (36000, "Verse");
                const auto verse = markers.lastDroppedId;
                markers.drop (40000, "Bridge");
                const auto bridge = markers.lastDroppedId;
                expect (settle (show), "setup: the session never settled");
                main.flushMixerUndo();   // the setup's mixer changes are not the step undone
                expect (main.rippleTimeline ("Ripple Delete", engine.allTimelineTracks(), 9600, 14400) > 0,
                        "setup: the ripple");
                markers.renameMarker (markers.indexOfId (verse), "Chorus");
                markers.removeById (bridge);
                settle (show);
                main.editUndo();
                expectEquals (markers.getCount(), 1, "undo revived a marker deleted since");
                const int at = markers.indexOfId (verse);
                expect (at >= 0, "the renamed marker went");
                if (at >= 0)
                {
                    expectEquals (markers.getMarker (at).name, juce::String ("Chorus"), "undo put the old name back");
                    expectEquals (markers.getMarker (at).sampleOffset, (juce::int64) 36000, "the marker did not move back");
                }
                markers.replaceAll ({});
            }
            main.cues.clear();
            main.currentCueIndex = -1;
        }

        static int cacheEntries (juce::AudioThumbnailCache& cache)
        {
            juce::MemoryOutputStream out;
            cache.writeToStream (out);
            juce::MemoryInputStream in (out.getData(), out.getDataSize(), false);
            in.readInt();            // magic
            return in.readInt();     // entry count
        }

        void continuedTakesKeepOneWaveform()
        {
            beginTest ("A continued take's waveform replaces the shorter one in memory and in WaveCache.wfm");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                const auto take = takeIn (s.dir, 1);
                row.setWaveformFiles (take, {});
                expect (pumpUntil ([&] { return row.waveformsLoaded() && cacheEntries (s.cache) == 1; }, 5000),
                        "setup: the first take never scanned");
                for (int part = 2; part <= 3; ++part)
                {
                    expect (writeWav (take.getSiblingFile ("Track_01_part" + juce::String::formatted ("%02d", part) + ".wav"), kRate),
                            "setup: the continued part");
                    row.reloadCurrentWaveformFiles();
                    expect (pumpUntil ([&] { return row.waveformsLoaded()
                                                 && row.getWaveformSecondsForTests() > part - 0.5; }, 5000),
                            "setup: the continued take never scanned");
                    pumpUntil ([&] { return cacheEntries (s.cache) == 1; }, 1000);
                    expectEquals (cacheEntries (s.cache), 1,
                                  "the shorter take's peaks stayed cached beside the continued take");
                }
            }
        }

        void clipPromptsFollowTheirClip()
        {
            const auto prompt = [] { return dynamic_cast<juce::AlertWindow*> (topModal()); };

            beginTest ("Rename clip and Set gain land on the clip they opened on, whatever moved under them");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                for (juce::int64 at : { 12000, 24000, 36000 }) s.engine.splitTrackAtSample (0, at);
                expectEquals ((int) s.clips (0).size(), 4, "setup: four clips");
                const auto clipAt24k = [&]() -> const Clip*
                {
                    for (const auto& c : s.clips (0)) if (c.timelineStartSamples == 24000) return &c;
                    return nullptr;
                };

                row.applyClipMenuChoice (2, 412);
                if (auto* aw = prompt())
                {
                    s.engine.deleteClip (0, 0);   // the list shifts under the prompt
                    aw->getTextEditor ("name")->setText ("KEEP");
                    aw->exitModalState (1);
                }
                else expect (false, "setup: the rename prompt");
                // The answer arrives a moment after the prompt leaves.
                pumpUntil ([&] { return clipAt24k() != nullptr && clipAt24k()->name == "KEEP"; }, 1000);
                expect (clipAt24k() != nullptr && clipAt24k()->name == "KEEP", "the clip the prompt was for kept its name");
                for (const auto& c : s.clips (0))
                    if (c.timelineStartSamples != 24000)
                        expect (c.name != "KEEP", "another clip took the name");

                const int idx = (int) (clipAt24k() - s.clips (0).data());
                row.applyClipMenuChoice (idx, 510);
                if (auto* aw = prompt())
                {
                    s.engine.deleteClip (0, 0);
                    aw->getTextEditor ("dB")->setText ("-7");
                    aw->exitModalState (1);
                }
                else expect (false, "setup: the gain prompt");
                pumpUntil ([&] { return clipAt24k() != nullptr && std::abs (clipAt24k()->gainDb + 7.0f) < 0.01f; }, 1000);
                expect (clipAt24k() != nullptr && std::abs (clipAt24k()->gainDb + 7.0f) < 0.01f,
                        "the gain missed the clip the prompt was for");
                for (const auto& c : s.clips (0))
                    if (c.timelineStartSamples != 24000)
                        expect (std::abs (c.gainDb) < 0.01f, "another clip took the gain");
            }

            beginTest ("A clip prompt whose clip went changes nothing");
            {
                Session s (1, { kRate });
                Row row (0, false, s.engine, s.formats, s.cache);
                sizeRow (row);
                s.engine.splitTrackAtSample (0, 16000);
                s.engine.splitTrackAtSample (0, 32000);
                expectEquals ((int) s.clips (0).size(), 3, "setup: three clips");
                row.applyClipMenuChoice (1, 510);
                if (auto* aw = prompt())
                {
                    s.engine.deleteClip (0, 1);
                    aw->getTextEditor ("dB")->setText ("-7");
                    aw->exitModalState (1);
                }
                else expect (false, "setup: the gain prompt");
                pumpUntil ([] { return false; }, 300);   // the answer arrives a moment after the prompt leaves
                for (const auto& c : s.clips (0))
                    expect (std::abs (c.gainDb) < 0.01f, "a gain meant for a deleted clip landed on another");
            }
        }
    };

    static EditRowFixTests editRowFixTests;
}
