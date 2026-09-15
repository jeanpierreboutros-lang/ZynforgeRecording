#pragma once
#include <juce_core/juce_core.h>
#include "PathSafety.h"
#include "AtomicFile.h"
#include <vector>

namespace zynforge
{
// Durable two-phase renames. An interrupted operation is rolled back on open;
// neither failed moves nor old recovery files are ever silently discarded.
class TrackFileTransaction
{
public:
    using Move = std::pair<juce::File, juce::File>;
    juce::File folder;
    juce::var journal;

    static bool recoverOne (const juce::File& dir)
    {
        if (! dir.getChildFile ("journal.json").existsAsFile()) return true; // no moves started
        auto v = juce::JSON::parse (dir.getChildFile ("journal.json"));
        auto* o = v.getDynamicObject();
        if (o == nullptr) return false;
        const auto phase = o->getProperty ("phase").toString();
        if (phase == "committed" || phase == "recovered") return true;
        auto* moves = o->getProperty ("moves").getArray();
        if (moves == nullptr) return false;
        const auto session = dir.getParentDirectory().getParentDirectory();
        const auto backups = session.getChildFile ("Session File Backups");
        // File::isAChildOf is lexical and follows no symlinks. A downloaded
        // session could therefore make "Audio Files" or "Session File
        // Backups" a symlink and make recovery rename/copy files outside the
        // session. Resolve every existing path prefix before touching data.
        if (! isContainedPath (session, backups) || ! isContainedPath (backups, dir))
            return false;
        for (const auto& m : *moves)
            if (! isContainedPath (session, juce::File (m["source"].toString()))
                || ! isContainedPath (session, juce::File (m["dest"].toString()))
                || ! isContainedPath (dir, juce::File (m["temp"].toString()))) return false;
        // During install every original has been staged. Bring installed
        // destinations back to their unique staging names before restoring.
        if (phase == "install")
            for (const auto& m : *moves)
            {
                juce::File temp (m["temp"].toString()), dest (m["dest"].toString());
                if (! temp.existsAsFile() && (! dest.existsAsFile() || ! dest.moveFileTo (temp))) return false;
            }
        o->setProperty ("phase", "restore");
        if (! atomicfile::writeText (dir.getChildFile ("journal.json"),
                                     juce::JSON::toString (v))) return false;
        for (const auto& m : *moves)
        {
            juce::File temp (m["temp"].toString()), source (m["source"].toString());
            if (temp.existsAsFile() && (source.exists() || ! temp.moveFileTo (source))) return false;
        }
        if (auto* metadata = o->getProperty ("metadata").getArray())
            for (const auto& m : *metadata)
            {
                if (! isContainedPath (dir, juce::File (m["backup"].toString()))
                    || ! isContainedPath (session, juce::File (m["source"].toString()))) return false;
                if (! juce::File (m["backup"].toString()).copyFileTo (juce::File (m["source"].toString()))) return false;
            }
        o->setProperty ("phase", "recovered");
        return atomicfile::writeText (dir.getChildFile ("journal.json"),
                                      juce::JSON::toString (v));
    }

    static bool recover (const juce::File& session)
    {
        for (const auto& dir : session.getChildFile ("Session File Backups").findChildFiles (
                 juce::File::findDirectories, false, "reorder_*"))
            if (! recoverOne (dir)) return false;
        return true;
    }

    bool begin (const juce::File& session, const std::vector<Move>& moves)
    {
        if (! recover (session)) return false;
        const auto backups = session.getChildFile ("Session File Backups");
        if (! isContainedPath (session, backups)) return false;
        for (const auto& m : moves)
            if (! isContainedPath (session, m.first) || ! isContainedPath (session, m.second))
                return false;
        folder = backups.getChildFile ("reorder_" + juce::Uuid().toString());
        if (folder.createDirectory().failed()) return false;
        auto* o = new juce::DynamicObject(); journal = juce::var (o);
        o->setProperty ("phase", "stage");
        juce::Array<juce::var> entries, metadata;
        int i = 0;
        for (const auto& m : moves)
        {
            auto* e = new juce::DynamicObject();
            e->setProperty ("source", m.first.getFullPathName());
            e->setProperty ("dest", m.second.getFullPathName());
            e->setProperty ("temp", folder.getChildFile (juce::String (i++) + m.first.getFileExtension()).getFullPathName());
            entries.add (juce::var (e));
        }
        for (const auto& f : session.findChildFiles (juce::File::findFiles, false, "*.zfproj;session_mix.json"))
        {
            auto backup = folder.getChildFile ("metadata_" + f.getFileName());
            if (! f.copyFileTo (backup)) return false;
            auto* e = new juce::DynamicObject();
            e->setProperty ("source", f.getFullPathName()); e->setProperty ("backup", backup.getFullPathName());
            metadata.add (juce::var (e));
        }
        o->setProperty ("moves", entries); o->setProperty ("metadata", metadata);
        if (! save()) return false;
        for (const auto& e : entries)
            if (! juce::File (e["source"].toString()).moveFileTo (juce::File (e["temp"].toString())))
            { recoverOne (folder); return false; }
        o->setProperty ("phase", "install");
        if (! save()) { recoverOne (folder); return false; }
        for (const auto& e : entries)
        {
            juce::File dest (e["dest"].toString());
            if (dest.exists() || dest.getParentDirectory().createDirectory().failed()
                || ! juce::File (e["temp"].toString()).moveFileTo (dest))
            { recoverOne (folder); return false; }
        }
        return true;
    }

    bool commit()
    {
        journal.getDynamicObject()->setProperty ("phase", "committed");
        return save();
    }
private:
    static bool isContainedPath (const juce::File& root, const juce::File& child)
    {
        return pathsafety::isStrictDescendant (root, child);
    }

    bool save()
    {
        return atomicfile::writeText (folder.getChildFile ("journal.json"),
                                      juce::JSON::toString (journal));
    }
};
}
