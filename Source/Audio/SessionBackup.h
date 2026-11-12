#pragma once

#include <juce_core/juce_core.h>
#include "SessionMixFile.h"
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <algorithm>
#include <optional>

// Full "backup session" snapshot. Copies the files that DEFINE a session --
// the .zfproj (setlist/cues/playlists/UI), session_mix.json,
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

    // Retention counts snapshots, so a save that changed nothing must not take a
    // place: left alone, an idle hour would push every earlier state out. The
    // project's save time changes on every save and is not a change.
    inline bool snapshotMatchesSession (const juce::File& snapshot, const juce::File& sessionDir)
    {
        const auto content = [] (const juce::File& file) -> juce::String
        {
            auto parsed = juce::JSON::parse (file);
            if (auto* project = parsed.getDynamicObject(); project != nullptr && file.hasFileExtension ("zfproj"))
            {
                project->removeProperty ("updatedAt");
                return juce::JSON::toString (parsed);
            }
            return file.loadFileAsString();
        };
        const auto projects = sessionDir.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                                         false, "*.zfproj");
        if (projects.size() != snapshot.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                                        false, "*.zfproj").size())
            return false;
        for (const auto& project : projects)
        {
            const auto saved = snapshot.getChildFile (project.getFileName());
            if (! saved.existsAsFile() || content (project) != content (saved)) return false;
        }
        for (const auto* name : { "session_mix.json", "session_settings.json", "markers.json" })
        {
            const auto file = sessionDir.getChildFile (name), saved = snapshot.getChildFile (name);
            if (file.existsAsFile() != saved.existsAsFile()
                || (file.existsAsFile() && content (file) != content (saved)))
                return false;
        }
        return true;
    }

    // A snapshot folder is "<session>_<stamp>_<sequence>": the session's
    // folder name plus this many bytes.
    inline constexpr int kSnapshotExtraBytes = 1 + 19 + 1 + 19;

    struct SnapshotName
    {
        juce::String stamp;
        juce::int64 sequence { 0 };

        bool operator< (const SnapshotName& other) const
        {
            return stamp != other.stamp ? stamp < other.stamp : sequence < other.sequence;
        }
    };

    // "<stamp>", then nothing, "_<sequence>" or a legacy "(<n>)". Empty for
    // anything else, so a journal or a similarly named folder is never taken
    // for a snapshot.
    inline std::optional<SnapshotName> parseSnapshotSuffix (const juce::String& suffix)
    {
        bool valid = suffix.length() >= 19;
        for (int i = 0; valid && i < 19; ++i)
            valid = (i == 4 || i == 7) ? suffix[i] == '-'
                  : i == 10 ? suffix[i] == '_'
                  : (i == 13 || i == 16) ? suffix[i] == '-'
                  : suffix[i] >= '0' && suffix[i] <= '9';
        if (! valid) return {};
        auto tail = suffix.substring (19).trim();
        juce::int64 sequence = 0;
        if (tail.isNotEmpty())
        {
            if (tail.startsWithChar ('_')) tail = tail.substring (1);
            else if (tail.startsWithChar ('(') && tail.endsWithChar (')'))
                tail = tail.substring (1, tail.length() - 1); // legacy JUCE collision name
            else return {};
            if (tail.isEmpty()) return {};
            for (const auto digit : tail)
            {
                if (digit < '0' || digit > '9'
                    || sequence > (std::numeric_limits<juce::int64>::max() - (digit - '0')) / 10)
                    return {};
                sequence = sequence * 10 + digit - '0';
            }
        }
        return SnapshotName { suffix.substring (0, 19), sequence };
    }

    // A snapshot's name starts with the session's name when it was taken, which
    // a Save As or a rename in Finder has since changed.
    inline std::optional<SnapshotName> parseSnapshotName (const juce::String& folderName)
    {
        for (int split = folderName.lastIndexOfChar ('_'); split > 0;
             split = folderName.substring (0, split).lastIndexOfChar ('_'))
            if (auto name = parseSnapshotSuffix (folderName.substring (split + 1)))
                return name;
        return {};
    }

    // What writeSnapshot puts in a snapshot, and what Finder adds when someone
    // looks inside (.DS_Store, and ._ AppleDouble files on exFAT and shares).
    inline bool isSnapshotDocument (const juce::String& name)
    {
        const auto plain = name.startsWith ("._") ? name.substring (2) : name;
        return plain == ".DS_Store" || plain.endsWithIgnoreCase (".zfproj")
            || plain == "session_mix.json" || plain == "session_settings.json" || plain == "markers.json";
    }

    inline bool holdsOnlySessionDocuments (const juce::File& snapshot)
    {
        for (const auto& entry : juce::RangedDirectoryIterator (snapshot, false, "*",
                                                                juce::File::findFilesAndDirectories))
            if (entry.isDirectory() || ! isSnapshotDocument (entry.getFile().getFileName())) return false;
        return true;
    }

    // Optional clock input makes retention tests deterministic; ordinary callers
    // retain the existing current-time behaviour. `keptForeign` receives every
    // snapshot-named folder that retention leaves alone because it holds more
    // than session documents.
    inline juce::File writeSnapshot (const juce::File& sessionDir, int keepNewest = 10,
                                    juce::Time snapshotTime = juce::Time::getCurrentTime(),
                                    juce::Array<juce::File>* keptForeign = nullptr)
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
            if (const auto name = parseSnapshotSuffix (folder.getFileName().substring (prefix.length())))
                entries.push_back ({ folder, name->stamp, name->sequence });
        }
        std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
        {
            if (a.stamp != b.stamp) return a.stamp < b.stamp;
            if (a.sequence != b.sequence) return a.sequence < b.sequence;
            return a.file.getFileName() < b.file.getFileName();
        });
        if (! entries.empty() && snapshotMatchesSession (entries.back().file, sessionDir))
            return entries.back().file;

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
        // arbitrary similarly named folders and links are never pruned. Nor is
        // a snapshot holding anything but the documents copied into it: one
        // opened as a session may hold a take. It is kept, takes no place in
        // the window, and is reported.
        entries.push_back ({ snap, stamp, sequence });
        entries.erase (std::remove_if (entries.begin(), entries.end(), [keptForeign] (const Entry& e)
        {
            if (holdsOnlySessionDocuments (e.file)) return false;
            if (keptForeign != nullptr) keptForeign->add (e.file);
            return true;
        }), entries.end());
        const auto removeCount = entries.size() > (size_t) keepNewest
            ? entries.size() - (size_t) keepNewest : 0;
        // An emptied mix can fill the whole window. The last copy that still
        // has strips is then the only way back, so retention keeps it too.
        const auto hasStrips = [] (const Entry& e) { return sessionmix::stripCountIn (e.file) > 0; };
        auto keepPopulated = entries.size();
        if (std::none_of (entries.begin() + (std::ptrdiff_t) removeCount, entries.end(), hasStrips))
            for (auto i = removeCount; i-- > 0 && keepPopulated == entries.size();)
                if (hasStrips (entries[i])) keepPopulated = i;
        for (size_t i = 0; i < removeCount; ++i)
            if (i != keepPopulated && ! entries[i].file.isSymbolicLink())
                entries[i].file.deleteRecursively (false);
        return snap;
    }
}
