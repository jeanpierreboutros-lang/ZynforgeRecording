// The 2026-11-16 review of take file names (Source/Audio/TakeNames.h): two
// takes on one strip's number, song files in a session folder's root, one
// folder listing per operation, renders that follow their strip, decomposed
// labels, report names, QC of a take still owing its lead-in, and the notice
// for strips that keep Track_NN names.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/SessionInterchange.h"
#include "../Audio/SongDetector.h"
#include "../Audio/TakeNames.h"
#include "../Audio/TrackOrderLog.h"
#include "../UI/EditPage.h"

namespace zynforge
{
    class TakeNamesFixTests final : public juce::UnitTest
    {
    public:
        TakeNamesFixTests() : juce::UnitTest ("Take names fixes 2026-11-16", "zynforge") {}

        static constexpr int kBlock = 512;

        static juce::File freshDir (const juce::String& tag)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-namesfix-" + tag + "-" + juce::Uuid().toString());
            dir.deleteRecursively();
            dir.createDirectory();
            return dir;
        }

        static bool writeDc (const juce::File& f, float value, int len, int channels = 1)
        {
            f.getParentDirectory().createDirectory();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            if (os == nullptr) return false;
            auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (48000.0)
                                                       .withNumChannels (channels)
                                                       .withBitsPerSample (24));
            if (writer == nullptr) return false;
            juce::AudioBuffer<float> buffer (channels, len);
            for (int c = 0; c < channels; ++c) juce::FloatVectorOperations::fill (buffer.getWritePointer (c), value, len);
            return writer->writeFromAudioSampleBuffer (buffer, 0, len);
        }

        static juce::StringArray namesIn (const juce::File& dir)
        {
            juce::StringArray names;
            for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
                names.add (f.getFileName());
            names.sort (true);
            return names;
        }

        struct Pass
        {
            bool started { false };
            juce::String error;
            std::vector<int> keepLegacy;
        };

        // One recorder pass on names.size() mono tracks; `armed` picks the
        // strips (all when empty). `during` runs between start and STOP.
        static Pass record (const juce::File& dir, const juce::StringArray& names, int blocks,
                            std::function<void (MultitrackRecorder&)> arm = {},
                            std::vector<int> armed = {},
                            std::function<void (MultitrackRecorder&)> during = {})
        {
            MultitrackRecorder rec;
            rec.prepare (48000.0, kBlock, names.size());
            for (int i = 0; i < names.size(); ++i)
            {
                rec.getTrack (i).setNameThreadSafe (names[i]);
                const bool on = armed.empty() || std::find (armed.begin(), armed.end(), i) != armed.end();
                rec.getTrack (i).armed.store (on, std::memory_order_relaxed);
            }
            if (arm) arm (rec);
            Pass pass;
            pass.started = rec.startRecording (dir);
            pass.error = rec.getStartError();
           #ifndef ZF_RED_BASELINE
            pass.keepLegacy = rec.getStripsKeepingLegacyNames();
           #endif
            if (! pass.started) return pass;
            if (during) during (rec);
            std::vector<std::vector<float>> data ((size_t) names.size(), std::vector<float> ((size_t) kBlock, 0.25f));
            std::vector<const float*> ptrs;
            for (auto& d : data) ptrs.push_back (d.data());
            for (int b = 0; b < blocks; ++b) rec.processBlock (ptrs.data(), names.size(), kBlock);
            rec.stopRecording();
            return pass;
        }

        static void feed (AudioEngine& engine, int channels, int blocks)
        {
            juce::AudioBuffer<float> in (channels, kBlock), out (2, kBlock);
            in.clear();
            juce::AudioIODeviceCallbackContext context;
            for (int b = 0; b < blocks; ++b)
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), channels,
                                                         out.getArrayOfWritePointers(), 2, kBlock, context);
        }

        // An engine that recorded `names` into `dir` and has it open.
        void engineSession (AudioEngine& engine, const juce::File& dir, const juce::StringArray& names)
        {
            engine.clearAllStripOverrides();
            engine.prepareForTests (48000.0, kBlock);
            engine.setStripCount (names.size());
            for (int i = 0; i < names.size(); ++i)
            {
                engine.setTrackName (i, names[i]);
                engine.setTrackInputRouting (i, i);
                engine.setTrackArmed (i, true);
            }
            expect (engine.startRecording (dir), engine.getRecorder().getStartError());
            feed (engine, names.size(), 20);
            engine.stopRecording();
            for (int i = 0; i < names.size(); ++i) engine.setTrackArmed (i, false);
            engine.setActiveSessionDir (dir);
            engine.claimMixerFor (dir);
        }

        static int listings() { return takenames::folderListingsOnThisThread(); }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restore { [] { AudioEngine::setTestModeSkipAudioInit (false); } };

            clashIsDetectedNotPicked();
            playbackNamesTheClash();
            recordRefusesTheClash();
            moveRefusesTheClash();
            rootReadsOnlyLegacyNames();
            oneListingPerOperation();
            rendersDoNotClaimAndFollowTheirStrip();
            decomposedLabelsStillParse();
            reportKeepsNamesFromTheStart();
            qcCountsATakeOwingItsLeadIn();
            legacyNamesNotice();
        }

        // CAP-01 = PLAY-02, SES-01: a second stem for one number.
        void clashIsDetectedNotPicked()
        {
            beginTest ("Two takes on one number are found and neither is picked (CAP-01, SES-01)");
            struct Case { const char* what; std::vector<const char*> files; };
            const Case cases[] = {
                { "Finder duplicate",      { "05 KiCK.wav", "05 KiCK copy.wav" } },
                { "Keep Both copy",        { "05 KiCK.wav", "05 KiCK 2.wav" } },
                { "sync copy",             { "05 KiCK.wav", "05 KiCK (1).wav" } },
                { "DAW glue",              { "05 KiCK.wav", "05 KiCK-glued.wav" } },
                { "main renamed in Finder",{ "05 Kick In.wav", "05 KiCK_part02.wav" } },
                { "older build beside it", { "05 KiCK.wav", "Track_05.wav" } },
            };
            for (const auto& c : cases)
            {
                const auto session = freshDir ("clash");
                const auto audio = session.getChildFile ("Audio Files");
                for (auto* name : c.files) expect (writeDc (audio.getChildFile (name), 0.1f, 64));
                expect (! findTrackMedia (audio, 4).exists(),
                        juce::String (c.what) + ": picked " + findTrackMedia (audio, 4).getFileName());
               #ifndef ZF_RED_BASELINE
                const takenames::StemIndex index (audio);
                expect (index.clashes (4), c.what);
                const auto why = index.clash (4);
                for (auto* name : c.files)
                    expect (why.contains (name) || why.contains (juce::String (name).upToFirstOccurrenceOf ("_part", false, false)),
                            juce::String (c.what) + ": " + why);
                expect (why.contains ("move one of them out of Audio Files"), why);
                const TrackMediaIndex media (audio);
                expect (media.find (4) == juce::File());
                expect (media.clash (4).isNotEmpty());
               #endif
                session.deleteRecursively();
            }

            beginTest ("One stem per number is no clash: parts, a punch sidecar, two containers (CAP-01)");
            {
                const auto session = freshDir ("plain");
                const auto audio = session.getChildFile ("Audio Files");
                for (auto* name : { "05 KiCK.wav", "05 KiCK_part02.wav", "05 KiCK_part03.wav",
                                    "05 KiCK.punchbase.wav", "05 KiCK.flac", "Track_06.wav", "Track_06_part02.wav" })
                    expect (writeDc (audio.getChildFile (name), 0.1f, 64));
                expectEquals (findTrackMedia (audio, 4).getFileName(), juce::String ("05 KiCK.wav"));
                expectEquals (findTrackMedia (audio, 5).getFileName(), juce::String ("Track_06.wav"));
               #ifndef ZF_RED_BASELINE
                const takenames::StemIndex index (audio);
                expect (! index.clashes (4) && ! index.clashes (5));
                expect (index.clashingTracks().empty());
               #endif
                session.deleteRecursively();
            }
        }

        void playbackNamesTheClash()
        {
            beginTest ("Playback leaves a clashing strip silent, marks the load incomplete and names the files (PLAY-02)");
            {
                const auto session = freshDir ("play");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("01 SNARE.wav"), 0.1f, 1000));
                expect (writeDc (audio.getChildFile ("05 KiCK.wav"), 0.1f, 1000));
                expect (writeDc (audio.getChildFile ("05 KiCK_part02.wav"), 0.1f, 500));
                expect (writeDc (audio.getChildFile ("05 KiCK copy.wav"), 0.1f, 1000));
                AudioEngine engine;
                engine.loadSession (session);
                auto& player = engine.getPlayer();
                expectEquals (player.getTrackLengthSamples (0), (juce::int64) 1000, "the plain strip plays");
                expectEquals (player.getTrackLengthSamples (4), (juce::int64) 0, "the copy played as strip 5's take");
                expect (player.trackFailedToOpen (4), "the clash was not marked");
                expect (! SessionPlayer::preparedLoadComplete (player.prepareSessionLoad (session, 48000.0, kBlock)));
               #ifndef ZF_RED_BASELINE
                const auto clashes = player.loadClashes();
                expectEquals (clashes.size(), 1);
                expect (clashes[0].startsWith ("Track 5 has two take files"), clashes[0]);
                expect (clashes[0].contains ("05 KiCK copy.wav") && clashes[0].contains ("05 KiCK.wav"), clashes[0]);
               #endif
                session.deleteRecursively();
            }

            beginTest ("A main renamed away from its parts, and a Track_NN beside a named take, play neither (PLAY-02, SES-01)");
            for (const auto& files : { juce::StringArray { "05 Kick In.wav", "05 KiCK_part02.wav" },
                                       juce::StringArray { "05 KiCK.wav", "Track_05.wav" } })
            {
                const auto session = freshDir ("renamed");
                const auto audio = session.getChildFile ("Audio Files");
                for (const auto& name : files) expect (writeDc (audio.getChildFile (name), 0.1f, 700));
                AudioEngine engine;
                engine.loadSession (session);
                expect (engine.getPlayer().trackFailedToOpen (4), files.joinIntoString (" + "));
                expectEquals (engine.getPlayer().getTrackLengthSamples (4), (juce::int64) 0, files.joinIntoString (" + "));
                session.deleteRecursively();
            }

            beginTest ("A plain named session plays as before, parts and all");
            {
                const auto session = freshDir ("plainplay");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("05 KiCK.wav"), 0.1f, 1000));
                expect (writeDc (audio.getChildFile ("05 KiCK_part02.wav"), 0.1f, 500));
                AudioEngine engine;
                expectEquals (engine.loadSession (session), 5);
                expectEquals (engine.getPlayer().getTrackLengthSamples (4), (juce::int64) 1500);
                expect (! engine.getPlayer().trackFailedToOpen (4));
                expect (SessionPlayer::preparedLoadComplete (engine.getPlayer().prepareSessionLoad (session, 48000.0, kBlock)));
               #ifndef ZF_RED_BASELINE
                expect (engine.getPlayer().loadClashes().isEmpty());
               #endif
                session.deleteRecursively();
            }
        }

        void recordRefusesTheClash()
        {
            beginTest ("RECORD, a continue and a punch on a clashing strip are refused, naming the files (CAP-01)");
            for (int mode = 0; mode < 3; ++mode)
            {
                const auto session = freshDir ("rec");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("01 KiCK.wav"), 0.1f, 4 * kBlock));
                expect (writeDc (audio.getChildFile ("01 KiCK copy.wav"), 0.1f, 4 * kBlock));
                const auto before = namesIn (audio);
                const auto pass = record (session, { "KiCK" }, 4, [mode] (MultitrackRecorder& rec)
                {
                    if (mode == 1) rec.armContinue (4 * kBlock);
                    if (mode == 2) rec.armPunchIn (2 * kBlock);
                });
                const juce::String what = mode == 0 ? "fresh" : mode == 1 ? "continue" : "punch";
                expect (! pass.started, what + " went live: " + namesIn (audio).joinIntoString (" | "));
                expect (pass.error.contains ("01 KiCK copy.wav") && pass.error.contains ("01 KiCK.wav"),
                        what + ": " + pass.error);
                expect (pass.error.contains ("move one of them out of Audio Files"), what + ": " + pass.error);
                expect (namesIn (audio) == before, what + ": a refused start wrote or moved a file");
                session.deleteRecursively();
            }

            beginTest ("A clash on a strip not recording does not stop the others (CAP-01)");
            {
                const auto session = freshDir ("recother");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("02 SNARE.wav"), 0.1f, 64));
                expect (writeDc (audio.getChildFile ("02 SNARE copy.wav"), 0.1f, 64));
                const auto pass = record (session, { "KiCK", "SNARE" }, 4, {}, { 0 });
                expect (pass.started, pass.error);
                expect (audio.getChildFile ("01 KiCK.wav").existsAsFile());
                session.deleteRecursively();
            }

            beginTest ("A punch refusal names the clash before the take is touched (CAP-01)");
            {
                const auto dir = freshDir ("punchref");
                AudioEngine engine;
                engineSession (engine, dir, { "KiCK", "SNARE" });
                const auto audio = dir.getChildFile ("Audio Files");
                expect (audio.getChildFile ("01 KiCK.wav").copyFileTo (audio.getChildFile ("01 KiCK copy.wav")));
                engine.setTrackArmed (0, true);
                const auto why = engine.punchRefusal (2 * kBlock, -1);
               #ifndef ZF_RED_BASELINE
                expect (why.contains ("01 KiCK copy.wav"), why);
               #else
                expect (why.isNotEmpty(), "punch not refused on a clash");
               #endif
                engine.setTrackArmed (0, false);
                engine.clearSessionState();
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
        }

        void moveRefusesTheClash()
        {
            beginTest ("A strip move refuses a number two takes claim and renames nothing (CAP-01)");
            {
                const auto dir = freshDir ("move");
                AudioEngine engine;
                engineSession (engine, dir, { "KiCK", "SNARE", "HAT" });
                const auto audio = dir.getChildFile ("Audio Files");
                expect (audio.getChildFile ("02 SNARE.wav").copyFileTo (audio.getChildFile ("02 SNARE copy.wav")));
                engine.loadSession (dir, true);
                engine.claimMixerFor (dir);
                const auto before = namesIn (audio);
                expect (! engine.reorderTracks ({ 1, 0, 2 }), "a move renamed one of two takes");
                expect (! engine.reorderTracks ({ 0, 2 }), "a delete archived one of two takes");
                expect (engine.getReorderRefusal().contains ("02 SNARE copy.wav")
                        && engine.getReorderRefusal().contains ("02 SNARE.wav"), engine.getReorderRefusal());
                expect (namesIn (audio) == before, "a refused move touched a take file");
                // A move that leaves the clashing number where it is goes ahead.
                expect (engine.reorderTracks ({ 2, 1, 0 }), engine.getReorderRefusal());
                expect (audio.getChildFile ("01 HAT.wav").existsAsFile() && audio.getChildFile ("03 KiCK.wav").existsAsFile(),
                        namesIn (audio).joinIntoString (" | "));
                engine.clearSessionState();
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }

            beginTest ("takeMoves returns nothing for a step that would move a clashing number (CAP-01)");
            {
                const auto dir = freshDir ("moves");
                const auto audio = dir.getChildFile ("Audio Files");
                for (auto* name : { "01 KiCK.wav", "02 SNARE.wav", "02 SNARE copy.wav" })
                    expect (writeDc (audio.getChildFile (name), 0.1f, 64));
                expect (trackorder::takeMoves (audio, dir.getChildFile ("Removed"), { 1, 0 }).empty(),
                        "a backup or mirror following the log would rename one of two takes");
               #ifndef ZF_RED_BASELINE
                juce::String clash;
                expect (trackorder::takeMoves (audio, dir.getChildFile ("Removed"), { 1, 0 }, &clash).empty());
                expect (clash.contains ("02 SNARE copy.wav"), clash);
               #endif
                dir.deleteRecursively();
            }
        }

        // CAP-03 = PLAY-01.
        void rootReadsOnlyLegacyNames()
        {
            beginTest ("A song file in the session folder is not strip 1's take (CAP-03)");
            {
                const auto session = freshDir ("root");
                expect (writeDc (session.getChildFile ("01 Intro.wav"), 0.5f, 2000));
                session.getChildFile ("Audio Files").createDirectory();
                AudioEngine engine;
                engine.loadSession (session);
                expectEquals (engine.getPlayer().getTrackLengthSamples (0), (juce::int64) 0, "the song plays on strip 1");
                expect (! findTrackMedia (session.getChildFile ("Audio Files"), 0).exists());
                expectEquals (qc::sessionTakes (session).size(), 0, "QC checks the song as strip 1");
                engine.setActiveSessionDir (session);
                expect (! engine.getTrackAudioFile (0).exists(), "EDIT and export read the song as strip 1");

                const auto pass = record (session, { "KiCK" }, 4);
                expect (pass.started, "RECORD refused over a song file: " + pass.error);
                expect (session.getChildFile ("Audio Files").getChildFile ("01 KiCK.wav").existsAsFile());
                expect (session.getChildFile ("01 Intro.wav").existsAsFile(), "the song was touched");
                engine.clearSessionState();
                session.deleteRecursively();
            }

            beginTest ("A Track_NN take in the session folder is still read (CAP-03)");
            {
                const auto session = freshDir ("legacyroot");
                expect (writeDc (session.getChildFile ("Track_01.wav"), 0.5f, 2000));
                expect (writeDc (session.getChildFile ("02 Walk In.wav"), 0.5f, 900));
                AudioEngine engine;
                engine.loadSession (session);
                expectEquals (engine.getPlayer().getTrackLengthSamples (0), (juce::int64) 2000);
                expectEquals (engine.getPlayer().getTrackLengthSamples (1), (juce::int64) 0);
                expectEquals (findTrackMedia (session.getChildFile ("Audio Files"), 0).getFileName(), juce::String ("Track_01.wav"));
                expectEquals (qc::sessionTakes (session).size(), 1);
                const auto pass = record (session, { "KiCK" }, 4);
                expect (! pass.started && pass.error.contains ("older layout"), pass.error);
                engine.clearSessionState();
                session.deleteRecursively();
            }
        }

        // EDIT-01 = CAP-06 = SES-07.
        void oneListingPerOperation()
        {
            constexpr int strips = 60;
            const auto dir = freshDir ("listings");
            juce::StringArray names;
            for (int i = 0; i < strips; ++i) names.add ("CH " + juce::String (i + 1));
            AudioEngine engine;
            engineSession (engine, dir, names);
            for (int i = 0; i < 40; ++i)   // a show's worth of other files in Audio Files
                expect (writeDc (dir.getChildFile ("Audio Files").getChildFile ("readme " + juce::String (i) + ".wav"), 0.0f, 8));

            beginTest ("One EDIT refresh of a 60-strip session lists Audio Files once (EDIT-01)");
            {
                EditPage page (engine);
                page.setBounds (0, 0, 1200, 800);
                const int before = listings();
                page.refresh();
                const int used = listings() - before;
                logMessage ("listings for one EDIT refresh of 60 strips: " + juce::String (used));
                expectLessOrEqual (used, 4, "listings for one EDIT refresh: " + juce::String (used));
            }

            beginTest ("Punch refusal and Detect Songs list the folder once, not once per strip (EDIT-01)");
            {
                for (int i = 0; i < strips; ++i) engine.setTrackArmed (i, true);
                int before = listings();
                engine.punchRefusal (kBlock, -1);
                logMessage ("listings for a punch refusal on 60 armed strips: " + juce::String (listings() - before));
                expectLessOrEqual (listings() - before, 2, "punch refusal: " + juce::String (listings() - before));
                for (int i = 0; i < strips; ++i) engine.setTrackArmed (i, false);
                before = listings();
                engine.songScanVoices();
                expectLessOrEqual (listings() - before, 2, "song scan: " + juce::String (listings() - before));
            }

            beginTest ("A continue on 60 strips lists the take folder a fixed number of times (CAP-06)");
            {
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> first (fm.createReaderFor (findTrackMedia (dir.getChildFile ("Audio Files"), 0)));
                expect (first != nullptr);
                const auto length = first != nullptr ? first->lengthInSamples : (juce::int64) 0;
                const int before = listings();
                const auto pass = record (dir, names, 2, [length] (MultitrackRecorder& rec) { rec.armContinue (length); });
                expect (pass.started, pass.error);
                const int used = listings() - before;
                logMessage ("listings for a 60-strip continue, start to STOP: " + juce::String (used));
                expectLessOrEqual (used, 24, "listings for a 60-strip continue, start to STOP: " + juce::String (used));
            }
            engine.clearSessionState();
            engine.clearAllStripOverrides();
            dir.deleteRecursively();
        }

        // CAP-04 = ENG-07.
        void rendersDoNotClaimAndFollowTheirStrip()
        {
            beginTest ("A render or a peak file left under a number does not name its next take (CAP-04)");
            {
                const auto session = freshDir ("render");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("03 SNARE_consolidated_1.wav"), 0.1f, 64));
                audio.getChildFile ("02 HAT.wav.reapeaks").replaceWithText ("peaks");
                expect (takenames::existingStem (audio, 2).isEmpty(), takenames::existingStem (audio, 2));
                expect (takenames::existingStem (audio, 1).isEmpty(), takenames::existingStem (audio, 1));
                const auto pass = record (session, { "KiCK", "TOM", "BASS" }, 4);
                expect (pass.started, pass.error);
                expect (audio.getChildFile ("03 BASS.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (audio.getChildFile ("02 TOM.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (! audio.getChildFile ("03 SNARE.wav").exists());
                session.deleteRecursively();
            }

            beginTest ("A strip move moves its renders, and a delete archives them (CAP-04)");
            {
                const auto dir = freshDir ("rendermove");
                for (auto* name : { "01 KiCK.wav", "01 KiCK_consolidated_1.wav", "02 SNARE.wav", "02 OLD_consolidated_2.wav" })
                    expect (writeDc (dir.getChildFile (name), 0.1f, 64));
                std::map<juce::String, juce::String> byName;
                for (const auto& m : trackorder::takeMoves (dir, dir.getChildFile ("Removed"), { 1, 0 }))
                    byName[m.first.getFileName()] = m.second.getRelativePathFrom (dir);
                expectEquals (byName["01 KiCK_consolidated_1.wav"], juce::String ("02 KiCK_consolidated_1.wav"));
                expectEquals (byName["02 OLD_consolidated_2.wav"], juce::String ("01 OLD_consolidated_2.wav"));
                byName.clear();
                for (const auto& m : trackorder::takeMoves (dir, dir.getChildFile ("Removed"), { -1, 0 }))
                    byName[m.first.getFileName()] = m.second.getRelativePathFrom (dir);
                expectEquals (byName["01 KiCK_consolidated_1.wav"], juce::String ("Removed/01 KiCK_consolidated_1.wav"));
                dir.deleteRecursively();
            }

            beginTest ("A clip on a render follows its strip through a move (CAP-04)");
            {
                const auto dir = freshDir ("renderclip");
                AudioEngine engine;
                engineSession (engine, dir, { "KiCK", "SNARE" });
                const auto audio = dir.getChildFile ("Audio Files");
                const auto render = audio.getChildFile ("01 KiCK_consolidated_1.wav");
                expect (writeDc (render, 0.3f, 2 * kBlock));
                engine.loadSession (dir, true);
                engine.claimMixerFor (dir);
                expect (engine.pasteClip (0, 0, 0, 2 * kBlock, 0, 0, 0.0f, "render", render) >= 0);
                expect (engine.reorderTracks ({ 1, 0 }), engine.getReorderRefusal());
                const auto moved = audio.getChildFile ("02 KiCK_consolidated_1.wav");
                expect (moved.existsAsFile() && ! render.exists(), namesIn (audio).joinIntoString (" | "));
                bool follows = false;
                for (const auto& c : engine.clipsFor (1)) follows = follows || c.audioFile == moved;
                expect (follows, "the clip still names the render's old path");
                engine.clearSessionState();
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
        }

        void decomposedLabelsStillParse()
        {
            beginTest ("A label read back decomposed (NFD) past 64 bytes is still the take (TAKE-NFD)");
            {
                juce::String composed, decomposed;
                for (int i = 0; i < 32; ++i)
                {
                    composed   << juce::String::fromUTF8 ("\xc3\xa9");   // U+00E9, 2 bytes
                    decomposed << juce::String::fromUTF8 ("e\xcc\x81");  // e + U+0301, 3 bytes
                }
                expectEquals (takenames::labelFor (composed), composed, "the label the strip got");
                expect (decomposed.getNumBytesAsUTF8() > (size_t) takenames::kMaxLabelBytes);
                const auto parsed = takenames::parse ("05 " + decomposed + ".wav");
                expectEquals (parsed.number, 5, "the decomposed take is not recognised");
                expect (takenames::isMain (parsed));
                expectEquals (takenames::partNumber (takenames::parse ("05 " + decomposed + "_part02.wav")), 2);
                // Everything labelFor would never write is still no take.
                for (auto* name : { "05 Vox, lead.wav", "05  KiCK.wav", "05 KiCK .wav", "05 12.wav",
                                    "05 Ki'CK.wav", "05 Ki[CK].wav", "05 \tKiCK.wav" })
                    expectEquals (takenames::parse (name).number, 0, name);
            }
        }

        void reportKeepsNamesFromTheStart()
        {
            beginTest ("The report names each strip as it was when the take started (REPORT-NAMES)");
            {
                const auto session = freshDir ("report");
                const auto pass = record (session, { "KiCK", "SNARE" }, 6, {}, {},
                                          [] (MultitrackRecorder& rec) { rec.getTrack (0).setNameThreadSafe ("KICK IN"); });
                expect (pass.started, pass.error);
                const auto report = juce::JSON::parse (session.getChildFile ("session.report.json"));
                const auto* tracks = report.getProperty ("tracks", {}).getArray();
                expect (tracks != nullptr && tracks->size() == 2);
                if (tracks != nullptr && tracks->size() == 2)
                {
                    expectEquals ((*tracks)[0]["name"].toString(), juce::String ("KiCK"));
                    expectEquals ((*tracks)[0]["files"][0].toString(), juce::String ("01 KiCK.wav"));
                }
                session.deleteRecursively();
            }
        }

        // PLAY-03.
        void qcCountsATakeOwingItsLeadIn()
        {
            beginTest ("QC lists a take that still owes its lead-in as not read (PLAY-03)");
            {
                const auto session = freshDir ("qc");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("01 SNARE.wav"), 0.1f, 4800));
                expect (writeDc (audio.getChildFile ("05 KiCK_part02.wav"), 0.1f, 4800));
                expect (writeDc (audio.getChildFile ("06 TOM.wav"), 0.1f, 4800));
                expect (writeDc (audio.getChildFile ("06 TOM copy.wav"), 0.1f, 4800));
                struct Entry { int trackIndex; juce::File file; juce::String problem; };
                std::vector<Entry> entries;
               #ifndef ZF_RED_BASELINE
                for (const auto& e : qc::sessionTakeEntries (session))
                    entries.push_back ({ e.trackIndex, e.file, e.problem });
               #else
                for (const auto& f : qc::sessionTakes (session))   // what the old QC handed the scan
                    entries.push_back ({ mainTakeTrackNumber (f) - 1, f, {} });
               #endif
                expectEquals ((int) entries.size(), 3, "QC left a strip out");
                std::vector<qc::TrackQc> results;
                for (const auto& e : entries)
                {
                    qc::TrackQc q;
                    if (e.problem.isNotEmpty()) { q.error = e.problem; }
                    else q = qc::analyzeFile (e.file);
                    q.trackIndex = e.trackIndex;
                    results.push_back (q);
                }
                const auto summary = qc::statusSummary (results, 0);
                expect (summary.contains ("2 COULD NOT BE READ"), summary);
                for (const auto& e : entries)
                {
                    if (e.trackIndex == 4)
                        expectEquals (e.problem, juce::String ("take does not open (its silent lead-in is still owed)"));
                    if (e.trackIndex == 5)
                        expect (e.problem.contains ("06 TOM copy.wav"), e.problem);
                }
                session.deleteRecursively();
            }

            beginTest ("Detect Songs names the strip whose take will not open (PLAY-03)");
            {
                const auto session = freshDir ("songs");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("01 SNARE.wav"), 0.1f, 4800));
                expect (writeDc (audio.getChildFile ("02 KiCK_part02.wav"), 0.1f, 4800));
                AudioEngine engine;
                engine.loadSession (session);
                engine.setStripCount (2);
                engine.setTrackName (1, "KiCK");
                engine.setActiveSessionDir (session);
                expect (engine.pasteClip (1, 0, 0, 4800, 0, 0, 0.0f, "x") >= 0);
                const auto voices = engine.songScanVoices();
                double rate = 48000.0;
                bool failed = false;
                juce::String unreadable;
                songs::scanVoices (voices, {}, rate, nullptr, &failed, &unreadable);
                expect (failed);
                expect (unreadable.contains ("strip 2") && unreadable.contains ("KiCK"), "named: \"" + unreadable + "\"");
                engine.clearSessionState();
                engine.clearAllStripOverrides();
                session.deleteRecursively();
            }
        }

        void legacyNamesNotice()
        {
            beginTest ("A named strip continuing a Track_NN take is listed for the notice; nothing is renamed");
            {
                const auto session = freshDir ("notice");
                const auto audio = session.getChildFile ("Audio Files");
                expect (writeDc (audio.getChildFile ("Track_01.wav"), 0.1f, 4 * kBlock));
                expect (writeDc (audio.getChildFile ("Track_02.wav"), 0.1f, 4 * kBlock));
                expect (writeDc (audio.getChildFile ("03 HAT.wav"), 0.1f, 4 * kBlock));
                const auto pass = record (session, { "KiCK", "2", "HAT" }, 2,
                                          [] (MultitrackRecorder& rec) { rec.armContinue (4 * kBlock); });
                expect (pass.started, pass.error);
               #ifndef ZF_RED_BASELINE
                expect (pass.keepLegacy == std::vector<int> { 0 }, "only strip 1 has a name its files lack");
               #else
                expect (false, "no notice exists");
               #endif
                expect (audio.getChildFile ("Track_01_part02.wav").existsAsFile(), namesIn (audio).joinIntoString (" | "));
                expect (! audio.getChildFile ("01 KiCK.wav").exists());
                session.deleteRecursively();
            }
        }
    };

    static TakeNamesFixTests takeNamesFixTests;
}
