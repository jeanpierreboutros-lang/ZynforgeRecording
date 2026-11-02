// EDIT rows against the rest of the window: whole-session swaps, the three
// views' routing, and the row's gestures. Driven through a real MainComponent
// and the row's real mouse handlers.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../UI/ChannelStrip.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"
#include "../UI/MainComponent.h"
#include "../UI/PatchPage.h"
#include "../UI/RenameLabel.h"

#include <cmath>
#include <functional>
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
            liveEnvelopeMeetsThePlayhead();
            scanRepaintsOnceMore();
            fadeToolGrabs();
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

                const int aEnd = laneX (row, 0.5);
                drag (row, { aEnd - 2, bodyY }, { aEnd - 22, bodyY });
                expect (s.clips (0)[0].fileLengthSamples < 24000, "the outgoing clip's end was out of reach");

                const int bStart = laneX (row, 0.45);
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
    };

    static EditRowFixTests editRowFixTests;
}
