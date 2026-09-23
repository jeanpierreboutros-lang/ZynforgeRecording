#pragma once

#include "AtomicFile.h"
#include "FastHash.h"
#include "PathSafety.h"
#include "TimelineExport.h"

#include <atomic>
#include <map>
#include <vector>

namespace zynforge::showhandoff
{
    struct Result
    {
        bool ok { false };
        juce::String message;
        int fileCount { 0 };
        juce::int64 bytes { 0 };
        bool captureReportWarning { false };
    };

    inline bool collectFiles (const juce::File& root, const juce::File& dir,
                              std::map<juce::String, juce::File>& files,
                              const std::atomic<bool>& cancel)
    {
        if (cancel.load (std::memory_order_relaxed) || dir.isSymbolicLink()) return false;
        for (const auto& child : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
        {
            if (cancel.load (std::memory_order_relaxed) || child.isSymbolicLink()) return false;
            if (child.isDirectory())
            {
                if (! collectFiles (root, child, files, cancel)) return false;
            }
            else if (child.existsAsFile())
                files.emplace (child.getRelativePathFrom (root), child);
            else
                return false;
        }
        return true;
    }

    inline juce::String channelMapCsv (const juce::File& session)
    {
        const auto mix = juce::JSON::parse (session.getChildFile ("session_mix.json"));
        if (! mix.isObject()) return {};
        auto* strips = mix.getProperty ("strips", {}).getArray();
        if (strips == nullptr || strips->isEmpty()) return {};

        juce::String csv = "Track,Name,InputIndex1,OutputIndex1,StereoLeft\r\n";
        for (int i = 0; i < strips->size(); ++i)
        {
            const auto& strip = (*strips)[i];
            const int input = (int) strip.getProperty ("inRoute", -1);
            const int output = (int) strip.getProperty ("outRoute", -1);
            csv << (i + 1) << ','
                << timelineexport::csvField (strip.getProperty ("name", "").toString()) << ','
                << (input >= 0 ? juce::String (input + 1) : juce::String()) << ','
                << (output >= 0 ? juce::String (output + 1) : juce::String()) << ','
                << ((bool) strip.getProperty ("stereo", false) ? "yes" : "no") << "\r\n";
        }
        return csv;
    }

    // The copy itself runs on MainComponent's owned export worker. This pass
    // checks EVERY copied file against the current source and publishes the
    // manifest only after the full tree and generated sidecars are complete.
    // It verifies transfer, not the original capture or external backup disks.
    inline Result verifyAndWrite (const juce::File& source, const juce::File& dest,
                                  const juce::String& timelineCsv,
                                  const std::atomic<bool>& cancel)
    {
        if (! source.isDirectory() || ! dest.isDirectory()
            || source.isSymbolicLink() || dest.isSymbolicLink()
            || pathsafety::isSameOrDescendant (source, dest))
            return { false, "Invalid handoff source or destination" };
        if (timelineCsv.isEmpty()) return { false, "Timeline metadata is unavailable" };

        constexpr const char* kMapName = "Handoff Channel Map.csv";
        constexpr const char* kTimelineName = "Handoff Timeline.csv";
        constexpr const char* kManifestName = "Handoff Manifest.json";
        constexpr const char* kIncompleteName = "HANDOFF INCOMPLETE.txt";
        for (auto* name : { kMapName, kTimelineName, kManifestName })
            if (source.getChildFile (name).exists() || dest.getChildFile (name).exists())
                return { false, "A handoff sidecar name already exists" };
        if (source.getChildFile (kIncompleteName).exists())
            return { false, "Source session has an incomplete handoff marker" };

        std::map<juce::String, juce::File> originals, copies;
        if (! collectFiles (source, source, originals, cancel)
            || ! collectFiles (dest, dest, copies, cancel))
            return { false, "Cancelled or unsafe link in the session" };
        // The caller keeps this marker throughout copying, hashing and
        // manifest publication. It is removed only after this function
        // succeeds, so a crash at any earlier point stays visibly incomplete.
        copies.erase (kIncompleteName);
        if (originals.size() != copies.size())
            return { false, "The copied session has missing or extra files" };

        juce::Array<juce::var> fileEntries;
        std::map<juce::String, juce::String> audioHashes;
        juce::int64 totalBytes = 0;
        int audioCount = 0;
        const auto supportedTake = [] (const juce::String& name)
        {
            const auto ext = juce::File (name).getFileExtension().toLowerCase();
            if (! (ext == ".wav" || ext == ".flac" || ext == ".aif" || ext == ".aiff"))
                return false;
            const auto stem = juce::File (name).getFileNameWithoutExtension();
            if (! stem.startsWith ("Track_")) return false;
            const auto rest = stem.substring (6);
            const auto digits = rest.upToFirstOccurrenceOf ("_", false, false);
            if (digits.isEmpty() || digits.length() > 3
                || ! digits.containsOnly ("0123456789")
                || digits.getIntValue() < 1 || digits.getIntValue() > 256)
                return false;
            if (rest == digits) return true;
            const auto suffix = rest.substring (digits.length());
            return suffix.startsWith ("_part")
                && suffix.substring (5).isNotEmpty()
                && suffix.substring (5).containsOnly ("0123456789");
        };
        bool folderHasAudio = false;
        for (const auto& [relative, file] : originals)
            if (relative.startsWith ("Audio Files/")
                && supportedTake (relative.substring (12)))
                { folderHasAudio = true; break; }
        for (const auto& [relative, original] : originals)
        {
            if (cancel.load (std::memory_order_relaxed)) return { false, "Handoff cancelled" };
            const auto found = copies.find (relative);
            if (found == copies.end()) return { false, "Missing copied file: " + relative };
            const auto& copy = found->second;
            const auto size = original.getSize();
            if (size < 0 || copy.getSize() != size)
                return { false, "Copy size mismatch: " + relative };
            const auto originalSha = hashing::fileSha256 (original, &cancel);
            const auto copySha = hashing::fileSha256 (copy, &cancel);
            if (originalSha.isEmpty() || copySha.isEmpty() || originalSha != copySha)
                return { false, "Copy hash mismatch or read failed: " + relative };

            juce::DynamicObject::Ptr entry (new juce::DynamicObject());
            entry->setProperty ("path", relative);
            entry->setProperty ("bytes", size);
            entry->setProperty ("sha256", originalSha);
            entry->setProperty ("kind", "copied-and-verified");
            fileEntries.add (juce::var (entry.get()));
            totalBytes += size;
            const bool inAudioFolder = relative.startsWith ("Audio Files/");
            const auto name = inAudioFolder ? relative.substring (12) : relative;
            if ((folderHasAudio ? inAudioFolder : ! inAudioFolder) && supportedTake (name))
            {
                audioHashes.emplace (name, originalSha);
                ++audioCount;
            }
        }
        if (audioCount == 0) return { false, "No recorded audio in the session" };

        const auto mapCsv = channelMapCsv (dest);
        if (mapCsv.isEmpty()) return { false, "Saved channel map is missing or invalid" };
        for (const auto& sidecar : { std::pair<juce::String, juce::String> (kMapName, mapCsv),
                                     std::pair<juce::String, juce::String> (kTimelineName, timelineCsv) })
        {
            if (cancel.load (std::memory_order_relaxed)
                || ! atomicfile::writeText (dest.getChildFile (sidecar.first), sidecar.second))
                return { false, "Could not write handoff sidecar: " + sidecar.first };
            const auto file = dest.getChildFile (sidecar.first);
            const auto sha = hashing::fileSha256 (file, &cancel);
            if (sha.isEmpty()) return { false, "Could not hash handoff sidecar" };
            juce::DynamicObject::Ptr entry (new juce::DynamicObject());
            entry->setProperty ("path", sidecar.first);
            entry->setProperty ("bytes", file.getSize());
            entry->setProperty ("sha256", sha);
            entry->setProperty ("kind", "generated");
            fileEntries.add (juce::var (entry.get()));
        }

        const auto reportFile = dest.getChildFile ("session.report.json");
        const auto report = reportFile.existsAsFile() ? juce::JSON::parse (reportFile) : juce::var();
        const bool pending = ! report.isObject() || (bool) report.getProperty ("sha256Pending", true);
        int reported = 0, matched = 0;
        bool mirrorReportedFailed = false;
        std::map<juce::String, bool> covered;
        if (auto* tracks = report.getProperty ("tracks", {}).getArray())
            for (const auto& track : *tracks)
            {
                if (auto* mirrors = track.getProperty ("mirrors", {}).getArray())
                    for (const auto& mirror : *mirrors)
                        mirrorReportedFailed = mirrorReportedFailed
                            || (bool) mirror.getProperty ("failed", false);
                auto* names = track.getProperty ("files", {}).getArray();
                auto* hashes = track.getProperty ("sha256", {}).getArray();
                if (names == nullptr || hashes == nullptr) continue;
                for (int i = 0; i < names->size(); ++i)
                {
                    ++reported;
                    const auto name = (*names)[i].toString();
                    covered[name] = true;
                    const auto found = audioHashes.find (name);
                    if (i < hashes->size() && found != audioHashes.end()
                        && (*hashes)[i].toString() == found->second)
                        ++matched;
                }
            }
        const int unreported = juce::jmax (0, audioCount - (int) covered.size());
        const bool captureWarning = pending || reported == 0 || matched != reported || unreported > 0
                                 || (juce::int64) report.getProperty ("missedSamples", 0) > 0
                                 || (bool) report.getProperty ("primaryFailed", false)
                                 || (bool) report.getProperty ("backupFailed", false)
                                 || (int) report.getProperty ("mirrorsSkipped", 0) > 0
                                 || mirrorReportedFailed;

        juce::DynamicObject::Ptr capture (new juce::DynamicObject());
        capture->setProperty ("reportPresent", report.isObject());
        capture->setProperty ("reportHashPending", pending);
        capture->setProperty ("reportedPrimaryFiles", reported);
        capture->setProperty ("matchedPrimaryFiles", matched);
        capture->setProperty ("unreportedAudioFiles", unreported);
        capture->setProperty ("missedSamplesReported", report.getProperty ("missedSamples", {}));
        capture->setProperty ("mirrorFailureReported", mirrorReportedFailed);
        capture->setProperty ("mirrorsSkippedReported", report.getProperty ("mirrorsSkipped", 0));
        capture->setProperty ("requiresManualReview", captureWarning);
        capture->setProperty ("externalBackupsIncluded", false);

        juce::DynamicObject::Ptr manifest (new juce::DynamicObject());
        manifest->setProperty ("formatVersion", 1);
        manifest->setProperty ("generatedAt", juce::Time::getCurrentTime().toISO8601 (true));
        manifest->setProperty ("sessionName", source.getFileName());
        manifest->setProperty ("transferVerified", true);
        manifest->setProperty ("scope", "Source session vs portable handoff copy; not a show-readiness certificate");
        manifest->setProperty ("captureReport", juce::var (capture.get()));
        manifest->setProperty ("files", juce::var (fileEntries));
        if (! atomicfile::writeText (dest.getChildFile (kManifestName),
                                     juce::JSON::toString (juce::var (manifest.get()), true)))
            return { false, "Could not publish handoff manifest" };
        return { true, captureWarning ? "Copy verified; capture report needs review" : "Copy verified",
                 (int) originals.size() + 2, totalBytes, captureWarning };
    }
}
