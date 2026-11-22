// Recorder fixes from the 2026-11-22 audit. A pass never lands on take parts
// whose main file is gone (a fresh take, a continue, a punch, and a backup that
// holds such parts); a lead-in reserves only free part numbers, and an
// abandoned start or a dropped lead-in removes only files that pass made; a
// punch's lead-in marker says where capture began, during the take and when
// STOP cannot write the silence; and a punch recovery that fails says so.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/MultitrackRecorder.h"
#include "../Audio/CaptureCopy.h"
#include "../Audio/MultiPartReader.h"

namespace zynforge
{
class Fix20261122RecorderTests final : public juce::UnitTest
{
public:
    Fix20261122RecorderTests() : juce::UnitTest ("Fixes 2026-11-22 recorder", "zynforge") {}

    static constexpr double kRate = 48000.0;
    static constexpr int kBlock = 512;
    using Copy = MultitrackRecorder::CaptureCopy;
    using Lead = MultitrackRecorder::PendingLeadIn;

    struct Folder
    {
        juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("zf-fix1122-" + juce::Uuid().toString());
        Folder()  { root.createDirectory(); }
        ~Folder()
        {
            MultitrackRecorder::deleteSessionAfterCancellingReports (session());
            root.deleteRecursively();
        }
        juce::File session() const     { return root.getChildFile ("session"); }
        juce::File audio() const       { return session().getChildFile ("Audio Files"); }
        juce::File backup() const      { return root.getChildFile ("backup"); }
        juce::File backupAudio() const { return backup().getChildFile ("session").getChildFile ("Audio Files"); }
    };

