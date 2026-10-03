#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/SettingsFile.h"
#include "../UI/MainComponent.h"
#include "../UI/EditTrackRow.h"
#include "../Audio/MidiControlSurface.h"
#include "../Audio/OscRemote.h"
#include <thread>

namespace zynforge
{
class OctoberRegressionTests final : public juce::UnitTest
{
public:
    OctoberRegressionTests() : UnitTest ("October audit regressions", "zynforge") {}
    struct Folder
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-october-" + juce::Uuid().toString());
        Folder() { dir.createDirectory(); }
        ~Folder() { dir.deleteRecursively(); }
        juce::File track (int n = 1) const
        { return dir.getChildFile ("Audio Files/Track_" + juce::String::formatted ("%02d.wav", n)); }
    };
    static bool write (const juce::File& file, int samples = 4800, int channels = 1)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
            file.createOutputStream().release(), 48000.0, (unsigned) channels, 24, {}, 0));
        if (! writer) return false;
        juce::AudioBuffer<float> audio (channels, samples);
        for (int c = 0; c < channels; ++c)
            juce::FloatVectorOperations::fill (audio.getWritePointer (c), 0.25f + 0.1f * (float) c, samples);
        return writer->writeFromAudioSampleBuffer (audio, 0, samples);
    }
    static std::unique_ptr<juce::AudioFormatReader> read (const juce::File& file)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        return ConcatReader::create (fm, findTakeParts (file));
    }
    static void feed (MultitrackRecorder& r, int channels, int blocks = 10)
    {
        float signal[256]; std::fill_n (signal, 256, 0.25f);
        std::vector<const float*> inputs ((size_t) channels, signal);
        for (int b = 0; b < blocks; ++b) r.processBlock (inputs.data(), channels, 256);
    }
    static void setup (AudioEngine& e, int tracks = 2)
    {
        e.clearAllStripOverrides(); e.setStripCount (tracks); e.prepareForTests (48000, 256);
        e.setMasterGainDbFast (0); e.getMasterState().muted.store (false);
    }
    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        beginTest ("Rearmed continuation pads the missing pass without shifting audio");
        {
            Folder f, backup, mirror; MultitrackRecorder r; r.prepare (48000, 256, 2);
            r.setBackupDirectory (backup.dir);
            r.getTrack (0).armed = true; r.getTrack (1).armed = true;
            expect (r.startRecording (f.dir)); feed (r, 2); r.stopRecording();
            r.getTrack (1).armed = false; r.armContinue (0);
            expect (r.startRecording (f.dir)); feed (r, 2); r.stopRecording();
            r.getTrack (1).armed = true; r.armContinue (0);
            r.setMirrors ({ { mirror.dir, CaptureFormat::Wav24 } });
            expect (r.startRecording (f.dir)); feed (r, 2); r.stopRecording();
            auto a = read (f.track()), b = read (f.track (2)); expect (a && b);
            if (a && b)
            {
                expectEquals (a->lengthInSamples, (juce::int64) 7680);
                expectEquals (b->lengthInSamples, a->lengthInSamples);
                juce::AudioBuffer<float> data (1, 7680); b->read (&data, 0, 7680, 0, true, false);
                expectWithinAbsoluteError (data.getMagnitude (0, 2560, 2560), 0.0f, 0.00001f);
                expectWithinAbsoluteError (data.getSample (0, 6000), 0.25f, 0.00001f);
            }
            for (const auto& destination : { backup.dir, mirror.dir })
            {
                auto copy = read (destination.getChildFile (f.dir.getFileName()).getChildFile ("Audio Files/Track_02.wav"));
                expect (copy != nullptr);
                if (copy) expectEquals (copy->lengthInSamples, (juce::int64) 7680);
            }
        }
        beginTest ("Foreign same-name backup is preserved for fresh and continued capture");
        {
            Folder root, backup;
            auto first = root.dir.getChildFile ("A/Show"), second = root.dir.getChildFile ("B/Show");
            MultitrackRecorder r; r.prepare (48000, 256, 1); r.getTrack (0).armed = true;
            r.setBackupDirectory (backup.dir);
            expect (r.startRecording (first)); feed (r, 1); r.stopRecording();
            const auto copy = backup.dir.getChildFile ("Show/Audio Files/Track_01.wav");
            const auto size = copy.getSize();
            expect (! r.startRecording (second)); expectEquals (copy.getSize(), size);
            r.armContinue (0); expect (! r.startRecording (second)); expectEquals (copy.getSize(), size);
            const auto relocated = root.dir.getChildFile ("Moved/Show");
            relocated.getParentDirectory().createDirectory(); expect (first.moveFileTo (relocated));
            r.armContinue (0); expect (r.startRecording (relocated)); feed (r, 1); r.stopRecording();
            auto continued = read (copy); expect (continued != nullptr);
            if (continued) expectEquals (continued->lengthInSamples, (juce::int64) 5120);
        }
        beginTest ("Pre-roll is aligned across tracks and counted in the transport");
        {
            Folder f; MultitrackRecorder r; r.prepare (48000, 256, 8);
            for (int c = 0; c < 8; ++c) r.getTrack (c).armed = true;
            r.setPreRollSeconds (1); r.setCaptureFormat (CaptureFormat::Flac24);
            float ramp[256]; const float* inputs[8]; std::fill_n (inputs, 8, ramp);
            juce::int64 position = 0;
            auto push = [&]
            {
                for (int i = 0; i < 256; ++i) ramp[i] = (float) (position + i) / 1000000.0f;
                position += 256; r.processBlock (inputs, 8, 256);
            };
            for (int b = 0; b < 200; ++b) push();
            std::atomic<bool> running { true };
            std::thread audio ([&] { while (running.load()) { push(); juce::Thread::sleep (2); } });
            expect (r.startRecording (f.dir));
            running.store (false); audio.join();
            const auto clock = r.getSamplesSinceStart(); r.stopRecording();
            float first = -1.0f;
            for (int c = 1; c <= 8; ++c)
            {
                auto rd = read (f.dir.getChildFile ("Audio Files/Track_" + juce::String::formatted ("%02d.flac", c)));
                expect (rd != nullptr); if (! rd) continue;
                expectEquals (rd->lengthInSamples, clock);
                juce::AudioBuffer<float> data (1, (int) rd->lengthInSamples);
                expect (rd->read (&data, 0, data.getNumSamples(), 0, true, false));
                if (c == 1) first = data.getSample (0, 0);
                expectWithinAbsoluteError (data.getSample (0, 0), first, 0.000001f);
                if (data.getNumSamples() > 48000)
                    expectWithinAbsoluteError (data.getSample (0, 48000) - data.getSample (0, 47999), 0.000001f, 0.0000002f);
            }
        }
        beginTest ("Changing pre-roll during input does not invalidate callback storage");
        {
            MultitrackRecorder r; r.prepare (48000, 256, 2);
            std::atomic<bool> running { true };
            std::thread audio ([&] { while (running.load()) feed (r, 2, 1); });
            for (int i = 0; i < 30; ++i) r.setPreRollSeconds (i % 3);
            running.store (false); audio.join();
            expectEquals (r.getPreRollSeconds(), 2);
        }
        beginTest ("Changing recorded mono/stereo layout is refused before opening new parts");
        {
            Folder f; MultitrackRecorder r; r.prepare (48000, 256, 2);
            r.getTrack (0).armed = true; r.getTrack (1).armed = true;
            expect (r.startRecording (f.dir)); feed (r, 2); r.stopRecording();
            r.getTrack (0).isStereo = true; r.armContinue (0);
            expect (! r.startRecording (f.dir));
            expect (! f.dir.getChildFile ("Audio Files/Track_01_part02.wav").exists());
            auto a = read (f.track()); expect (a != nullptr);
            if (a) expectEquals (a->lengthInSamples, (juce::int64) 2560);
            Folder stereo; expect (write (stereo.track(), 4800, 2));
            r.getTrack (0).isStereo = false; r.getTrack (0).armed = false;
            r.armContinue (0); expect (! r.startRecording (stereo.dir));
            expect (! stereo.track (2).exists());
        }
        beginTest ("Missing media fails renders and destructive analysis without changing clips");
        {
            Folder f; expect (write (f.track())); AudioEngine e; setup (e); e.loadSession (f.dir);
            f.track().deleteFile(); juce::AudioBuffer<float> out;
            expect (! e.renderTrackArrangement (0, out, 4800));
            expect (! e.consolidateRange (0, 0, 4800));
            e.stripSilence (0, -50, 100, 100, 0);
            expectEquals ((int) e.clipsFor (0).size(), 1);
        }
        beginTest ("Legacy root media bounces and locked consolidation leaves one audible copy");
        {
            Folder f; expect (write (f.dir.getChildFile ("Track_01.wav")));
            AudioEngine e; setup (e, 1); e.loadSession (f.dir); juce::AudioBuffer<float> out;
            expect (e.renderStereoMix (out, 4800));
            expect (out.getMagnitude (0, out.getNumSamples()) > 0.1f);
            e.setClipLocked (0, 0, true); expect (! e.consolidateRange (0, 0, 4800));
            expectEquals ((int) e.clipsFor (0).size(), 1);
            e.setClipLocked (0, 0, false); expect (e.consolidateRange (0, 0, 4800));
        }
        beginTest ("Fresh sessions reset the previous mixer and saved VCA state round-trips");
        {
            Folder f; AudioEngine e; setup (e);
            e.setVcaGainDb (0, -18); e.setVcaMuted (0, true); e.setVcaSoloed (0, true);
            expect (e.saveSessionMixTo (f.dir));
            e.getRecorder().getTrack (0).muted = true; e.setTrackIsBus (0, true);
            e.clearSessionState();
            expect (! e.getRecorder().getTrack (0).muted.load());
            expect (! e.getRecorder().getTrack (0).isBus.load());
            expectWithinAbsoluteError (e.getVca (0).gainDb.load(), 0.0f, 0.0001f);
            expect (e.loadSessionMixFrom (f.dir));
            expectWithinAbsoluteError (e.getVca (0).gainDb.load(), -18.0f, 0.0001f);
            expect (e.getVca (0).muted.load() && e.getVca (0).soloed.load());
            e.clearAllStripOverrides();
        }
        beginTest ("Stopped daemon device loss remains visible");
        {
            AudioEngine e; setup (e); EngineStatus status;
            status.recording = false; status.captureDeviceLost = true;
            e.setExternalCaptureStatus (status); e.setExternalRecording (false);
            expect (e.captureStatus().captureDeviceLost);
            e.invalidateExternalCaptureStatus(); expect (! e.captureStatus().captureDeviceLost);
        }
        beginTest ("Auto-arm detects disarmed unmonitored inputs");
        {
            AudioEngine e; setup (e); e.setAutoArmOnInputDetect (true);
            for (int i = 0; i < 15; ++i) { feed (e.getRecorder(), 2, 1); e.serviceAutoArm (12, 0.01f); }
            expect (e.getRecorder().getTrack (0).armed.load()); e.setAutoArmOnInputDetect (false);
        }
        beginTest ("Daemon reload appends new media to an edited arrangement");
        {
            Folder f; expect (write (f.track())); AudioEngine e; setup (e); e.loadSession (f.dir);
            expect (e.splitTrackAtSample (0, 2400));
            expect (write (f.dir.getChildFile ("Audio Files/Track_01_part02.wav")));
            e.loadSession (f.dir, true, true); juce::AudioBuffer<float> out;
            expect (e.renderTrackArrangement (0, out, 9600));
            expect (out.getMagnitude (0, 4800, 4800) > 0.2f);
        }
        beginTest ("Offline solo aux bus includes its sends");
        {
            Folder f; expect (write (f.track())); AudioEngine e; setup (e); e.loadSession (f.dir);
            e.setTrackIsBus (1, true); e.setTrackSend (0, 0, 1, 0, true);
            e.setTrackSoloed (1, true); juce::AudioBuffer<float> out;
            expect (e.renderStereoMix (out, 4800));
            expectWithinAbsoluteError (out.getSample (0, 100), 0.25f / std::sqrt (2.0f), 0.0001f);
            e.setTrackMuted (0, true); expect (e.renderStereoMix (out, 4800));
            expectWithinAbsoluteError (out.getSample (0, 100), 0.0f, 0.0001f);
            e.setTrackSend (0, 0, 1, 0, false); expect (e.renderStereoMix (out, 4800));
            expect (out.getSample (0, 100) > 0.1f);
        }
        beginTest ("Healing requires identical channel, gain and mute state");
        {
            Folder f; expect (write (f.track())); AudioEngine e; setup (e); e.loadSession (f.dir);
            expect (e.splitTrackAtSample (0, 2400)); auto& clips = e.clipsFor (0);
            clips[1].gainDb = -6; expect (! e.healSeparationAt (0, 2400));
            clips[1].gainDb = 0; clips[1].muted = true; expect (! e.healSeparationAt (0, 2400));
            clips[1].muted = false; clips[1].sourceChannel = 1; expect (! e.healSeparationAt (0, 2400));
            clips[1].sourceChannel = clips[0].sourceChannel; expect (e.healSeparationAt (0, 2400));
        }
        beginTest ("Automation copy and paste preserve tension");
        {
            AudioEngine e; setup (e); const auto p = AudioEngine::AutomationParam::Volume;
            e.addAutomationPoint (0, p, 0, -12); e.addAutomationPoint (0, p, 10000, 0);
            e.setAutomationTensionAt (0, p, 0, 1, 0.7f);
            const auto copied = e.copyAutomationRange (0, p, 0, 10000);
            e.pasteAutomationRange (1, p, 20000, copied);
            expectWithinAbsoluteError (e.getAutomation (1, p)[0].tension, 0.7f, 0.0001f);
        }
        beginTest ("Multipart readers reject channel layout changes");
        {
            Folder f; expect (write (f.track()));
            expect (write (f.dir.getChildFile ("Audio Files/Track_01_part02.wav"), 4800, 2));
            expect (read (f.track()) == nullptr);
        }
        beginTest ("Offline mirror paths survive a restart");
        {
            Folder f;
            const auto offline = f.dir.getChildFile ("unmounted");
            { AudioEngine e; setup (e); expect (e.setMirrors ({ { offline, CaptureFormat::Wav24 } })); }
            AudioEngine restored;
            const auto& mirrors = restored.getRecorder().getMirrors();
            expectEquals ((int) mirrors.size(), 1);
            if (! mirrors.empty()) expect (mirrors.front().root == offline);
            restored.setMirrors ({});
        }
        beginTest ("Grouped cut and paste retain every selected channel and clip mute");
        {
            Folder f; expect (write (f.track())); expect (write (f.track (2)));
            MainComponent::s_testConstruct = true;
            {
                MainComponent main; setup (main.engine); main.engine.loadSession (f.dir);
                main.currentView = MainComponent::View::Edit;
                main.engine.setClipMuted (1, 0, true);
                main.editPage->setSelectedClips ({ { 0, 0 }, { 1, 0 } }, { 0, 0 });
                main.editClipboardCut (true);
                expect (main.engine.clipsFor (0).empty() && main.engine.clipsFor (1).empty());
                main.editClipboardPaste();
                expectEquals ((int) main.engine.clipsFor (0).size(), 1);
                expectEquals ((int) main.engine.clipsFor (1).size(), 1);
                if (! main.engine.clipsFor (1).empty()) expect (main.engine.clipsFor (1)[0].muted);
                expectEquals ((int) main.editPage->getSelectedClips().size(), 2);
            }
            MainComponent::s_testConstruct = false;
        }
        beginTest ("Slow snapped drag accumulates motion and keeps the original group peer");
        {
            Folder f; expect (write (f.track(), 48000)); expect (write (f.track (2), 48000));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            for (int t = 0; t < 2; ++t)
            {
                e.clipsFor (t)[0].fileLengthSamples = 4000;
                e.setTrackEditGroup (t, 0);
            }
            e.getMarkers().drop (0); e.getMarkers().drop (12000);
            e.setSnapMode (AudioEngine::SnapMode::Markers);
            juce::AudioFormatManager fm; fm.registerBasicFormats(); juce::AudioThumbnailCache cache (8);
            EditPage::TrackRow row (0, false, e, fm, cache);
            row.setSize (row.getHeaderWidth() + 1008, 100);
            row.draggingClipIdx = 0; row.draggingClipModeInt = 2; row.dragStartX = row.getHeaderWidth() + 4;
            for (int dx = 1; dx <= 150; ++dx)
            {
                const juce::Point<float> down ((float) row.dragStartX, 50.0f);
                juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(),
                    down.translated ((float) dx, 0), juce::ModifierKeys::leftButtonModifier,
                    1, 0, 0, 0, 0, &row, &row, juce::Time::getCurrentTime(), down,
                    juce::Time::getCurrentTime(), 1, true);
                row.mouseDrag (event);
            }
            expectEquals (e.clipsFor (0)[0].timelineStartSamples, (juce::int64) 12000);
            expectEquals (e.clipsFor (1)[0].timelineStartSamples, (juce::int64) 12000);
        }
        beginTest ("OSC and MCU respect LOCK and MCU gain follows both stereo halves");
        {
            AudioEngine e; setup (e); e.setTrackStereo (0, true);
            OscRemote osc (e); MidiControlSurface mcu (e);
            e.setControlsLocked (true);
            osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/mute", (juce::int32) 1));
            expect (! e.getRecorder().getTrack (0).muted.load());
            mcu.handleIncomingMidiMessage (nullptr, juce::MidiMessage::pitchWheel (9, 0));
            expectWithinAbsoluteError (e.getMasterState().gainDb.load(), 0.0f, 0.0001f);
            e.setControlsLocked (false);
            mcu.handleIncomingMidiMessage (nullptr, juce::MidiMessage::pitchWheel (1, 6000));
            const float expected = mcu::faderToDb (6000);
            expectWithinAbsoluteError (e.getRecorder().getTrack (0).gainDb.load(), expected, 0.0001f);
            expectWithinAbsoluteError (e.getRecorder().getTrack (1).gainDb.load(), expected, 0.0001f);
            e.clearAllStripOverrides();
        }
        beginTest ("Every preferences writer shares the isolated settings file");
        {
            expect (isolatedSettings.load());
            juce::PropertiesFile::Options options;
            auto settings = makeSettingsFile (options);
            expect (settings->getFile().isAChildOf (juce::File::getSpecialLocation (juce::File::tempDirectory)));
            StripColours colours; StripRouting routing;
            colours.setColour (231, juce::Colours::red); routing.setInput (231, -1);
            settings->reload();
            expect (settings->containsKey ("strip_color_231"));
            expectEquals (settings->getIntValue ("strip_in_231", 999), -1);
            colours.clearColour (231); routing.clearInput (231);
        }
    }
};
static OctoberRegressionTests octoberRegressionTests;
}
