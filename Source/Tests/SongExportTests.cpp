// Export Songs: marker ranges, the range render, and the batch that writes
// each song as a mix, stems or both.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/SongExport.h"
#include "../UI/ExportSongsDialog.h"
#include "../UI/MainComponent.h"

#include <thread>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    namespace sx = songexport;

    class SongExportTests final : public juce::UnitTest
    {
    public:
        SongExportTests() : juce::UnitTest ("Song export", "zynforge") {}

        struct Folder
        {
            juce::File dir { juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zf-songs-" + juce::Uuid().toString()).getChildFile ("Show") };
            Folder() { audio().createDirectory(); exports().createDirectory(); }
            ~Folder() { dir.getParentDirectory().deleteRecursively(); }
            juce::File audio()   const { return dir.getChildFile ("Audio Files"); }
            juce::File exports() const { return dir.getChildFile ("Export Files"); }
            juce::File songs()   const { return exports().getChildFile ("Songs"); }
        };

        static bool writeNoise (const juce::File& file, juce::int64 frames, int channels, int seed)
        {
            file.deleteFile();
            std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
            if (stream == nullptr) return false;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                juce::WavAudioFormat().createWriterFor (stream.get(), 48000.0, (unsigned) channels, 32, {}, 0));
            if (writer == nullptr) return false;
            stream.release();
            juce::Random random (seed);
            juce::AudioBuffer<float> block (channels, 4096);
            for (juce::int64 done = 0; done < frames;)
            {
                const int n = (int) juce::jmin<juce::int64> (4096, frames - done);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < n; ++i)
                        block.setSample (c, i, (random.nextFloat() * 2.0f - 1.0f) * 0.25f);
                if (! writer->writeFromAudioSampleBuffer (block, 0, n)) return false;
                done += n;
            }
            return true;
        }

        static void setup (AudioEngine& e, int strips)
        {
            e.clearAllStripOverrides();
            e.setStripCount (strips);
            e.prepareForTests (48000, 256);
            e.getMasterState().muted.store (false);
            e.getMasterState().gainDb.store (0.0f);
            for (int i = 0; i < strips; ++i)
            {
                auto& t = e.getRecorder().getTrack (i);
                t.gainDb.store (0.0f); t.pan.store (0.0f); t.muted.store (false); t.soloed.store (false);
                t.isStereo.store (false); t.setNameThreadSafe ("Strip " + juce::String (i + 1));
                e.setTrackVcaGroup (i, -1);
            }
        }

        // A stereo pair, a mono strip with edits, and a cross-track clip:
        // everything a render reads.
        static void buildArrangement (AudioEngine& e, const Folder& f)
        {
            jassert (e.getRecorder().getNumTracks() >= 4);
            e.setTrackStereo (0, true);
            e.getRecorder().getTrack (0).setNameThreadSafe ("Pair");
            e.getRecorder().getTrack (0).pan.store (-1.0f);
            e.getRecorder().getTrack (1).pan.store (1.0f);
            e.getRecorder().getTrack (2).setNameThreadSafe ("Mono.1");
            e.loadSession (f.dir);

            auto& mono = e.clipsFor (2);
            Clip straddling;
            straddling.name = "Straddles";
            straddling.timelineStartSamples = 40000;
            straddling.fileStartSamples = 3000;
            straddling.fileLengthSamples = 120000;
            straddling.fadeInSamples = 5000;
            straddling.fadeOutSamples = 7000;
            straddling.fadeCurve = 1;
            straddling.gainDb = -4.0f;
            mono = { straddling };

            Clip cross;
            cross.audioFile = f.audio().getChildFile ("Track_01.wav");
            cross.sourceChannel = 1;
            cross.timelineStartSamples = 90000;
            cross.fileStartSamples = 100;
            cross.fileLengthSamples = 50000;
            e.clipsFor (3) = { cross };
        }

        static juce::AudioBuffer<float> whole (AudioEngine& e, int track, juce::int64 length)
        {
            juce::AudioBuffer<float> out (1, (int) length);
            out.clear();
            e.forEachArrangementWindow (track, 0, length, [&] (const float* data, juce::int64 at, int n)
            {
                juce::FloatVectorOperations::copy (out.getWritePointer (0) + (int) at, data, n);
                return true;
            });
            return out;
        }

        void expectSameSpan (const juce::AudioBuffer<float>& ranged, int channelR,
                             const juce::AudioBuffer<float>& full, int channelF,
                             juce::int64 start, const juce::String& what)
        {
            int wrong = 0;
            for (int i = 0; i < ranged.getNumSamples(); ++i)
                if (! juce::exactlyEqual (ranged.getSample (channelR, i), full.getSample (channelF, (int) start + i))) ++wrong;
            expectEquals (wrong, 0, what + ": samples differ from the whole-session render");
        }

        static std::unique_ptr<juce::AudioFormatReader> reader (const juce::File& file)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file));
        }

        static juce::Array<juce::File> filesIn (const juce::File& dir)
        {
            // Dot-files included: a leftover partial is exactly what to look for.
            juce::Array<juce::File> out;
            if (dir.isDirectory())
                dir.findChildFiles (out, juce::File::findFiles, true, "*");
            return out;
        }

        static sx::Choice choiceFor (const std::vector<sx::Range>& songs, bool mix, bool stems)
        {
            sx::Choice choice;
            choice.songs = songs;
            for (size_t i = 0; i < songs.size(); ++i) choice.numbers.push_back ((int) i + 1);
            choice.mix = mix;
            choice.stems = stems;
            choice.format.format = ExportFormat::Wav24;
            choice.format.bitsPerSample = 32;
            choice.format.sampleRate = 48000.0;
            return choice;
        }

        static Marker marker (juce::int64 sample, const juce::String& name)
        {
            Marker m;
            m.sampleOffset = sample;
            m.name = name;
            return m;
        }

        void runTest() override
        {
            // Before the first engine exists: without it the engine opens the
            // real audio device.
            AudioEngine::setTestModeSkipAudioInit (true);
            beginTest ("Ranges run from each marker to the next, the last to the session end");
            {
                expect (sx::rangesFromMarkers ({}, 1000).empty(), "no markers, no songs");
                expect (sx::rangesFromMarkers ({ marker (10, "A") }, 0).empty(), "an empty session has no songs");

                auto one = sx::rangesFromMarkers ({ marker (100, "Only") }, 1000);
                expectEquals ((int) one.size(), 1);
                expectEquals (one[0].start, (juce::int64) 100);
                expectEquals (one[0].end, (juce::int64) 1000);
                expectEquals (one[0].name, juce::String ("Only"));

                auto atZero = sx::rangesFromMarkers ({ marker (0, "Top"), marker (400, "Next") }, 1000);
                expectEquals ((int) atZero.size(), 2);
                expectEquals (atZero[0].start, (juce::int64) 0);
                expectEquals (atZero[0].end, (juce::int64) 400);
                expectEquals (atZero[1].start, (juce::int64) 400);
                expectEquals (atZero[1].end, (juce::int64) 1000);

                auto past = sx::rangesFromMarkers ({ marker (100, "In"), marker (1000, "At end"), marker (5000, "Past") }, 1000);
                expectEquals ((int) past.size(), 1, "a marker at or past the end starts nothing");
                expectEquals (past[0].end, (juce::int64) 1000);

                auto dupes = sx::rangesFromMarkers ({ marker (100, ""), marker (100, "Named"), marker (100, "Third"),
                                                      marker (300, "Later") }, 1000);
                expectEquals ((int) dupes.size(), 2, "markers on one spot are one song");
                expectEquals (dupes[0].name, juce::String ("Named"));
                expectEquals (dupes[0].end, (juce::int64) 300, "no zero-length song");

                auto unsorted = sx::rangesFromMarkers ({ marker (500, "B"), marker (100, "A") }, 1000);
                expectEquals (unsorted[0].name, juce::String ("A"));
                expectEquals (unsorted[0].end, (juce::int64) 500);

                auto blank = sx::rangesFromMarkers ({ marker (100, "  ") }, 1000);
                expectEquals (blank[0].name, juce::String ("Song 1"));
            }

            beginTest ("Song names become safe folder names");
            {
                expect (! sx::safeName ("a/b:c\\d").containsAnyOf ("/\\:"));
                expectEquals (sx::safeName ("Out 2.1"), juce::String ("Out 2_1"), "a dot would read as an extension");
                expectEquals (sx::safeName ("   "), juce::String ("Untitled"));
            }

            beginTest ("A range render is the same span of a whole render, sample for sample");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 200000, 2, 1));
                expect (writeNoise (f.audio().getChildFile ("Track_03.wav"), 200000, 1, 2));
                AudioEngine e; setup (e, 4);
                buildArrangement (e, f);
                const auto length = e.getArrangementLengthSamples();
                expect (length > 150000);

                juce::AudioBuffer<float> mix;
                expect (e.renderStereoMix (mix, length));
                expect (mix.getMagnitude (0, mix.getNumSamples()) > 0.0f);
                const auto mono = whole (e, 2, length);

                // Off the 64k window, and through the straddling clip's fades and body.
                const std::vector<std::pair<juce::int64, juce::int64>> spans
                    { { 0, 777 }, { 1, 512 }, { 511, 513 }, { 41000, 70001 }, { 65535, 65537 },
                      { 65536, 131072 }, { 70001, 131073 }, { 150001, length }, { 12345, length } };
                for (const auto& [start, end] : spans)
                {
                    const auto label = juce::String (start) + "-" + juce::String (end);
                    juce::AudioBuffer<float> ranged (2, (int) (end - start));
                    ranged.clear();
                    int peakWindow = 0;
                    juce::int64 reached = start;
                    expect (e.forEachStereoMixWindow (start, end,
                        [&] (const juce::AudioBuffer<float>& w, juce::int64 at, int n)
                        {
                            expectEquals (at, reached, "windows must arrive in timeline order");
                            reached += n;
                            peakWindow = juce::jmax (peakWindow, w.getNumSamples());
                            for (int c = 0; c < 2; ++c)
                                ranged.copyFrom (c, (int) (at - start), w, c, 0, n);
                            return true;
                        }));
                    expectEquals (reached, end, label + " stopped short");
                    expect (peakWindow <= 1 << 16, "a range render must stay windowed");
                    expectSameSpan (ranged, 0, mix, 0, start, "mix L " + label);
                    expectSameSpan (ranged, 1, mix, 1, start, "mix R " + label);

                    juce::AudioBuffer<float> stem (1, (int) (end - start));
                    expect (e.forEachArrangementWindow (2, start, end, [&] (const float* data, juce::int64 at, int n)
                    {
                        juce::FloatVectorOperations::copy (stem.getWritePointer (0) + (int) (at - start), data, n);
                        return true;
                    }));
                    expectSameSpan (stem, 0, mono, 0, start, "stem " + label);

                    juce::AudioBuffer<float> pair (2, (int) (end - start));
                    expect (e.forEachStereoPairWindow (0, start, end, [&] (const juce::AudioBuffer<float>& w, juce::int64 at, int n)
                    {
                        for (int c = 0; c < 2; ++c)
                            pair.copyFrom (c, (int) (at - start), w, c, 0, n);
                        return true;
                    }));
                    const auto left = whole (e, 0, length), right = whole (e, 1, length);
                    expectSameSpan (pair, 0, left, 0, start, "pair L " + label);
                    expectSameSpan (pair, 1, right, 0, start, "pair R " + label);
                }
                e.clearAllStripOverrides();
            }

            beginTest ("Songs written to disk hold the same samples, across a straddling clip");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 200000, 2, 1));
                expect (writeNoise (f.audio().getChildFile ("Track_03.wav"), 200000, 1, 2));
                AudioEngine e; setup (e, 4);
                buildArrangement (e, f);
                const auto length = e.getArrangementLengthSamples();

                juce::AudioBuffer<float> mix;
                expect (e.renderStereoMix (mix, length));
                const auto mono = whole (e, 2, length);

                // 100001 falls inside the clip on strip 3 (40000..160000).
                const std::vector<sx::Range> songs { { 40321, 100001, "First" }, { 100001, length, "Second" } };
                auto plan = sx::buildPlan (e, choiceFor (songs, true, true), f.exports());
                std::atomic<bool> cancel { false };
                const auto outcome = sx::run (e, plan, cancel, nullptr);
                expect (outcome.complete(), outcome.firstError);
                expectEquals (outcome.written, outcome.attempted);

                const auto second = f.songs().getChildFile ("02 - Second");
                expect (second.isDirectory(), "songs go in Export Files/Songs/NN - Name");
                const auto mixFile = reader (second.getChildFile ("02 - Second - Mix.wav"));
                const auto stemFile = reader (second.getChildFile ("Track_03 - Mono_1.wav"));
                const auto pairFile = reader (second.getChildFile ("Track_01 - Pair.wav"));
                expect (mixFile != nullptr && stemFile != nullptr && pairFile != nullptr);
                if (mixFile != nullptr && stemFile != nullptr && pairFile != nullptr)
                {
                    const int n = (int) (length - 100001);
                    expectEquals (mixFile->lengthInSamples, (juce::int64) n, "the boundary must be sample-accurate");
                    expectEquals ((int) mixFile->numChannels, 2);
                    expectEquals ((int) stemFile->numChannels, 1);
                    expectEquals ((int) pairFile->numChannels, 2, "a pair is one interleaved file");

                    juce::AudioBuffer<float> read (2, n);
                    expect (mixFile->read (&read, 0, n, 0, true, true));
                    expectSameSpan (read, 0, mix, 0, 100001, "mix file L");
                    expectSameSpan (read, 1, mix, 1, 100001, "mix file R");

                    juce::AudioBuffer<float> stemRead (1, n);
                    expect (stemFile->read (&stemRead, 0, n, 0, true, false));
                    expectSameSpan (stemRead, 0, mono, 0, 100001, "stem file");
                    expect (stemRead.getMagnitude (0, 0, 1000) > 0.0f, "the straddling clip must play from the boundary");
                }
                e.clearAllStripOverrides();
            }

            beginTest ("Stems honour the arrangement and skip strips silent in that song");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 100000, 1, 1));
                expect (writeNoise (f.audio().getChildFile ("Track_02.wav"), 100000, 1, 2));
                AudioEngine e; setup (e, 3);
                e.loadSession (f.dir);
                auto& first = e.clipsFor (0)[0];
                first.fileLengthSamples = 10000;
                auto& take = e.clipsFor (1)[0];
                take.timelineStartSamples = 50000;
                take.fileLengthSamples = 10000;
                take.muted = true;

                const std::vector<sx::Range> songs { { 0, 20000, "A" }, { 40000, 70000, "B" } };
                const auto plan = sx::buildPlan (e, choiceFor (songs, false, true), f.exports());
                expectEquals ((int) plan.songs[0].targets.size(), 1, "only strip 1 plays in song A");
                expectEquals ((int) plan.songs[1].targets.size(), 0, "a muted clip is not a stem");
                e.clearAllStripOverrides();
            }

            beginTest ("Stereo pair = one file: on is one interleaved stem, off is L and R");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 20000, 2, 1));
                AudioEngine e; setup (e, 2);
                e.setTrackStereo (0, true);
                e.getRecorder().getTrack (0).setNameThreadSafe ("OH");
                e.loadSession (f.dir);

                const std::vector<sx::Range> songs { { 0, 20000, "Song" } };
                auto choice = choiceFor (songs, false, true);
                const auto joined = sx::buildPlan (e, choice, f.exports());
                expectEquals ((int) joined.songs[0].targets.size(), 1);
                expect (joined.songs[0].targets[0].kind == sx::Target::Kind::StereoStem);

                choice.stereoOneFile = false;
                const auto apart = sx::buildPlan (e, choice, f.exports());
                expectEquals ((int) apart.songs[0].targets.size(), 2);
                std::atomic<bool> cancel { false };
                const auto outcome = sx::run (e, apart, cancel, nullptr);
                expect (outcome.complete(), outcome.firstError);
                const auto folder = f.songs().getChildFile ("01 - Song");
                expect (folder.getChildFile ("Track_01 - OH L.wav").existsAsFile());
                const auto right = reader (folder.getChildFile ("Track_01 - OH R.wav"));
                expect (right != nullptr);
                if (right != nullptr) expectEquals ((int) right->numChannels, 1);
                e.clearAllStripOverrides();
            }

            beginTest ("Formats match the export choices, in session rate and converted");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 48000, 1, 1));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                const std::vector<sx::Range> songs { { 0, 48000, "Song" } };
                std::atomic<bool> cancel { false };

                const auto runWith = [&] (ExportFormat format, int bits, double rate, const juce::String& folderName)
                {
                    auto choice = choiceFor (songs, true, false);
                    choice.format.format = format;
                    choice.format.bitsPerSample = bits;
                    choice.format.sampleRate = rate;
                    const auto outcome = sx::run (e, sx::buildPlan (e, choice, f.exports()), cancel, nullptr);
                    expect (outcome.complete(), outcome.firstError);
                    return f.songs().getChildFile (folderName);
                };

                auto folder = runWith (ExportFormat::Flac24, 24, 48000.0, "01 - Song");
                auto rd = reader (folder.getChildFile ("01 - Song - Mix.flac"));
                expect (rd != nullptr, "FLAC mix missing");
                if (rd != nullptr) { expectEquals ((int) rd->bitsPerSample, 24); expectEquals (rd->lengthInSamples, (juce::int64) 48000); }

                folder = runWith (ExportFormat::Aiff24, 16, 48000.0, "01 - Song (2)");
                rd = reader (folder.getChildFile ("01 - Song - Mix.aif"));
                expect (rd != nullptr, "a second run must not reuse the folder");
                if (rd != nullptr) expectEquals ((int) rd->bitsPerSample, 16);

                folder = runWith (ExportFormat::Wav24, 24, 44100.0, "01 - Song (3)");
                rd = reader (folder.getChildFile ("01 - Song - Mix.wav"));
                expect (rd != nullptr, "converted WAV missing");
                if (rd != nullptr)
                {
                    expectEquals (rd->sampleRate, 44100.0);
                    expectEquals (rd->lengthInSamples, (juce::int64) 44100);
                }
                e.clearAllStripOverrides();
            }

            beginTest ("An abandoned file stream leaves nothing, and a commit installs one file");
            {
                Folder f;
                TrackExporter exporter;
                ExportOptions options;
                options.sampleRate = 48000.0;
                juce::AudioBuffer<float> block (1, 256);
                block.clear();
                const auto stem = f.exports().getChildFile ("Out");
                {
                    ExportFileStream out (exporter, stem, options, 48000.0, 1);
                    expect (out.isOpen());
                    expect (out.write (block, 256));
                    expectEquals (filesIn (f.exports()).size(), 1, "a partial should be staged while writing");
                }
                expectEquals (filesIn (f.exports()).size(), 0, "an abandoned export left a partial");
                {
                    ExportFileStream out (exporter, stem, options, 48000.0, 1);
                    expect (out.write (block, 256));
                    juce::String error;
                    expect (out.commit (error), error);
                }
                const auto files = filesIn (f.exports());
                expectEquals (files.size(), 1);
                expectEquals (files[0].getFileName(), juce::String ("Out.wav"));
            }

            beginTest ("Cancelling leaves no truncated file, finished songs stand");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 48000 * 240, 1, 1));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                const juce::int64 end = 48000 * 240;
                const std::vector<sx::Range> songs { { 0, 2000, "Short" }, { 2000, end, "Long" } };
                const auto plan = sx::buildPlan (e, choiceFor (songs, true, false), f.exports());

                // Cancel while the long song's partial is open: the worker is
                // inside a file, with audio already written to it.
                std::atomic<bool> cancel { false };
                sx::Outcome outcome;
                std::thread worker ([&] { outcome = sx::run (e, plan, cancel, nullptr); });
                const auto longFolder = f.songs().getChildFile ("02 - Long");
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 20000.0;
                while (filesIn (longFolder).isEmpty() && juce::Time::getMillisecondCounterHiRes() < deadline)
                    std::this_thread::yield();
                expect (! filesIn (longFolder).isEmpty(), "never saw the partial");
                cancel.store (true);
                worker.join();

                expect (outcome.cancelled);
                expectEquals (outcome.written, 1, "the finished song counts");
                expect (! outcome.complete());
                expect (f.songs().getChildFile ("01 - Short/01 - Short - Mix.wav").existsAsFile());
                expect (filesIn (longFolder).isEmpty(), "a cancelled song left a file");
                expect (! longFolder.exists(), "a cancelled song left an empty folder");
                expect (sx::statusLine (outcome, "Songs").contains ("cancelled"));
                e.clearAllStripOverrides();
            }

            beginTest ("A failed file makes the batch INCOMPLETE, in the track export's words");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 20000, 1, 1));
                expect (writeNoise (f.audio().getChildFile ("Track_02.wav"), 20000, 1, 2));
                AudioEngine e; setup (e, 2);
                e.loadSession (f.dir);
                const std::vector<sx::Range> songs { { 0, 20000, "Song" } };
                const auto plan = sx::buildPlan (e, choiceFor (songs, false, true), f.exports());
                expectEquals (plan.totalFiles(), 2);
                f.audio().getChildFile ("Track_02.wav").deleteFile();

                std::atomic<bool> cancel { false };
                const auto outcome = sx::run (e, plan, cancel, nullptr);
                expectEquals (outcome.attempted, 2);
                expectEquals (outcome.written, 1);
                expect (! outcome.complete());
                const auto line = sx::statusLine (outcome, "Songs");
                expect (line.startsWith ("Export INCOMPLETE: 1 of 2 failed or missing"), line);
                expect (line.contains ("Track_02"), "the status must name what failed: " + line);

                sx::Outcome none;
                none.attempted = 2;
                expect (sx::statusLine (none, "Songs").startsWith ("Export failed"));
                sx::Outcome good;
                good.attempted = good.written = 3;
                expect (sx::statusLine (good, "Songs").startsWith ("Exported 3 file(s)"));
                e.clearAllStripOverrides();
            }

            beginTest ("A take refuses the render at the engine");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 20000, 1, 1));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                const std::vector<sx::Range> songs { { 0, 20000, "Song" } };
                const auto plan = sx::buildPlan (e, choiceFor (songs, true, false), f.exports());

                e.setExternalRecording (true);
                std::atomic<bool> cancel { false };
                const auto outcome = sx::run (e, plan, cancel, nullptr);
                e.setExternalRecording (false);
                expect (! outcome.complete());
                expectEquals (outcome.written, 0);
                expect (outcome.firstError.isNotEmpty());
                expect (filesIn (f.exports()).isEmpty(), "a refused run wrote a file");
                e.clearAllStripOverrides();
            }

            beginTest ("The dialog lists every range, ticked and named, and returns the edits");
            {
                const std::vector<sx::Range> songs { { 0, 48000, "One" }, { 48000, 96000, "Two" }, { 96000, 144000, "Three" } };
                ExportSongsContent content (songs, 48000.0, nullptr);
                auto choice = content.currentChoice();
                expectEquals ((int) choice.songs.size(), 3, "every row starts ticked");
                expect (choice.mix && choice.stems && choice.stereoOneFile);

                content.rows[1]->tick.setToggleState (false, juce::dontSendNotification);
                content.rows[2]->name.setText ("  Encore  ", false);
                choice = content.currentChoice();
                expectEquals ((int) choice.songs.size(), 2);
                expectEquals (choice.songs[1].name, juce::String ("Encore"));
                expectEquals (choice.numbers[1], 3, "NN stays the song's place in the set");

                content.rows[0]->name.setText ("", false);
                expectEquals (content.currentChoice().songs[0].name, juce::String ("One"), "a blank name falls back to the marker");

                content.contentBox.setSelectedId (1, juce::sendNotificationSync);
                expect (content.currentChoice().mix);
                expect (! content.currentChoice().stems);
                expect (! content.stereoOneFile.isEnabled(), "the tick means nothing without stems");

                content.setAll (false);
                expect (! content.exportButton.isEnabled(), "nothing ticked, nothing to export");
            }

            beginTest ("Menu item 967 starts the export, then cancels it, through the session job");
            {
                Folder f;
                expect (writeNoise (f.audio().getChildFile ("Track_01.wav"), 20000, 1, 1));
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                {
                    MainComponent host;
                    setup (host.engine, 1);
                    host.engine.loadSession (f.dir);
                    host.engine.setActiveSessionDir (f.dir);

                    const std::vector<sx::Range> songs { { 0, 20000, "Song" } };
                    host.startSongExport (choiceFor (songs, true, true));
                    expect (host.songExportRunning);
                    expect (host.sessionIoBusy.load());
                    expect (host.engine.isSessionTransitionActive());

                    host.lastMenuStateSig.clear();
                    host.refreshMenuStateIfChanged();
                    expect (host.lastMenuStateSig.endsWith ("|1"), "the running flag must be in the menu signature");
                    host.menuItemSelected (967, 0);
                    expect (pump ([&] { return ! host.sessionIoBusy.load(); }));
                    expect (! host.songExportRunning, "the flag must clear with the job");
                    expect (! host.engine.isSessionTransitionActive());
                    host.engine.clearAllStripOverrides();
                }
                MainComponent::s_testConstruct = false;
            }
        }

        static bool pump (const std::function<bool()>& done, int timeoutMs = 10000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false);
               #else
                juce::Thread::sleep (5);
               #endif
            }
            return done();
        }
    };

    static SongExportTests songExportTests;
}
