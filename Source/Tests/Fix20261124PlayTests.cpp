// Playback fixes from the 2026-11-24 audit. Each test holds a promise the
// engineer hears at the desk: dragging a clip handle while the show loops never
// drops a block, a loop whose start sits just before a clip wraps without a
// stall on every pass, and the Markers list shows where the markers are now and
// locates the one its row shows.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/SessionPlayer.h"
#include "../UI/MarkerListDialog.h"

#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <functional>
#include <thread>

namespace zynforge
{
    class Fix20261124PlayTests final : public juce::UnitTest
    {
    public:
        Fix20261124PlayTests() : juce::UnitTest ("Fixes 2026-11-24 playback", "zynforge") {}

        static constexpr int kRate = 48000;
        static constexpr int kBlock = 512;

        struct Folder
        {
            juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zf-play1124-" + juce::Uuid().toString());
            Folder() { dir.getChildFile ("Audio Files").createDirectory(); }
            ~Folder() { dir.deleteRecursively(); }
            juce::File track (int n) const
            {
                return dir.getChildFile ("Audio Files").getChildFile ("Track_" + juce::String::formatted ("%02d.wav", n));
            }
        };

        // Mono 24-bit WAV holding `value` throughout.
        static bool writeTake (const juce::File& file, juce::int64 samples, float value)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
                file.createOutputStream().release(), (double) kRate, 1, 24, {}, 0));
            if (writer == nullptr) return false;
            constexpr int chunk = 32768;
            juce::AudioBuffer<float> data (1, chunk);
            for (int i = 0; i < chunk; ++i) data.setSample (0, i, value);
            for (juce::int64 at = 0; at < samples; at += chunk)
                if (! writer->writeFromAudioSampleBuffer (data, 0, (int) juce::jmin ((juce::int64) chunk, samples - at)))
                    return false;
            return true;
        }

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
            return predicate();
        }
        static void pumpFor (int ms) { pumpUntil ([] { return false; }, ms); }

        static Clip clip (juce::int64 timelineStart, juce::int64 fileStart, juce::int64 length)
        {
            Clip c;
            c.timelineStartSamples = timelineStart;
            c.fileStartSamples     = fileStart;
            c.fileLengthSamples    = length;
            return c;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };

            clipEditsNeverDropABlock();          // PLAY-01
            loopWrapCuesEveryClipItWaitsFor();   // PLAY-02
            markersListFollowsTheMarkers();      // PLAY-04
        }

    private:
        // ── PLAY-01 ───────────────────────────────────────────────────────
        void clipEditsNeverDropABlock()
        {
            beginTest ("PLAY-01: clip edits while the session plays never drop a block");
            Folder f;
            constexpr int kTracks = 4;
            for (int t = 1; t <= kTracks; ++t)
                expect (writeTake (f.track (t), (juce::int64) kRate * 30, 0.25f), "setup: take written");
            SessionPlayer player;
            player.prepare ((double) kRate, kBlock);
            expectEquals (player.loadSession (f.dir), kTracks);

            // Short clips back to back over the first 20 s: the same audio as
            // the take, cut as finely as a heavily edited show. Two different
            // cuts of it, so every edit really changes the list.
            const auto pieces = [] (int length)
            {
                std::vector<Clip> list;
                for (juce::int64 at = 0; at < (juce::int64) kRate * 20; at += length)
                    list.push_back (clip (at, at, length));
                return list;
            };
            const auto cutA = pieces (2400), cutB = pieces (2000);
            for (int t = 0; t < kTracks; ++t) player.setTrackClips (t, cutA);

            player.setPositionSamples (kRate);
            player.start();
            juce::AudioBuffer<float> out (kTracks, kBlock);
            const auto render = [&]
            {
                out.clear();
                player.processBlock (out.getArrayOfWritePointers(), kTracks, kBlock);
            };
            // The locate is a jump: it may stand still until the takes are in RAM.
            for (int i = 0; i < 400 && player.getPositionSamples() == kRate; ++i)
            {
                render();
                juce::Thread::sleep (2);
            }
            expect (player.getPositionSamples() > kRate, "setup: playback never started");

            // A drag: the clip list of every track replaced again and again.
            std::atomic<bool> stopEditing { false };
            std::atomic<int> edits { 0 };
            std::thread editor ([&]
            {
                for (int n = 0; ! stopEditing.load(); ++n)
                    for (int t = 0; t < kTracks; ++t)
                    {
                        player.setTrackClips (t, (n & 1) != 0 ? cutB : cutA);
                        ++edits;
                    }
            });

            int dropped = 0, silent = 0;
            for (int blocks = 0; blocks < 300; ++blocks)
            {
                const auto before = player.getPositionSamples();
                render();
                if (player.getPositionSamples() - before != kBlock) ++dropped;
                else
                {
                    bool gap = false;
                    for (int ch = 0; ch < kTracks && ! gap; ++ch)
                        for (int s = 0; s < kBlock && ! gap; ++s)
                            gap = out.getSample (ch, s) == 0.0f;
                    if (gap) ++silent;
                }
                juce::Thread::sleep (2);
            }
            stopEditing = true;
            editor.join();
            player.stop();
            expect (edits.load() > 100, "setup: the edits did not run alongside the render");
            expectEquals (dropped, 0, "a clip edit made a block stand still (the render's locks were busy)");
            expectEquals (silent, 0, "a clip edit made a track silent for a block");
        }

        // ── PLAY-02 ───────────────────────────────────────────────────────
        void loopWrapCuesEveryClipItWaitsFor()
        {
            beginTest ("PLAY-02: a loop whose start sits just before a clip wraps without a stall, every pass");
            Folder f;
            expect (writeTake (f.track (1), (juce::int64) kRate * 60, 0.25f), "setup: take written");
            expect (writeTake (f.track (2), (juce::int64) kRate * 60, 0.5f), "setup: take written");
            SessionPlayer player;
            player.prepare ((double) kRate, kBlock);
            expectEquals (player.loadSession (f.dir), 2);

            const juce::int64 loopStart = kRate, loopEnd = (juce::int64) kRate * 4;
            const juce::int64 clipStart = loopStart + 4000;   // a few thousand samples into the loop
            // Track 1: the loop starts in a gap; the clip after it reads from 30 s.
            player.setTrackClips (0, { clip (clipStart, (juce::int64) kRate * 30, loopEnd + kRate - clipStart) });
            // Track 2: the loop starts inside a clip, and the next one (same
            // file, 40 s in) starts a few thousand samples later.
            player.setTrackClips (1, { clip (0, 0, clipStart),
                                       clip (clipStart, (juce::int64) kRate * 40, loopEnd + kRate - clipStart) });
            player.setLoopRegion (loopStart, loopEnd);
            player.setLoopEnabled (true);
            // Start inside the loop, past the clip heads: nothing has read them
            // when the first wrap comes, as after any pass long enough to let
            // them go.
            player.setPositionSamples (loopStart + kRate);
            player.start();

            juce::AudioBuffer<float> out (2, kBlock);
            bool started = false;
            int held = 0, wraps = 0, faults = 0;
            for (int i = 0; i < 900 && wraps < 3; ++i)
            {
                const auto before = player.getPositionSamples();
                out.clear();
                player.processBlock (out.getArrayOfWritePointers(), 2, kBlock);
                const auto after = player.getPositionSamples();
                const auto moved = after >= before ? after - before : (loopEnd - before) + (after - loopStart);
                if (after < before) ++wraps;
                if (moved > 0) started = true;
                else if (started) ++held;
                // Track 2 has audio everywhere in the loop: every sample the
                // playhead passed must have reached it.
                int heard = 0;
                for (int s = 0; s < kBlock; ++s) heard += out.getSample (1, s) != 0.0f ? 1 : 0;
                if (started && heard != (int) moved) ++faults;
                juce::Thread::sleep (2);
            }
            player.stop();
            expect (wraps >= 2, "setup: the loop did not wrap twice");
            expectEquals (held, 0, "a loop wrap stood still, silent, waiting for a clip just after the loop start");
            expectEquals (faults, 0, "a sample the playhead passed never reached the track");
        }

        // ── PLAY-04 ───────────────────────────────────────────────────────
        // Counts every repaint that reaches the component it is attached to.
        struct RepaintRecorder final : public juce::CachedComponentImage
        {
            explicit RepaintRecorder (std::atomic<int>& count) : repaints (count) {}
            void paint (juce::Graphics&) override {}
            bool invalidateAll() override { ++repaints; return true; }
            bool invalidate (const juce::Rectangle<int>& area) override
            {
                if (! area.isEmpty()) ++repaints;
                return true;
            }
            void releaseResources() override {}
            std::atomic<int>& repaints;
        };

        static juce::MouseEvent doubleClickOn (juce::Component& c)
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::Point<float> at (10.0f, 10.0f);
            return { juce::Desktop::getInstance().getMainMouseSource(), at,
                     juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, at, now, 2, false };
        }

        void markersListFollowsTheMarkers()
        {
            beginTest ("PLAY-04: the Markers list redraws when markers move, and a double-click locates the marker its row shows");
            Folder f;
            expect (writeTake (f.track (1), (juce::int64) kRate * 5, 0.25f), "setup: take written");
            AudioEngine engine;
            engine.clearAllStripOverrides();
            engine.prepareForTests ((double) kRate, kBlock);
            engine.setStripCount (1);
            engine.setActiveSessionDir (f.dir);
            expectEquals (engine.loadSession (f.dir), 1);
            auto& markers = engine.getMarkers();
            markers.clear();
            markers.drop (kRate, "Verse");
            const auto verse = markers.lastDroppedId;
            markers.drop ((juce::int64) kRate * 2, "Chorus");
            const auto chorus = markers.lastDroppedId;

            {
                MarkerListDialog dialog (engine);
                dialog.setVisible (true);
                auto* table = dynamic_cast<juce::TableListBox*> (dialog.getChildComponent (0));
                expect (table != nullptr, "setup: the list");
                if (table == nullptr) return;
                auto* model = table->getTableListBoxModel();
                std::atomic<int> repaints { 0 };
                dialog.setCachedComponentImage (new RepaintRecorder (repaints));
                pumpFor (600);   // the list's first refresh
                expectEquals (model->getNumRows(), 2);
                table->selectRow (0);   // Verse

                // Verse is dragged past Chorus: same count, new order.
                repaints = 0;
                markers.setMarkerSample (markers.indexOfId (verse), (juce::int64) kRate * 3);
                expectEquals (markers.indexOfId (chorus), 0, "setup: the markers re-sorted");

                // Before the list's next refresh its first row still shows Verse.
                model->cellDoubleClicked (0, 2, doubleClickOn (*table));
                expectEquals (engine.getPlayer().getPositionSamples(), (juce::int64) kRate * 3,
                              "a double-click located the marker now at that index, not the one its row shows");

                pumpFor (600);
                expect (repaints.load() > 0, "the list never redrew after a marker moved: its times and order are stale");
                expectEquals (table->getSelectedRow(), 1, "the selection stayed on the row, not on the marker");
                model->cellDoubleClicked (0, 2, doubleClickOn (*table));
                expectEquals (engine.getPlayer().getPositionSamples(), (juce::int64) kRate * 2,
                              "after the refresh the first row does not locate Chorus");

                // A rename keeps the count too.
                repaints = 0;
                markers.renameMarker (markers.indexOfId (chorus), "Hook");
                pumpFor (600);
                expect (repaints.load() > 0, "the list never redrew after a rename");
                dialog.setCachedComponentImage (nullptr);
            }
            markers.clear();
            engine.setActiveSessionDir ({});
        }
    };

    static Fix20261124PlayTests fix20261124PlayTests;
}
