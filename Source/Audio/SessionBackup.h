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
            proj.copyFileTo (snap.getChildFile (proj.getFileName()));

        for (const auto* n : { "session_mix.json", "session_settings.json", "markers.json" })
        {
            const auto f = sessionDir.getChildFile (n);
            if (f.existsAsFile()) f.copyFileTo (snap.getChildFile (n));
        }

        // Keep the N most recent backup folders; prune older ones.
        auto snaps = backupsDir.findChildFiles (juce::File::findDirectories, false, "*");
        if (snaps.size() > keepNewest)
        {
            snaps.sort();   // "<Session>_<stamp>" -> alphabetical == chronological
            for (int i = 0; i < snaps.size() - keepNewest; ++i)
                snaps[i].deleteRecursively();
        }
        return snap;
    }
}
