#pragma once

#include "AtomicFile.h"
#include "SessionBackup.h"
#include "SessionMixFile.h"
#include "../UI/SessionProjPath.h"
#include <algorithm>
#include <vector>

// What happens when a session's own metadata cannot be read. Opening falls back
// to defaults, and the next save would then replace the only copy of something
// a text editor might have repaired. So a damaged file is moved aside under a
// dated name before anything loads, and the newest backup holding a readable
// copy is named so the engineer can take it instead.
namespace zynforge::sessionrepair
{
    struct DamagedFile
    {
        juce::String name;   // the file's own name in the session folder
        juce::File   keptAs; // the dated copy; empty when it could not be moved
        juce::File   backup; // snapshot folder with a good copy; empty when none
        juce::String problem;
    };

    inline juce::String datedNameFor (const juce::File& file, juce::Time when = juce::Time::getCurrentTime())
    {
        return file.getFileName() + ".damaged-" + when.formatted ("%Y-%m-%d_%H-%M-%S");
    }

    // Never replaces an earlier kept copy, even within the same second.
    inline juce::File keepAside (const juce::File& file)
    {
        const auto kept = atomicfile::firstFreeName (file.getParentDirectory(), datedNameFor (file));
        return file.moveFileTo (kept) ? kept : juce::File();
    }

    inline bool holdsObject (const juce::File& file)
    {
        return juce::JSON::parse (file).isObject();
    }

    // A mix is readable when it has a strip count at all. A backup only counts
    // as good when it has strips, or restoring it would change nothing.
    inline bool readable (const juce::File& file)
    {
        return file.getFileName() == sessionmix::kFileName
            ? sessionmix::stripCount (juce::JSON::parse (file)) >= 0
            : holdsObject (file);
    }

    inline bool goodBackupCopy (const juce::File& file)
    {
        return file.getFileName() == sessionmix::kFileName
            ? sessionmix::stripCount (juce::JSON::parse (file)) > 0
            : holdsObject (file);
    }

    // Newest first, so the first good copy is the one to offer. Every snapshot in
    // the folder counts, whatever session name it carries, and they are ordered
    // by the time in that name: a name of its own can't be sorted across a Save
    // As, and used as a wildcard it can't hold a comma or a semicolon.
    inline juce::File newestBackupOf (const juce::File& sessionDir, const juce::String& fileName)
    {
        std::vector<std::pair<sessionbackup::SnapshotName, juce::File>> snapshots;
        for (const auto& folder : sessionDir.getChildFile ("Session File Backups")
                                            .findChildFiles (juce::File::findDirectories, false))
            if (const auto name = sessionbackup::parseSnapshotName (folder.getFileName());
                name.has_value() && ! folder.isSymbolicLink())
                snapshots.emplace_back (*name, folder);
        std::sort (snapshots.begin(), snapshots.end(), [] (const auto& a, const auto& b)
                   { return b.first < a.first; });
        for (const auto& [name, folder] : snapshots)
            if (const auto copy = folder.getChildFile (fileName); copy.existsAsFile() && goodBackupCopy (copy))
                return folder;
        return {};
    }

    inline bool hasTakeAudio (const juce::File& sessionDir)
    {
        const auto audio = sessionDir.getChildFile ("Audio Files");
        const auto takes = (audio.isDirectory() ? audio : sessionDir)
            .findChildFiles (juce::File::findFiles, false,
                             "Track_*.wav;Track_*.flac;Track_*.aif;Track_*.aiff");
        return std::any_of (takes.begin(), takes.end(), [] (const juce::File& take)
                            { return ! take.getFileName().containsIgnoreCase (".punchbase"); });
    }

    // Something only a session has. Any one will do, because any of them can be
    // damaged or missing in a real session. Opening anything else would make it
    // one: the next autosave would write a session's files into that folder.
    inline bool looksLikeSession (const juce::File& dir)
    {
        return hasTakeAudio (dir)
            || findSessionProj (dir).existsAsFile()
            || dir.getChildFile (sessionmix::kFileName).existsAsFile()
            || dir.getChildFile ("session_settings.json").existsAsFile()
            || dir.getChildFile ("markers.json").existsAsFile()
            || dir.getChildFile ("Audio Files").isDirectory()
            || dir.getChildFile ("Session File Backups").isDirectory();
    }

