#include "../Audio/AudioEngine.h"
#include "../Audio/TrackFileTransaction.h"
#include "../Audio/MultiPartReader.h"

namespace zynforge
{
class SeptemberRegressionTests final : public juce::UnitTest
{
public:
    SeptemberRegressionTests() : UnitTest ("September session-integrity regressions", "zynforge") {}
    struct Directory
    {
        juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("zf-september-" + juce::Uuid().toString());
        Directory() { file.createDirectory(); file.getChildFile ("Audio Files").createDirectory(); }
        ~Directory() { file.deleteRecursively(); }
        juce::File track (int n) const { return file.getChildFile ("Audio Files").getChildFile (juce::String::formatted ("Track_%02d.wav", n)); }
    };
    static bool write (const juce::File& file, float value, int length = 4800, int channels = 1)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr) return false;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), 48000, (unsigned int) channels, 24, {}, 0));
        if (writer == nullptr) return false;
        stream.release(); juce::AudioBuffer<float> buffer (channels, length);
        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), value + (float) ch * 0.1f, length);
        return writer->writeFromAudioSampleBuffer (buffer, 0, length);
    }
    static float sample (const juce::File& file)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr) return -99.0f;
        juce::AudioBuffer<float> data (1, 1); reader->read (&data, 0, 1, 0, true, false);
        return data.getSample (0, 0);
    }
    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        beginTest ("Backup collision refuses start without touching existing audio");
        {
            Directory dir; expect (write (dir.track (1), 0.3f));
            MultitrackRecorder rec; rec.prepare (48000, 256, 1); rec.getTrack (0).armed.store (true);
            rec.setBackupDirectory (dir.file.getParentDirectory());
            expect (! rec.startRecording (dir.file));
            expectWithinAbsoluteError (sample (dir.track (1)), 0.3f, 0.001f);
        }
        beginTest ("Arms are frozen for a take even if a caller mutates the live flag");
        {
            Directory dir; MultitrackRecorder rec; rec.prepare (48000, 256, 2);
            rec.getTrack (0).armed.store (true); expect (rec.startRecording (dir.file));
            juce::AudioBuffer<float> data (2, 256); data.clear(); const float* inputs[] { data.getReadPointer (0), data.getReadPointer (1) };
            rec.processBlock (inputs, 2, 256);
            rec.getTrack (0).armed.store (false); rec.getTrack (1).armed.store (true);
            rec.processBlock (inputs, 2, 256); rec.stopRecording();
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            auto reader = ConcatReader::create (fm, findTakeParts (dir.track (1)));
            expect (reader != nullptr); if (reader) expectEquals (reader->lengthInSamples, (juce::int64) 512);
            expect (! dir.track (2).existsAsFile());
        }
        beginTest ("Deleted arrangements remain silent in bounce and crop");
        {
            Directory dir; expect (write (dir.track (1), 0.3f)); expect (write (dir.track (2), 0.2f));
            AudioEngine e; e.setActiveSessionDir (dir.file); e.loadSession (dir.file); e.setStripCount (2);
            expect (e.deleteClip (0, 0));
            float peak = 0;
            expect (e.forEachArrangementWindow (0, 0, 4800, [&] (const float* p, juce::int64, int n)
            { for (int i = 0; i < n; ++i) peak = juce::jmax (peak, std::abs (p[i])); return true; }));
            expectWithinAbsoluteError (peak, 0.0f, 0.0001f);
            e.cropToRange (0, 2400); expect (e.clipsFor (0).empty());
            e.loadSession (dir.file, true); expect (e.clipsFor (0).empty());
        }
        beginTest ("Pasted media on an unrecorded track extends playback and survives JSON");
        {
            Directory dir; expect (write (dir.track (1), 0.3f));
            AudioEngine e; e.setActiveSessionDir (dir.file); e.loadSession (dir.file); e.setStripCount (3);
            expect (e.pasteClip (2, 9600, 0, 4800, 0, 0, 0, "external", dir.track (1), 0, 1) >= 0);
            expectEquals (e.getPlayer().getNumTracks(), 3);
            expect (e.getPlayer().getTotalLengthSamples() >= 14400);
            float peak = 0;
            expect (e.forEachArrangementWindow (2, 9600, 14400, [&] (const float* p, juce::int64, int n)
            { for (int i = 0; i < n; ++i) peak = juce::jmax (peak, std::abs (p[i])); return true; }));
            expectWithinAbsoluteError (peak, 0.3f, 0.001f);
            const auto saved = e.playlistsToJson(); e.loadSession (dir.file); e.loadPlaylistsFromJson (saved);
            expectEquals ((int) e.clipsFor (2).size(), 1);
            expectEquals (e.clipsFor (2)[0].fadeCurve, 1);
            expectEquals (e.clipsFor (2)[0].sourceChannel, 0);
        }
        beginTest ("Missing explicit media never falls back to own audio");
        {
            Directory dir; expect (write (dir.track (1), 0.3f));
            AudioEngine e; e.setActiveSessionDir (dir.file); e.loadSession (dir.file); e.setStripCount (1);
            e.deleteClip (0, 0); e.pasteClip (0, 0, 0, 1000, 0, 0, 0, "missing", dir.file.getChildFile ("missing.wav"));
            float peak = 0;
            expect (e.forEachArrangementWindow (0, 0, 1000, [&] (const float* p, juce::int64, int n)
            { for (int i = 0; i < n; ++i) peak = juce::jmax (peak, std::abs (p[i])); return true; }));
            expectWithinAbsoluteError (peak, 0.0f, 0.0001f);
            expect (! e.normalizeClip (0, 0, -1));
        }
        beginTest ("Locked clips cannot split or ripple; deleting earlier take retains active identity");
        {
            Directory dir; expect (write (dir.track (1), 0.3f));
            AudioEngine e; e.setActiveSessionDir (dir.file); e.loadSession (dir.file); e.setStripCount (1);
            e.setClipLocked (0, 0, true);
            expect (! splitClipAt (e.clipsFor (0), 0, 100));
            e.rippleDeleteRange (0, 100, 200);
            expectEquals ((int) e.clipsFor (0).size(), 1); expectEquals (e.clipsFor (0)[0].fileLengthSamples, (juce::int64) 4800);
            e.newTakeFromCurrent (0, "second"); e.newTakeFromCurrent (0, "third"); e.setActiveTake (0, 1);
            e.deleteTake (0, 0); expectEquals (e.getActiveTakeIdx (0), 0); expectEquals (e.getTakeName (0, 0), juce::String ("second"));
        }
        beginTest ("Empty automation snapshot removes new lanes");
        {
            AudioEngine e; e.loadAutomationFromJson (juce::var (juce::Array<juce::var>()));
            const auto before = e.automationToJson(); e.addAutomationPoint (2, AudioEngine::AutomationParam::Volume, 100, -12);
            e.loadAutomationFromJson (before); expect (e.getAutomation (2, AudioEngine::AutomationParam::Volume).empty());
        }
        beginTest ("Session mix restores strip UUIDs independently of global application settings");
        {
            Directory dir; AudioEngine e; e.setStripCount (2);
            const auto id0 = e.getRecorder().getTrack (0).stripId;
            const auto id1 = e.getRecorder().getTrack (1).stripId;
            expect (e.saveSessionMixTo (dir.file));
            e.getRecorder().getTrack (0).stripId = "another-session-0";
            e.getRecorder().getTrack (1).stripId = "another-session-1";
            expect (e.loadSessionMixFrom (dir.file));
            expectEquals (e.getRecorder().getTrack (0).stripId, id0);
            expectEquals (e.getRecorder().getTrack (1).stripId, id1);
            e.setStripCount (2);
            expectEquals (e.getRecorder().getTrack (0).stripId, id0);
            expectEquals (e.getRecorder().getTrack (1).stripId, id1);
        }
        beginTest ("Reorder and deletion preserve audio, UUID, automation and persisted gains");
        {
            Directory dir; expect (write (dir.track (1), 0.1f)); expect (write (dir.track (2), 0.2f)); expect (write (dir.track (3), 0.3f));
            AudioEngine e; e.setActiveSessionDir (dir.file); e.loadSession (dir.file); e.setStripCount (3);
            e.setTrackName (0, "A"); e.setTrackName (1, "B"); e.setTrackName (2, "C"); e.setTrackGainDb (1, -7);
            const auto id = e.getRecorder().getTrack (1).stripId;
            e.addAutomationPoint (1, AudioEngine::AutomationParam::Volume, 100, -12);
            expect (e.reorderTracks ({ 1, 0, 2 }));
            expectWithinAbsoluteError (sample (dir.track (1)), 0.2f, 0.001f);
            expectEquals (e.getRecorder().getTrack (0).stripId, id);
            expectEquals ((int) e.getAutomation (0, AudioEngine::AutomationParam::Volume).size(), 1);
            e.setStripCount (3); expectWithinAbsoluteError (e.getRecorder().getTrack (0).gainDb.load(), -7.0f, 0.001f);
            expect (e.removeStripAt (0));
            expectEquals (e.getRecorder().getTrack (0).name, juce::String ("A"));
            expectWithinAbsoluteError (sample (dir.track (1)), 0.1f, 0.001f);
            expect (! dir.file.getChildFile ("Removed Tracks").findChildFiles (juce::File::findFiles, true, "*.wav").isEmpty());
        }
        beginTest ("Moving stereo past mono keeps both halves adjacent to their native file");
        {
            Directory dir; expect (write (dir.track (1), 0.1f, 4800, 2)); expect (write (dir.track (3), 0.3f));
            AudioEngine e; e.setActiveSessionDir (dir.file); e.loadSession (dir.file); e.setStripCount (3);
            e.setTrackStereo (0, true);
            expect (e.reorderTracks ({ 2, 0, 1 }));
            expect (! e.getRecorder().getTrack (0).isStereo.load()); expect (e.getRecorder().getTrack (1).isStereo.load());
            expectWithinAbsoluteError (sample (dir.track (1)), 0.3f, 0.001f);
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            auto r = ConcatReader::create (fm, findTakeParts (dir.track (2)));
            expect (r != nullptr); if (r) expectEquals ((int) r->numChannels, 2);
            expect (! dir.track (3).existsAsFile());
        }
        beginTest ("Failed destination install rolls back without overwriting existing data");
        {
            Directory dir; expect (write (dir.track (1), 0.1f)); expect (write (dir.track (2), 0.2f));
            TrackFileTransaction tx;
            expect (! tx.begin (dir.file, { { dir.track (1), dir.track (2) } }));
            expectWithinAbsoluteError (sample (dir.track (1)), 0.1f, 0.001f);
            expectWithinAbsoluteError (sample (dir.track (2)), 0.2f, 0.001f);
        }
        beginTest ("Interrupted file reorder restores all original media");
        {
            Directory dir; expect (write (dir.track (1), 0.1f)); expect (write (dir.track (2), 0.2f));
            TrackFileTransaction tx;
            expect (tx.begin (dir.file, { { dir.track (1), dir.track (2) }, { dir.track (2), dir.track (1) } }));
            expect (TrackFileTransaction::recover (dir.file));
            expectWithinAbsoluteError (sample (dir.track (1)), 0.1f, 0.001f);
            expectWithinAbsoluteError (sample (dir.track (2)), 0.2f, 0.001f);
        }
        beginTest ("Loop boundary fills the entire device block without inserted silence");
        {
            Directory dir; expect (write (dir.track (1), 0.3f));
            SessionPlayer p; p.loadSession (dir.file); p.setLoopRegion (0, 100); p.setLoopEnabled (true); p.start();
            juce::AudioBuffer<float> data (1, 256); float* outputs[] { data.getWritePointer (0) };
            p.processBlock (outputs, 1, 256);
            expectWithinAbsoluteError (data.getSample (0, 99), 0.3f, 0.001f);
            expectWithinAbsoluteError (data.getSample (0, 100), 0.3f, 0.001f);
            expectWithinAbsoluteError (data.getSample (0, 255), 0.3f, 0.001f);
            expectEquals (p.getPositionSamples(), (juce::int64) 56);
        }
    }
};
static SeptemberRegressionTests septemberRegressionTests;
}
