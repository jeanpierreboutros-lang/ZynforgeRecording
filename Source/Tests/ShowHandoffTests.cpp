#include <juce_core/juce_core.h>

#include "../Audio/ShowHandoff.h"

namespace zynforge
{
    class ShowHandoffTests final : public juce::UnitTest
    {
    public:
        ShowHandoffTests() : UnitTest ("Show handoff", "zynforge") {}

        void runTest() override
        {
            const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zynforge-handoff-" + juce::Uuid().toString());
            const juce::ScopeGuard cleanup { [&] { root.deleteRecursively(); } };
            const auto source = root.getChildFile ("Show");
            const auto dest = root.getChildFile ("Show Handoff");
            expect (source.getChildFile ("Audio Files").createDirectory().wasOk());
            expect (dest.getChildFile ("Audio Files").createDirectory().wasOk());
            const auto audio = source.getChildFile ("Audio Files/Track_01.wav");
            const auto audioCopy = dest.getChildFile ("Audio Files/Track_01.wav");
            expect (audio.replaceWithText ("sample audio bytes"));
            expect (audioCopy.replaceWithText ("sample audio bytes"));

            const juce::String mix = R"({"trackCount":1,"strips":[{"name":"=Vox","inRoute":0,"outRoute":1,"stereo":false}]})";
            expect (source.getChildFile ("session_mix.json").replaceWithText (mix));
            expect (dest.getChildFile ("session_mix.json").replaceWithText (mix));
            const juce::String markers = R"({"markers":[{"name":"Song 1","sample":48000}]})";
            expect (source.getChildFile ("markers.json").replaceWithText (markers));
            expect (dest.getChildFile ("markers.json").replaceWithText (markers));
            const auto hash = hashing::fileSha256 (audio);
            const auto report = "{\"sha256Pending\":false,\"missedSamples\":0,\"tracks\":[{\"files\":[\"Track_01.wav\"],\"sha256\":[\""
                + hash + "\"]}]}";
            expect (source.getChildFile ("session.report.json").replaceWithText (report));
            expect (dest.getChildFile ("session.report.json").replaceWithText (report));
            const auto incomplete = dest.getChildFile ("HANDOFF INCOMPLETE.txt");
            expect (incomplete.replaceWithText ("copy in progress"));

            std::atomic<bool> cancel { false };
            beginTest ("Portable copy verifies every file and writes safe sidecars");
            auto result = showhandoff::verifyAndWrite (source, dest, "timeline,marker\r\n", cancel);
            expect (result.ok, result.message);
            expect (! result.captureReportWarning, "complete matching source report was flagged");
            expectEquals (result.fileCount, 6);
            const auto csv = dest.getChildFile ("Handoff Channel Map.csv").loadFileAsString();
            expect (csv.contains ("'=Vox"), "untrusted console name was not CSV-escaped");
            const auto manifest = juce::JSON::parse (dest.getChildFile ("Handoff Manifest.json"));
            expect ((bool) manifest.getProperty ("transferVerified", false));
            expect (! (bool) manifest.getProperty ("captureReport", {}).getProperty ("requiresManualReview", true));
            expect (dest.getChildFile ("markers.json").existsAsFile(), "markers not included");
            expect (incomplete.existsAsFile(), "verifier removed the safety marker before caller completion");

            beginTest ("A changed destination file cannot be certified");
            dest.getChildFile ("Handoff Manifest.json").deleteFile();
            dest.getChildFile ("Handoff Channel Map.csv").deleteFile();
            dest.getChildFile ("Handoff Timeline.csv").deleteFile();
            expect (audioCopy.replaceWithText ("changed audio bytes"));
            result = showhandoff::verifyAndWrite (source, dest, "timeline,marker\r\n", cancel);
            expect (! result.ok, "changed copy was incorrectly verified");
            expect (! dest.getChildFile ("Handoff Manifest.json").existsAsFile());

            beginTest ("Pending capture hashes remain an explicit warning");
            expect (audioCopy.replaceWithText ("sample audio bytes"));
            const auto pendingReport = "{\"sha256Pending\":true,\"tracks\":[{\"files\":[\"Track_01.wav\"],\"sha256\":[\"\"]}]}";
            expect (source.getChildFile ("session.report.json").replaceWithText (pendingReport));
            expect (dest.getChildFile ("session.report.json").replaceWithText (pendingReport));
            result = showhandoff::verifyAndWrite (source, dest, "timeline,marker\r\n", cancel);
            expect (result.ok, result.message);
            expect (result.captureReportWarning, "pending original capture report was presented as clean");

            beginTest ("Transfer can succeed while original capture hash mismatches");
            dest.getChildFile ("Handoff Manifest.json").deleteFile();
            dest.getChildFile ("Handoff Channel Map.csv").deleteFile();
            dest.getChildFile ("Handoff Timeline.csv").deleteFile();
            const auto wrongReport = "{\"sha256Pending\":false,\"tracks\":[{\"files\":[\"Track_01.wav\"],\"sha256\":[\"bad\"]}]}";
            expect (source.getChildFile ("session.report.json").replaceWithText (wrongReport));
            expect (dest.getChildFile ("session.report.json").replaceWithText (wrongReport));
            result = showhandoff::verifyAndWrite (source, dest, "timeline,marker\r\n", cancel);
            expect (result.ok, result.message);
            expect (result.captureReportWarning, "a mismatched source capture hash was presented as clean");

            beginTest ("Skipped mirrors remain visible in the verified handoff");
            dest.getChildFile ("Handoff Manifest.json").deleteFile();
            dest.getChildFile ("Handoff Channel Map.csv").deleteFile();
            dest.getChildFile ("Handoff Timeline.csv").deleteFile();
            const auto skippedReport = "{\"sha256Pending\":false,\"missedSamples\":0,\"mirrorsSkipped\":1,\"tracks\":[{\"files\":[\"Track_01.wav\"],\"sha256\":[\""
                + hash + "\"]}]}";
            expect (source.getChildFile ("session.report.json").replaceWithText (skippedReport));
            expect (dest.getChildFile ("session.report.json").replaceWithText (skippedReport));
            result = showhandoff::verifyAndWrite (source, dest, "timeline,marker\r\n", cancel);
            expect (result.ok, result.message);
            expect (result.captureReportWarning, "skipped mirror was presented as clean");
            const auto skippedManifest = juce::JSON::parse (dest.getChildFile ("Handoff Manifest.json"));
            expectEquals ((int) skippedManifest.getProperty ("captureReport", {})
                              .getProperty ("mirrorsSkippedReported", 0), 1);

            beginTest ("Legacy root-level take is accepted and hash-verified");
            const auto legacy = root.getChildFile ("Legacy");
            const auto legacyCopy = root.getChildFile ("Legacy Handoff");
            expect (legacy.createDirectory().wasOk());
            expect (legacyCopy.createDirectory().wasOk());
            const auto legacyAudio = legacy.getChildFile ("Track_01.wav");
            expect (legacyAudio.replaceWithText ("legacy audio"));
            expect (legacyCopy.getChildFile ("Track_01.wav").replaceWithText ("legacy audio"));
            expect (legacy.getChildFile ("session_mix.json").replaceWithText (mix));
            expect (legacyCopy.getChildFile ("session_mix.json").replaceWithText (mix));
            const auto legacyHash = hashing::fileSha256 (legacyAudio);
            const auto legacyReport = "{\"sha256Pending\":false,\"tracks\":[{\"files\":[\"Track_01.wav\"],\"sha256\":[\""
                + legacyHash + "\"]}]}";
            expect (legacy.getChildFile ("session.report.json").replaceWithText (legacyReport));
            expect (legacyCopy.getChildFile ("session.report.json").replaceWithText (legacyReport));
            result = showhandoff::verifyAndWrite (legacy, legacyCopy, "timeline,marker\r\n", cancel);
            expect (result.ok, result.message);
            expect (! result.captureReportWarning, "legacy take was not matched to its report hash");
        }
    };

    static ShowHandoffTests showHandoffTests;
}
