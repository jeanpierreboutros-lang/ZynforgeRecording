#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <algorithm>

// Full "backup session" snapshot. Copies the files that DEFINE a session --
// the .zfproj (setlist/cues/playlists/automation/UI), session_mix.json,
// session_settings.json, markers.json -- into a timestamped sub-folder of the
// session's own "Session File Backups/" folder, then prunes to the newest N.
//
// Audio in Audio Files/ is deliberately NOT copied. These snapshots restore
// metadata against the current media; they cannot undo a committed punch that
// replaced audio. Independent media backups are a separate responsibility.
//
// Pure (file ops only) so it's unit-testable without the UI.
namespace zynforge::sessionbackup
{
    // Test-only delay seam. Load a shared callback before invocation so a test
    // can remove its hook safely after a worker has entered it.
    inline std::shared_ptr<const std::function<void()>> beforeSnapshotForTests;

    // Optional clock input makes retention tests deterministic; ordinary callers
    // retain the existing current-time behaviour.
    inline juce::File writeSnapshot (const juce::File& sessionDir, int keepNewest = 10,
                                    juce::Time snapshotTime = juce::Time::getCurrentTime())
    {
        if (keepNewest <= 0 || ! sessionDir.isDirectory() || sessionDir.isSymbolicLink()) return {};

        if (const auto hook = std::atomic_load (&beforeSnapshotForTests)) (*hook)();

        // Serialize naming/pruning across in-process save callers. A snapshot
        // always orders after the last retained save, even when the clock moves
        // backward or old collision suffixes have been removed by retention.
        static std::mutex snapshotMutex;
        const std::lock_guard<std::mutex> guard (snapshotMutex);
        const auto backupsDir = sessionDir.getChildFile ("Session File Backups");
        if (backupsDir.isSymbolicLink() || ! backupsDir.createDirectory().wasOk()) return {};
        struct Entry { juce::File file; juce::String stamp; juce::int64 sequence; };
        std::vector<Entry> entries;
        const auto prefix = sessionDir.getFileName() + "_";
        for (const auto& folder : backupsDir.findChildFiles (juce::File::findDirectories, false))
        {
            if (folder.isSymbolicLink() || ! folder.getFileName().startsWith (prefix)) continue;
            const auto suffix = folder.getFileName().substring (prefix.length());
            bool valid = suffix.length() >= 19;
            for (int i = 0; valid && i < 19; ++i)
                valid = (i == 4 || i == 7) ? suffix[i] == '-'
                      : i == 10 ? suffix[i] == '_'
                      : (i == 13 || i == 16) ? suffix[i] == '-'
                      : suffix[i] >= '0' && suffix[i] <= '9';
            if (! valid) continue;
            auto tail = suffix.substring (19).trim();
            juce::int64 sequence = 0;
            if (tail.isNotEmpty())
            {
                if (tail.startsWithChar ('_')) tail = tail.substring (1);
                else if (tail.startsWithChar ('(') && tail.endsWithChar (')'))
                    tail = tail.substring (1, tail.length() - 1); // legacy JUCE collision name
                else continue;
                if (tail.isEmpty()) continue;
                for (const auto digit : tail)
                {
                    if (digit < '0' || digit > '9'
                        || sequence > (std::numeric_limits<juce::int64>::max() - (digit - '0')) / 10)
                    { valid = false; break; }
                    sequence = sequence * 10 + digit - '0';
                }
            }
            if (valid) entries.push_back ({ folder, suffix.substring (0, 19), sequence });
        }
        std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
        {
            if (a.stamp != b.stamp) return a.stamp < b.stamp;
            if (a.sequence != b.sequence) return a.sequence < b.sequence;
            return a.file.getFileName() < b.file.getFileName();
        });
        auto stamp = snapshotTime.formatted ("%Y-%m-%d_%H-%M-%S");
        juce::int64 sequence = 0;
        if (! entries.empty() && stamp <= entries.back().stamp)
        {
            stamp = entries.back().stamp;
            if (entries.back().sequence == std::numeric_limits<juce::int64>::max()) return {};
            sequence = entries.back().sequence + 1;
        }
        const auto snap = backupsDir.getChildFile (prefix + stamp + "_"
                            + juce::String (sequence).paddedLeft ('0', 19));
        if (snap.exists() || snap.isSymbolicLink() || ! snap.createDirectory().wasOk()) return {};

        for (auto& proj : sessionDir.findChildFiles (juce::File::findFiles, false, "*.zfproj"))
            if (! proj.copyFileTo (snap.getChildFile (proj.getFileName())))
            {
                snap.deleteRecursively();
                return {};
            }

        for (const auto* n : { "session_mix.json", "session_settings.json", "markers.json" })
        {
            const auto f = sessionDir.getChildFile (n);
            if (f.existsAsFile() && ! f.copyFileTo (snap.getChildFile (n)))
            {
                snap.deleteRecursively();
                return {};
            }
        }

        // Only recognized real directories are owned snapshots. Journals,
        // arbitrary similarly named folders and links are never pruned.
        entries.push_back ({ snap, stamp, sequence });
        const auto removeCount = entries.size() > (size_t) keepNewest
            ? entries.size() - (size_t) keepNewest : 0;
        for (size_t i = 0; i < removeCount; ++i)
            if (! entries[i].file.isSymbolicLink()) entries[i].file.deleteRecursively (false);
        return snap;
    }
}
