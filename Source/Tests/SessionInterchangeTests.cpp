// The DAW handoff: the interchange model, the .RPP and .dawproject writers,
// and the job paths that run them.
//
// With ZF_EXPORT_FIXTURE_DIR set, the last test also leaves a whole session on
// disk: its Reaper project, DAWproject file and the engine's own mix. REAPER
// and xmllint are not part of this suite; tools/verify_reaper_export.py feeds
// them that session.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Audio/ProjectExport.h"
#include "../Audio/ShowHandoff.h"
#include "../UI/MainComponent.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    namespace ix = interchange;
    namespace px = projectexport;

    class SessionInterchangeTests final : public juce::UnitTest
    {
    public:
        SessionInterchangeTests() : juce::UnitTest ("Session interchange", "zynforge") {}

        struct Folder
        {
            juce::File dir { juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zf-interchange-" + juce::Uuid().toString()).getChildFile ("Show") };
            Folder() { audio().createDirectory(); exports().createDirectory(); }
            ~Folder() { dir.getParentDirectory().deleteRecursively(); }
            juce::File audio()   const { return dir.getChildFile ("Audio Files"); }
            juce::File exports() const { return dir.getChildFile ("Export Files"); }
        };

        // Deterministic noise, different per file, so a clip played from the
        // wrong place cannot pass by accident.
        static bool writeAudio (const juce::File& file, juce::int64 frames, int channels, int seed,
                                float level = 0.25f, int bits = 24)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
            if (stream == nullptr) return false;
            std::unique_ptr<juce::AudioFormatWriter> writer;
            const auto rate = 48000.0;
            if (file.hasFileExtension ("flac"))
                writer.reset (juce::FlacAudioFormat().createWriterFor (stream.get(), rate, (unsigned) channels, bits, {}, 0));
            else if (file.hasFileExtension ("aif;aiff"))
                writer.reset (bits == 32
                    ? FloatAiffWriter::create (stream.get(), rate, (unsigned) channels)
                    : juce::AiffAudioFormat().createWriterFor (stream.get(), rate, (unsigned) channels, bits, {}, 0));
            else
                writer.reset (juce::WavAudioFormat().createWriterFor (stream.get(), rate, (unsigned) channels, bits, {}, 0));
            if (writer == nullptr) return false;
            stream.release();

            juce::Random random (seed);
            juce::AudioBuffer<float> block (channels, 4096);
            for (juce::int64 done = 0; done < frames;)
            {
                const int n = (int) juce::jmin<juce::int64> (4096, frames - done);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < n; ++i)
                        block.setSample (c, i, (random.nextFloat() * 2.0f - 1.0f) * level);
                if (! writer->writeFromAudioSampleBuffer (block, 0, n)) return false;
                done += n;
            }
            return flushFloatAiffBeforeClose (writer.get());
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
                t.colourARGB.store (0);
                e.setTrackVcaGroup (i, -1);
            }
        }

        static ix::Project resolved (AudioEngine& e, juce::String* error = nullptr)
        {
            std::atomic<bool> cancel { false };
            auto r = ix::resolve (e.captureInterchange(), cancel);
            if (error != nullptr) *error = r.error;
            return r.project;
        }

        static juce::String rppFor (const ix::Project& project, const juce::File& dest,
                                    const juce::StringArray& notes = {})
        {
            return reaperproject::build (project, dest, notes, 1700000000);
        }

        static ix::StripInput mono (const juce::String& name, std::vector<Clip> clips)
        {
            ix::StripInput strip;
            strip.name = name;
            strip.clips = std::move (clips);
            return strip;
        }

        static Clip clipOf (const juce::File& file, juce::int64 at, juce::int64 offset, juce::int64 length)
        {
            Clip c;
            c.name = "clip";
            c.audioFile = file;
            c.timelineStartSamples = at;
            c.fileStartSamples = offset;
            c.fileLengthSamples = length;
            return c;
        }

        void runTest() override
        {
            // Before the first engine exists: without it the engine opens the
            // real audio device.
            AudioEngine::setTestModeSkipAudioInit (true);
            std::atomic<bool> noCancel { false };

            beginTest ("A stereo pair is one track reading one interleaved file");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 2, 1));
                AudioEngine e; setup (e, 2);
                e.setTrackStereo (0, true);
                e.getRecorder().getTrack (0).setNameThreadSafe ("Overheads");
                e.getRecorder().getTrack (1).setNameThreadSafe ("Overheads R");
                e.getRecorder().getTrack (0).pan.store (-1.0f);
                e.getRecorder().getTrack (1).pan.store (1.0f);
                e.loadSession (f.dir);

                juce::String error;
                const auto project = resolved (e, &error);
                expect (error.isEmpty(), error);
                expectEquals ((int) project.tracks.size(), 1, "the R half became its own track");
                if (project.tracks.size() == 1)
                {
                    const auto& track = project.tracks[0];
                    expectEquals (track.name, juce::String ("Overheads"));
                    expectEquals (track.channels, 2);
                    expectEquals ((int) track.items.size(), 1);
                    if (! track.items.empty())
                    {
                        expectEquals (track.items[0].channel, -1, "a stereo item plays the whole file");
                        expectEquals (track.items[0].fileChannels, 2);
                    }
                    const auto rpp = rppFor (project, f.exports().getChildFile ("Show.RPP"));
                    expect (rpp.contains ("PANMODE 6"), "a stereo track pans each side");
                    expect (rpp.contains ("-1.000000 1.000000"), "side pans missing from VOLPAN");
                    expect (! rpp.contains ("CHANMODE"), "a stereo item must not pick a side");
                }
            }

            beginTest ("Pair halves edited apart stay two tracks rather than lose a side");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 2, 1));
                AudioEngine e; setup (e, 2);
                e.setTrackStereo (0, true);
                e.loadSession (f.dir);
                e.clipsFor (1)[0].fileLengthSamples = 2400;

                const auto project = resolved (e);
                expectEquals ((int) project.tracks.size(), 2);
                if (project.tracks.size() == 2)
                {
                    expect (project.tracks[0].name.endsWith (" L"));
                    expect (project.tracks[1].name.endsWith (" R"));
                    expectEquals (project.tracks[1].items[0].channel, 1, "the right side must stay the right side");
                    expect (rppFor (project, f.exports().getChildFile ("Show.RPP")).contains ("CHANMODE 4"));
                }
                expect (project.notes.joinIntoString (" ").contains ("stereo pair"));
            }

            beginTest ("A legacy pair of mono files keeps both files");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                expect (writeAudio (f.audio().getChildFile ("Track_02.wav"), 4800, 1, 2));
                AudioEngine e; setup (e, 2);
                e.setTrackStereo (0, true);
                e.loadSession (f.dir);
                const auto project = resolved (e);
                expectEquals ((int) project.tracks.size(), 2);
                if (project.tracks.size() == 2)
                    expect (project.tracks[1].items[0].file.getFileName() == "Track_02.wav");
            }

            beginTest ("A take in parts becomes consecutive clips, one per file");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                expect (writeAudio (f.audio().getChildFile ("Track_01_part02.wav"), 2400, 1, 2));
                expect (writeAudio (f.audio().getChildFile ("Track_01_part03.wav"), 2400, 1, 3));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                auto& clip = e.clipsFor (0)[0];
                clip.fileStartSamples = 1000;
                clip.fileLengthSamples = 6000;      // 3800 + 2200: stops inside part 2
                clip.timelineStartSamples = 500;

                const auto project = resolved (e);
                expectEquals ((int) project.tracks.size(), 1);
                const auto& items = project.tracks[0].items;
                expectEquals ((int) items.size(), 2, "a clip across a part boundary is two items");
                if (items.size() == 2)
                {
                    expect (items[0].file.getFileName() == "Track_01.wav");
                    expectEquals (items[0].position, (juce::int64) 500);
                    expectEquals (items[0].sourceOffset, (juce::int64) 1000);
                    expectEquals (items[0].length, (juce::int64) 3800);
                    expect (items[1].file.getFileName() == "Track_01_part02.wav");
                    expectEquals (items[1].position, (juce::int64) 4300, "the second piece must follow on");
                    expectEquals (items[1].sourceOffset, (juce::int64) 0);
                    expectEquals (items[1].length, (juce::int64) 2200);
                }
            }

            beginTest ("A cross-track clip reads its own file and channel");
            {
                Folder f;
                const auto other = f.audio().getChildFile ("Track_02.wav");
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                expect (writeAudio (other, 4800, 2, 2));
                AudioEngine e; setup (e, 3);
                e.loadSession (f.dir);
                auto pasted = clipOf (other, 2000, 100, 1000);
                pasted.sourceChannel = 1;
                e.clipsFor (0).push_back (pasted);

                const auto project = resolved (e);
                const auto& items = project.tracks[0].items;
                expectEquals ((int) items.size(), 2);
                if (items.size() == 2)
                {
                    expect (items[1].file == other);
                    expectEquals (items[1].channel, 1);
                    const auto rpp = rppFor (project, f.exports().getChildFile ("Show.RPP"));
                    expect (rpp.contains ("CHANMODE 4"), "REAPER must play the right side");
                }
                expect (px::notesFor (project, px::Format::DawProject).joinIntoString (" ").contains ("cannot pick one channel"),
                        "DAWproject has no channel selection and must say so");
            }

            beginTest ("Only the active comp take is exported");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                expectEquals (e.newTakeFromCurrent (0, "Take 2"), 1);
                e.setActiveTake (0, 1);
                expect (e.splitTrackAtSample (0, 2400));

                expectEquals ((int) resolved (e).tracks[0].items.size(), 2, "the active take has two clips");
                e.setActiveTake (0, 0);
                expectEquals ((int) resolved (e).tracks[0].items.size(), 1, "switching take must change the export");
            }

            beginTest ("A bus plays no media, even after a stereo take");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 2, 1));
                ix::SessionInput in;
                in.sessionDir = f.dir;
                in.strips.push_back (ix::StripInput());
                ix::StripInput bus;
                bus.isBus = true;
                in.strips.push_back (bus);
                const auto r = ix::resolve (in, noCancel);
                expect (r.ok(), r.error);
                expectEquals ((int) r.project.tracks.size(), 2);
                expect (r.project.tracks[1].items.empty(), "the bus picked up the take's right channel");
            }

            beginTest ("Deleting every clip stays silent; a strip never edited plays its file");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                ix::SessionInput in;
                in.sessionDir = f.dir;
                in.strips.push_back (mono ("Emptied", {}));
                ix::StripInput fresh;
                fresh.name = "Fresh";
                in.strips.push_back (fresh);
                expect (writeAudio (f.audio().getChildFile ("Track_02.wav"), 4800, 1, 2));
                const auto r = ix::resolve (in, noCancel);
                expect (r.ok(), r.error);
                expect (r.project.tracks[0].items.empty(), "an empty list is an edit, not a default");
                expectEquals ((int) r.project.tracks[1].items.size(), 1);
            }

            beginTest ("Fades: length and shape reach the writers, ordinary and split alike");
            {
                Folder f;
                const auto file = f.audio().getChildFile ("Track_01.wav");
                expect (writeAudio (file, 48000, 1, 1));
                auto ordinary = clipOf (file, 0, 0, 24000);
                ordinary.fadeInSamples = 4800;
                ordinary.fadeOutSamples = 2400;

                auto equal = clipOf (file, 24000, 24000, 24000);
                equal.fadeInSamples = 2400;
                equal.fadeCurve = 1;

                // A split inside the fade: 0..1000 of a 4000-sample ramp, then the rest.
                auto whole = clipOf (file, 0, 0, 10000);
                whole.fadeInSamples = 4000;
                whole.fadeOutSamples = 2000;
                const auto left  = clipSlice (whole, 0, 1000);
                const auto right = clipSlice (whole, 1000, 9000);
                expect (left.fadeInRampSamples == 4000 && right.fadeInRampOffsetSamples == 1000);

                ix::SessionInput in;
                in.sessionDir = f.dir;
                in.strips.push_back (mono ("Fades", { ordinary, equal }));
                in.strips.push_back (mono ("Split", { left, right }));
                const auto r = ix::resolve (in, noCancel);
                expect (r.ok(), r.error);

                const auto& a = r.project.tracks[0].items;
                expectEquals (a[0].fadeIn, (juce::int64) 4800);
                expectEquals (a[0].fadeOut, (juce::int64) 2400);
                const auto& b = r.project.tracks[1].items;
                expectEquals (b[0].fadeIn, (juce::int64) 1000, "the piece at silence keeps a fade over its own span");
                expectEquals (b[1].fadeIn, (juce::int64) 0, "a piece starting up the ramp must not dip to silence");
                expectEquals (b[1].fadeOut, (juce::int64) 2000, "the far end of the clip is an ordinary fade");
                expect (r.project.notes.joinIntoString (" ").contains ("fades cut by a split"), "the approximation must be stated");

                const auto rpp = rppFor (r.project, f.exports().getChildFile ("Show.RPP"));
                expect (rpp.contains ("FADEIN 0 0.10000000000000 0 0 0 0 0"), "4800 samples linear");
                expect (rpp.contains ("FADEOUT 0 0.05000000000000 0 0 0 0 0"));
                expect (rpp.contains ("FADEIN 7 0.05000000000000 0 7 0 0 0"), "equal power is shape 7");

                const auto xml = dawproject::buildProjectXml (r.project, f.exports().getChildFile ("Show.dawproject"), "1");
                expect (xml.contains ("fadeInTime=\"0.1"), xml.substring (0, 400));
                expect (xml.contains ("fadeOutTime=\"0.05"));
                expect (px::notesFor (r.project, px::Format::DawProject).joinIntoString (" ").contains ("no fade shape"));
                expect (! px::notesFor (r.project, px::Format::Reaper).joinIntoString (" ").contains ("no fade shape"));
            }

            beginTest ("A fade across a part boundary keeps its start and end");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                expect (writeAudio (f.audio().getChildFile ("Track_01_part02.wav"), 4800, 1, 2));
                auto clip = clipOf ({}, 0, 0, 9600);
                clip.fadeOutSamples = 6000;     // 1200 in part one, 4800 in part two
                ix::SessionInput in;
                in.sessionDir = f.dir;
                in.strips.push_back (mono ("Parts", { clip }));
                const auto r = ix::resolve (in, noCancel);
                expect (r.ok(), r.error);
                const auto& items = r.project.tracks[0].items;
                expectEquals ((int) items.size(), 2);
                expectEquals (items[0].fadeOut, (juce::int64) 0, "the earlier piece stays at level");
                expectEquals (items[1].fadeOut, (juce::int64) 4800, "the last piece reaches silence");
            }

            beginTest ("Clip gain and clip mute reach the item");
            {
                Folder f;
                const auto file = f.audio().getChildFile ("Track_01.wav");
                expect (writeAudio (file, 4800, 1, 1));
                auto clip = clipOf (file, 0, 0, 4800);
                clip.gainDb = -6.0f;
                clip.muted = true;
                ix::SessionInput in;
                in.sessionDir = f.dir;
                in.strips.push_back (mono ("Gain", { clip }));
                const auto r = ix::resolve (in, noCancel);
                expect (r.ok(), r.error);

                const auto dest = f.exports().getChildFile ("Show.RPP");
                const auto rpp = rppFor (r.project, dest);
                expect (rpp.contains ("VOLPAN 0.5011872336 0 1 -1"), "item gain goes in the first VOLPAN field");
                expect (rpp.contains ("MUTE 1 0"));
                const auto xml = dawproject::buildProjectXml (r.project, f.exports().getChildFile ("Show.dawproject"), "1");
                expect (xml.contains ("enable=\"false\""));
                expect (px::notesFor (r.project, px::Format::DawProject).joinIntoString (" ").contains ("no clip gain"),
                        "DAWproject has no clip gain field and must say so");
            }

            beginTest ("Track fader folds the VCA in; floors and mutes stay off");
            {
                ix::StripInput strip;
                strip.faderDb = -6.0f;
                strip.vcaAssigned = true;
                strip.vcaDb = 6.0f;
                ix::SessionInput in;
                in.strips.push_back (strip);
                strip.faderDb = -60.0f;
                strip.pan = 0.5f;
                in.strips.push_back (strip);
                strip.faderDb = 0.0f;
                strip.vcaMuted = true;
                in.strips.push_back (strip);
                const auto r = ix::resolve (in, noCancel);
                expect (r.ok(), r.error);
                expectWithinAbsoluteError (r.project.tracks[0].gain, 1.0, 1.0e-6);
                expectEquals (r.project.tracks[1].gain, 0.0, "a fader at its floor is off whatever the VCA says");
                expect (r.project.tracks[2].muted, "a muted VCA mutes the track");
                expect (r.project.notes.joinIntoString (" ").contains ("VCA"));

                const auto rpp = rppFor (r.project, juce::File ("/tmp/Show.RPP"));
                expect (rpp.contains ("PANMODE 3"), "a mono track is in balance mode");
                expect (rpp.contains (juce::String (" ") + reaperproject::kConstantPowerLaw + " "),
                        "the pan law must match the session's");
            }

            beginTest ("Automation, sends and tempo changes are left out, and the summary says so");
            {
                ix::SessionInput in;
                in.hasTempoMap = true;
                in.anySolo = true;
                ix::StripInput strip;
                strip.automationLanes = 2;
                strip.hasSends = true;
                in.strips.push_back (strip);
                const auto r = ix::resolve (in, noCancel);
                const auto notes = r.project.notes.joinIntoString (" ");
                expect (notes.contains ("Automation is not exported (2 lanes)"));
                expect (notes.contains ("Aux sends"));
                expect (notes.contains ("Tempo changes"));
                expect (notes.contains ("Solo"));
            }

            beginTest ("Names survive quotes, ampersands, non-ASCII and edge spaces, in both formats");
            {
                using reaperproject::quoted;
                expectEquals (quoted ("Kick"), juce::String ("\"Kick\""));
                expectEquals (quoted ("  spaced  "), juce::String ("\"  spaced  \""), "edge spaces stay");
                expectEquals (quoted ("say \"hi\""), juce::String ("'say \"hi\"'"));
                expectEquals (quoted ("it's \"hi\""), juce::String ("`it's \"hi\"`"));
                expectEquals (quoted ("`it's \"hi\"`"), juce::String ("`'it's \"hi\"'`"),
                              "with all three marks only the backticks change");
                expectEquals (quoted (""), juce::String ("\"\""));

                const auto name = juce::String::fromUTF8 ("  Kick & \"Snare\" <1>\n>\n<TRACK \xc3\xa9\x01 ");
                ix::SessionInput in;
                in.sessionName = name;
                ix::StripInput strip;
                strip.name = name;
                in.strips.push_back (strip);
                in.markers.push_back ({ 48000, name });
                const auto r = ix::resolve (in, noCancel);
                expect (r.ok(), r.error);

                const auto rpp = rppFor (r.project, juce::File ("/tmp/Show.RPP"));
                expect (rpp.contains ("'  Kick & \"Snare\" <1> > <TRACK "), rpp.substring (0, 300));
                expectEquals (rpp.indexOf ("<TRACK\n"), rpp.lastIndexOf ("<TRACK\n"), "a name opened a second track");

                const auto xml = dawproject::buildProjectXml (r.project, juce::File ("/tmp/Show.dawproject"), "1");
                const auto parsed = juce::parseXML (xml);
                expect (parsed != nullptr, "not well-formed");
                const auto metadata = juce::parseXML (dawproject::buildMetadataXml (r.project.sessionName, {}));
                expect (metadata != nullptr, "metadata not well-formed");
                if (parsed != nullptr)
                {
                    auto* track = parsed->getChildByName ("Structure")->getChildElement (1);
                    const auto read = track->getStringAttribute ("name");
                    expect (read.startsWith ("  Kick & \"Snare\" <1>"), read);
                    expect (read.containsChar (0xe9), "non-ASCII lost");
                    expect (read.endsWith (" "), "edge space lost");
                }
                if (metadata != nullptr)
                    expect (metadata->getChildByName ("Title")->getAllSubText().containsChar (0xe9));
            }

            beginTest ("Audio paths are relative to the written file and use the right container");
            {
                Folder f;
                const auto flac = f.audio().getChildFile ("Track_01.flac");
                const auto aiff = f.audio().getChildFile ("Track_02.aif");
                const auto wav  = f.audio().getChildFile ("Track_03.wav");
                expect (writeAudio (flac, 4800, 1, 1));
                expect (writeAudio (aiff, 4800, 1, 2));
                expect (writeAudio (wav,  4800, 1, 3));
                AudioEngine e; setup (e, 3);
                e.loadSession (f.dir);
                const auto project = resolved (e);

                const auto dest = f.exports().getChildFile ("Show.RPP");
                const auto rpp = rppFor (project, dest);
                expect (rpp.contains ("<SOURCE FLAC\n        FILE \"../Audio Files/Track_01.flac\""), rpp);
                expect (rpp.contains ("<SOURCE WAVE\n        FILE \"../Audio Files/Track_02.aif\""));
                expect (rpp.contains ("<SOURCE WAVE\n        FILE \"../Audio Files/Track_03.wav\""));
                expect (! rpp.contains (f.dir.getFullPathName()), "an absolute path escaped into the project");

                const auto xml = dawproject::buildProjectXml (project, f.exports().getChildFile ("Show.dawproject"), "1");
                expect (xml.contains ("path=\"../Audio Files/Track_01.flac\""), xml);
                expect (xml.contains ("external=\"true\""));
                expect (! xml.contains (f.dir.getFullPathName()));
            }

            beginTest ("Missing media refuses the export and writes nothing");
            {
                Folder f;
                const auto first = f.audio().getChildFile ("Track_01.wav");
                expect (writeAudio (first, 4800, 1, 1));
                expect (writeAudio (f.audio().getChildFile ("Track_01_part02.wav"), 4800, 1, 2));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                const auto dest = f.exports().getChildFile ("Show.RPP");

                const auto input = e.captureInterchange();
                f.audio().getChildFile ("Track_01_part02.wav").deleteFile();
                expect (e.clipsFor (0)[0].fileLengthSamples == 9600);

                auto result = px::write (input, px::Format::Reaper, dest, "1", noCancel);
                expect (! result.ok, "a clip past the end of its media exported anyway");
                expect (result.error.contains ("runs past the end"), result.error);
                expect (! dest.exists(), "a refused export left a file");

                ix::SessionInput gone;
                gone.sessionDir = f.dir;
                gone.strips.push_back (mono ("Vox", { clipOf (f.audio().getChildFile ("Track_09.wav"), 0, 0, 100) }));
                for (auto format : { px::Format::Reaper, px::Format::DawProject })
                {
                    const auto file = px::uniqueDestination (f.exports(), "Show", format);
                    result = px::write (gone, format, file, "1", noCancel);
                    expect (! result.ok);
                    expect (result.error.contains ("Vox"), "the error must name the track");
                    expect (result.error.contains ("Track_09.wav"), "the error must name the file");
                    expect (! file.exists());
                }

                // A hole in the parts is missing media even when no clip reaches it.
                expect (writeAudio (f.audio().getChildFile ("Track_01_part03.wav"), 4800, 1, 3));
                ix::SessionInput holed;
                holed.sessionDir = f.dir;
                holed.strips.push_back (mono ("Holed", { clipOf ({}, 0, 0, 100) }));
                const auto r = ix::resolve (holed, noCancel);
                expect (! r.ok(), "a take with a missing middle part exported anyway");
                expect (r.error.contains ("Track_01_part02.wav"), r.error);
            }

            beginTest ("A cancelled resolve writes nothing");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                ix::SessionInput in;
                in.sessionDir = f.dir;
                in.strips.push_back (ix::StripInput());
                std::atomic<bool> cancelled { true };
                const auto dest = f.exports().getChildFile ("Show.RPP");
                expect (! px::write (in, px::Format::Reaper, dest, "1", cancelled).ok);
                expect (! dest.exists());
            }

            beginTest ("Exports get a unique name and never replace an earlier one");
            {
                Folder f;
                ix::SessionInput in;
                in.sessionDir = f.dir;
                in.sessionName = "Show";
                in.strips.push_back (ix::StripInput());
                for (auto format : { px::Format::Reaper, px::Format::DawProject })
                {
                    const auto first = px::uniqueDestination (f.exports(), "Show", format);
                    expect (px::write (in, format, first, "1", noCancel).ok);
                    const auto second = px::uniqueDestination (f.exports(), "Show", format);
                    expect (first != second, "a second export would replace the first");
                    expect (second.getFileName().startsWith ("Show (2)."), second.getFileName());
                    expect (px::write (in, format, second, "1", noCancel).ok);
                    expect (first.existsAsFile() && second.existsAsFile());
                }
                juce::Array<juce::File> leftovers;
                f.exports().findChildFiles (leftovers, juce::File::findFiles, false, "*.tmp");
                expectEquals (leftovers.size(), 0, "a staging file was left behind");
            }

            beginTest ("A .dawproject is a zip of the two documents, wired up as the format asks");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 48000, 1, 1));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                e.getRecorder().getTrack (0).colourARGB.store (0xffff8000);
                e.getMarkers().setContext (f.dir, 48000.0);
                e.getMarkers().drop (24000, "Verse");

                const auto dest = f.exports().getChildFile ("Show.dawproject");
                const auto result = px::write (e.captureInterchange(), px::Format::DawProject, dest, "1.2", noCancel);
                expect (result.ok, result.error);

                juce::ZipFile zip (dest);
                expectEquals (zip.getNumEntries(), 2);
                expect (zip.getEntry ("project.xml") != nullptr);
                expect (zip.getEntry ("metadata.xml") != nullptr);
                std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (*zip.getEntry ("project.xml")));
                expect (stream != nullptr);
                const auto root = stream != nullptr ? juce::parseXML (stream->readEntireStreamAsString()) : nullptr;
                expect (root != nullptr);
                if (root != nullptr)
                {
                    expect (root->hasTagName ("Project"));
                    expectEquals (root->getStringAttribute ("version"), juce::String ("1.0"));

                    // The schema's element order, which importers may rely on.
                    juce::StringArray order;
                    for (auto* child : root->getChildIterator()) order.add (child->getTagName());
                    expectEquals (order.joinIntoString (" "), juce::String ("Application Transport Structure Arrangement"));

                    auto* structure = root->getChildByName ("Structure");
                    expectEquals (structure->getNumChildElements(), 2, "master plus one track");
                    auto* master = structure->getChildElement (0)->getChildByName ("Channel");
                    expectEquals (master->getStringAttribute ("role"), juce::String ("master"));
                    auto* track = structure->getChildElement (1);
                    expectEquals (track->getStringAttribute ("color"), juce::String ("#ff8000"));
                    auto* channel = track->getChildByName ("Channel");
                    expectEquals (channel->getStringAttribute ("destination"), master->getStringAttribute ("id"),
                                  "a track must route to the master");

                    juce::StringArray controls;
                    for (auto* child : channel->getChildIterator()) controls.add (child->getTagName());
                    expectEquals (controls.joinIntoString (" "), juce::String ("Mute Pan Volume"));

                    auto* arrangement = root->getChildByName ("Arrangement");
                    auto* lanes = arrangement->getChildByName ("Lanes");
                    expectEquals (lanes->getStringAttribute ("timeUnit"), juce::String ("seconds"));
                    auto* clips = lanes->getChildByName ("Clips");
                    expectEquals (clips->getStringAttribute ("track"), track->getStringAttribute ("id"));
                    auto* clip = clips->getChildByName ("Clip");
                    expect (clip != nullptr);
                    auto* audio = clip->getChildByName ("Audio");
                    expectEquals (audio->getIntAttribute ("sampleRate"), 48000);
                    expectEquals (audio->getIntAttribute ("channels"), 1);
                    auto* file = audio->getChildByName ("File");
                    expectEquals (file->getStringAttribute ("path"), juce::String ("../Audio Files/Track_01.wav"));
                    expectEquals (file->getStringAttribute ("external"), juce::String ("true"));

                    auto* marker = arrangement->getChildByName ("Markers")->getChildByName ("Marker");
                    expectEquals (marker->getStringAttribute ("name"), juce::String ("Verse"));
                    expectWithinAbsoluteError (marker->getDoubleAttribute ("time"), 0.5, 1.0e-9);

                    // Every id unique; every reference lands on one.
                    juce::StringArray ids;
                    const std::function<void (const juce::XmlElement&)> walk = [&] (const juce::XmlElement& e2)
                    {
                        if (e2.hasAttribute ("id")) ids.add (e2.getStringAttribute ("id"));
                        for (auto* child : e2.getChildIterator()) walk (*child);
                    };
                    walk (*root);
                    auto unique = ids;
                    unique.removeDuplicates (false);
                    expectEquals (unique.size(), ids.size(), "duplicate xs:ID");
                }
            }

            beginTest ("Reaper project header, markers, tempo and colour");
            {
                ix::SessionInput in;
                in.sessionName = "Show";
                in.sampleRate = 44100.0;
                in.tempoBpm = 132.5;
                in.timeSigNum = 3;
                in.timeSigDen = 8;
                in.masterDb = -6.0f;
                in.masterMuted = true;
                in.markers.push_back ({ 44100, "One" });
                ix::StripInput strip;
                strip.name = "Coloured";
                strip.colourArgb = 0xffff8000;
                strip.muted = true;
                in.strips.push_back (strip);
                const auto r = ix::resolve (in, noCancel);
                const auto rpp = rppFor (r.project, juce::File ("/tmp/Show.RPP"), { "first note", "second\nnote" });
                expect (rpp.startsWith ("<REAPER_PROJECT 0.1"));
                expect (rpp.trimEnd().endsWith (">"));
                expect (rpp.contains ("SAMPLERATE 44100 1 0"));
                expect (rpp.contains ("TEMPO 132.500000 3 8"));
                expect (rpp.contains ("ITEMMIX 1"), "overlapping clips must sum, as they do in the session");
                expect (rpp.contains ("TIMELOCKMODE 0"), "the audio must not follow tempo edits");
                expect (rpp.contains ("MASTERMUTESOLO 1"));
                expect (rpp.contains ("MASTER_VOLUME 0.5011872336"));
                expect (rpp.contains ("MARKER 1 1.00000000000000 \"One\" 0"));
                expect (rpp.contains ("PEAKCOL 16810239"), "R in the low byte, bit 24 set");
                expect (rpp.contains ("MUTESOLO 1 0 0"));
                expect (rpp.contains ("<NOTES 0 0\n    |first note\n    |second note\n  >"));
            }

            beginTest ("Exports refuse during a take, local or daemon, at the engine and the host");
            {
                Folder f;
                expect (writeAudio (f.audio().getChildFile ("Track_01.wav"), 4800, 1, 1));
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                {
                    MainComponent host;
                    setup (host.engine, 1);
                    host.engine.loadSession (f.dir);
                    host.engine.setActiveSessionDir (f.dir);
                    expect (host.engine.exportRefusal().isEmpty());

                    host.engine.setExternalRecording (true);
                    expect (host.engine.exportRefusal().isNotEmpty(), "a daemon take must refuse an export");
                    host.exportDawProject (px::Format::Reaper);
                    host.exportDawProject (px::Format::DawProject);
                    host.exportSongs();
                    expect (! host.sessionIoBusy.load(), "a refused export must not claim the session");
                    host.startSongExport (songexport::Choice());
                    expect (! host.sessionIoBusy.load(), "a dialog left open across RECORD must still refuse");
                    expect (f.exports().getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0,
                            "a refused export wrote something");
                    host.engine.setExternalRecording (false);

                    host.exportDawProject (px::Format::Reaper);
                    expect (host.sessionIoBusy.load(), "the export must run as a session job");
                    expect (pump ([&] { return ! host.sessionIoBusy.load(); }));
                    expect (f.exports().getChildFile (f.dir.getFileName() + ".RPP").existsAsFile());
                    expect (! host.engine.isSessionTransitionActive());
                    host.engine.clearAllStripOverrides();
                }
                MainComponent::s_testConstruct = false;
            }

            beginTest ("A second export while a job runs is refused, not queued");
            {
                Folder f;
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                {
                    MainComponent host;
                    setup (host.engine, 1);
                    host.engine.setActiveSessionDir (f.dir);
                    expect (host.sessionJobs.reserve());
                    host.exportDawProject (px::Format::Reaper);
                    expect (! f.exports().getChildFile (f.dir.getFileName() + ".RPP").exists());
                    host.sessionJobs.release();
                    host.engine.clearAllStripOverrides();
                }
                MainComponent::s_testConstruct = false;
            }

            beginTest ("The verified handoff carries both project files, reading its own audio");
            {
                Folder f;
                const auto track = f.audio().getChildFile ("Track_01.wav");
                expect (writeAudio (track, 4800, 1, 1));
                expect (f.dir.getChildFile ("session_mix.json").replaceWithText (
                    R"({"trackCount":1,"strips":[{"name":"Vox","inRoute":0,"outRoute":1,"stereo":false}]})"));
                AudioEngine e; setup (e, 1);
                e.loadSession (f.dir);
                auto pasted = clipOf (track, 4800, 0, 2400);
                e.clipsFor (0).push_back (pasted);
                const auto input = e.captureInterchange();

                const auto dest = f.dir.getSiblingFile ("Show Handoff");
                expect (dest.createDirectory().wasOk());
                expect (f.dir.copyDirectoryTo (dest));

                juce::String warning;
                const auto files = px::writeForHandoff (input, dest, "1", noCancel, warning);
                expect (warning.isEmpty(), warning);
                expectEquals ((int) files.size(), 2);

                std::atomic<bool> cancel { false };
                for (const auto& file : files) file.deleteFile();
                auto generated = [&] (const juce::File& copy, juce::String& w)
                { return px::writeForHandoff (input, copy, "1", cancel, w); };
                const auto result = showhandoff::verifyAndWrite (f.dir, dest, "t,m\r\n", cancel, generated);
                expect (result.ok, result.message);

                const auto rpp = dest.getChildFile ("Show.RPP");
                expect (rpp.existsAsFile());
                expect (rpp.loadFileAsString().contains ("FILE \"Audio Files/Track_01.wav\""),
                        "the handoff's project must read the handoff's audio");
                expect (dest.getChildFile ("Show.dawproject").existsAsFile());

                const auto manifest = dest.getChildFile ("Handoff Manifest.json").loadFileAsString();
                expect (manifest.contains ("Show.RPP"), "the project file must be in the manifest");
                expect (manifest.contains ("Show.dawproject"));

                // A project that cannot be written costs the note, not the copy.
                auto broken = input;
                broken.strips[0].clips->push_back (clipOf (f.audio().getChildFile ("Gone.wav"), 0, 0, 100));
                const auto second = f.dir.getSiblingFile ("Show Handoff 2");
                expect (second.createDirectory().wasOk());
                expect (f.dir.copyDirectoryTo (second));
                const auto partial = showhandoff::verifyAndWrite (f.dir, second, "t,m\r\n", cancel,
                    [&] (const juce::File& copy, juce::String& w)
                    { return px::writeForHandoff (broken, copy, "1", cancel, w); });
                expect (partial.ok, partial.message);
                expect (partial.message.contains ("not written"), partial.message);
                expect (! second.getChildFile ("Show.RPP").exists());
            }

            if (const auto root = juce::SystemStats::getEnvironmentVariable ("ZF_EXPORT_FIXTURE_DIR", {});
                root.isNotEmpty())
            {
                beginTest ("Fixture session for tools/verify_reaper_export.py");
                writeFixture (juce::File (root));
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

        // Exercises everything the writers map, at one sample rate, and leaves
        // the engine's own mix beside the project so a real render can be
        // compared with it sample for sample.
        void writeFixture (const juce::File& root)
        {
            const auto session = root.getChildFile ("Fixture \"Show\" & Co");
            session.deleteRecursively();
            const auto audio = session.getChildFile ("Audio Files");
            const auto exports = session.getChildFile ("Export Files");
            audio.createDirectory();
            exports.createDirectory();

            constexpr juce::int64 s = 48000;
            expect (writeAudio (audio.getChildFile ("Track_01.wav"), 10 * s, 1, 11));           // mono edits
            expect (writeAudio (audio.getChildFile ("Track_02.wav"), 10 * s, 2, 12));           // stereo pair (strips 2+3)
            expect (writeAudio (audio.getChildFile ("Track_04.wav"), 3 * s, 1, 14));            // take in two parts
            expect (writeAudio (audio.getChildFile ("Track_04_part02.wav"), 3 * s, 1, 15));
            expect (writeAudio (audio.getChildFile ("Track_05.flac"), 4 * s, 1, 16));
            expect (writeAudio (audio.getChildFile ("Track_06.aif"), 4 * s, 1, 17));
            expect (writeAudio (audio.getChildFile ("Track_07.aif"), 4 * s, 1, 18, 0.25f, 32)); // float AIFF
            expect (writeAudio (audio.getChildFile ("Track_08.wav"), 4 * s, 1, 19, 0.25f, 32)); // float WAV
            expect (writeAudio (audio.getChildFile ("Track_09.wav"), 4 * s, 2, 20));            // unlinked stereo: L strip
            expect (writeAudio (audio.getChildFile ("Track_12.wav"), 4 * s, 2, 21));            // pair, narrow pan

            AudioEngine e; setup (e, 13);
            e.setTrackStereo (1, true);
            e.setTrackStereo (11, true);
            auto& rec = e.getRecorder();
            rec.getTrack (0).setNameThreadSafe (juce::String::fromUTF8 ("  Kick & \"Snare\" caf\xc3\xa9 "));
            rec.getTrack (1).setNameThreadSafe ("Overheads");
            rec.getTrack (1).pan.store (-1.0f);
            rec.getTrack (2).pan.store (1.0f);
            rec.getTrack (0).colourARGB.store (0xffff8000);
            rec.getTrack (1).colourARGB.store (0xff2080c0);
            rec.getTrack (3).setNameThreadSafe ("Bass parts");
            rec.getTrack (3).pan.store (0.5f);
            rec.getTrack (3).gainDb.store (-3.0f);
            rec.getTrack (4).setNameThreadSafe ("Flac");
            rec.getTrack (4).pan.store (-0.25f);
            rec.getTrack (5).setNameThreadSafe ("Aiff 24");
            rec.getTrack (6).setNameThreadSafe ("Aiff float");
            rec.getTrack (7).setNameThreadSafe ("Wav float");
            rec.getTrack (7).muted.store (true);
            rec.getTrack (8).setNameThreadSafe ("Left of stereo");
            rec.getTrack (9).setNameThreadSafe ("Right of stereo");
            rec.getTrack (10).setNameThreadSafe ("Cross track");
            rec.getTrack (11).setNameThreadSafe ("Narrow pair");
            rec.getTrack (11).pan.store (-0.5f);
            rec.getTrack (12).pan.store (0.25f);
            e.getMasterState().gainDb.store (-2.0f);
            e.setSessionTempoBpm (97.0f);
            e.addTempoChange (s, 120.0f);      // left out, so the project carries a note
            e.loadSession (session);

            auto& kick = e.clipsFor (0);
            kick.clear();
            auto a = clipOf ({}, s / 2, s, 2 * s);
            a.name = "Faded";
            a.fadeInSamples = s / 10;
            a.fadeOutSamples = s / 5;
            a.gainDb = -6.0f;
            auto b = clipOf ({}, 3 * s, 4 * s, 2 * s);
            b.name = "Equal power";
            b.fadeInSamples = s / 2;
            b.fadeOutSamples = s / 2;
            b.fadeCurve = 1;
            auto muted = clipOf ({}, 5 * s + 1000, 0, s);
            muted.muted = true;
            kick = { a, b, muted };

            // A straddling clip on the take in parts, and a split in its fade.
            auto& parts = e.clipsFor (3);
            parts.clear();
            auto across = clipOf ({}, s, 2 * s, 2 * s);
            across.fadeOutSamples = s / 4;
            parts.push_back (across);

            // An enclosed clip must sum, not replace.
            auto& flac = e.clipsFor (4);
            flac.clear();
            flac.push_back (clipOf ({}, 0, 0, 3 * s));
            flac.push_back (clipOf ({}, s, s, s / 2));

            // A clip from another strip's file.
            auto& cross = e.clipsFor (10);
            cross.clear();
            auto pasted = clipOf (audio.getChildFile ("Track_02.wav"), 2 * s, s, s);
            pasted.sourceChannel = 1;
            cross.push_back (pasted);

            e.getMarkers().setContext (session, 48000.0);
            e.getMarkers().drop (s, juce::String::fromUTF8 ("Verse \"1\" & caf\xc3\xa9"));
            e.getMarkers().drop (3 * s, "it's `all` \"here\"");

            std::atomic<bool> cancel { false };
            const auto input = e.captureInterchange();
            const auto rpp = px::uniqueDestination (exports, input.sessionName, px::Format::Reaper);
            auto result = px::write (input, px::Format::Reaper, rpp, "fixture", cancel);
            expect (result.ok, result.error);
            logMessage ("RPP notes: " + result.notes.joinIntoString (" | "));
            const auto dp = px::uniqueDestination (exports, input.sessionName, px::Format::DawProject);
            result = px::write (input, px::Format::DawProject, dp, "fixture", cancel);
            expect (result.ok, result.error);
            logMessage ("DAWproject notes: " + result.notes.joinIntoString (" | "));

            juce::AudioBuffer<float> mix;
            const auto length = e.getArrangementLengthSamples();
            expect (e.renderStereoMix (mix, length));
            const auto reference = root.getChildFile ("engine-mix.wav");
            reference.deleteFile();
            std::unique_ptr<juce::FileOutputStream> stream (reference.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> writer (
                juce::WavAudioFormat().createWriterFor (stream.get(), 48000.0, 2, 32, {}, 0));
            expect (writer != nullptr);
            if (writer != nullptr)
            {
                stream.release();
                expect (writer->writeFromAudioSampleBuffer (mix, 0, mix.getNumSamples()));
            }
            e.clearAllStripOverrides();
            e.clearTempoMap();
        }
    };

    static SessionInterchangeTests sessionInterchangeTests;
}
