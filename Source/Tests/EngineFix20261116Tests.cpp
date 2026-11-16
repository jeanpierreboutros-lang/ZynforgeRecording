// Engine fixes from the 2026-11-16 review: what a strip move or delete may do
// (a session whose drive is gone, a hash pass still reading the take, the
// project's media lengths, the report's file names, clip evidence, the LTC
// strip), the ramps a cue recall runs, Use Take Stamp at an unknown frame rate,
// a project save beside playback, a remote PLAY from a placed cursor, and a
// marker asked for while a helper take has gone quiet. The device restart in
// place is in EngineDeviceTests, beside the rig it needs.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/SessionMixFile.h"
#include "../Network/DeskMacro.h"
#include "SilentAudioDevice.h"

#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <thread>

namespace zynforge
{
class EngineFix20261116Tests final : public juce::UnitTest
{
public:
    EngineFix20261116Tests() : juce::UnitTest ("Engine fixes 2026-11-16", "zynforge") {}

    static constexpr double kRate = 48000.0;

    struct Session
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("zf-engfix1116-" + juce::Uuid().toString());
        Session()  { dir.getChildFile ("Audio Files").createDirectory(); }
        ~Session() { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); }
        juce::File take (int n) const
        { return dir.getChildFile ("Audio Files/" + juce::String::formatted ("Track_%02d.wav", n)); }
        juce::File report() const  { return dir.getChildFile ("session.report.json"); }
        juce::File project() const { return dir.getChildFile (dir.getFileName() + ".zfproj"); }
    };

    static bool writeTake (const juce::File& file, float value, int samples = 4800,
                           const juce::StringPairArray& metadata = {})
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
            file.createOutputStream().release(), kRate, 1, 24, metadata, 0));
        if (writer == nullptr) return false;
        juce::AudioBuffer<float> audio (1, samples);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), value, samples);
        return writer->writeFromAudioSampleBuffer (audio, 0, samples);
    }

    static float firstSample (const juce::File& file)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));
        if (reader == nullptr) return -1.0f;
        juce::AudioBuffer<float> one (1, 1);
        reader->read (&one, 0, 1, 0, true, false);
        return one.getSample (0, 0);
    }

    static bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 10000)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
        return done();
    }

    // Two strips, A and B, that are this session's mixer, with a take each.
    static void openTwoStrips (AudioEngine& e, const Session& s)
    {
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (2);
        e.setTrackName (0, "A");
        e.setTrackName (1, "B");
        e.setActiveSessionDir (s.dir);
        e.claimMixerFor (s.dir);
        e.saveSessionMixTo (s.dir);
        writeTake (s.take (1), 0.25f);
        writeTake (s.take (2), 0.5f);
    }

    static void closeEngine (AudioEngine& e)
    {
        e.clearSessionState();
        e.setStripCount (0);
        e.clearAllStripOverrides();
    }

    static juce::String nameOf (AudioEngine& e, int i)
    {
        return i < e.getRecorder().getNumTracks() ? e.getRecorder().getTrack (i).getNameThreadSafe() : juce::String();
    }

    // A report as STOP leaves it, one entry per strip.
    static void writeReport (const Session& s, bool hashesPending)
    {
        juce::Array<juce::var> tracks;
        const char* names[] = { "A", "B" };
        for (int i = 0; i < 2; ++i)
        {
            auto* t = new juce::DynamicObject();
            t->setProperty ("index", i);
            t->setProperty ("name", names[i]);
            t->setProperty ("clipCount", i == 0 ? 3 : 0);
            t->setProperty ("lastClipSample", i == 0 ? 1200 : -1);
            t->setProperty ("totalSamplesPrimary", 4800);
            t->setProperty ("files", juce::Array<juce::var> { juce::var (s.take (i + 1).getFileName()) });
            t->setProperty ("sha256", juce::Array<juce::var> { juce::var (hashesPending ? "" : (i == 0 ? "hash-a" : "hash-b")) });
            tracks.add (juce::var (t));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("stoppedAt", juce::Time::getCurrentTime().toISO8601 (true));
        root->setProperty ("numTracks", 2);
        root->setProperty ("sampleRate", kRate);
        root->setProperty ("sha256Pending", hashesPending);
        root->setProperty ("tracks", tracks);
        s.report().replaceWithText (juce::JSON::toString (juce::var (root), true));
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        moveRefusedWithTheDriveGone();          // ENG-01
        everyRefusalSaysWhy();                  // ENG-08 = MIX-06
        stripCallersShowTheReason();            // ENG-08 = MIX-06, MainComponentStrips.cpp
        moveWaitsForTheHashPass();              // CAP-02
        moveRewritesTheReport();                // CAP-02
        moveCarriesMediaLengths();              // ENG-04
        moveCarriesClipEvidence();              // ENG-09
        gainAndPanRampApart();                  // ENG-05 = PLAY-05
        ltcStripBelongsToItsSession();          // ENG-06
        takeStampWaitsForAFrameRate();          // ENG-03
        projectSaveNeverWaitsOnTheRender();     // MEDIALEN-LOCK
        remotePlayStartsAtTheCursor();          // REMOTE-PLAY-CURSOR
        helperMarkerRefusalIsTruthful();        // HELPER-MARKERS
    }

private:
    // ── ENG-01 ─────────────────────────────────────────────────────────
    void moveRefusedWithTheDriveGone()
    {
        beginTest ("ENG-01: a strip move or delete is refused while the session's drive is gone");
        Session s;
        AudioEngine e;
        openTwoStrips (e, s);
        const auto unplugged = s.dir.getSiblingFile (s.dir.getFileName() + "-unplugged");
        expect (s.dir.moveFileTo (unplugged), "fixture: could not take the session away");

        expect (! e.reorderTracks ({ 1, 0 }), "the strips moved in memory with the take files out of reach");
        expectEquals (nameOf (e, 0), juce::String ("A"));
        expect (e.getReorderRefusal().containsIgnoreCase ("missing"), "no reason: " + e.getReorderRefusal());
        expect (! e.reorderTracks ({ 1 }), "a strip was deleted with the take files out of reach");
        expectEquals (e.getRecorder().getNumTracks(), 2);

        // Back on its drive, the same move runs and the files follow.
        expect (unplugged.moveFileTo (s.dir), "fixture: could not put the session back");
        expect (e.reorderTracks ({ 1, 0 }), e.getReorderRefusal());
        expectEquals (nameOf (e, 0), juce::String ("B"));
        expectWithinAbsoluteError (firstSample (s.take (1)), 0.5f, 0.001f, "strip 1's file is not B's take");
        closeEngine (e);
        unplugged.deleteRecursively();
    }

    // ── ENG-08 = MIX-06 ────────────────────────────────────────────────
    void everyRefusalSaysWhy()
    {
        beginTest ("ENG-08: every strip move the engine refuses says why");
        {
            Session s;
            AudioEngine e;
            openTwoStrips (e, s);
            e.claimMixerFor ({});   // a mixer that is not this session's
            expect (! e.reorderTracks ({ 1, 0 }));
            expect (e.getReorderRefusal().isNotEmpty(), "a foreign mixer was refused without a reason");
            closeEngine (e);
        }
        {
            Session s;
            AudioEngine e;
            openTwoStrips (e, s);
            const auto marker = s.dir.getChildFile ("Audio Files/Pending Lead-ins/Track_01.json");
            marker.getParentDirectory().createDirectory();
            marker.replaceWithText ("{}");
            expect (! e.reorderTracks ({ 1, 0 }));
            expect (e.getReorderRefusal().containsIgnoreCase ("lead-in"),
                    "an owed lead-in was refused without a reason: " + e.getReorderRefusal());
            expect (! e.reorderTracks ({}), "a saved session was emptied");
            expect (e.getReorderRefusal().isNotEmpty(), "emptying a saved session was refused without a reason");
            closeEngine (e);
        }
    }

    void stripCallersShowTheReason()
    {
        beginTest ("ENG-08: a refused strip delete shows the engine's reason, not disk permissions");
        MainComponent::s_testConstruct = true;
        const juce::ScopeGuard restore { [] { MainComponent::s_testConstruct = false; } };
        Session s;
        MainComponent main;
        main.stopTimer();
        auto& e = main.engine;
        openTwoStrips (e, s);
        const auto marker = s.dir.getChildFile ("Audio Files/Pending Lead-ins/Track_01.json");
        marker.getParentDirectory().createDirectory();
        marker.replaceWithText ("{}");

        main.selectedLogical = { 0 };
        main.deleteSelectedStrips();
        auto shown = main.statusLabel.getText();
        expect (! shown.containsIgnoreCase ("disk permissions"), "Delete blamed the disk: " + shown);
        expect (shown.containsIgnoreCase ("lead-in"), "Delete did not say why: " + shown);
        expectEquals (e.getRecorder().getNumTracks(), 2);

        main.removeStrip (1, 1);
        shown = main.statusLabel.getText();
        expect (shown.containsIgnoreCase ("lead-in"), "Remove did not say why: " + shown);
        expectEquals (e.getRecorder().getNumTracks(), 2);
        closeEngine (e);
    }

    // ── CAP-02 ─────────────────────────────────────────────────────────
    void moveWaitsForTheHashPass()
    {
        beginTest ("CAP-02: a strip move waits for this session's hash pass, and says it ends on its own");
        Session s;
        AudioEngine e;
        openTwoStrips (e, s);
        writeReport (s, true);
        juce::WaitableEvent atPublish, release, finished;
        MultitrackRecorder::resumeReportHashes (s.dir, [&] { finished.signal(); },
                                                [&] { atPublish.signal(); release.wait (20000); });
        expect (atPublish.wait (10000), "fixture: the hash pass never reached its publish");

        expect (! e.reorderTracks ({ 1, 0 }), "a strip moved while the hash pass was reading the take");
        expectEquals (nameOf (e, 0), juce::String ("A"));
        expect (e.getReorderRefusal().containsIgnoreCase ("hash"), "no reason: " + e.getReorderRefusal());
        expect (s.take (1).existsAsFile() && s.take (2).existsAsFile());

        release.signal();
        expect (finished.wait (10000), "fixture: the hash pass never ended");
        const auto report = juce::JSON::parse (s.report());
        expect (! (bool) report.getProperty ("sha256Failed", true), "the hash pass failed an intact take");
        expect (e.reorderTracks ({ 1, 0 }), "the move was still refused after the pass: " + e.getReorderRefusal());
        closeEngine (e);
    }

    void moveRewritesTheReport()
    {
        beginTest ("CAP-02: a strip move rewrites session.report.json with the files' new names and places");
        Session s;
        AudioEngine e;
        openTwoStrips (e, s);
        writeReport (s, false);
        expect (e.reorderTracks ({ 1, 0 }), e.getReorderRefusal());
        auto report = juce::JSON::parse (s.report());
        auto* tracks = report.getProperty ("tracks", {}).getArray();
        expect (tracks != nullptr && tracks->size() == 2);
        if (tracks != nullptr && tracks->size() == 2)
        {
            const auto& first = tracks->getReference (0);
            const auto& second = tracks->getReference (1);
            expectEquals (first.getProperty ("name", "").toString(), juce::String ("B"), "the report still lists A first");
            expectEquals ((int) first.getProperty ("index", -1), 0);
            expectEquals (first.getProperty ("files", {})[0].toString(), juce::String ("Track_01.wav"));
            expectEquals (first.getProperty ("sha256", {})[0].toString(), juce::String ("hash-b"));
            expectEquals (second.getProperty ("name", "").toString(), juce::String ("A"));
            expectEquals ((int) second.getProperty ("index", -1), 1);
            expectEquals (second.getProperty ("files", {})[0].toString(), juce::String ("Track_02.wav"));
            expectEquals (second.getProperty ("sha256", {})[0].toString(), juce::String ("hash-a"));
            expectEquals ((int) second.getProperty ("clipCount", -1), 3, "A's clips stayed at strip 1");
        }

        // A delete drops the strip from the report; the next strip takes its place.
        expect (e.reorderTracks ({ 1 }), e.getReorderRefusal());
        report = juce::JSON::parse (s.report());
        tracks = report.getProperty ("tracks", {}).getArray();
        expect (tracks != nullptr && tracks->size() == 1, "a deleted strip is still in the report");
        if (tracks != nullptr && tracks->size() == 1)
        {
            const auto& only = tracks->getReference (0);
            expectEquals (only.getProperty ("name", "").toString(), juce::String ("A"));
            expectEquals (only.getProperty ("files", {})[0].toString(), juce::String ("Track_01.wav"));
            expectEquals ((int) only.getProperty ("index", -1), 0);
        }
        expectEquals ((int) report.getProperty ("numTracks", -1), 1);
        closeEngine (e);
    }

    // ── ENG-04 ─────────────────────────────────────────────────────────
    void moveCarriesMediaLengths()
    {
        beginTest ("ENG-04: a strip move or delete carries the project's media lengths with the strips");
        Session s;
        AudioEngine e;
        openTwoStrips (e, s);
        s.project().replaceWithText (R"({"zynforgeSession":true,"playlists":[],"mediaLengths":[1000,2000]})");
        expect (e.reorderTracks ({ 1, 0 }), e.getReorderRefusal());
        auto lengths = juce::JSON::parse (s.project()).getProperty ("mediaLengths", {});
        expect (lengths.size() == 2);
        expectEquals ((int) lengths[0], 2000, "strip 1 kept A's media length");
        expectEquals ((int) lengths[1], 1000);
        expect (e.reorderTracks ({ 1 }), e.getReorderRefusal());
        lengths = juce::JSON::parse (s.project()).getProperty ("mediaLengths", {});
        expect (lengths.size() == 1, "a deleted strip's length stayed");
        expectEquals ((int) lengths[0], 1000);
        closeEngine (e);
    }

    // ── ENG-09 ─────────────────────────────────────────────────────────
    void moveCarriesClipEvidence()
    {
        beginTest ("ENG-09: a strip move carries its clip evidence with the strip");
        AudioEngine e;
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (2);
        auto& a = e.getRecorder().getTrack (0);
        a.clipped.store (true);
        a.clipCount.store (5);
        a.lastClipSample.store (1234);
        expect (e.reorderTracks ({ 1, 0 }), e.getReorderRefusal());
        auto& moved = e.getRecorder().getTrack (1);
        auto& stayed = e.getRecorder().getTrack (0);
        expectEquals (moved.clipCount.load(), 5, "CLIP x5 stayed at the strip's old place");
        expect (moved.clipped.load());
        expectEquals (moved.lastClipSample.load(), (juce::int64) 1234);
        expectEquals (stayed.clipCount.load(), 0, "the strip moved up took over another strip's clips");
        expect (! stayed.clipped.load());
        closeEngine (e);
    }

    // ── ENG-05 = PLAY-05 ───────────────────────────────────────────────
    void gainAndPanRampApart()
    {
        beginTest ("ENG-05: a fader move during a cue ramp leaves the pan ramp to finish");
        AudioEngine e;
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (2);
        auto& t = e.getRecorder().getTrack (0);
        e.setTrackPan (0, -0.5f);
        e.setTrackPanRamped (0, 0.5f, 0.25);
        e.setTrackGainDbRamped (0, -6.0f, 0.25);
        e.tickRamps (1200);
        e.setTrackGainDb (0, -3.0f);   // the engineer's fader, 25 ms in
        e.tickRamps (48000);
        expectWithinAbsoluteError (t.pan.load(), 0.5f, 0.001f, "the pan stopped part-way");
        expectWithinAbsoluteError (t.gainDb.load(), -3.0f, 0.001f);

        beginTest ("ENG-05: a gain ramp never moves a pan");
        auto& r = e.getRecorder().getTrack (1);
        e.setTrackPanRamped (1, 1.0f, 0.0);   // an old cue's pan, long done
        e.setTrackPan (1, -0.2f);             // a pan set by hand since
        e.setTrackGainDbRamped (1, -6.0f, 0.25);
        e.tickRamps (48000);
        expectWithinAbsoluteError (r.pan.load(), -0.2f, 0.001f, "a gain ramp swung the pan to a stale target");
        expectWithinAbsoluteError (r.gainDb.load(), -6.0f, 0.001f);

        beginTest ("ENG-05: a pan set by hand ends only the pan ramp");
        e.setTrackPanRamped (0, -1.0f, 0.25);
        e.setTrackGainDbRamped (0, -12.0f, 0.25);
        e.tickRamps (1200);
        e.setTrackPan (0, 0.25f);
        e.tickRamps (48000);
        expectWithinAbsoluteError (t.pan.load(), 0.25f, 0.001f, "a hand-set pan was ramped away");
        expectWithinAbsoluteError (t.gainDb.load(), -12.0f, 0.001f, "a hand-set pan stopped the gain ramp");
        closeEngine (e);
    }

    // ── ENG-06 ─────────────────────────────────────────────────────────
    void ltcStripBelongsToItsSession()
    {
        beginTest ("ENG-06: the LTC strip belongs to its session, by strip");
        AudioEngine e;
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (3);
        Session a;
        e.setActiveSessionDir (a.dir);
        e.claimMixerFor (a.dir);
        e.setLtcSourceStrip (2);
        expect (e.saveSessionMixTo (a.dir));

        // Another show with three strips of its own.
        Session b;
        {
            juce::Array<juce::var> strips;
            for (int i = 0; i < 3; ++i)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("index", i);
                o->setProperty ("uid", "other-show-" + juce::String (i));
                o->setProperty ("name", "Other " + juce::String (i + 1));
                strips.add (juce::var (o));
            }
            auto* root = new juce::DynamicObject();
            root->setProperty ("formatVersion", 2);
            root->setProperty ("trackCount", 3);
            root->setProperty ("strips", strips);
            b.dir.getChildFile (sessionmix::kFileName).replaceWithText (juce::JSON::toString (juce::var (root)));
        }
        e.clearSessionState();
        expect (e.loadSessionMixFrom (b.dir));
        expectEquals (e.getLtcSourceStrip(), 0, "another show's strip 2 feeds the timecode decoder");

        e.clearSessionState();
        expect (e.loadSessionMixFrom (a.dir));
        expectEquals (e.getLtcSourceStrip(), 2, "the show's own LTC strip was not restored");
        e.setStripCount (4);
        expectEquals (e.getLtcSourceStrip(), 2, "adding a strip lost the LTC strip");

        // New Session: a clean slate of new strips.
        e.clearSessionState();
        e.clearAllStripOverrides();
        e.setStripCount (3);
        expectEquals (e.getLtcSourceStrip(), 0, "a new session's strip 2 feeds the timecode decoder");
        closeEngine (e);
    }

    // ── ENG-03 ─────────────────────────────────────────────────────────
    void takeStampWaitsForAFrameRate()
    {
        beginTest ("ENG-03: Use Take Stamp refuses until a frame rate is known, then converts at it");
        Session s;
        // 10:00:30.5 into the day.
        const auto stamp = (juce::int64) (kRate * (10 * 3600 + 30)) + (juce::int64) (kRate / 2);
        expect (writeTake (s.take (1), 0.25f, 4800,
                           juce::WavAudioFormat::createBWAVMetadata ("", "", "", juce::Time::getCurrentTime(), stamp, "")));
        AudioEngine e;
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (1);
        e.setActiveSessionDir (s.dir);
        expectEquals (e.loadSession (s.dir), 1);
        expect (! e.setChaseStartFromTake(), "the stamp was converted at a guessed 30 fps");
        expectEquals (e.getChaseStartTimecode(), juce::String ("00:00:00:00"));

        e.getTimecodeChase().feedMtcFullFrame (1, 0, 0, 0, 25.0f, false);
        expect (e.setChaseStartFromTake(), "a known rate was refused");
        expectEquals (e.getChaseStartTimecode(), juce::String ("10:00:30:12"), "not converted at 25 fps");
        closeEngine (e);
    }

    // ── MEDIALEN-LOCK ──────────────────────────────────────────────────
    void projectSaveNeverWaitsOnTheRender()
    {
        beginTest ("MEDIALEN-LOCK: a project save reads the media lengths without the render's lock");
        Session s;
        writeTake (s.take (1), 0.25f, 4800);
        writeTake (s.take (2), 0.5f, 9600);
        AudioEngine e;
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (2);
        expectEquals (e.loadSession (s.dir), 2);

        std::atomic<bool> held { false }, done { false };
        juce::WaitableEvent letGo;
        auto& lock = e.getPlayer().tracksLock;
        std::thread render ([&] { const juce::ScopedLock sl (lock); held = true; letGo.wait (5000); });
        while (! held.load()) juce::Thread::sleep (1);
        juce::var lengths;
        std::thread save ([&] { lengths = e.mediaLengthsToJson(); done = true; });
        for (int i = 0; i < 300 && ! done.load(); ++i) juce::Thread::sleep (1);
        expect (done.load(), "the save waited on the player's track lock, which the render try-locks");
        letGo.signal();
        render.join();
        save.join();
        expect (lengths.size() == 2);
        expectEquals ((int) lengths[0], 4800);
        expectEquals ((int) lengths[1], 9600);
        closeEngine (e);
    }

    // ── REMOTE-PLAY-CURSOR ─────────────────────────────────────────────
    void remotePlayStartsAtTheCursor()
    {
        beginTest ("REMOTE-PLAY-CURSOR: a remote PLAY starts where PLAY would, at a placed EDIT cursor");
        Session s;
        writeTake (s.take (1), 0.25f, 48000);
        AudioEngine e;
        testaudio::useSilentDevice (e, kRate, 2);
        e.prepareForTests (kRate, testaudio::kSilentBlock);
        e.clearAllStripOverrides();
        e.setStripCount (1);
        expectEquals (e.loadSession (s.dir), 1);
        e.getPlayer().setPositionSamples (0);
        e.setEditCursorSample (24000);
        juce::String error;
        expect (e.performRemoteTransport (AudioEngine::RemoteTransportAction::StartPlay, error), error);
        expectEquals (e.getPlayer().getPositionSamples(), (juce::int64) 24000, "a remote PLAY ignored the cursor");
        e.stopPlayback();

        // No cursor: it plays on from where the player is.
        e.clearEditCursor();
        e.getPlayer().setPositionSamples (12000);
        expect (e.performRemoteTransport (AudioEngine::RemoteTransportAction::TogglePlay, error), error);
        expectEquals (e.getPlayer().getPositionSamples(), (juce::int64) 12000);
        e.stopPlayback();
        e.getDeviceManager().closeAudioDevice();
        closeEngine (e);
    }

    // ── HELPER-MARKERS ─────────────────────────────────────────────────
    void helperMarkerRefusalIsTruthful()
    {
        beginTest ("HELPER-MARKERS: a marker during a helper take gone quiet is refused, and the remotes say why");
        Session s;
        AudioEngine e;
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (1);
        e.getMarkers().setContext (s.dir, kRate);
        e.beginExternalTake (s.dir, kRate);
        EngineStatus status;
        status.source = "daemon";
        status.recording = true;
        status.sampleRate = kRate;
        status.positionSamples = (juce::int64) (kRate * 60);
        e.setExternalCaptureStatus (status, juce::Time::currentTimeMillis() - 5000);
        juce::StringArray shown;
        e.getRemotes().setHost ({ [&shown] (const juce::String& m) { shown.add (m); }, {}, {} });

        const auto action = e.getRemotes().onConsoleScene (3, "Chorus", scenefollow::Path::OscListener);
        expect (action == scenefollow::Action::DropMarker);
        expectEquals (e.getMarkers().getCount(), 0, "a marker landed at an unknown position");
        const auto sceneText = shown.joinIntoString (" | ");
        expect (sceneText.containsIgnoreCase ("helper"), "the scene path said nothing true: " + sceneText);

        shown.clear();
        DeskMacroListener macro (e);
        macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker"), 10000.0);
        const auto macroText = shown.joinIntoString (" | ");
        expect (! macroText.containsIgnoreCase ("no session"), "the desk macro blamed the session: " + macroText);
        expect (macroText.containsIgnoreCase ("helper"), "the desk macro did not say why: " + macroText);
        expectEquals (e.getMarkers().getCount(), 0);

        // A helper that reports again takes markers again.
        e.setExternalCaptureStatus (status, juce::Time::currentTimeMillis());
        expect (e.dropMarkerAtCurrentPosition ("Fresh") > 0, "a fresh status still refused the marker");
        e.setExternalRecording (false);
        e.getRemotes().setHost ({});
        closeEngine (e);
    }
};

static EngineFix20261116Tests engineFix20261116Tests;
}