    static bool writeWav (const juce::File& file, float value, int samples)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
            file.createOutputStream().release(), kRate, 1, 24, {}, 0));
        if (writer == nullptr) return false;
        juce::AudioBuffer<float> audio (1, samples);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), value, samples);
        return writer->writeFromAudioSampleBuffer (audio, 0, samples);
    }

    static juce::int64 fileLength (const juce::File& file)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        return reader != nullptr ? reader->lengthInSamples : -1;
    }

    static float firstSample (const juce::File& file)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr) return 999.0f;
        juce::AudioBuffer<float> one (1, 1);
        reader->read (&one, 0, 1, 0, true, false);
        return one.getSample (0, 0);
    }

    // The whole take, its parts stitched, as playback reads it.
    static juce::int64 takeLength (const juce::File& main)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        const auto reader = ConcatReader::create (fm, findTakeParts (main));
        return reader != nullptr ? reader->lengthInSamples : -1;
    }

    static float takeMean (const juce::File& main, juce::int64 start, juce::int64 length)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        const auto reader = ConcatReader::create (fm, findTakeParts (main));
        if (reader == nullptr || length <= 0 || start + length > reader->lengthInSamples) return 999.0f;
        juce::AudioBuffer<float> audio (1, (int) length);
        reader->read (&audio, 0, (int) length, start, true, false);
        double sum = 0.0;
        for (int i = 0; i < (int) length; ++i) sum += audio.getSample (0, i);
        return (float) (sum / (double) length);
    }

    static juce::int64 markerStart (const juce::File& marker)
    {
        const auto v = juce::JSON::parse (marker);
        return v.isObject() ? (juce::int64) v["timelineStart"] : -1;
    }

    static bool waitFor (const std::function<bool()>& done, int timeoutMs = 5000)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
            juce::Thread::sleep (2);
        return done();
    }

    // `blocks` blocks of DC on every channel; playhead >= 0 is where on the
    // timeline the first one sits (a rolling punch).
    static void feed (MultitrackRecorder& rec, int channels, float value, int blocks, juce::int64 playhead = -1)
    {
        std::vector<float> buffer ((size_t) kBlock, value);
        std::vector<const float*> inputs ((size_t) channels, buffer.data());
        for (int b = 0; b < blocks; ++b)
            rec.processBlock (inputs.data(), channels, kBlock,
                              playhead >= 0 ? playhead + (juce::int64) b * kBlock : -1);
        rec.drainPendingForTests();
    }

    // A first take of DC `value` on track 1, on the backup too when one is given.
    void recordBase (const juce::File& session, int blocks, float value, const juce::File& backup = {})
    {
        MultitrackRecorder rec;
        rec.prepare (kRate, kBlock, 1);
        if (backup != juce::File()) expect (rec.setBackupDirectory (backup));
        rec.getTrack (0).armed.store (true);
        expect (rec.startRecording (session), "fixture: the base take would not start: " + rec.getStartError());
        rec.waitForCopiesOpenForTests();
        feed (rec, 1, value, blocks);
        rec.stopRecording();
    }

    void runTest() override
    {
        strayPartsRefuseAPunch();
        strayPartsRefuseEveryPass();
        strayPartsOnABackup();
        leadInsOnlyRemoveTheirOwn();
        abandonedStartRemovesItsOwn();
        markerFollowsCaptureOrigin();
        stopKeepsCaptureOriginInMarker();
        failedRecoverySaysSo();
    }

    // ── Fix 1 (critical) ───────────────────────────────────────────────
    void strayPartsRefuseAPunch()
    {
        beginTest ("Fix 1: a punch on a strip with parts but no main file is refused, and the part stays");
        Folder f;
        expect (writeWav (f.audio().getChildFile ("Track_01.wav"), 0.5f, 90 * kBlock));
        const auto stray = f.audio().getChildFile ("Track_02_part02.wav");
        expect (writeWav (stray, 0.4f, 4800));

        MultitrackRecorder rec;
        rec.prepare (kRate, kBlock, 2);
        rec.getTrack (0).armed.store (true);
        rec.getTrack (1).armed.store (true);   // no main file: the punch would reserve a lead-in at _part02
        rec.armPunchIn (20 * kBlock);
        expect (! rec.startRecording (f.session()), "a punch started over parts with no main file");
        const auto why = rec.getStartError();
        expect (why.contains ("track 2") && why.contains ("Track_02_part02.wav") && why.contains ("main file"),
                "the refusal does not name the strip and its parts: " + why);
        expect (! rec.isPunchInArmed(), "a refused punch kept its arm");

        expect (stray.existsAsFile(), "the earlier part was deleted");
        expectEquals (fileLength (stray), (juce::int64) 4800);
        expectWithinAbsoluteError (firstSample (stray), 0.4f, 1.0e-3f);
        expect (! f.audio().getChildFile ("Track_02.wav").exists());
        expect (! f.audio().getChildFile ("Track_02_part03.wav").exists());
        expect (! f.audio().getChildFile ("Pending Lead-ins").exists(), "a lead-in marker was left behind");
        expect (! f.audio().getChildFile ("Track_01.punchbase.wav").exists(), "the refusal came after a take moved");
        expectWithinAbsoluteError (firstSample (f.audio().getChildFile ("Track_01.wav")), 0.5f, 1.0e-3f);
    }

    void strayPartsRefuseEveryPass()
    {
        beginTest ("Fix 1: a fresh take, a continue and a punch all refuse a strip whose main file is gone");
        Folder f;
        const auto stray = f.audio().getChildFile ("05 KiCK_part02.wav");
        expect (writeWav (stray, 0.4f, 4800));

        for (int mode = 0; mode < 3; ++mode)
        {
            const juce::String label = mode == 0 ? "fresh" : mode == 1 ? "continue" : "punch";
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 5);
            rec.getTrack (4).armed.store (true);
            if (mode == 1) rec.armContinue (0);
            if (mode == 2) rec.armPunchIn (4096);
            expect (! rec.startRecording (f.session()), label + ": started over parts with no main file");
            const auto why = rec.getStartError();
            expect (why.contains ("track 5") && why.contains ("05 KiCK_part02.wav"), label + ": " + why);
            expect (why.contains ("move those part files"), label + " does not say what to do: " + why);
            expect (! rec.isPunchInArmed() && ! rec.isContinueArmed(), label + ": an arm outlived the refusal");

            expect (stray.existsAsFile(), label + ": the part was deleted");
            expectWithinAbsoluteError (firstSample (stray), 0.4f, 1.0e-3f);
            expect (! f.audio().getChildFile ("05 KiCK.wav").exists(),
                    label + ": a new main file would play the old part as its tail");
            expect (! f.audio().getChildFile ("05 KiCK_part03.wav").exists(), label);
            expect (! f.audio().getChildFile ("Pending Lead-ins").exists(), label);
        }
    }

    void strayPartsOnABackup()
    {
        beginTest ("Fix 1: a backup holding parts with no main file leaves the take and keeps them");
        Folder f;
        f.backup().createDirectory();
        recordBase (f.session(), 90, 0.5f, f.backup());
        const auto stray = f.backupAudio().getChildFile ("Track_02_part02.wav");
        expect (f.backupAudio().getChildFile ("Track_01.wav").existsAsFile(), "fixture: the backup has no base take");
        expect (writeWav (stray, 0.4f, 4800));

        const juce::int64 armedAt = 20 * kBlock;
        MultitrackRecorder rec;
        rec.prepare (kRate, kBlock, 2);
        expect (rec.setBackupDirectory (f.backup()));
        rec.getTrack (0).armed.store (true);
        rec.getTrack (1).armed.store (true);   // new on the primary: a lead-in there and on the backup
        rec.armPunchIn (armedAt);
        expect (rec.startRecording (f.session()), "the primary was refused for the backup's files: " + rec.getStartError());
        rec.waitForCopiesOpenForTests();
        feed (rec, 2, -0.3f, 8, armedAt);
        rec.stopRecording();

        expect (stray.existsAsFile(), "STOP deleted the backup's earlier part");
        expectEquals (fileLength (stray), (juce::int64) 4800);
        expectWithinAbsoluteError (firstSample (stray), 0.4f, 1.0e-3f);
        expect (! f.backupAudio().getChildFile ("Track_02.wav").exists());
        expect (rec.getCopyProblems().joinIntoString (" | ").contains ("part files"),
                "the backup left without saying why: " + rec.getCopyProblems().joinIntoString (" | "));

        // The primary's punch and new track are not held back by it.
        const auto t1 = f.audio().getChildFile ("Track_01.wav");
        expectWithinAbsoluteError (takeMean (t1, armedAt, 8 * kBlock), -0.3f, 1.0e-3f);
        const auto t2 = f.audio().getChildFile ("Track_02.wav");
        expectEquals (takeLength (t2), armedAt + 8 * kBlock);
        expectWithinAbsoluteError (takeMean (t2, 0, armedAt), 0.0f, 1.0e-6f);
    }

    void leadInsOnlyRemoveTheirOwn()
    {
        beginTest ("Fix 1: a lead-in reserves only free part numbers, and a drop removes only its own part");
        Folder f;
        const auto dir = f.root.getChildFile ("Audio Files");
        dir.createDirectory();
        const auto newCopy = []
        {
            auto copy = std::make_shared<Copy>();
            copy->sampleRate  = kRate;
            copy->sessionName = "session";
            copy->passOrigin  = juce::Time::getCurrentTime();
            copy->latches     = std::make_shared<MultitrackRecorder::CaptureLatches>();
            copy->tracks.resize (2);
            return copy;
        };
        const MultitrackRecorder::LeadInPlan punch { true, false, true, (juce::int64) kRate, kRate };

        // The audio part is already on the disk: the track does not open, and
        // nothing is reserved that a drop or an abandon could later remove.
        {
            const auto old = dir.getChildFile ("Track_02_part02.wav");
            expect (writeWav (old, 0.4f, 4800));
            auto copy = newCopy();
            std::vector<Lead> leads;
            expect (! MultitrackRecorder::openTrackFile (*copy, 1, dir, { 0, 24, ".wav" }, "Track_02", 1,
                                                         punch, false, 0, leads),
                    "a track opened on a part it did not create");
            expect (leads.empty(), "a lead-in was reserved over an existing part");
            expect (copy->tracks[1].partFiles.isEmpty());
            expectWithinAbsoluteError (firstSample (old), 0.4f, 1.0e-3f);

            // A lead that does not own its part, as one read back from a marker.
            Lead lead;
            lead.audioDir = dir; lead.trackName = "Track_02"; lead.ext = ".wav";
            lead.firstPart = 1;  lead.parts = 1;
            std::vector<MultitrackRecorder::WriterChannel::StampedPart> stamps;
            expect (MultitrackRecorder::dropEmptyLeadIn (lead, stamps));
            expect (old.existsAsFile(), "a drop deleted a part this pass did not create");
            expectWithinAbsoluteError (firstSample (old), 0.4f, 1.0e-3f);
            old.deleteFile();
        }

        // An AIFF lead-in long enough for several slots: one slot is a file
        // already. STOP would have deleted it to write silence there.
        {
            MultitrackRecorder::setAutoSplitThresholdBytesForTests (20000 * 3);
            const juce::ScopeGuard restore { [] { MultitrackRecorder::setAutoSplitThresholdBytesForTests (0); } };
            const auto slot = dir.getChildFile ("Track_02_part02.aif");
            expect (slot.replaceWithText ("someone's audio"));
            auto copy = newCopy();
            std::vector<Lead> leads;
            expect (! MultitrackRecorder::openTrackFile (*copy, 1, dir, { 1, 24, ".aif" }, "Track_02", 1,
                                                         punch, false, 0, leads),
                    "a lead-in reserved a slot that holds a file");
            expect (leads.empty());
            expectEquals (slot.loadFileAsString(), juce::String ("someone's audio"));
            expectEquals (dir.getNumberOfChildFiles (juce::File::findFiles), 1, "the refused track left a file");
            slot.deleteFile();
        }

        // Free slots: the part this pass made is its own, and an empty one goes.
        {
            auto copy = newCopy();
            std::vector<Lead> leads;
            expect (MultitrackRecorder::openTrackFile (*copy, 1, dir, { 0, 24, ".wav" }, "Track_02", 1,
                                                       punch, false, 0, leads));
            expectEquals ((int) leads.size(), 1);
            if (leads.size() == 1)
            {
                expect (leads[0].ownsAudioPart);
                const auto part = dir.getChildFile ("Track_02_part02.wav");
                expect (part.existsAsFile());
                copy->tracks[1].writer.reset();
                expect (MultitrackRecorder::dropEmptyLeadIn (leads[0], copy->stampedParts));
                expect (! part.exists(), "an empty part this pass made was kept");
                expect (copy->stampedParts.empty());
            }
        }
    }

    void abandonedStartRemovesItsOwn()
    {
        beginTest ("Fix 1: an abandoned start removes the lead-in part it made and nothing that was there");
        Folder f;
        const auto base = f.audio().getChildFile ("Track_02.wav");
        expect (writeWav (base, 0.5f, 4800));
        // Strip 2's next part will not open: a folder stands at its name.
        const auto blocker = f.audio().getChildFile ("Track_02_part02.wav");
        expect (blocker.createDirectory().wasOk());
        expect (blocker.getChildFile ("keep").replaceWithText ("x"));

        MultitrackRecorder rec;
        rec.prepare (kRate, kBlock, 2);
        rec.getTrack (0).armed.store (true);   // no take yet: a lead-in up to strip 2's end
        rec.getTrack (1).armed.store (true);
        rec.armContinue (0);
        expect (! rec.startRecording (f.session()), "a start went live with a track that did not open");
        expect (! f.audio().getChildFile ("Track_01_part02.wav").exists(), "the lead-in's own part was left behind");
        expect (! f.audio().getChildFile ("Track_01.wav").exists());
        expect (! f.audio().getChildFile ("Pending Lead-ins/Track_01.json").exists(), "the lead-in's marker was left behind");
        expect (base.existsAsFile());
        expectWithinAbsoluteError (firstSample (base), 0.5f, 1.0e-3f);
        expect (blocker.getChildFile ("keep").existsAsFile());
    }

    // ── Fix 2 ──────────────────────────────────────────────────────────
    void markerFollowsCaptureOrigin()
    {
        beginTest ("Fix 2: a punch lead-in's marker moves to where capture began, during the take");
        Folder f;
        recordBase (f.session(), 90, 0.5f);
        const juce::int64 armedAt = 20 * kBlock;
        const juce::int64 capturedAt = 25 * kBlock;   // start work took five blocks

        MultitrackRecorder rec;
        rec.prepare (kRate, kBlock, 2);
        rec.getTrack (0).armed.store (true);
        rec.getTrack (1).armed.store (true);   // new track: a deferred lead-in
        rec.armPunchIn (armedAt);
        expect (rec.startRecording (f.session()), rec.getStartError());
        const auto marker = f.audio().getChildFile ("Pending Lead-ins/Track_02.json");
        expect (marker.existsAsFile(), "fixture: no lead-in marker");
        expectEquals (markerStart (marker), armedAt, "fixture: the marker did not start at the armed point");

        feed (rec, 2, -0.3f, 4, capturedAt);
        expect (waitFor ([&] { return markerStart (marker) == capturedAt; }),
                "a crash now would line the new track up from where the punch was armed: "
                    + juce::String (markerStart (marker)));

        // What a crash right here would leave: that marker and the audio after
        // it. Recovery lines the audio up where it was played.
        {
            Folder crash;
            const auto audio = crash.root.getChildFile ("Audio Files");
            expect (audio.getChildFile ("Pending Lead-ins").createDirectory().wasOk());
            expect (audio.getChildFile ("Pending Lead-ins/Track_02.json").replaceWithText (marker.loadFileAsString()));
            expect (writeWav (audio.getChildFile ("Track_02_part02.wav"), -0.3f, 4 * kBlock));
            expect (MultitrackRecorder::recoverInterruptedPunches (audio));
            const auto t2 = audio.getChildFile ("Track_02.wav");
            expectEquals (fileLength (t2), capturedAt, "recovery's silence does not reach where capture began");
            expectWithinAbsoluteError (takeMean (t2, capturedAt, 4 * kBlock), -0.3f, 1.0e-3f);
        }

        feed (rec, 2, -0.3f, 4, capturedAt + 4 * kBlock);
        rec.stopRecording();
        const auto t2 = f.audio().getChildFile ("Track_02.wav");
        expectEquals (takeLength (t2), capturedAt + 8 * kBlock);
        expectWithinAbsoluteError (takeMean (t2, 0, capturedAt), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError (takeMean (t2, capturedAt, 8 * kBlock), -0.3f, 1.0e-3f);
        expect (! f.audio().getChildFile ("Pending Lead-ins").exists(), "the marker outlived STOP");
    }

    void stopKeepsCaptureOriginInMarker()
    {
        beginTest ("Fix 2: a lead-in STOP cannot write leaves its marker at where capture began");
        Folder f;
        const auto dir = f.root.getChildFile ("Audio Files");
        const juce::int64 armedAt = 20 * kBlock;
        const juce::int64 capturedAt = 25 * kBlock;
        expect (writeWav (dir.getChildFile ("Track_02_part02.wav"), -0.3f, 4 * kBlock));   // the pass's audio
        Lead lead;
        lead.audioDir = dir;  lead.trackName = "Track_02"; lead.ext = ".wav";
        lead.firstPart = 1;   lead.parts = 1;   lead.sampleRate = kRate;
        lead.timelineStart = armedAt;           // as written at RECORD
        lead.ownsAudioPart = true;
        expect (MultitrackRecorder::writeLeadInMarker (lead));
        const auto marker = dir.getChildFile ("Pending Lead-ins/Track_02.json");
        expectEquals (markerStart (marker), armedAt);

        // The silence will not go in: something stands at the slot's name.
        const auto slot = dir.getChildFile ("Track_02.wav");
        expect (slot.createDirectory().wasOk());
        expect (slot.getChildFile ("keep").replaceWithText ("x"));
        expect (! MultitrackRecorder::finishLeadIn (lead, capturedAt), "fixture: the silence was written");
        expectEquals (markerStart (marker), capturedAt, "the marker kept the armed position");

        // Once the slot is free, the next open lines the audio up where it was played.
        expect (slot.deleteRecursively());
        expect (MultitrackRecorder::recoverInterruptedPunches (dir));
        expectEquals (fileLength (slot), capturedAt);
        expectWithinAbsoluteError (takeMean (slot, capturedAt, 4 * kBlock), -0.3f, 1.0e-3f);
        expect (! marker.exists());
    }

    // ── Fix 3 ──────────────────────────────────────────────────────────
    void failedRecoverySaysSo()
    {
        beginTest ("Fix 3: a punch recovery that fails says it could not put the punch back, and why");
        {
            Folder f;
            const auto original = f.audio().getChildFile ("Track_01.punchbase.wav");
            const auto partial  = f.audio().getChildFile ("Track_01.wav");
            expect (writeWav (original, 0.5f, 4800));   // set aside by a punch that never finished
            expect (writeWav (partial, -0.3f, 1200));    // what that punch had written
            // Nowhere to keep the partial punch: a file stands where the backups folder goes.
            expect (f.session().getChildFile ("Session File Backups").replaceWithText ("not a folder"));

            juce::String problem;
            expect (! MultitrackRecorder::recoverInterruptedPunches (f.audio(), nullptr, nullptr, {}, &problem));
            expect (problem.contains ("Interrupted Punch") && problem.contains ("could not be created"), problem);

            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            rec.armContinue (0);
            expect (! rec.startRecording (f.session()));
            const auto why = rec.getStartError();
            expect (why.contains ("could not be put back"), "a failed recovery was reported as done: " + why);
            expect (! why.contains ("was put back"), why);
            expect (why.contains ("Session File Backups"), "the refusal does not say what failed: " + why);
            expect (why.contains ("reopen the session"), "the refusal does not say what to do: " + why);

            expectWithinAbsoluteError (firstSample (original), 0.5f, 1.0e-3f);
            expectWithinAbsoluteError (firstSample (partial), -0.3f, 1.0e-3f);
        }

        beginTest ("Fix 3: a punch recovery that worked still asks for the session to be reopened");
        {
            Folder f;
            const auto original = f.audio().getChildFile ("Track_01.punchbase.wav");
            expect (writeWav (original, 0.5f, 4800));
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            rec.armContinue (0);
            expect (! rec.startRecording (f.session()));
            expect (rec.getStartError().contains ("was put back"), rec.getStartError());
            expectWithinAbsoluteError (firstSample (f.audio().getChildFile ("Track_01.wav")), 0.5f, 1.0e-3f);
            expect (! original.exists());
        }
    }
};

static Fix20261122RecorderTests fix20261122RecorderTests;
}
