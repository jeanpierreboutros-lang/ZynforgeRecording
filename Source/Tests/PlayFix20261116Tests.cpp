// Playback, EDIT view and cue fixes from the 2026-11-16 audit. Each test holds
// the engineer-facing promise: a good take reloads clean after STOP, a strip
// whose take will not open still comes back, a loop plays every sample, a song
// scan run twice leaves one set of markers, the QC report reads in timeline
// time, Duplicate never plays two clips summed, a cue never lands on the wrong
// strips, and the EDIT lanes keep what they were showing.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/PlaybackReader.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/SessionPlayer.h"
#include "../UI/EditPage.h"
#include "../UI/EditTimeline.h"
#include "../UI/EditTrackRow.h"
#include "../UI/FloatingLaunch.h"
#include "../UI/LivePeakHistory.h"
#include "../UI/MainComponent.h"
#include "../UI/SessionProjPath.h"
#include "../UI/WaveCacheFile.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cmath>
#include <functional>

namespace zynforge
{
    class PlayFix20261116Tests final : public juce::UnitTest
    {
    public:
        PlayFix20261116Tests() : juce::UnitTest ("Play fixes 2026-11-16", "zynforge") {}

        static constexpr int kRate = 48000;

        struct Folder
        {
            juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zf-playfix-" + juce::Uuid().toString());
            Folder() { dir.getChildFile ("Audio Files").createDirectory(); }
            ~Folder() { dir.deleteRecursively(); }
            juce::File audio (const juce::String& name) const
            { return dir.getChildFile ("Audio Files").getChildFile (name); }
            juce::File track (int n) const
            { return audio ("Track_" + juce::String::formatted ("%02d.wav", n)); }
        };

