// Take files named for their strips (2026-11-16): "01 KiCK.wav" instead of
// "Track_01.wav". The number stays the identity; the name is a label fixed when
// the take starts. See Source/Audio/TakeNames.h.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/AudioImport.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/TakeNames.h"
#include "../Audio/TrackOrderLog.h"

namespace zynforge
{
    class TakeNameTests final : public juce::UnitTest
    {
    public:
        TakeNameTests() : juce::UnitTest ("Take file names", "zynforge") {}

        static juce::File freshDir (const juce::String& tag)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-takenames-" + tag + "-" + juce::Uuid().toString());
            dir.deleteRecursively();
            dir.createDirectory();
            return dir;
        }

        static bool writeDc (const juce::File& f, float value, int len, int channels = 1, double sr = 48000.0)
        {
            f.getParentDirectory().createDirectory();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            if (os == nullptr) return false;
            auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (sr)
                                                       .withNumChannels (channels)
                                                       .withBitsPerSample (24));
            if (writer == nullptr) return false;
            juce::AudioBuffer<float> buffer (channels, len);
            for (int c = 0; c < channels; ++c) juce::FloatVectorOperations::fill (buffer.getWritePointer (c), value, len);
            return writer->writeFromAudioSampleBuffer (buffer, 0, len);
        }

        // One recorder pass on `names.size()` mono tracks, every one armed.
        struct Pass
        {
            bool started { false };
            juce::String error;
        };

        static Pass record (const juce::File& dir, const juce::StringArray& names, int blocks,
                            float value = 0.25f, std::function<void (MultitrackRecorder&)> arm = {})
        {
            constexpr int block = 512;
            MultitrackRecorder rec;
            rec.prepare (48000.0, block, names.size());
            for (int i = 0; i < names.size(); ++i)
            {
                rec.getTrack (i).setNameThreadSafe (names[i]);
                rec.getTrack (i).armed.store (true, std::memory_order_relaxed);
            }
            if (arm) arm (rec);
            Pass pass;
            pass.started = rec.startRecording (dir);
            pass.error = rec.getStartError();
            if (! pass.started) return pass;
            std::vector<std::vector<float>> data ((size_t) names.size(), std::vector<float> ((size_t) block, value));
            std::vector<const float*> ptrs;
            for (auto& d : data) ptrs.push_back (d.data());
            for (int b = 0; b < blocks; ++b) rec.processBlock (ptrs.data(), names.size(), block);
            rec.stopRecording();
            return pass;
        }

        static juce::StringArray namesIn (const juce::File& dir)
        {
            juce::StringArray names;
            for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
                names.add (f.getFileName());
            names.sort (true);
            return names;
        }

        void runTest() override
        {
            using namespace takenames;

            beginTest ("A strip name becomes a file label a file system and a wildcard read literally");
            expectEquals (labelFor ("KiCK"), juce::String ("KiCK"));
            expectEquals (labelFor ("  VOX   1 "), juce::String ("VOX 1"));
            expectEquals (labelFor ("VOX 1, LEAD"), juce::String ("VOX 1 LEAD"));
            expectEquals (labelFor ("GTR 80's"), juce::String ("GTR 80s"));
            expectEquals (labelFor ("A/B:C\\D"), juce::String ("A B C D"));
            expectEquals (labelFor ("KICK_IN"), juce::String ("KICK IN"));
            expectEquals (labelFor ("Vox 1.2"), juce::String ("Vox 1 2"));
            expectEquals (labelFor ("[x]*?;#|<>"), juce::String ("x"));
            expectEquals (labelFor ("SN_part02"), juce::String ("SN part02"));
            expectEquals (labelFor (juce::String::fromUTF8 ("\xd8\xb7\xd8\xa8\xd9\x84 1")),
                          juce::String::fromUTF8 ("\xd8\xb7\xd8\xa8\xd9\x84 1"));
            expectEquals (labelFor ("TAB\tNEW\nLINE"), juce::String ("TAB NEW LINE"));
            expect (labelFor ("").isEmpty());
            expect (labelFor ("12").isEmpty(), "a name of only digits says nothing the number does not");
            expect (labelFor (" 1 2 ").isEmpty());
            expect (labelFor ("...___").isEmpty());
            {
                juce::String accents;
                for (int i = 0; i < 40; ++i) accents << juce::String::fromUTF8 ("\xc3\xa9");   // 2 bytes each
                const auto cut = labelFor (accents);
                expectEquals ((int) cut.getNumBytesAsUTF8(), kMaxLabelBytes);
                expectEquals (cut.length(), kMaxLabelBytes / 2);
            }

            beginTest ("A new take is NN Label, or Track_NN when the strip has no name of its own");
            expectEquals (freshStem (0, "KiCK"), juce::String ("01 KiCK"));
            expectEquals (freshStem (6, "7"), juce::String ("Track_07"));
            expectEquals (freshStem (6, ""), juce::String ("Track_07"));
            expectEquals (freshStem (99, "Lead"), juce::String ("100 Lead"));
            for (const auto* name : { "KiCK", "VOX 1, LEAD", "a_b.c", "GTR 80's", "  x  ", "[*]", "MIX LR" })
            {
                const auto stem = freshStem (4, name);
                expect (isStem (stem), stem);
                expectEquals (parse (stem + ".wav").number, 5, stem);
                expect (! labelFor (name).containsAnyOf ("*?[]\\,;'\"_."), "glob-safe label: " + stem);
            }

            beginTest ("A file name reads back to its track; anything else is not a take");
            {
                const auto named = parse ("01 KiCK.wav");
                expectEquals (named.number, 1);
                expectEquals (named.stem, juce::String ("01 KiCK"));
                expectEquals (named.rest, juce::String (".wav"));
                expect (isMain (named));
                expectEquals (parse ("Track_07_part02.wav").number, 7);
                expectEquals (partNumber (parse ("07 KiCK_part02.wav")), 2);
                expect (isTakeAudio (parse ("07 KiCK_part02.flac")));
                expect (! isTakeAudio (parse ("07 KiCK.punchbase.wav")), "a punch sidecar is not the take");
                expect (! isTakeAudio (parse ("07 KiCK_consolidated_1.wav")), "a render is not the take");
                expectEquals (parse ("07 KiCK_consolidated_1.wav").number, 7);
                expectEquals (parse ("1 intro.wav").number, 0);
                expectEquals (parse ("001 X.wav").number, 0);
                expectEquals (parse ("07 Vox, lead.wav").number, 0);
                expectEquals (parse ("07  KiCK.wav").number, 0);
                expectEquals (parse ("._07 KiCK.wav").number, 0);
                expectEquals (parse ("Track_0123.wav").number, 0);
                expectEquals (parse ("Track_00.wav").number, 0);
                expectEquals (parse ("Track_01 - KiCK.wav").number, 0, "an export file is not a take");
                expectEquals (mainTakeTrackNumber (juce::File ("/x/07 KiCK.wav")), 7);
                expectEquals (mainTakeTrackNumber (juce::File ("/x/07 KiCK_part02.wav")), 0);
            }

            beginTest ("One listing gives each track its stem; a sidecar alone still claims the number");
            {
                const auto dir = freshDir ("index");
                expect (writeDc (dir.getChildFile ("Track_01.wav"), 0.1f, 64));
                expect (writeDc (dir.getChildFile ("02 SNARE.wav"), 0.1f, 64));
                expect (writeDc (dir.getChildFile ("02 SNARE_part02.wav"), 0.1f, 64));
                expect (writeDc (dir.getChildFile ("03 HAT.punchbase.wav"), 0.1f, 64));
                expect (writeDc (dir.getChildFile ("readme.wav"), 0.1f, 64));
                const StemIndex index (dir);
                expectEquals (index.stem (0), juce::String ("Track_01"));
                expectEquals (index.stem (1), juce::String ("02 SNARE"));
                expectEquals (index.stem (2), juce::String ("03 HAT"));
                expect (index.stem (3).isEmpty());
                expectEquals (index.stemOrLegacy (3), juce::String ("Track_04"));
                expectEquals (findTrackMedia (dir, 1).getFileName(), juce::String ("02 SNARE.wav"));
                expect (! findTrackMedia (dir, 2).exists(), "a sidecar is not the take");
                expectEquals (takeAudioFiles (dir).size(), 3);
                expectEquals (renumbered ("03 SNARE", 0), juce::String ("01 SNARE"));
                expectEquals (renumbered ("Track_03", 9), juce::String ("Track_10"));
                dir.deleteRecursively();
            }

            beginTest ("A named strip records NN Label.wav; an unnamed one keeps Track_NN.wav");
            {
                const auto dir = freshDir ("record");
                const auto pass = record (dir, { "KiCK", "2", "VOX 1, LEAD" }, 20);
                expect (pass.started, pass.error);
                const auto audio = dir.getChildFile ("Audio Files");
                expect (audio.getChildFile ("01 KiCK.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (audio.getChildFile ("Track_02.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (audio.getChildFile ("03 VOX 1 LEAD.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (! audio.getChildFile ("Track_01.wav").exists());

                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (audio.getChildFile ("01 KiCK.wav")));
                expect (reader != nullptr);
                if (reader != nullptr)
                    expect (reader->metadataValues[juce::WavAudioFormat::bwavDescription].startsWith ("sTRK1=KiCK"),
                            "the strip name still rides in the file as well");

                const auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
                const auto* tracks = report.getProperty ("tracks", {}).getArray();
                expect (tracks != nullptr && tracks->size() == 3);
                if (tracks != nullptr && tracks->size() == 3)
                {
                    expectEquals ((*tracks)[0]["files"][0].toString(), juce::String ("01 KiCK.wav"));
                    expectEquals ((*tracks)[1]["files"][0].toString(), juce::String ("Track_02.wav"));
                }

                beginTest ("The session plays its named takes on their own strips");
                {
                    AudioEngine eng;
                    expectEquals (eng.loadSession (dir), 3);
                    for (int t = 0; t < 3; ++t)
                        expectEquals (eng.getPlayer().getTrackLengthSamples (t), (juce::int64) 20 * 512);
                    expectEquals (qc::sessionTakes (dir).size(), 3);
                    expect (eng.getTrackAudioFile (2).getFileName() == "03 VOX 1 LEAD.wav");
                }

                beginTest ("A continue after a rename keeps the take's name");
                {
                    const auto more = record (dir, { "KICK IN", "BASS", "VOX" }, 10, 0.5f,
                                              [] (MultitrackRecorder& rec) { rec.armContinue (20 * 512); });
                    expect (more.started, more.error);
                    expect (audio.getChildFile ("01 KiCK_part02.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                    expect (audio.getChildFile ("Track_02_part02.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                    expect (! audio.getChildFile ("01 KICK IN.wav").exists(), "a rename does not start a second file");
                    expect (! audio.getChildFile ("02 BASS.wav").exists());
                    expect (MultitrackRecorder::takeIsMultiPart (dir, 0));
                    AudioEngine eng;
                    expectEquals (eng.loadSession (dir), 3);
                    expectEquals (eng.getPlayer().getTrackLengthSamples (0), (juce::int64) 30 * 512);
                }

                beginTest ("A fresh take is refused on a strip whose named take exists, whatever its name now");
                {
                    const auto before = namesIn (audio);
                    const auto refused = record (dir, { "SOMETHING ELSE", "2", "VOX" }, 4);
                    expect (! refused.started);
                    expect (refused.error.contains ("already has a take"), refused.error);
                    expect (namesIn (audio) == before, "nothing was written or replaced");
                }
                dir.deleteRecursively();
            }

            beginTest ("A punch on a named take splices back into the same name");
            {
                const auto dir = freshDir ("punch");
                expect (record (dir, { "SNARE" }, 40, 0.25f).started);
                const auto punched = record (dir, { "SNARE TOP" }, 10, -0.5f,
                                             [] (MultitrackRecorder& rec) { rec.armPunchIn (10 * 512); });
                expect (punched.started, punched.error);
                const auto audio = dir.getChildFile ("Audio Files");
                expect (audio.getChildFile ("01 SNARE.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (! audio.getChildFile ("01 SNARE TOP.wav").exists());
                for (const auto& name : namesIn (audio))
                    expect (! name.contains ("punch"), "nothing of the punch is left beside the take: " + name);
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                if (auto r = ConcatReader::create (fm, findTakeParts (audio.getChildFile ("01 SNARE.wav"))))
                {
                    expectEquals (r->lengthInSamples, (juce::int64) 40 * 512);
                    juce::AudioBuffer<float> one (1, 1);
                    r->read (&one, 0, 1, 15 * 512, true, false);
                    expectWithinAbsoluteError (one.getSample (0, 0), -0.5f, 0.01f);
                    r->read (&one, 0, 1, 30 * 512, true, false);
                    expectWithinAbsoluteError (one.getSample (0, 0), 0.25f, 0.01f);
                }
                else expect (false, "the punched take opens");
                dir.deleteRecursively();
            }

            beginTest ("A session from before names continues as Track_NN");
            {
                const auto dir = freshDir ("legacy");
                const auto audio = dir.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("Track_01.wav"), 0.1f, 4 * 512));
                const auto more = record (dir, { "KiCK" }, 4, 0.2f,
                                          [] (MultitrackRecorder& rec) { rec.armContinue (4 * 512); });
                expect (more.started, more.error);
                expect (audio.getChildFile ("Track_01_part02.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (! audio.getChildFile ("01 KiCK.wav").exists(), "one stem per strip in a folder");
                dir.deleteRecursively();
            }

            beginTest ("A strip move renumbers named files and keeps their names");
            {
                const auto dir = freshDir ("move");
                expect (writeDc (dir.getChildFile ("01 KiCK.wav"), 0.1f, 64));
                expect (writeDc (dir.getChildFile ("02 SNARE.wav"), 0.1f, 64));
                expect (writeDc (dir.getChildFile ("02 SNARE_part02.wav"), 0.1f, 64));
                expect (writeDc (dir.getChildFile ("Track_03.wav"), 0.1f, 64));
                const auto moves = trackorder::takeMoves (dir, dir.getChildFile ("Removed"), { 1, 0, -1 });
                std::map<juce::String, juce::String> byName;
                for (const auto& m : moves) byName[m.first.getFileName()] = m.second.getRelativePathFrom (dir);
                expectEquals ((int) moves.size(), 4);
                expectEquals (byName["01 KiCK.wav"], juce::String ("02 KiCK.wav"));
                expectEquals (byName["02 SNARE.wav"], juce::String ("01 SNARE.wav"));
                expectEquals (byName["02 SNARE_part02.wav"], juce::String ("01 SNARE_part02.wav"));
                expectEquals (byName["Track_03.wav"], juce::String ("Removed/Track_03.wav"));
                dir.deleteRecursively();
            }

            beginTest ("A lead-in owed to a named take is finished after a crash");
            {
                const auto dir = freshDir ("leadin");
                const auto audio = dir.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("05 KiCK_part02.wav"), 0.3f, 512));
                auto* m = new juce::DynamicObject();
                m->setProperty ("track", "05 KiCK");   m->setProperty ("ext", ".wav");
                m->setProperty ("firstPart", 1);        m->setProperty ("parts", 1);
                m->setProperty ("container", 0);        m->setProperty ("bitDepth", 24);
                m->setProperty ("channels", 1);         m->setProperty ("sampleRate", 48000.0);
                m->setProperty ("priorLength", 0);      m->setProperty ("timelineStart", 1000);
                const auto marker = audio.getChildFile ("Pending Lead-ins").getChildFile ("05 KiCK.json");
                marker.getParentDirectory().createDirectory();
                expect (marker.replaceWithText (juce::JSON::toString (juce::var (m))));
                expect (MultitrackRecorder::describeOwedLeadIns (audio).contains ("5"),
                        MultitrackRecorder::describeOwedLeadIns (audio));
                expect (MultitrackRecorder::recoverPendingLeadIns (audio));
                expect (! marker.exists(), "the marker is done with");
                expect (! marker.withFileExtension ("unreadable").exists(), "a named marker is not set aside as unreadable");
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> silence (fm.createReaderFor (audio.getChildFile ("05 KiCK.wav")));
                expect (silence != nullptr && silence->lengthInSamples == 1000);
                dir.deleteRecursively();
            }

            beginTest ("An imported file lands as NN Name.wav");
            {
                const auto dir = freshDir ("import");
                const auto source = dir.getChildFile ("Intro, live.wav");
                expect (writeDc (source, 0.2f, 1024));
                const auto audio = dir.getChildFile ("Session").getChildFile ("Audio Files");
                audio.createDirectory();
                const auto result = audioimport::importFiles ({ source }, audio, 4, 48000.0);
                expectEquals ((int) result.tracks.size(), 1);
                if (! result.tracks.empty())
                {
                    expectEquals (result.tracks[0].file.getFileName(), juce::String ("05 Intro live.wav"));
                    expect (result.tracks[0].file.existsAsFile());
                    expectEquals (result.tracks[0].name, juce::String ("Intro, live"));
                }
                dir.deleteRecursively();
            }
        }
    };

    static TakeNameTests takeNameTests;
}
