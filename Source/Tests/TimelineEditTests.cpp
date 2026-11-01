// Clip editing, renders, song tools and analysis: what they promise the
// engineer. Everything runs headless through the engine and the pure helpers.

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"
#include "../UI/MainComponent.h"

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
            e.pasteClip (1, 0, 0, 4800, 0, 0, 0.0f, "copied", f.track(), 0);
            e.deleteClip (1, 0);
            auto* entries = e.playlistsToJson().getArray();
            expect (entries != nullptr);
            juce::Array<juce::var> noTakes;
            juce::DynamicObject::Ptr entry (new juce::DynamicObject());
            entry->setProperty ("track", 1);
            entry->setProperty ("takes", juce::Array<juce::var>());
            noTakes.add (juce::var (entry.get()));
            expect (write (f.track (2)));
            e.loadSession (f.dir, true, true);
            e.loadPlaylistsFromJson (juce::var (noTakes));
            expect (e.getTakeCount (1) >= 1, "the kept clips fell out of the playlist the save writes");
        }
    }

    void malformedActiveTake()
    {
        beginTest ("An out-of-range active take loads as a real take");
        for (const int bad : { -1, 7 })
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            e.newTakeFromCurrent (0, "Take 2");
            auto* obj = e.playlistsToJson()[0].getDynamicObject();
            expect (obj != nullptr);
            juce::DynamicObject::Ptr entry (new juce::DynamicObject (*obj));
            entry->setProperty ("activeTake", bad);
            juce::Array<juce::var> doc;
            doc.add (juce::var (entry.get()));
            e.loadPlaylistsFromJson (juce::var (doc));
            const int active = e.getActiveTakeIdx (0);
            expect (active >= 0 && active < e.getTakeCount (0));
            e.deleteTake (0, 1 - active);
            expectEquals (e.getTakeCount (0), 1);
            expect (! e.clipsFor (0).empty());
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
            e.newTakeFromCurrent (0, "Take 2");

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

            for (int take = 0; take < e.getTakeCount (0); ++take)
                expectEquals (clipSpan (e.getTakeClips (0, take)), (juce::int64) 38000,
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
            e.newTakeFromCurrent (0, "Take 2");

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
            for (int take = 0; take < e.getTakeCount (0); ++take)
            {
                expectEquals (clipSpan (e.getTakeClips (0, take)), (juce::int64) 10000);
                expectEquals (e.getTakeClips (0, take)[0].timelineStartSamples, (juce::int64) 0);
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

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        undoKeepsRecordedAudio();
        malformedActiveTake();
        cropAndRipple();
    }
};

static TimelineEditTests timelineEditTests;
}
