#pragma once

#include <juce_core/juce_core.h>

// Full "backup session" snapshot. Copies the files that DEFINE a session --
// the .zfproj (setlist/cues/playlists/automation/UI), session_mix.json,
// session_settings.json, markers.json -- into a timestamped sub-folder of the
// session's own "Session File Backups/" folder, then prunes to the newest N.
//
// The immutable, multi-GB audio in Audio Files/ is deliberately NOT copied:
// it lives once, is never mutated, and duplicating it every few minutes would
// make the backup folder unusable. To restore, copy a snapshot's files back
// over the session root (the existing Audio Files/ are reused as-is).
//
// Pure (file ops only) so it's unit-testable without the UI.
namespace zynforge::sessionbackup
{
    inline juce::File writeSnapshot (const juce::File& sessionDir, int keepNewest = 10)
    {
        if (! sessionDir.isDirectory()) return {};

        const auto backupsDir = sessionDir.getChildFile ("Session File Backups");
        if (! backupsDir.createDirectory().wasOk()) return {};

        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
        auto snap = backupsDir.getChildFile (sessionDir.getFileName() + "_" + stamp)
                              .getNonexistentSibling();   // unique even on same-second saves
        if (! snap.createDirectory().wasOk()) return {};

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

        // Transaction journals share this directory and are NOT disposable
        // snapshots. Only our own timestamped session snapshots may be pruned.
        juce::Array<juce::File> snaps;
        const auto prefix = sessionDir.getFileName() + "_";
        for (const auto& folder : backupsDir.findChildFiles (juce::File::findDirectories, false, "*"))
        {
            const auto suffix = folder.getFileName().substring (prefix.length());
            bool timestamp = suffix.length() >= 19;
            for (int i = 0; timestamp && i < 19; ++i)
                timestamp = (i == 4 || i == 7) ? suffix[i] == '-'
                          : i == 10 ? suffix[i] == '_'
                          : (i == 13 || i == 16) ? suffix[i] == '-'
                          : juce::CharacterFunctions::isDigit (suffix[i]);
            if (folder.getFileName().startsWith (prefix) && timestamp)
                snaps.add (folder);
        }
        if (snaps.size() > keepNewest)
        {
            snaps.sort();   // "<Session>_<stamp>" -> alphabetical == chronological
            for (int i = 0; i < snaps.size() - keepNewest; ++i)
                snaps[i].deleteRecursively();
        }
        return snap;
    }
}
