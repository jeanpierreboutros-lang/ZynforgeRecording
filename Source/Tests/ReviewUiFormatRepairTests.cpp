#include "../UI/MainComponent.h"
#include "../Audio/SettingsFile.h"
#include "../Audio/TrackExporter.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Audio/PunchSplice.h"

namespace zynforge
{
class ReviewUiFormatRepairTests final : public juce::UnitTest
{
public:
    ReviewUiFormatRepairTests() : UnitTest ("Review UI and format repairs", "zynforge") {}

    void expectFloatFile (const juce::File& file, int channels, float left, float right = 0.0f)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        expect (reader != nullptr, file.getFullPathName());
        if (reader == nullptr) return;
        expect (reader->usesFloatingPointData, "32-bit float selection produced integer PCM");
        expectEquals ((int) reader->numChannels, channels);
        expectWithinAbsoluteError (reader->sampleRate, 48000.0, 0.01);
        juce::AudioBuffer<float> audio (channels, 32);
        expect (reader->read (&audio, 0, 32, 0, true, true));
        expectWithinAbsoluteError (audio.getSample (0, 0), left, 0.00001f);
        if (channels == 2) expectWithinAbsoluteError (audio.getSample (1, 0), right, 0.00001f);
    }

    void runTest() override
    {
        beginTest ("Isolated UI and format fixture setup");
        AudioEngine::setTestModeSkipAudioInit (true);
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-review-ui-format-" + juce::Uuid().toString());
        expect (root.createDirectory().wasOk());
        const juce::ScopeGuard cleanup { [&] { root.deleteRecursively(); } };

        beginTest ("Undo bookkeeping does not rewrite every strip after a mixer gesture");
        {
            MainComponent host;
            host.stopTimer();
            host.engine.clearAllStripOverrides();
            host.engine.setStripCount (27);
            host.rebaselineMixerUndo();
            host.engine.setTrackPan (0, 0.5f);
            const auto before = settingsReloadCountForTests.load();
            for (int tick = 0; tick < 4; ++tick) host.pollMixerUndo();
            expectEquals ((int) (settingsReloadCountForTests.load() - before), 0,
                          "Already-applied mixer state must not be persisted again");
            expect (host.undoManager.canUndo());
            host.editUndo();
            expectWithinAbsoluteError (host.engine.getRecorder().getTrack (0).pan.load(), 0.0f, 0.0001f);
            host.editRedo();
            expectWithinAbsoluteError (host.engine.getRecorder().getTrack (0).pan.load(), 0.5f, 0.0001f);
            host.engine.clearAllStripOverrides();
        }

        beginTest ("Space can arm STOP while recording metadata is saving");
        {
            MainComponent host;
            host.stopTimer();
            host.engine.clearAllStripOverrides();
            auto& recorder = host.engine.getRecorder();
            recorder.prepare (48000.0, 256, 1);
            recorder.getTrack (0).armed.store (true);
            const auto session = root.getChildFile ("keyboard-stop");
            expect (session.createDirectory().wasOk());
            expect (recorder.startRecording (session));
            host.engine.setActiveSessionDir (session);
            // The published UI state while the metadata worker owns a save.
            host.pendingMetadataSaves = 1;
            host.sessionIoBusy.store (true);
            host.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), nullptr);
            expect (host.stopArmedAtMs != 0, "Saving swallowed the recording STOP hotkey");
            expect (recorder.isRecording(), "First STOP must retain two-tap protection");
            host.pendingMetadataSaves = 0;
            host.sessionIoBusy.store (false);
            recorder.stopRecording();
            host.engine.clearAllStripOverrides();
            MultitrackRecorder::deleteSessionAfterCancellingReports (session);
        }

        beginTest ("AIFF float capture preserves over-unity samples in primary backup and mirror");
        {
            const auto session = root.getChildFile ("float-capture");
            const auto backup = root.getChildFile ("backup");
            const auto mirror = root.getChildFile ("mirror");
            session.createDirectory(); backup.createDirectory(); mirror.createDirectory();
            MultitrackRecorder recorder;
            recorder.prepare (48000.0, 256, 1);
            recorder.setCaptureFormat (CaptureFormat::Aiff32Float);
            recorder.setBackupCaptureFormat (CaptureFormat::Aiff32Float);
            recorder.setBackupDirectory (backup);
            expect (recorder.setMirrors ({ { mirror, CaptureFormat::Aiff32Float } }));
            recorder.getTrack (0).armed.store (true);
            expect (recorder.startRecording (session));
            std::array<float, 256> samples; samples.fill (1.5f);
            const float* input[] { samples.data() };
            recorder.processBlock (input, 1, 256);
            recorder.stopRecording();
            expectFloatFile (session.getChildFile ("Audio Files/Track_01.aif"), 1, 1.5f);
            expectFloatFile (backup.getChildFile ("float-capture/Audio Files/Track_01.aif"), 1, 1.5f);
            expectFloatFile (mirror.getChildFile ("float-capture/Audio Files/Track_01.aif"), 1, 1.5f);
            MultitrackRecorder::deleteSessionAfterCancellingReports (session);
        }

        beginTest ("AIFF float export preserves stereo polarity and floating-point headroom");
        {
            auto source = root.getChildFile ("source.wav");
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (source.createOutputStream().release(), 48000.0, 2, 32, {}, 0));
            expect (writer != nullptr);
            if (writer == nullptr) return;
            juce::AudioBuffer<float> audio (2, 256);
            for (int i = 0; i < 256; ++i) { audio.setSample (0, i, 1.5f); audio.setSample (1, i, -1.25f); }
            expect (writer->writeFromAudioSampleBuffer (audio, 0, 256)); writer.reset();
            ExportOptions options; options.format = ExportFormat::Aiff24; options.bitsPerSample = 32;
            TrackExporter exporter; juce::String error;
            const auto dest = root.getChildFile ("exported");
            expect (exporter.exportTrack (source, dest, options, error), error);
            expectFloatFile (dest.withFileExtension (".aif"), 2, 1.5f, -1.25f);
            juce::AudioFormatManager formats; formats.registerBasicFormats();
            const auto punched = root.getChildFile ("punched.aif");
            expect (splicePunchFile (formats, dest.withFileExtension (".aif"), source, 128, punched));
            expectFloatFile (punched, 2, 1.5f, -1.25f);
        }

        beginTest ("Float AIFF periodic flush preserves frames and subsequent append position");
        {
            const auto file = root.getChildFile ("flushed.aif");
            std::unique_ptr<juce::AudioFormatWriter> writer (FloatAiffWriter::create (
                file.createOutputStream().release(), 44100.0, 1));
            expect (writer != nullptr);
            if (writer == nullptr) return;
            float first[3] { 1.5f, -2.0f, 0.125f };
            const float* channels[] { first };
            expect (writer->writeFromFloatArrays (channels, 1, 3));
            expect (writer->flush());
            expect (writer->writeFromFloatArrays (channels, 1, 3));
            expect (writer->flush());
            juce::AiffAudioFormat aiff;
            std::unique_ptr<juce::AudioFormatReader> reader (aiff.createReaderFor (file.createInputStream().release(), true));
            expect (reader != nullptr);
            if (reader != nullptr)
            {
                expect (reader->usesFloatingPointData);
                expectEquals (reader->lengthInSamples, (juce::int64) 6);
                expectWithinAbsoluteError (reader->sampleRate, 44100.0, 0.01);
                juce::AudioBuffer<float> audio (1, 6);
                expect (reader->read (&audio, 0, 6, 0, true, false));
                for (int i = 0; i < 6; ++i)
                    expectWithinAbsoluteError (audio.getSample (0, i), first[i % 3], 0.000001f);
            }
        }
    }
};
static ReviewUiFormatRepairTests reviewUiFormatRepairTests;
}
