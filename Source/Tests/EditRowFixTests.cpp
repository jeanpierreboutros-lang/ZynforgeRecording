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
        static constexpr int kLaneW = 1000;
        using Row  = EditPage::TrackRow;
        using Lane = Row::LaneMode;

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
            AudioEngine::setTestModeSkipAudioInit (true);
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
                main.rebuildStripsIfLayoutChanged();
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
    };

    static EditRowFixTests editRowFixTests;
}
