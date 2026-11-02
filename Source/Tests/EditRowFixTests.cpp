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

#include <atomic>
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

        void theMasterIsNotAChannel (MainComponent& main)
        {
            beginTest ("-> Master in the MIXER keeps the strip's input, on both halves of a pair");
            {
                Show show (main);
                show.session ({ "KEYS", "VOX" }, true, true);
                main.rebuildStrips();
                expectEquals ((int) main.strips.size(), 2, "setup: a pair and a mono strip");

                show.engine.setTrackLinkedRouting (2, 6);
                auto* mono = outputComboOf (*main.strips[1]);
                expect (mono != nullptr, "setup: the mono strip's output combo");
                if (mono != nullptr)
                {
                    mono->setSelectedId (1, juce::sendNotificationSync);
                    expectEquals (patchOf (show.engine, 2), juce::String ("6/-1"),
                                  "the master took the strip's input with it");
                    mono->setSelectedId (8, juce::sendNotificationSync);    // Out 7
                    expectEquals (patchOf (show.engine, 2), juce::String ("6/6"), "a real channel stays linked");
                }

                show.engine.setTrackLinkedRouting (0, 4);
                show.engine.setTrackLinkedRouting (1, 5);
                auto* pair = outputComboOf (*main.strips[0]);
                expect (pair != nullptr, "setup: the pair's output combo");
                if (pair != nullptr)
                {
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

            beginTest ("-> Master is refused during a take and its finalization");
            {
                const auto dir = tempDir ("take");
                expect (dir.createDirectory().wasOk());
                {
                    AudioEngine engine;
                    engine.clearAllStripOverrides();
                    engine.setStripCount (1);
                    engine.prepareForTests ((double) kRate, 256);
                    engine.getRecorder().getTrack (0).armed.store (true);
                    engine.setTrackLinkedRouting (0, 5);
                    engine.setActiveSessionDir (dir);
                    engine.claimMixerFor (dir);
                    expect (engine.startRecording (dir));
                    engine.sendStripToMaster (0, false);
                    expectEquals (patchOf (engine, 0), juce::String ("5/5"), "the output moved under a take");

                    juce::WaitableEvent entered, release;
                    std::atomic<bool> completed { false };
                    engine.getRecorder().beforeFinalizationForTests = [&] { entered.signal(); release.wait (5000); };
                    const juce::ScopeGuard releaseWorker { [&] { release.signal(); } };
                    expect (engine.stopRecordingAsync ([&] (bool) { completed = true; }));
                    expect (entered.wait (2000), "STOP never reached finalization");
                    engine.sendStripToMaster (0, false);
                    expectEquals (patchOf (engine, 0), juce::String ("5/5"), "the output moved while the take finalized");
                    engine.setTrackLinkedRouting (0, 2);
                    expectEquals (patchOf (engine, 0), juce::String ("5/5"),
                                  "half a linked patch landed while the take finalized");
                    release.signal();
                    expect (pumpUntil ([&] { return completed.load(); }, 10000), "STOP did not complete");
                    engine.clearAllStripOverrides();
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
                dir.deleteRecursively();
            }
        }
    };

    static EditRowFixTests editRowFixTests;
}