    // A folder inside a session's Session File Backups: a backup snapshot, the
    // kept originals, a move journal. Opened as a session, a snapshot would be
    // autosaved over, could take a RECORD, and retention in the real session
    // would later delete it with whatever was recorded there.
    inline bool isInsideBackups (const juce::File& dir)
    {
        return dir.getParentDirectory().getFileName().equalsIgnoreCase ("Session File Backups");
    }

    // The refusal, with the way back to that state.
    inline juce::String backupFolderRefusal (const juce::File& dir)
    {
        const auto session = dir.getParentDirectory().getParentDirectory();
        return dir.getFileName() + " is a backup of " + session.getFileName()
             + ", not a session; nothing was opened. To go back to it, close " + session.getFileName()
             + " and copy this folder's files into " + session.getFullPathName();
    }

    // A file name holds at most 255 bytes of UTF-8 (APFS, HFS+). A save derives
    // longer names than the session's own: the project's staging file and the
    // backup snapshot folder. New sessions and Save As copies stay under
    // kMaxSessionNameBytes, which also leaves room for an export staged as
    // "<name> - Mix" in the session's own Export Files.
    inline constexpr int kMaxNameBytes = 255;
    inline constexpr int kMaxSessionNameBytes = 200;

    inline juce::String nameTooLongForNewSession (const juce::String& name)
    {
        const auto bytes = (int) name.getNumBytesAsUTF8();
        if (bytes <= kMaxSessionNameBytes) return {};
        return "Session name is too long: " + juce::String (bytes) + " bytes, and at most "
             + juce::String (kMaxSessionNameBytes) + " fit (about 100 letters in Arabic, Hebrew or Greek). Use a shorter name";
    }

    // Why nothing can be saved into this folder under its present name (a
    // rename in Finder), or empty.
    inline juce::String nameTooLongToSave (const juce::File& dir, const juce::File& project, bool backup)
    {
        const auto folder = (int) dir.getFileName().getNumBytesAsUTF8();
        const auto staging = (int) project.getFileName().getNumBytesAsUTF8() + atomicfile::kStagingExtraBytes;
        if (staging <= kMaxNameBytes && (! backup || folder + sessionbackup::kSnapshotExtraBytes <= kMaxNameBytes))
            return {};
        return "the session folder name is too long to save into (" + juce::String (folder)
             + " bytes); rename it to at most " + juce::String (kMaxSessionNameBytes) + " bytes in Finder, then save";
    }

    // Run before anything in the session loads. `hasTakeAudio` also catches a
    // mix saved with no strips: readable, but it describes no channel of the
    // audio beside it.
    inline std::vector<DamagedFile> quarantineDamagedFiles (const juce::File& sessionDir)
    {
        std::vector<DamagedFile> damaged;
        const auto consider = [&] (const juce::File& file)
        {
            if (! file.existsAsFile()) return;
            const bool emptied = file.getFileName() == sessionmix::kFileName
                && sessionmix::stripCount (juce::JSON::parse (file)) == 0
                && hasTakeAudio (sessionDir);
            if (readable (file) && ! emptied) return;
            DamagedFile entry;
            entry.name = file.getFileName();
            entry.problem = emptied ? "lists no strips, but the session has audio" : "could not be read";
            entry.backup = newestBackupOf (sessionDir, entry.name);
            entry.keptAs = keepAside (file);
            damaged.push_back (std::move (entry));
        };
        consider (sessionDir.getChildFile (sessionmix::kFileName));
        consider (sessionDir.getChildFile ("session_settings.json"));
        consider (sessionDir.getChildFile ("markers.json"));
        consider (findSessionProj (sessionDir));
        return damaged;
    }

    // Copies each listed file from its backup, atomically, so a failed restore
    // leaves the session as it was. Returns the names that could not be restored.
    inline juce::StringArray restoreFromBackups (const juce::File& sessionDir,
                                                 const std::vector<DamagedFile>& damaged)
    {
        juce::StringArray failed;
        for (const auto& file : damaged)
        {
            if (file.backup == juce::File()) continue;
            juce::MemoryBlock bytes;
            if (! file.backup.getChildFile (file.name).loadFileAsData (bytes)
                || ! atomicfile::writeBytes (sessionDir.getChildFile (file.name), bytes.getData(), bytes.getSize()))
                failed.add (file.name);
        }
        return failed;
    }

    // For a writer about to replace a file that is damaged right now. Nothing
    // happens to a good file. Returns the dated copy when one was made.
    inline juce::File keepIfUnreadable (const juce::File& file)
    {
        return file.existsAsFile() && ! readable (file) ? keepAside (file) : juce::File();
    }
}
