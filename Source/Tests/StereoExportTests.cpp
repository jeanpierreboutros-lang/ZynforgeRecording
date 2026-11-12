// Headless tests for stereo-pair export (TrackExporter::exportStereoPair):
// two mono sources must interleave into ONE stereo file with the correct
// channel assignment, in each PCM format and across a sample-rate change.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/TrackExporter.h"
#include "FailingAudioReader.h"

namespace zynforge
{
    class StereoExportTests final : public juce::UnitTest
    {
    public:
        StereoExportTests() : juce::UnitTest ("Stereo-pair export", "zynforge") {}

        // Write `value` as a constant-DC mono WAV of `len` samples at `sr`.
        static void writeMonoDc (const juce::File& f, float value, int len, double sr)
        {
            std::vector<float> s ((size_t) len, value);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, 1, 24, {}, 0));
            os.release();
            const float* chans[1] = { s.data() };
            w->writeFromFloatArrays (chans, 1, len);
        }

        void runTest() override
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-stereoexp-" + juce::Uuid().toString());
            dir.createDirectory();

            const double sr = 48000.0;
            const int len = 24000;          // 0.5 s
            auto srcL = dir.getChildFile ("L.wav");
            auto srcR = dir.getChildFile ("R.wav");
            writeMonoDc (srcL,  0.30f, len, sr);
            writeMonoDc (srcR, -0.60f, len, sr);

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();

            beginTest ("exportStereoPair interleaves L->ch0, R->ch1 (WAV, same SR)");
            {
                TrackExporter ex;
                ExportOptions opts; opts.format = ExportFormat::Wav24;
                opts.sampleRate = sr; opts.bitsPerSample = 24;
                auto stem = dir.getChildFile ("pair");
                juce::String err;
                expect (ex.exportStereoPair (srcL, srcR, stem, opts, err), err);

                auto out = stem.withFileExtension (".wav");
                expect (out.existsAsFile());
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out));
                expect (rd != nullptr, "exported stereo file is unreadable");
                if (rd != nullptr)   // guard: expect() doesn't stop the test
                {
                    expectEquals ((int) rd->numChannels, 2, "export is not stereo");
                    expectEquals (rd->lengthInSamples, (juce::int64) len);

                    juce::AudioBuffer<float> buf (2, len);
                    rd->read (&buf, 0, len, 0, true, true);
                    // 24-bit quantisation -> small tolerance.
                    expectWithinAbsoluteError (buf.getSample (0, len / 2),  0.30f, 0.001f);
                    expectWithinAbsoluteError (buf.getSample (1, len / 2), -0.60f, 0.001f);
                    // Channels are distinct (not a doubled mono).
                    expect (std::abs (buf.getSample (0, 100) - buf.getSample (1, 100)) > 0.5f,
                            "channels look identical -- not truly stereo");
                }
            }

            beginTest ("exportStereoPair resamples to a new rate, stays stereo");
            {
                TrackExporter ex;
                ExportOptions opts; opts.format = ExportFormat::Wav24;
                opts.sampleRate = 44100.0; opts.bitsPerSample = 24;
                auto stem = dir.getChildFile ("pair44");
                juce::String err;
                expect (ex.exportStereoPair (srcL, srcR, stem, opts, err), err);

                auto out = stem.withFileExtension (".wav");
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out));
                expect (rd != nullptr);
                expectEquals ((int) rd->numChannels, 2);
                expectWithinAbsoluteError ((float) rd->sampleRate, 44100.0f, 1.0f);
                // ~0.5 s at 44.1k.
                expect (std::abs (rd->lengthInSamples - 22050) < 64);
                juce::AudioBuffer<float> buf (2, (int) rd->lengthInSamples);
                rd->read (&buf, 0, (int) rd->lengthInSamples, 0, true, true);
                const int mid = (int) rd->lengthInSamples / 2;
                expectWithinAbsoluteError (buf.getSample (0, mid),  0.30f, 0.01f);
                expectWithinAbsoluteError (buf.getSample (1, mid), -0.60f, 0.01f);
            }

            beginTest ("missing R source fails closed");
            {
                TrackExporter ex;
                ExportOptions opts;
                juce::String err;
                expect (! ex.exportStereoPair (srcL, dir.getChildFile ("nope.wav"),
                                               dir.getChildFile ("x"), opts, err));
                expect (err.isNotEmpty());
            }

            beginTest ("Mismatched legacy stereo source rates refuse without replacing output");
            {
                auto mismatched = dir.getChildFile ("right44.wav");
                writeMonoDc (mismatched, -0.6f, 44100, 44100.0);
                auto stem = dir.getChildFile ("mismatched");
                auto destination = stem.withFileExtension (".wav");
                expect (destination.replaceWithText ("previous deliverable"));
                TrackExporter exporter;
                ExportOptions options;
                juce::String error;
                expect (! exporter.exportStereoPair (srcL, mismatched, stem, options, error));
                expect (error.isNotEmpty());
                expect (destination.loadFileAsString() == "previous deliverable",
                        "refused stereo export replaced the existing destination");
            }

            const auto failingSource = dir.getChildFile ("source.zffault");
            expect (failingSource.replaceWithText ("synthetic reader fixture"));
            for (bool pair : { false, true })
                for (double destinationRate : { 48000.0, 44100.0 })
                {
                    beginTest (juce::String (pair ? "Stereo" : "Mono")
                               + " export rejects a read failure at "
                               + juce::String (destinationRate, 0) + " Hz and preserves output");
                    testaudio::ReadEvidence evidence;
                    TrackExporter exporter;
                    exporter.formatManager.registerFormat (new testaudio::FailingFormat (evidence), false);
                    ExportOptions options;
                    options.sampleRate = destinationRate;
                    auto stem = dir.getChildFile ("read-failure");
                    auto destination = stem.withFileExtension (".wav");
                    expect (destination.replaceWithText ("previous deliverable"));
                    juce::String error;
                    const bool success = pair
                        ? exporter.exportStereoPair (srcL, failingSource, stem, options, error)
                        : exporter.exportTrack (failingSource, stem, options, error);
                    expectEquals (evidence.successfulReads, 1);
                    expect (evidence.failedReads > 0, "fault was not exercised");
                    expect (! success, "a decoder failure was published as a complete export");
                    expect (error.isNotEmpty());
                    expect (destination.loadFileAsString() == "previous deliverable",
                            "failed export replaced the existing destination");
                    expectEquals (dir.findChildFiles (juce::File::findFiles, false, "*.partial*").size(), 0);
                }

            testExportsKeepTheBroadcastStamp (dir);
            dir.deleteRecursively();
        }

        // A take as the recorder leaves it: bext time stamp and channel names.
        static bool writeStampedTake (const juce::File& f, int channels, const juce::String& description,
                                      juce::int64 timeReference, double rate)
        {
            juce::StringPairArray meta;
            meta.set (juce::WavAudioFormat::bwavDescription, description);
            meta.set (juce::WavAudioFormat::bwavOriginator, "ZynForge Recording");
            meta.set (juce::WavAudioFormat::bwavOriginationDate, "2026-11-11");
            meta.set (juce::WavAudioFormat::bwavOriginationTime, "20:00:00");
            meta.set (juce::WavAudioFormat::bwavTimeReference, juce::String (timeReference));
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, (unsigned int) channels, 24, meta, 0));
            if (w == nullptr) return false;
            os.release();
            juce::AudioBuffer<float> audio (channels, 4800);
            for (int c = 0; c < channels; ++c) juce::FloatVectorOperations::fill (audio.getWritePointer (c), 0.1f * (c + 1), 4800);
            return w->writeFromAudioSampleBuffer (audio, 0, 4800);
        }

        juce::StringPairArray stampOf (juce::AudioFormatManager& fm, const juce::File& f)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            expect (r != nullptr, f.getFileName() + " is unreadable");
            return r != nullptr ? r->metadataValues : juce::StringPairArray();
        }

        void testExportsKeepTheBroadcastStamp (const juce::File& dir)
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            const juce::int64 eightPm = (juce::int64) 20 * 3600 * 48000;
            const auto stereo = dir.getChildFile ("Track_05.wav");
            expect (writeStampedTake (stereo, 2, "sTRK1=AUD L\r\nsTRK2=AUD R\r\nShow take", eightPm, 48000.0));

            beginTest ("A raw export keeps the take's time stamp and channel names");
            {
                TrackExporter ex;
                ExportOptions opts;
                juce::String err;
                const auto stem = dir.getChildFile ("raw-stereo");
                expect (ex.exportTrack (stereo, stem, opts, err), err);
                const auto meta = stampOf (fm, stem.withFileExtension (".wav"));
                expectEquals (meta[juce::WavAudioFormat::bwavTimeReference], juce::String (eightPm),
                              "the export lost its place in the day");
                expectEquals (meta[juce::WavAudioFormat::bwavDescription],
                              juce::String ("sTRK1=AUD L\r\nsTRK2=AUD R\r\nShow take"));
                expectEquals (meta[juce::WavAudioFormat::bwavOriginator], juce::String ("ZynForge Recording"));
            }

            beginTest ("A converted export moves the time stamp to the new rate");
            {
                TrackExporter ex;
                ExportOptions opts;
                opts.sampleRate = 96000.0;
                juce::String err;
                const auto stem = dir.getChildFile ("raw-96k");
                expect (ex.exportTrack (stereo, stem, opts, err), err);
                const auto meta = stampOf (fm, stem.withFileExtension (".wav"));
                expectEquals (meta[juce::WavAudioFormat::bwavTimeReference], juce::String (eightPm * 2),
                              "20:00:00 at 48 kHz is not 20:00:00 at 96 kHz");
            }

            beginTest ("One channel of a stereo take keeps only its own name");
            {
                TrackExporter ex;
                ExportOptions opts;
                opts.onlyChannel = 1;
                juce::String err;
                const auto stem = dir.getChildFile ("raw-right");
                expect (ex.exportTrack (stereo, stem, opts, err), err);
                const auto meta = stampOf (fm, stem.withFileExtension (".wav"));
                expectEquals (meta[juce::WavAudioFormat::bwavDescription], juce::String ("sTRK1=AUD R\r\nShow take"));
                expectEquals (meta[juce::WavAudioFormat::bwavTimeReference], juce::String (eightPm));
            }

            beginTest ("A pair exported from two mono takes names both sides");
            {
                const auto left = dir.getChildFile ("Track_07.wav"), right = dir.getChildFile ("Track_08.wav");
                expect (writeStampedTake (left,  1, "sTRK1=OH L\r\nShow take", eightPm, 48000.0));
                expect (writeStampedTake (right, 1, "sTRK1=OH R\r\nShow take", eightPm, 48000.0));
                TrackExporter ex;
                ExportOptions opts;
                juce::String err;
                const auto stem = dir.getChildFile ("pair-stamped");
                expect (ex.exportStereoPair (left, right, stem, opts, err), err);
                const auto meta = stampOf (fm, stem.withFileExtension (".wav"));
                expectEquals (meta[juce::WavAudioFormat::bwavDescription], juce::String ("sTRK1=OH L\r\nsTRK2=OH R\r\nShow take"));
                expectEquals (meta[juce::WavAudioFormat::bwavTimeReference], juce::String (eightPm));
            }
        }
    };

    static StereoExportTests stereoExportTests;

    // Opt-in kernel ENOSPC exercise. The launcher creates and validates a tiny
    // disposable disk image; ordinary test runs never fill or mount a volume.
    class BoundedVolumeExportTests final : public juce::UnitTest
    {
    public:
        BoundedVolumeExportTests() : UnitTest ("Audit bounded volume export", "zynforge") {}

        void runTest() override
        {
            const auto path = juce::SystemStats::getEnvironmentVariable ("ZYNFORGE_AUDIT_FULL_VOLUME", {});
            if (path.isEmpty())
            {
                logMessage ("Bounded-volume export probe not requested; use tools/audit_runtime_probe.py disk-full");
                return;
            }
            beginTest ("Validate the disposable bounded-volume fixture before export");
            const juce::File volume (path);
            const auto nonce = juce::SystemStats::getEnvironmentVariable ("ZYNFORGE_AUDIT_VOLUME_NONCE", {});
            const auto marker = juce::JSON::parse (volume.getChildFile ("audit-volume.json"));
            const auto capacity = volume.getVolumeTotalSize();
            const auto freeBytes = volume.getBytesFreeOnVolume();
            const auto destination = volume.getChildFile ("previous.wav");
            const bool valid = volume.isDirectory() && nonce.isNotEmpty()
                && marker["nonce"].toString() == nonce && (bool) marker["kernelEnospcObserved"]
                && capacity > 0 && capacity <= 64 * 1024 * 1024
                && freeBytes > 16 * 1024 && freeBytes < 256 * 1024
                && destination.loadFileAsString() == "previous deliverable";
            expect (valid, "refusing a missing, unbounded, or incorrectly prepared disk fixture");
            if (! valid) return;

            const auto sourceDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-bounded-export-" + juce::Uuid().toString());
            const juce::ScopeGuard cleanup { [&] { sourceDirectory.deleteRecursively(); } };
            const bool directoryCreated = sourceDirectory.createDirectory().wasOk();
            expect (directoryCreated);
            if (! directoryCreated) return;
            const auto source = sourceDirectory.getChildFile ("source.wav");
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> stream (source.createOutputStream());
                expect (stream != nullptr);
                if (stream == nullptr) return;
                std::unique_ptr<juce::AudioFormatWriter> writer (
                    wav.createWriterFor (stream.get(), 48000.0, 1, 24, {}, 0));
                expect (writer != nullptr);
                if (writer == nullptr) return;
                stream.release();
                std::vector<float> samples (384000, 0.25f);
                const float* channels[] { samples.data() };
                expect (writer->writeFromFloatArrays (channels, 1, (int) samples.size()));
            }
            for (const bool pair : { false, true })
                for (const double rate : { 48000.0, 44100.0 })
                {
                    beginTest (juce::String (pair ? "Stereo" : "Mono") + " export at "
                               + juce::String (rate, 0) + " Hz preserves previous output on kernel ENOSPC");
                    TrackExporter exporter;
                    ExportOptions options; options.sampleRate = rate;
                    juce::String error;
                    const auto stem = volume.getChildFile ("previous");
                    const bool success = pair
                        ? exporter.exportStereoPair (source, source, stem, options, error)
                        : exporter.exportTrack (source, stem, options, error);
                    expect (! success, "disk exhaustion was reported as successful export");
                    expect (error.contains ("Write failed"), "fixture failed before the audio write: " + error);
                    expectEquals (destination.loadFileAsString(), juce::String ("previous deliverable"));
                    expectEquals (volume.findChildFiles (juce::File::findFiles, false, "*.partial*").size(), 0,
                                  "failed export left a partial file");
                }
        }
    };
    static BoundedVolumeExportTests boundedVolumeExportTests;
}
