// Clip editing, renders, song tools and analysis: what they promise the
// engineer. Everything runs headless through the engine and the pure helpers.

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/CheckedReaderSource.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/ProjectExport.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/ShowHandoff.h"
#include "../Audio/SincResampler.h"
#include "../Audio/SongExport.h"
#include "../Audio/TrackExporter.h"
#include "../UI/MainComponent.h"
#include "SavedTakes.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/resource.h>
#include <csignal>

namespace zynforge
{
class TimelineEditTests final : public juce::UnitTest
{
public:
    TimelineEditTests() : UnitTest ("Timeline edits", "zynforge") {}

    struct Folder
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-timeline-" + juce::Uuid().toString());
        Folder() { dir.createDirectory(); }
        ~Folder() { dir.deleteRecursively(); }
        juce::File audio (const juce::String& name) const
        { return dir.getChildFile ("Audio Files").getChildFile (name); }
        juce::File track (int n = 1) const
        { return audio ("Track_" + juce::String::formatted ("%02d.wav", n)); }
    };

    // Mono 24-bit WAV holding a constant value.
    static bool write (const juce::File& file, int samples = 48000, float value = 0.25f,
                       double rate = 48000.0)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
            file.createOutputStream().release(), rate, 1, 24, {}, 0));
        if (! writer) return false;
        juce::AudioBuffer<float> audio (1, samples);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), value, samples);
        return writer->writeFromAudioSampleBuffer (audio, 0, samples);
    }

    static void setup (AudioEngine& e, int tracks = 1)
    {
        e.clearAllStripOverrides(); e.setStripCount (tracks); e.prepareForTests (48000, 256);
    }

    float renderedAt (AudioEngine& e, int track, juce::int64 total, int sample)
    {
        juce::AudioBuffer<float> out;
        expect (e.renderTrackArrangement (track, out, total), "render failed");
        return out.getNumSamples() > sample ? out.getSample (0, sample) : -1.0f;
    }

    static juce::int64 clipSpan (const std::vector<Clip>& clips)
    {
        juce::int64 total = 0;
        for (const auto& c : clips) total += c.fileLengthSamples;
        return total;
    }

    using Param = AudioEngine::AutomationParam;

    void undoKeepsRecordedAudio()
    {
        beginTest ("Undo and redo across a continued take keep the new audio");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            const auto before = e.playlistsToJson();
            expect (e.splitTrackAtSample (0, 12000));
            const auto after = e.playlistsToJson();

            expect (write (f.audio ("Track_01_part02.wav"), 24000, 0.5f));
            e.loadSession (f.dir, true, true);               // the take's stop
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 72000);

            e.restorePlaylistSnapshot (before);              // undo the split
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 72000, "undo cut the take short");
            expectWithinAbsoluteError (renderedAt (e, 0, 72000, 60000), 0.5f, 0.001f);
            expectEquals ((int) e.clipsFor (0).size(), 1, "an unedited take becomes one clip again");

            e.restorePlaylistSnapshot (after);               // redo the split
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 72000, "redo cut the take short");
            expectWithinAbsoluteError (renderedAt (e, 0, 72000, 60000), 0.5f, 0.001f);
            expectWithinAbsoluteError (renderedAt (e, 0, 72000, 1000), 0.25f, 0.001f);

            // The next save writes what undo left, so the audio must be there too.
            const auto saved = e.playlistsToJson();
            e.loadSession (f.dir);
            e.loadPlaylistsFromJson (saved);
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 72000);
        }

        beginTest ("An edit undone across a take keeps the arrangement around the new span");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            expect (e.setClipGainDb (0, 0, -6.0f));
            const auto trimmed = e.playlistsToJson();       // an edited single clip
            expect (e.setClipGainDb (0, 0, -12.0f));

            expect (write (f.audio ("Track_01_part02.wav"), 24000, 0.5f));
            e.loadSession (f.dir, true, true);
            e.restorePlaylistSnapshot (trimmed);
            expectEquals ((int) e.clipsFor (0).size(), 2, "the gain edit and the new span stay apart");
            expectWithinAbsoluteError (e.clipsFor (0)[0].gainDb, -6.0f, 0.001f);
            expectEquals (e.clipsFor (0)[1].timelineStartSamples, (juce::int64) 48000);
            expectWithinAbsoluteError (renderedAt (e, 0, 72000, 60000), 0.5f, 0.001f);
        }

        beginTest ("A continued span deleted by hand stays deleted through its own undo and redo");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            expect (e.splitTrackAtSample (0, 12000));
            expect (write (f.audio ("Track_01_part02.wav"), 24000, 0.5f));
            e.loadSession (f.dir, true, true);

            const auto before = e.playlistsToJson();
            const int last = (int) e.clipsFor (0).size() - 1;
            expect (e.deleteClip (0, last));
            const auto after = e.playlistsToJson();

            e.restorePlaylistSnapshot (before);
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 72000);
            e.restorePlaylistSnapshot (after);
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 48000,
                          "redoing a deliberate delete brought the clip back");
        }

        beginTest ("A restored playlist keeps a strip recorded since in its takes");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            juce::Array<juce::var> noTakes;
            juce::DynamicObject::Ptr entry (new juce::DynamicObject());
            entry->setProperty ("track", 1);
            entry->setProperty ("takes", juce::Array<juce::var>());
            noTakes.add (juce::var (entry.get()));
            expect (write (f.track (2)));
            e.loadSession (f.dir, true, true);
            e.loadPlaylistsFromJson (juce::var (noTakes));
            expect (savedtakes::count (e, 1) >= 1, "the kept clips fell out of the playlist the save writes");
        }
    }

    void malformedActiveTake()
    {
        beginTest ("An out-of-range active take loads as a real take");
        for (const int bad : { -1, 7 })
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            savedtakes::addTake (e, 0, "Take 2");
            const auto saved = e.playlistsToJson();
            auto* obj = saved[0].getDynamicObject();
            expect (obj != nullptr);
            juce::DynamicObject::Ptr entry (new juce::DynamicObject (*obj));
            entry->setProperty ("activeTake", bad);
            juce::Array<juce::var> doc;
            doc.add (juce::var (entry.get()));
            e.loadPlaylistsFromJson (juce::var (doc));
            const int active = e.getActiveTakeIdx (0);
            expect (active >= 0 && active < savedtakes::count (e, 0));
            expect (! e.clipsFor (0).empty());
            // The live clips are that take's own: an edit lands there and
            // nowhere else.
            expect (e.splitTrackAtSample (0, 24000));
            expectEquals ((int) savedtakes::clips (e, 0, active).size(), 2);
            expectEquals ((int) savedtakes::clips (e, 0, 1 - active).size(), 1);
        }
    }

    void cropAndRipple()
    {
        beginTest ("Ripple Delete on every strip moves markers, automation, tempo and other takes");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            e.getMarkers().drop (1000, "before");
            e.getMarkers().drop (15000, "inside");
            e.getMarkers().drop (30000, "after");
            e.addAutomationPoint (0, Param::Volume, 0, -6.0f);
            e.addAutomationPoint (0, Param::Volume, 40000, -6.0f);
            e.setTempoMap ({ { 1000, 100.0f }, { 15000, 120.0f }, { 30000, 140.0f } });
            savedtakes::addTake (e, 0, "Take 2");

            expectEquals (e.rippleDeleteTracks ({ 0, 1 }, 10000, 20000), 1);

            const auto& m = e.getMarkers().getAll();
            expectEquals ((int) m.size(), 2);
            expectEquals (m[0].sampleOffset, (juce::int64) 1000);
            expectEquals (m[1].sampleOffset, (juce::int64) 20000, "a marker stayed on the wrong audio");

            const auto& lane = e.getAutomation (0, Param::Volume);
            expectEquals (lane.back().samplePos, (juce::int64) 30000, "a ride stayed on the wrong audio");
            expectWithinAbsoluteError (e.automationValueAtOffline (0, Param::Volume, 25000, 0.0f), -6.0f, 0.01f);

            const auto& tempo = e.getTempoMap();
            expectEquals ((int) tempo.size(), 3);
            expectEquals (tempo[1].samplePos, (juce::int64) 10000);
            expectWithinAbsoluteError (tempo[1].bpm, 120.0f, 0.01f, "the tempo in force at the join was lost");
            expectEquals (tempo[2].samplePos, (juce::int64) 20000);

            expectEquals (savedtakes::count (e, 0), 2);
            for (int take = 0; take < savedtakes::count (e, 0); ++take)
                expectEquals (clipSpan (savedtakes::clips (e, 0, take)), (juce::int64) 38000,
                              "another take stayed on the old timeline");
        }

        beginTest ("Ripple Delete on one strip moves that strip's automation and nothing global");
        {
            Folder f; expect (write (f.track())); expect (write (f.track (2)));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            e.getMarkers().drop (30000, "song");
            e.addAutomationPoint (0, Param::Volume, 30000, -6.0f);
            e.addAutomationPoint (1, Param::Volume, 30000, -6.0f);
            e.setTempoMap ({ { 30000, 140.0f } });

            expectEquals (e.rippleDeleteTracks ({ 0 }, 10000, 20000), 1);
            expectEquals (e.getAutomation (0, Param::Volume).back().samplePos, (juce::int64) 20000);
            expectEquals (e.getAutomation (1, Param::Volume).back().samplePos, (juce::int64) 30000);
            expectEquals (e.getMarkers().getAll()[0].sampleOffset, (juce::int64) 30000);
            expectEquals (e.getTempoMap()[0].samplePos, (juce::int64) 30000);
        }

        beginTest ("Automation curves keep their value on both sides of a Ripple Delete");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.addAutomationPoint (0, Param::Volume, 0, -40.0f);
            e.addAutomationPoint (0, Param::Volume, 40000, 0.0f);
            const auto valueAt = [&e] (juce::int64 s) { return e.automationValueAtOffline (0, Param::Volume, s, 99.0f); };
            const float left = valueAt (9000), right = valueAt (30000);
            expect (e.rippleDeleteRange (0, 10000, 20000));
            expectWithinAbsoluteError (valueAt (9000), left, 0.01f);
            expectWithinAbsoluteError (valueAt (20000), right, 0.01f);
        }

        beginTest ("Crop moves everything to the kept range and drops what lies outside");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.getMarkers().drop (5000, "gone");
            e.getMarkers().drop (15000, "kept");
            e.getMarkers().drop (40000, "gone too");
            e.addAutomationPoint (0, Param::Volume, 0, -20.0f);
            e.addAutomationPoint (0, Param::Volume, 40000, 0.0f);
            const float atStart = e.automationValueAtOffline (0, Param::Volume, 10000, 99.0f);
            const float inRange = e.automationValueAtOffline (0, Param::Volume, 15000, 99.0f);
            e.setTempoMap ({ { 5000, 100.0f }, { 15000, 120.0f } });
            savedtakes::addTake (e, 0, "Take 2");

            expectEquals (e.cropToRange (10000, 20000), 1);

            expectEquals ((int) e.getMarkers().getAll().size(), 1);
            expectEquals (e.getMarkers().getAll()[0].sampleOffset, (juce::int64) 5000);
            expectWithinAbsoluteError (e.automationValueAtOffline (0, Param::Volume, 0, 99.0f), atStart, 0.01f);
            expectWithinAbsoluteError (e.automationValueAtOffline (0, Param::Volume, 5000, 99.0f), inRange, 0.01f);
            const auto& tempo = e.getTempoMap();
            expectEquals ((int) tempo.size(), 2);
            expectEquals (tempo[0].samplePos, (juce::int64) 0);
            expectWithinAbsoluteError (tempo[0].bpm, 100.0f, 0.01f);
            expectEquals (tempo[1].samplePos, (juce::int64) 5000);
            expectEquals (savedtakes::count (e, 0), 2);
            for (int take = 0; take < savedtakes::count (e, 0); ++take)
            {
                const auto clips = savedtakes::clips (e, 0, take);
                expectEquals (clipSpan (clips), (juce::int64) 10000);
                if (! clips.empty())
                    expectEquals (clips[0].timelineStartSamples, (juce::int64) 0);
            }
        }

        beginTest ("A locked clip or Automation Safe refuses the edit before anything moves");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.getMarkers().drop (30000, "song");
            e.addAutomationPoint (0, Param::Volume, 30000, -6.0f);
            e.setClipLocked (0, 0, true);
            expectEquals (e.rippleDeleteTracks ({ 0 }, 10000, 20000), -1);
            expectEquals (e.cropToRange (10000, 20000), -1);
            expectEquals (e.getMarkers().getAll()[0].sampleOffset, (juce::int64) 30000);
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 48000);
            e.setClipLocked (0, 0, false);

            e.setTrackAutomationSafe (0, true);
            expectEquals (e.rippleDeleteTracks ({ 0 }, 10000, 20000), -1);
            e.setTrackAutomationSafe (0, false);
            expectEquals (e.rippleDeleteTracks ({ 0 }, 10000, 20000), 1);
        }

        beginTest ("A timeline edit refuses during a take at the engine");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.setExternalRecording (true);
            expectEquals (e.cropToRange (10000, 20000), -1);
            expectEquals (e.rippleDeleteTracks ({ 0 }, 10000, 20000), -1);
            e.setExternalRecording (false);
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 48000);
        }

        beginTest ("Cues and their own automation move with a Crop");
        {
            AudioEngine e; setup (e);
            e.addAutomationPoint (0, Param::Volume, 12000, -6.0f);
            e.addAutomationPoint (0, Param::Volume, 18000, -3.0f);
            SetlistBar::Cue inside;
            inside.name = "kept"; inside.samplePos = 12000;
            inside.automation = e.automationToJson();
            inside.tempoCurve.push_back ({ 2000, 110.0f });
            SetlistBar::Cue outside;
            outside.name = "gone"; outside.samplePos = 40000;

            const timelinecut::Cut crop { 10000, 20000, true };
            const auto kept = MainComponent::cuesAfterCut ({ inside, outside }, crop, { 0 }, true);
            expectEquals ((int) kept.size(), 1);
            expectEquals (kept[0].samplePos, (juce::int64) 2000);
            expectEquals (kept[0].tempoCurve[0].offsetSamples, (juce::int64) 2000);
            e.loadAutomationFromJson (kept[0].automation);
            expectEquals (e.getAutomation (0, Param::Volume).front().samplePos, (juce::int64) 2000,
                          "a recalled cue would put its automation back on the old timeline");
        }

        beginTest ("One strip's Ripple Delete moves only that strip's lanes inside a cue");
        {
            AudioEngine e; setup (e, 2);
            e.addAutomationPoint (0, Param::Volume, 30000, -6.0f);
            e.addAutomationPoint (1, Param::Volume, 30000, -6.0f);
            SetlistBar::Cue cue;
            cue.name = "song"; cue.samplePos = 30000;
            cue.automation = e.automationToJson();

            const auto moved = MainComponent::cuesAfterCut ({ cue }, { 10000, 20000, false }, { 0 }, false);
            expectEquals (moved[0].samplePos, (juce::int64) 30000);
            e.loadAutomationFromJson (moved[0].automation);
            expectEquals (e.getAutomation (0, Param::Volume)[0].samplePos, (juce::int64) 20000);
            expectEquals (e.getAutomation (1, Param::Volume)[0].samplePos, (juce::int64) 30000);
        }

        beginTest ("Crop and Ripple Delete restore everything from one state");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.getMarkers().drop (15000, "m");
            e.addAutomationPoint (0, Param::Volume, 30000, -6.0f);
            e.setTempoMap ({ { 30000, 140.0f } });
            const auto before = e.captureTimelineState();
            expectEquals (e.cropToRange (20000, 40000), 1);
            expect (e.getMarkers().getAll().empty());
            e.restoreTimelineState (before);
            expectEquals (e.getMarkers().getAll()[0].sampleOffset, (juce::int64) 15000);
            expectEquals (e.getAutomation (0, Param::Volume)[0].samplePos, (juce::int64) 30000);
            expectEquals (e.getTempoMap()[0].samplePos, (juce::int64) 30000);
            expectEquals (clipSpan (e.clipsFor (0)), (juce::int64) 48000);
        }
    }

    void rendersTellTheTruth()
    {
        beginTest ("Master MUTE is monitoring: a mix render ignores it");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.setMasterMuted (true);
            juce::AudioBuffer<float> mix;
            expect (e.renderStereoMix (mix, 48000));
            expect (mix.getMagnitude (0, 48000) > 0.1f, "master MUTE silenced the render");
            e.setMasterMuted (false);
        }

        beginTest ("A silent mix render says so, and a solo left on is named");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.setTrackMuted (0, true);
            float peak = 1.0f;
            expect (e.bounceStereoMixToWav (f.dir.getChildFile ("mix.wav"), 48000, 48000.0, nullptr, &peak));
            expectEquals (peak, 0.0f);
            expect (songexport::mixRenderWarning (true, false).contains ("SILENT"));
            e.setTrackMuted (0, false);
            expect (songexport::mixRenderWarning (false, false).isEmpty());

            expect (! e.isAnySoloActive());
            e.setVcaSoloed (0, true);
            expect (e.isAnySoloActive(), "a soloed VCA shapes the mix like a soloed strip");
            expect (e.captureInterchange().anySolo, "a DAW export must note a VCA solo");
            e.setVcaSoloed (0, false);

            songexport::Outcome outcome;
            outcome.attempted = outcome.written = 2;
            expect (! songexport::statusLine (outcome, "Songs").contains ("WARNING"));
            outcome.silentMixes = 1;
            expect (songexport::statusLine (outcome, "Songs").contains ("WARNING"));
        }

        beginTest ("A render that loses its last bytes to a full disk is a failure");
        {
            Folder f; expect (write (f.track(), 48000));
            ExportOptions options;
            options.format = ExportFormat::Wav24;
            options.bitsPerSample = 24;
            options.sampleRate = 48000.0;
            TrackExporter exporter;
            juce::String error;
            const auto full = f.dir.getChildFile ("full");
            expect (exporter.exportTrack (f.track(), full, options, error), error);
            const auto size = full.withFileExtension (".wav").getSize();
            expect (full.withFileExtension (".wav").deleteFile());

            // Only the final flush can hit this limit: the buffer holds 16 KB.
            struct rlimit before {}; getrlimit (RLIMIT_FSIZE, &before);
            auto limited = before;
            limited.rlim_cur = (rlim_t) (size - 1000);
            const auto previousHandler = std::signal (SIGXFSZ, SIG_IGN);
            setrlimit (RLIMIT_FSIZE, &limited);
            const auto dest = f.dir.getChildFile ("short");
            const bool ok = exporter.exportTrack (f.track(), dest, options, error);
            setrlimit (RLIMIT_FSIZE, &before);
            std::signal (SIGXFSZ, previousHandler);
            expect (! ok, "a truncated file was installed as finished");
            expect (! dest.withFileExtension (".wav").exists());
            expect (error.contains ("Final write"), error);
        }

        beginTest ("A strip name exports to any file system");
        {
            const auto safe = songexport::safeName ("Vox: lead? *2");
            expect (! safe.containsAnyOf (":?*/\\"), safe);
            expect (safe.isNotEmpty());
        }

        beginTest ("A format's size limit refuses first, with the reason");
        {
            ExportOptions options;
            options.format = ExportFormat::Aiff24;
            options.bitsPerSample = 32;
            const juce::int64 threeHours = 48000LL * 3 * 3600;
            expect (TrackExporter::sizeRefusal (options, 2, threeHours).contains ("AIFF"));
            expect (TrackExporter::sizeRefusal (options, 1, 48000LL * 3600).isEmpty());
            options.bitsPerSample = 24;
            expect (TrackExporter::sizeRefusal (options, 2, threeHours).isEmpty());
            options.format = ExportFormat::Wav24;
            options.bitsPerSample = 32;
            expect (TrackExporter::sizeRefusal (options, 2, threeHours * 10).isEmpty());
        }

        beginTest ("Sample-rate conversion rejects aliases and keeps the passband");
        {
            const auto convert = [] (double hz, double fromRate, double toRate, int frames, int skip,
                                     bool linear = false)
            {
                juce::MemoryBlock mem;
                {
                    auto* out = new juce::MemoryOutputStream (mem, false);
                    juce::WavAudioFormat format;
                    std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (out, fromRate, 1, 32, {}, 0));
                    juce::AudioBuffer<float> b (1, frames);
                    for (int i = 0; i < frames; ++i)
                        b.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / fromRate));
                    writer->writeFromAudioSampleBuffer (b, 0, frames);
                }
                juce::WavAudioFormat format;
                std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (
                    new juce::MemoryInputStream (mem, false), true));
                CheckedReaderSource source (*reader);
                const int count = (int) (frames * toRate / fromRate) - skip;
                juce::AudioBuffer<float> out (1, count + skip);
                juce::AudioSourceChannelInfo info (&out, 0, count + skip);
                if (linear)
                {
                    juce::ResamplingAudioSource resampler (&source, false, 1);
                    resampler.setResamplingRatio (fromRate / toRate);
                    resampler.prepareToPlay (4096, toRate);
                    resampler.getNextAudioBlock (info);
                    resampler.releaseResources();
                }
                else
                {
                    SincResampler resampler (&source, 1);
                    resampler.setResamplingRatio (fromRate / toRate);
                    resampler.prepareToPlay (4096, toRate);
                    resampler.getNextAudioBlock (info);
                    resampler.releaseResources();
                }
                juce::AudioBuffer<float> middle (1, count - skip);
                middle.copyFrom (0, 0, out, 0, skip, count - skip);
                return (double) juce::Decibels::gainToDecibels (middle.getMagnitude (0, middle.getNumSamples()) / 0.5f);
            };
            // 23.5 kHz is above the new Nyquist: anything left of it is an alias.
            const double alias = convert (23500.0, 48000.0, 44100.0, 48000, 2000);
            const double before = convert (23500.0, 48000.0, 44100.0, 48000, 2000, true);
            logMessage ("Alias of 23.5 kHz at 48 -> 44.1 kHz: JUCE linear " + juce::String (before, 1)
                        + " dB, now " + juce::String (alias, 1) + " dB");
            expect (before > alias + 40.0, "the converter did not improve on linear interpolation");
            expect (alias < -90.0, "alias only " + juce::String (alias, 1) + " dB down");
            expect (std::abs (convert (1000.0, 48000.0, 44100.0, 48000, 2000)) < 0.01, "1 kHz level moved");
            expect (std::abs (convert (15000.0, 48000.0, 44100.0, 48000, 2000)) < 0.05, "15 kHz level moved");
            expect (std::abs (convert (15000.0, 44100.0, 96000.0, 44100, 4000)) < 0.05, "upsampled 15 kHz moved");
        }

        beginTest ("A converted export keeps the source's first sample and its length");
        {
            Folder f;
            const auto src = f.dir.getChildFile ("impulse.wav");
            {
                juce::WavAudioFormat format;
                std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
                    src.createOutputStream().release(), 96000.0, 1, 24, {}, 0));
                juce::AudioBuffer<float> b (1, 9600);
                b.clear(); b.setSample (0, 4800, 0.9f);
                writer->writeFromAudioSampleBuffer (b, 0, 9600);
            }
            ExportOptions options;
            options.format = ExportFormat::Wav24;
            options.bitsPerSample = 24;
            options.sampleRate = 48000.0;
            TrackExporter exporter;
            juce::String error;
            const auto dest = f.dir.getChildFile ("half");
            expect (exporter.exportTrack (src, dest, options, error), error);
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (dest.withFileExtension (".wav")));
            expectEquals ((juce::int64) reader->lengthInSamples, (juce::int64) 4800);
            juce::AudioBuffer<float> b (1, 4800);
            reader->read (&b, 0, 4800, 0, true, false);
            int peakAt = 0;
            for (int i = 0; i < 4800; ++i)
                if (std::abs (b.getSample (0, i)) > std::abs (b.getSample (0, peakAt))) peakAt = i;
            expectEquals (peakAt, 2400, "the conversion moved the audio in time");
        }
    }

    void clipboardAndHandoff()
    {
        beginTest ("A clip reading a file outside the session cannot be pasted");
        {
            Folder a, b; expect (write (a.track())); expect (write (b.track()));
            AudioEngine e; setup (e); e.loadSession (a.dir);
            expectEquals (e.pasteClip (0, 0, 0, 4800, 0, 0, 0.0f, "other", b.track(), 0), -1);
            expect (e.pasteClip (0, 0, 0, 4800, 0, 0, 0.0f, "own", a.track(), 0) >= 0);
        }

        beginTest ("A handoff knows when a clip reads outside the session");
        {
            Folder a, b; expect (write (a.track())); expect (write (b.track()));
            AudioEngine e; setup (e); e.loadSession (a.dir);
            auto input = e.captureInterchange();
            expect (projectexport::firstMediaOutsideSession (input) == juce::File());
            Clip stray;
            stray.audioFile = b.track();
            stray.fileLengthSamples = 4800;
            if (! input.strips[0].clips.has_value()) input.strips[0].clips = std::vector<Clip>();
            input.strips[0].clips->push_back (stray);
            expect (projectexport::firstMediaOutsideSession (input) == b.track());
        }

        beginTest ("Finder files are no part of what a handoff compares");
        {
            Folder f; expect (write (f.track()));
            const std::atomic<bool> cancel { false };
            f.dir.getChildFile (".DS_Store").replaceWithText ("x");
            f.dir.getChildFile ("._Track_01.wav").replaceWithText ("x");
            std::map<juce::String, juce::File> files;
            expect (showhandoff::collectFiles (f.dir, f.dir, files, cancel));
            expectEquals ((int) files.size(), 1);
        }
    }

    void clickAtSessionRate()
    {
        beginTest ("The session's audio rate does not follow the device");
        {
            Folder f; expect (write (f.track(), 44100, 0.25f, 44100.0));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            expectWithinAbsoluteError (e.getDeviceSampleRate(), 48000.0, 0.1);
            expectWithinAbsoluteError (e.getSessionAudioRate(), 44100.0, 0.1);
        }
    }

    // One song of 90 s at 100 ms windows, on two voices: on, off, on.
    void songsOnTheTimeline()
    {
        beginTest ("Detect Songs marks where the arrangement plays, not where the file holds it");
        {
            Folder f;
            const int rate = 8000;
            const auto writeShow = [&] (const juce::File& file)
            {
                file.getParentDirectory().createDirectory();
                juce::WavAudioFormat format;
                std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
                    file.createOutputStream().release(), rate, 1, 16, {}, 0));
                juce::AudioBuffer<float> b (1, rate);
                for (int second = 0; second < 200; ++second)
                {
                    const bool loud = (second >= 10 && second < 80) || (second >= 110 && second < 190);
                    juce::FloatVectorOperations::fill (b.getWritePointer (0), loud ? 0.5f : 0.0f, rate);
                    expect (writer->writeFromAudioSampleBuffer (b, 0, rate));
                }
            };
            writeShow (f.track (1));
            writeShow (f.track (2));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            const auto run = [&]
            {
                double sr = 48000.0;
                const songs::Params params;
                return songs::detect (songs::scanVoices (e.songScanVoices(), params, sr), sr, params);
            };
            auto found = run();
            expectEquals ((int) found.size(), 2);
            expectEquals (found[1].startSample, (juce::int64) 110 * rate);

            // Remove the first 100 s: file positions no longer are timeline positions.
            expectEquals (e.rippleDeleteTracks ({ 0, 1 }, 0, 100 * rate), 2);
            found = run();
            expectEquals ((int) found.size(), 1, "audio cut from the arrangement still made a song");
            expectEquals (found[0].startSample, (juce::int64) 10 * rate, "the marker went to the file position");
        }

        beginTest ("A generated click does not hold the song quorum open");
        {
            Folder f; expect (write (f.track()));  expect (write (f.track (2)));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            expectEquals ((int) e.songScanVoices().size(), 2);
            e.getRecorder().getTrack (1).referenceMedia.store (true);
            expectEquals ((int) e.songScanVoices().size(), 1);
        }

        beginTest ("A stereo pair is one voice of the quorum");
        {
            Folder f; expect (write (f.track()));  expect (write (f.track (2)));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            e.getRecorder().getTrack (0).isStereo.store (true);
            expectEquals ((int) e.songScanVoices().size(), 1);
        }
    }

    void markersThatCutSongs()
    {
        const auto marker = [] (juce::int64 at, const juce::String& name, const juce::String& type)
        {
            Marker m; m.sampleOffset = at; m.name = name; m.type = type;
            return m;
        };

        beginTest ("Export Songs cuts at song markers, not at splits, ranges or scenes");
        {
            const auto ranges = songexport::rangesFromMarkers (
                { marker (0, "One", "user"), marker (100, "Split", "split"),
                  marker (200, "Range In", "range"), marker (300, "Chorus", "scene"),
                  marker (400, "Two", "song"), marker (500, "Split", "user") }, 1000);
            expectEquals ((int) ranges.size(), 2);
            expectEquals (ranges[0].end, (juce::int64) 400);
            expectEquals (ranges[1].name, juce::String ("Two"));
        }

        beginTest ("A show with only scene markers still divides at them");
        {
            const auto ranges = songexport::rangesFromMarkers (
                { marker (100, "Split", "split"), marker (200, "Song A", "scene"),
                  marker (400, "Song B", "scene") }, 1000);
            expectEquals ((int) ranges.size(), 2);
            expectEquals (ranges[0].start, (juce::int64) 200);
        }

        beginTest ("A marker removed by id leaves the others alone");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.getMarkers().drop (100, "keep");
            e.getMarkers().drop (200, "Split", "split");
            const auto id = e.getMarkers().lastDroppedId;
            const auto dropped = e.getMarkers().getAll()[1];
            e.getMarkers().removeById (id);
            expectEquals (e.getMarkers().getCount(), 1);
            e.getMarkers().insert (dropped);
            expectEquals (e.getMarkers().getAll()[1].type, juce::String ("split"));
        }
    }

    void healing()
    {
        beginTest ("Heal Separation reaches only the split near the playhead");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            expect (e.splitTrackAtSample (0, 24000));
            expect (! e.healSeparationAt (0, 0), "a split half a second away was healed");
            expectEquals ((int) e.clipsFor (0).size(), 2);
            expect (e.healSeparationAt (0, 20000));
            expectEquals ((int) e.clipsFor (0).size(), 1);
        }
    }

    void analysisReports()
    {
        beginTest ("QC names a take it could not read; a read failure is not a complete scan");
        {
            Folder f;
            const auto missing = zynforge::qc::analyzeFile (f.track());
            expect (missing.error.isNotEmpty());

            const auto corrupt = f.audio ("Track_01.wav");
            corrupt.getParentDirectory().createDirectory();
            corrupt.replaceWithText ("this is no audio file at all");
            auto unreadable = zynforge::qc::analyzeFile (corrupt);
            expect (unreadable.error.isNotEmpty());
            unreadable.name = "Vox";

            expect (write (f.track (2)));
            auto good = zynforge::qc::analyzeFile (f.track (2));
            expect (good.error.isEmpty());
            good.name = "Gtr";

            const auto summary = zynforge::qc::statusSummary ({ unreadable, good }, -1);
            expect (summary.contains ("1 COULD NOT BE READ"), summary);
            expect (summary.startsWith ("1 track QC'd"), summary);
            const auto report = zynforge::qc::reportText ("Show", { unreadable, good });
            expect (report.contains ("QC INCOMPLETE"), report);
        }

        beginTest ("A cancelled QC scan stops");
        {
            Folder f; expect (write (f.track(), 480000));
            std::atomic<bool> cancel { true };
            expect (zynforge::qc::analyzeFile (f.track(), &cancel).peakDb <= -119.0f);
        }
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        undoKeepsRecordedAudio();
        malformedActiveTake();
        cropAndRipple();
        rendersTellTheTruth();
        clipboardAndHandoff();
        clickAtSessionRate();
        songsOnTheTimeline();
        markersThatCutSongs();
        healing();
        analysisReports();
    }
};

static TimelineEditTests timelineEditTests;
}