        // Mono 24-bit WAV holding `value`, with full-scale samples at `clipAt`
        // (a run long enough for the QC clip test) when it is not negative.
        static bool writeTake (const juce::File& file, juce::int64 samples, float value = 0.25f,
                               juce::int64 clipAt = -1)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
                file.createOutputStream().release(), (double) kRate, 1, 24, {}, 0));
            if (writer == nullptr) return false;
            constexpr int chunk = 32768;
            juce::AudioBuffer<float> data (1, chunk);
            for (juce::int64 at = 0; at < samples; at += chunk)
            {
                const int n = (int) juce::jmin ((juce::int64) chunk, samples - at);
                for (int i = 0; i < n; ++i)
                {
                    const auto pos = at + i;
                    data.setSample (0, i, clipAt >= 0 && pos >= clipAt && pos < clipAt + 8 ? 1.0f : value);
                }
                if (! writer->writeFromAudioSampleBuffer (data, 0, n)) return false;
            }
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

        static void setupEngine (AudioEngine& engine, int strips)
        {
            engine.clearAllStripOverrides();
            engine.prepareForTests ((double) kRate, 512);
            engine.setStripCount (strips);
        }

        static void openSession (AudioEngine& engine, const juce::File& dir, int strips)
        {
            setupEngine (engine, strips);
            engine.setActiveSessionDir (dir);
            engine.claimMixerFor (dir);
            engine.loadSession (dir);
            engine.setStripCount (strips);
        }

        static std::vector<EditPage::TrackRow*> rowsOf (juce::Component& page)
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
            return rows;
        }

        static juce::Viewport* viewportOf (juce::Component& page)
        {
            for (auto* child : page.getChildren())
                if (auto* viewport = dynamic_cast<juce::Viewport*> (child)) return viewport;
            return nullptr;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };

            slowFirstReadIsNotAFailure();        // PLAY-04
            unopenedTopStripIsCounted();         // PLAY-06
            loopCuesTheNextClip();               // PLAY-07
            duplicateNeverPlaysSummed();         // PLAY-10
            liveEnvelopeHolds();                 // EDIT-02, EDIT-05
            liveSpanNavigation();                // EDIT-03
            cleanCacheLoadIsNotRewritten();      // EDIT-04

            MainComponent main;
            main.setSize (1320, 820);
            duplicateFromTheEditMenu (main);     // PLAY-10
            songScanReplacesItsMarkers (main);   // PLAY-08
            qcReadsInTimelineTime (main);        // PLAY-09
            cuesBindByStripId (main);            // CUE-REBIND
        }

    private:
        // ── PLAY-04 ───────────────────────────────────────────────────────
        void slowFirstReadIsNotAFailure()
        {
            beginTest ("A take whose first read is slow still reloads as complete");
            {
                Folder f;
                for (int t = 1; t <= 8; ++t) expect (writeTake (f.track (t), kRate * 2), "setup: take written");
                SessionPlayer player;
                // Every first read misses: the disk is slower than the wait.
                player.setPrimeTimeoutForTests (0);
                auto prepared = player.prepareSessionLoad (f.dir, (double) kRate, 512);
                expect (SessionPlayer::preparedLoadComplete (prepared),
                        "a first read that was only slow reported the take could not be reopened");

                Clip other;
                other.audioFile = f.track (2);
                other.fileLengthSamples = kRate;
                auto withClip = player.prepareSessionLoad (f.dir, (double) kRate, 512, { other });
                expect (SessionPlayer::preparedLoadComplete (withClip),
                        "a slow first read of a clip's own file reported the load incomplete");
            }

            beginTest ("A first read the disk fails is still a failure, and a slow one is only pending");
            {
                struct Source final : public juce::AudioFormatReader
                {
                    explicit Source (bool fail) : juce::AudioFormatReader (nullptr, "test"), failing (fail)
                    {
                        sampleRate = 48000.0; lengthInSamples = kRate * 4; numChannels = 1;
                        bitsPerSample = 32; usesFloatingPointData = true;
                    }
                    bool readSamples (int* const* dest, int numDest, int offset, juce::int64, int num) override
                    {
                        while (gate.load()) juce::Thread::sleep (1);
                        for (int ch = 0; ch < numDest; ++ch)
                            if (auto* out = reinterpret_cast<float*> (dest[ch]))
                                juce::FloatVectorOperations::fill (out + offset, 0.5f, num);
                        return ! failing;
                    }
                    std::atomic<bool> gate { false };
                    bool failing;
                };
                juce::TimeSliceThread thread ("prime test");
                thread.startThread();
                {
                    PlaybackReader failed (new Source (true), thread, kRate);
                    expect (failed.prime (0, 512, 2000) == PlaybackReader::Prime::failed,
                            "a block the source could not read passed as primed");
                    auto* slowSource = new Source (false);
                    slowSource->gate = true;
                    PlaybackReader slow (slowSource, thread, kRate);
                    expect (slow.prime (0, 512, 20) == PlaybackReader::Prime::pending, "a slow read was not pending");
                    slowSource->gate = false;
                    expect (slow.prime (0, 512, 2000) == PlaybackReader::Prime::ready, "the slow read never came in");
                }
                thread.stopThread (-1);
            }

            beginTest ("A take that will not open still makes the reload incomplete");
            {
                Folder f;
                expect (writeTake (f.track (1), kRate));
                expect (writeTake (f.audio ("Track_02_part02.wav"), kRate));   // its main is missing
                SessionPlayer player;
                player.setPrimeTimeoutForTests (0);
                auto prepared = player.prepareSessionLoad (f.dir, (double) kRate, 512);
                expect (! SessionPlayer::preparedLoadComplete (prepared), "a take that does not open loaded as complete");
            }
        }

        // ── PLAY-06 ───────────────────────────────────────────────────────
        void unopenedTopStripIsCounted()
        {
            beginTest ("A top strip whose take will not open is still counted");
            {
                Folder f;
                expect (writeTake (f.track (1), kRate));
                // Strip 3 recorded only its continuation: its lead-in is owed.
                expect (writeTake (f.audio ("Track_03_part02.wav"), kRate));
                SessionPlayer player;
                expectEquals (player.loadSession (f.dir), 3,
                              "crash recovery would size the mixer without the strip whose take did not open");
                expect (player.trackFailedToOpen (2), "the strip is not reported as failing to open");
                expect (player.isLoaded());

                AudioEngine engine;
                setupEngine (engine, 0);
                expectEquals (engine.loadSession (f.dir), 3, "the engine's count left the strip out");
            }
        }

        // ── PLAY-07 ───────────────────────────────────────────────────────
        void loopCuesTheNextClip()
        {
            beginTest ("One reader holds the next clip's cue and the loop start's at once");
            {
                struct Source final : public juce::AudioFormatReader
                {
                    Source() : juce::AudioFormatReader (nullptr, "test")
                    {
                        sampleRate = 48000.0; lengthInSamples = (juce::int64) kRate * 120; numChannels = 1;
                        bitsPerSample = 32; usesFloatingPointData = true;
                    }
                    bool readSamples (int* const* dest, int numDest, int offset, juce::int64, int num) override
                    {
                        for (int ch = 0; ch < numDest; ++ch)
                            if (auto* out = reinterpret_cast<float*> (dest[ch]))
                                juce::FloatVectorOperations::fill (out + offset, 0.5f, num);
                        return true;
                    }
                };
                juce::TimeSliceThread thread ("cue test");
                thread.startThread();
                {
                    PlaybackReader reader (new Source(), thread, kRate * 2);
                    const juce::int64 playing = (juce::int64) kRate * 10, clip = (juce::int64) kRate * 60,
                                      wrap = (juce::int64) kRate * 90;
                    juce::AudioBuffer<float> out (1, 512);
                    reader.read (&out, 0, 512, playing, true, true);
                    reader.cue (clip, PlaybackReader::kNextClipCue);
                    reader.cue (wrap, PlaybackReader::kLoopWrapCue);
                    const auto both = [&] { return reader.isBuffered (clip, 512) && reader.isBuffered (wrap, 512)
                                                && reader.isBuffered (playing, 512); };
                    for (int i = 0; i < 3000 && ! both(); ++i)
                    {
                        reader.prepareAt (playing);
                        juce::Thread::sleep (1);
                    }
                    expect (both(), "a cue or the read window lost its block to the other cue");
                }
                thread.stopThread (-1);
            }

            beginTest ("A loop plays a clip that starts just before the loop end, every pass");
            {
                Folder f;
                expect (writeTake (f.track (1), (juce::int64) kRate * 60, 0.25f));
                SessionPlayer player;
                constexpr int block = 512;
                player.prepare ((double) kRate, block);
                expectEquals (player.loadSession (f.dir), 1);

                // The clip after 3.5 s reads from 40 s into the file: a block
                // no read window or loop start holds.
                Clip a, b;
                a.fileLengthSamples = kRate * 7 / 2;
                b.timelineStartSamples = kRate * 7 / 2;
                b.fileStartSamples = (juce::int64) kRate * 40;
                b.fileLengthSamples = kRate * 6;
                player.setTrackClips (0, { a, b });
                player.setLoopRegion (kRate, kRate * 4);
                player.setLoopEnabled (true);
                player.setPositionSamples (kRate);
                player.start();

                juce::AudioBuffer<float> out (1, block);
                int faults = 0;
                juce::int64 played = 0;
                for (int i = 0; i < 700; ++i)
                {
                    const auto before = player.getPositionSamples();
                    float* channels[] { out.getWritePointer (0) };
                    player.processBlock (channels, 1, block);
                    const auto after = player.getPositionSamples();
                    const auto moved = after >= before ? after - before
                                                       : (player.getLoopEnd() - before) + (after - player.getLoopStart());
                    int heard = 0;
                    for (int s = 0; s < block; ++s) heard += out.getSample (0, s) != 0.0f ? 1 : 0;
                    if (heard != (int) moved) ++faults;
                    played += moved;
                    juce::Thread::sleep (2);
                }
                player.stop();
                expect (played > (juce::int64) kRate * 5, "setup: the loop did not play two passes");
                expectEquals (faults, 0, "audio dropped out where the clip near the loop end starts");
            }
        }

        // ── PLAY-10 ───────────────────────────────────────────────────────
        float renderedAt (AudioEngine& engine, int track, juce::int64 total, int sample)
        {
            juce::AudioBuffer<float> out;
            expect (engine.renderTrackArrangement (track, out, total), "render failed");
            return out.getNumSamples() > sample ? out.getSample (0, sample) : -1.0f;
        }

        static bool anyOverlap (const std::vector<Clip>& clips)
        {
            for (size_t i = 0; i < clips.size(); ++i)
                for (size_t j = i + 1; j < clips.size(); ++j)
                {
                    const auto& a = clips[i];
                    const auto& b = clips[j];
                    if (a.timelineStartSamples < b.timelineStartSamples + b.fileLengthSamples
                        && b.timelineStartSamples < a.timelineStartSamples + a.fileLengthSamples)
                        return true;
                }
            return false;
        }

        void duplicateNeverPlaysSummed()
        {
            beginTest ("Duplicate never lays its copy over the next clip");
            {
                Folder f;
                expect (writeTake (f.track (1), kRate * 4, 0.25f));
                AudioEngine engine;
                openSession (engine, f.dir, 1);
                expect (engine.splitTrackAtSample (0, kRate), "setup: split");
                expect (engine.duplicateClip (0, 0), "the duplicate was refused");
                expect (! anyOverlap (engine.clipsFor (0)), "the copy sits on top of the next clip");
                expectWithinAbsoluteError (renderedAt (engine, 0, kRate * 4, kRate * 3 / 2), 0.25f, 0.001f,
                                           "the copy and the clip under it play summed");
                expectWithinAbsoluteError (renderedAt (engine, 0, kRate * 4, kRate * 3), 0.25f, 0.001f,
                                           "the audio after the copy went");
            }

            beginTest ("Duplicate refuses when a locked clip is in the way, and changes nothing");
            {
                Folder f;
                expect (writeTake (f.track (1), kRate * 4, 0.25f));
                AudioEngine engine;
                openSession (engine, f.dir, 1);
                expect (engine.splitTrackAtSample (0, kRate), "setup: split");
                expect (engine.setClipLocked (0, 1, true), "setup: lock");
                const auto before = juce::JSON::toString (engine.playlistsToJson());
                expect (! engine.duplicateClip (0, 0), "a copy was laid over a locked clip");
                expectEquals (juce::JSON::toString (engine.playlistsToJson()), before, "a refused duplicate changed the clips");
            }
        }

        // ── EDIT-02 / EDIT-05 ─────────────────────────────────────────────
        void liveEnvelopeHolds()
        {
            AudioEngine engine;
            setupEngine (engine, 1);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            juce::AudioThumbnailCache cache (8);
            constexpr int held = 400;

            const auto holdTake = [] (EditPage::TrackRow& row)
            {
                row.setLiveRecording (true);
                for (int i = 0; i < held; ++i) row.appendLivePeakL (0.5f);
                row.holdLiveEnvelopeUntilScanned();
            };

            beginTest ("RECORD during the post-STOP rescan keeps the take it was holding");
            {
                EditPage::TrackRow row (0, false, engine, formats, cache);
                holdTake (row);
                expect (row.isHoldingLiveEnvelope(), "setup: the row holds its take");
                // A continue: the new audio goes after the held take.
                row.setLiveRecording (true, held + 50);
                expectWithinAbsoluteError (row.getLiveEnvelopePeakForTests (0, held), 0.5f, 0.001f,
                                           "the previous take vanished from the lane for the next take");
                expectEquals (row.getLiveEnvelopePeakForTests (held, held + 50), 0.0f,
                              "the gap before the new audio is not silent");
                expectEquals (row.getLiveEnvelopeSamplesForTests(),
                              (juce::int64) (held + 50) * TrackState::kLiveBinSamples,
                              "the new audio does not start at the record head");
            }

            beginTest ("A row that does not record the next take keeps holding its own");
            {
                EditPage::TrackRow row (0, false, engine, formats, cache);
                holdTake (row);
                // The next take stops; this row was not armed for it.
                row.holdLiveEnvelopeUntilScanned();
                expect (row.isHoldingLiveEnvelope(),
                        "a row that did not record dropped the take it was still waiting to scan");
                expectWithinAbsoluteError (row.getLiveEnvelopePeakForTests (0, held), 0.5f, 0.001f);
            }

            beginTest ("Silence padded in one step reads exactly as silence appended bin by bin");
            {
                juce::Random random (0x51e7);
                for (int round = 0; round < 6; ++round)
                {
                    LivePeakHistory padded (TrackState::kLiveBinSamples), appended (TrackState::kLiveBinSamples);
                    for (int step = 0; step < 12; ++step)
                    {
                        const int loud = random.nextInt (3000);
                        for (int i = 0; i < loud; ++i)
                        {
                            const float p = random.nextFloat();
                            padded.append (p);
                            appended.append (p);
                        }
                        const juce::int64 gap = round < 3 ? random.nextInt (5000) : random.nextInt (400000);
                        padded.appendSilence (gap);
                        for (juce::int64 i = 0; i < gap; ++i) appended.append (0.0f);
                    }
                    bool same = padded.binCount() == appended.binCount() && padded.size() == appended.size()
                             && padded.getBinsPerPoint() == appended.getBinsPerPoint();
                    for (size_t i = 0; same && i < padded.size(); ++i) same = padded.at (i) == appended.at (i);
                    for (int r = 0; same && r < 200; ++r)
                    {
                        const auto a = (juce::int64) random.nextInt ((int) juce::jmax ((juce::int64) 1, padded.binCount()));
                        const auto b = a + random.nextInt (100000);
                        same = padded.maxPeakInBinRange (a, b) == appended.maxPeakInBinRange (a, b);
                    }
                    expect (same, "padded silence differs from appended silence in round " + juce::String (round));
                }
            }

            beginTest ("A long gap in the helper's status pads the live waveform instead of wiping it");
            {
                EditPage::TrackRow row (0, false, engine, formats, cache);
                row.setLiveRecording (true);
                row.appendRemotePeakL (0.5f, 100);
                // Over 44 s without a status at 48 kHz: 9000 bins.
                row.appendRemotePeakL (0.75f, 100 + 9000);
                expectWithinAbsoluteError (row.getLiveEnvelopePeakForTests (0, 100), 0.5f, 0.001f,
                                           "everything drawn before the gap was wiped");
                expectEquals (row.getLiveEnvelopePeakForTests (100, 100 + 8999), 0.0f, "the gap is not drawn silent");
                expectWithinAbsoluteError (row.getLiveEnvelopePeakForTests (100 + 8999, 100 + 9000), 0.75f, 0.001f);
                expectEquals (row.getLiveEnvelopeSamplesForTests(),
                              (juce::int64) (100 + 9000) * TrackState::kLiveBinSamples,
                              "the waveform does not reach the capture clock");
            }
        }

        // ── EDIT-03 ───────────────────────────────────────────────────────
        void liveSpanNavigation()
        {
            Folder f;
            const auto takeLength = (juce::int64) kRate * 60;
            expect (writeTake (f.track (1), takeLength));
            AudioEngine engine;
            openSession (engine, f.dir, 1);
            EditPage page (engine);
            page.setBounds (0, 0, 1400, 300);
            page.refresh();
            auto* viewport = viewportOf (page);
            expect (viewport != nullptr, "setup: the page's viewport");
            if (viewport == nullptr) return;

            auto& rec = engine.getRecorder();
            rec.getTrack (0).armed.store (true);
            engine.armContinue (takeLength);
            expect (engine.startRecording (f.dir), "setup: " + rec.getStartError());
            std::vector<float> audio (512, 0.25f);
            const float* input[] { audio.data() };
            for (int b = 0; b < 40; ++b) rec.processBlock (input, 1, 512);
            expect (pumpUntil ([&] { return page.isTimelineReadOnly(); }, 3000), "setup: the page saw the take");

            constexpr int header = brand::space::editHeaderW;
            const auto span = [&] { return recordingTimelineSpanSamples (engine.getRecordTimelineSamples(),
                                                                         engine.getPlayer().getTotalLengthSamples(),
                                                                         (double) kRate); };
            const auto laneX = [&] (juce::int64 sample)
            {
                const int contentW = viewport->getViewedComponent()->getWidth();
                return header + 4 + (int) ((double) sample / (double) span() * (double) (contentW - header - 8));
            };

            beginTest ("During a continue take, a locate scrolls to where the lanes draw that time");
            {
                page.setZoom (8.0f);
                const auto target = (juce::int64) kRate * 30;
                page.scrollToSample (target);
                const int centre = header + juce::jmax (0, viewport->getViewWidth() - header) / 2;
                const int shown = laneX (target) - viewport->getViewPositionX();
                expect (std::abs (shown - centre) <= 3,
                        "the locate scrolled to " + juce::String (shown) + " px, not the centre " + juce::String (centre));
            }

            beginTest ("During a continue take, Zoom to Selection fits the selection the lanes draw");
            {
                page.setZoom (1.0f);
                const auto a = (juce::int64) kRate * 60, b = (juce::int64) kRate * 120;
                page.zoomToSamples (a, b);
                const int width = laneX (b) - laneX (a);
                const int visibleLane = viewport->getViewWidth() - header;
                expect (width > visibleLane * 3 / 4 && width <= visibleLane,
                        "the selection is " + juce::String (width) + " px wide in a "
                        + juce::String (visibleLane) + " px lane");
                const int left = laneX (a) - viewport->getViewPositionX();
                expect (left >= header - 2 && laneX (b) - viewport->getViewPositionX() <= viewport->getViewWidth() + 2,
                        "the selection is not on screen");
            }

            engine.stopRecording();
            pumpUntil ([&] { return ! engine.isCaptureFinalizing(); }, 5000);
            rec.getTrack (0).armed.store (false);
            engine.clearAllStripOverrides();
        }

        // ── EDIT-04 ───────────────────────────────────────────────────────
        void cleanCacheLoadIsNotRewritten()
        {
            beginTest ("Opening a session whose saved peaks load cleanly leaves WaveCache.wfm alone");
            {
                Folder f;
                expect (writeTake (f.track (1), kRate));
                expect (writeTake (f.track (2), kRate));
                AudioEngine engine;
                openSession (engine, f.dir, 2);
                const auto cacheFile = wavecache::fileIn (f.dir);
                {
                    EditPage first (engine);
                    first.setBounds (0, 0, 1400, 300);
                    first.refresh();
                    expect (pumpUntil ([&] { return cacheFile.existsAsFile() && first.waveCacheIdleForTests(); }, 10000),
                            "setup: the peaks were never saved");
                }
                const auto written = cacheFile.getLastModificationTime();
                const auto size = cacheFile.getSize();
                juce::Thread::sleep (20);

                int saves = 0;
                {
                    EditPage page (engine);
                    page.beforeWaveCacheSaveForTests = [&saves] { ++saves; };
                    page.setBounds (0, 0, 1400, 300);
                    page.refresh();
                    expect (pumpUntil ([&] { return page.waveCacheResetRanForTests() && page.waveCacheIdleForTests(); }, 10000),
                            "setup: the saved peaks never loaded");
                    const auto rows = rowsOf (page);
                    expect (pumpUntil ([&] { return ! rows.empty()
                                                 && std::all_of (rows.begin(), rows.end(),
                                                                 [] (auto* r) { return r->waveformsLoaded(); }); }, 10000),
                            "setup: the lanes never drew");
                    pumpFor (400);
                    expectEquals (saves, 0, "a clean load queued a rewrite of the whole WaveCache.wfm");
                }
                expect (cacheFile.getLastModificationTime() == written && cacheFile.getSize() == size,
                        "closing the page rewrote the WaveCache.wfm it had loaded unchanged");
            }

            beginTest ("New peaks after a clean load still reach WaveCache.wfm");
            {
                Folder f;
                expect (writeTake (f.track (1), kRate));
                AudioEngine engine;
                openSession (engine, f.dir, 1);
                const auto cacheFile = wavecache::fileIn (f.dir);
                {
                    EditPage first (engine);
                    first.setBounds (0, 0, 1400, 300);
                    first.refresh();
                    expect (pumpUntil ([&] { return cacheFile.existsAsFile() && first.waveCacheIdleForTests(); }, 10000),
                            "setup: the peaks were never saved");
                }
                // A take the saved peaks do not cover, older than the cache
                // (copied in with its own dates): it scans after the load.
                expect (writeTake (f.track (2), kRate, 0.5f));
                const auto past = cacheFile.getLastModificationTime() - juce::RelativeTime::hours (1);
                f.track (2).setLastModificationTime (past);
                f.track (2).setCreationTime (past);
                openSession (engine, f.dir, 2);

                int saves = 0;
                EditPage page (engine);
                page.beforeWaveCacheSaveForTests = [&saves] { ++saves; };
                page.setBounds (0, 0, 1400, 300);
                page.refresh();
                expect (pumpUntil ([&] { return saves > 0; }, 10000), "the new take's peaks never reached WaveCache.wfm");
            }
        }

        void duplicateFromTheEditMenu (MainComponent& main)
        {
            Folder f;
            expect (writeTake (f.track (1), kRate * 4, 0.25f));
            auto& engine = main.engine;
            openSession (engine, f.dir, 1);
            main.currentView = MainComponent::View::Edit;
            auto& page = *main.editPage;
            page.setBounds (0, 0, 1400, 300);
            page.refresh();
            expect (engine.splitTrackAtSample (0, kRate) && engine.splitTrackAtSample (0, kRate * 2), "setup: splits");

            beginTest ("Duplicating two neighbouring clips of one strip is refused before anything changes");
            {
                page.setSelectedClips ({ { 0, 0 }, { 0, 1 } }, { 0, 0 });
                const auto before = juce::JSON::toString (engine.playlistsToJson());
                main.editDuplicateSelectedClip();
                expectEquals (juce::JSON::toString (engine.playlistsToJson()), before,
                              "the copy of one selected clip was laid over the other");
                expect (main.statusLabel.getText().contains ("one at a time"), main.statusLabel.getText());
            }

            beginTest ("Duplicate from the Edit menu replaces what lies under the copy and selects the copy");
            {
                page.setSelectedClips ({ { 0, 0 } }, { 0, 0 });
                main.editDuplicateSelectedClip();
                const auto& clips = engine.clipsFor (0);
                expect (! anyOverlap (clips), "the copy plays summed with the clip under it");
                const int sel = page.getSelectedClipIndex();
                expect (sel >= 0 && sel < (int) clips.size()
                            && clips[(size_t) sel].timelineStartSamples == kRate
                            && clips[(size_t) sel].name.endsWith ("(copy)"),
                        "the selection is not on the copy");
                expect (main.undoManager.undo(), "the duplicate is not one undo step");
                const auto& undone = engine.clipsFor (0);
                expect (undone.size() == 3 && std::none_of (undone.begin(), undone.end(),
                                                            [] (const Clip& c) { return c.name.endsWith ("(copy)"); }),
                        "undo did not bring the clip under the copy back");
            }
            main.undoManager.clearUndoHistory();
            page.clearSelectedClip();
            main.currentView = MainComponent::View::Mix;
            engine.setActiveSessionDir ({});
            engine.clearAllStripOverrides();
        }

        // ── PLAY-08 ───────────────────────────────────────────────────────
        static int countOfType (const MarkersManager& markers, const juce::String& type)
        {
            int n = 0;
            for (const auto& m : markers.getAll()) n += m.type == type ? 1 : 0;
            return n;
        }

        void songScanReplacesItsMarkers (MainComponent& main)
        {
            beginTest ("Detect Songs run again replaces its markers, as one undoable step");
            {
                Folder f;
                auto& engine = main.engine;
                setupEngine (engine, 1);
                engine.setActiveSessionDir (f.dir);
                auto& markers = engine.getMarkers();
                markers.setContext (f.dir, (double) kRate);
                markers.clear();
                markers.drop (1000, "Doors", "user");
                main.undoManager.clearUndoHistory();

                expectEquals (main.placeSongMarkers ({ kRate * 10, kRate * 200 }), 2);
                expectEquals (main.placeSongMarkers ({ kRate * 12, kRate * 210, kRate * 400 }), 3);
                expectEquals (countOfType (markers, "song"), 3, "a second scan added a second set of song markers");
                expectEquals (countOfType (markers, "user"), 1, "the scan touched a marker it did not make");

                expect (main.undoManager.undo(), "the second scan is not on the undo stack");
                expectEquals (countOfType (markers, "song"), 2, "undo did not bring back the first scan's markers");
                bool firstSet = false;
                for (const auto& m : markers.getAll())
                    firstSet = firstSet || (m.type == "song" && m.sampleOffset == (juce::int64) kRate * 200);
                expect (firstSet, "undo brought back other markers");
                expect (main.undoManager.redo());
                expectEquals (countOfType (markers, "song"), 3);
                expectEquals (countOfType (markers, "user"), 1);
                main.undoManager.clearUndoHistory();
                markers.clearContext();
                engine.setActiveSessionDir ({});
            }
        }

        // ── PLAY-09 ───────────────────────────────────────────────────────
        void qcReadsInTimelineTime (MainComponent& main)
        {
            beginTest ("The QC report gives clipping and length in timeline time after a crop");
            {
                Folder f;
                // 10 s, a clip 6 s in.
                expect (writeTake (f.track (1), kRate * 10, 0.1f, kRate * 6));
                auto& engine = main.engine;
                openSession (engine, f.dir, 1);
                expect (engine.cropToRange (kRate * 4, kRate * 10) > 0, "setup: crop");

                main.runQcAnalysis();
                const auto exports = f.dir.getChildFile ("Export Files");
                const auto report = [&]
                {
                    const auto found = exports.findChildFiles (juce::File::findFiles, false, "QC Report*.txt");
                    return found.isEmpty() ? juce::File() : found.getFirst();
                };
                expect (pumpUntil ([&] { return report().existsAsFile() && ! main.analysisBusy.load(); }, 20000),
                        "the QC report was never written");
                pumpFor (100);
                closeModalsForTake();
                pumpFor (50);
                const auto text = report().loadFileAsString();
                expect (text.contains ("at 00:00:02.000"), "the clip is not reported where it plays:\n" + text);
                expect (! text.contains ("at 00:00:06.000"), "the clip is reported at its file position:\n" + text);
                expect (text.contains ("Length: 00:00:06.000"), "the length is not the timeline's:\n" + text);
                engine.setActiveSessionDir ({});
                engine.clearAllStripOverrides();
            }
        }

        // ── CUE-REBIND ────────────────────────────────────────────────────
        static void writeCueProject (const juce::File& dir, const juce::StringArray& uids, const std::vector<bool>& mutes)
        {
            juce::Array<juce::var> strips;
            for (int i = 0; i < uids.size(); ++i)
            {
                juce::DynamicObject::Ptr s (new juce::DynamicObject());
                s->setProperty ("uid", uids[i]);
                s->setProperty ("gainDb", 0.0);
                s->setProperty ("pan", 0.0);
                s->setProperty ("in", -2);
                s->setProperty ("out", -1);
                s->setProperty ("mute", (bool) mutes[(size_t) i]);
                s->setProperty ("solo", false);
                s->setProperty ("mon", false);
                s->setProperty ("rec", false);
                strips.add (juce::var (s.get()));
            }
            juce::DynamicObject::Ptr cue (new juce::DynamicObject());
            cue->setProperty ("name", "Verse");
            cue->setProperty ("samplePos", 0.0);
            cue->setProperty ("strips", juce::var (strips));
            juce::DynamicObject::Ptr root (new juce::DynamicObject());
            root->setProperty ("setlist", juce::Array<juce::var> { juce::var (cue.get()) });
            dir.getChildFile (dir.getFileName() + ".zfproj")
               .replaceWithText (juce::JSON::toString (juce::var (root.get())));
        }

        void cuesBindByStripId (MainComponent& main)
        {
            auto& engine = main.engine;
            const auto prepare = [&] (const juce::File& dir, const juce::StringArray& stripIds)
            {
                setupEngine (engine, stripIds.size());
                engine.setActiveSessionDir (dir);
                auto& rec = engine.getRecorder();
                for (int i = 0; i < stripIds.size(); ++i)
                {
                    rec.getTrack (i).stripId = stripIds[i];
                    rec.getTrack (i).muted.store (false);
                }
            };

            beginTest ("With session_mix.json missing, a cue recalls onto its own strips after a strip move");
            {
                Folder f;
                // Saved as A, B; the strips were moved to B, A since.
                writeCueProject (f.dir, { "uid-A", "uid-B" }, { true, false });
                prepare (f.dir, { "uid-B", "uid-A" });
                main.loadSetlistFromActiveSession();
                main.jumpToCue (0);
                auto& rec = engine.getRecorder();
                expect (rec.getTrack (1).muted.load(), "strip A did not get its own mute");
                expect (! rec.getTrack (0).muted.load(), "strip B got strip A's mute: the cue was bound by position");
            }

            beginTest ("A cue whose strips are all unknown is refused with a message");
            {
                Folder f;
                writeCueProject (f.dir, { "uid-gone-1", "uid-gone-2" }, { true, true });
                f.dir.getChildFile ("session_mix.json").replaceWithText ("{ damaged");
                prepare (f.dir, { "uid-1", "uid-2" });
                main.loadSetlistFromActiveSession();
                main.jumpToCue (0);
                auto& rec = engine.getRecorder();
                expect (! rec.getTrack (0).muted.load() && ! rec.getTrack (1).muted.load(),
                        "a cue recalled onto strips it was never taken from");
                expect (main.statusLabel.getText().containsIgnoreCase ("not"),
                        "nothing said the cue was not recalled: " + main.statusLabel.getText());
            }

            beginTest ("An older session's cue still rebinds by position when its mixer saved no ids");
            {
                Folder f;
                writeCueProject (f.dir, { "old-1", "old-2" }, { true, false });
                f.dir.getChildFile ("session_mix.json")
                   .replaceWithText (R"({ "trackCount": 2, "strips": [ { "index": 0 }, { "index": 1 } ] })");
                prepare (f.dir, { "uid-1", "uid-2" });
                main.loadSetlistFromActiveSession();
                main.jumpToCue (0);
                auto& rec = engine.getRecorder();
                expect (rec.getTrack (0).muted.load() && ! rec.getTrack (1).muted.load(),
                        "a legacy cue no longer recalls by position");
            }
            main.cues.clear();
            engine.setActiveSessionDir ({});
            engine.clearAllStripOverrides();
        }
    };

    static PlayFix20261116Tests playFix20261116Tests;
}
