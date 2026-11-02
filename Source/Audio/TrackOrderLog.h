#pragma once

#include <juce_core/juce_core.h>

#include "AtomicFile.h"
#include "TrackFileTransaction.h"

#include <vector>

// A take file is named by its strip's position, so moving a strip renames the
// files. The session keeps every such move here, in its own transaction, as
// the order of `destination` said it (old index -> new index, -1 = removed).
// A backup or mirror cannot move with the primary at that moment: its drive may
// be missing, or may hang, and a strip move runs on the message thread. Each
// copy instead records how far along this log it is, in `.zynforge-primary`,
// and catches up on its own thread before it next joins a take.
namespace zynforge::trackorder
{
    inline constexpr const char* kLogName = ".zynforge-track-order.json";

    struct Step
    {
        juce::String from, to;
        std::vector<int> destination;
    };

    struct Log
    {
        juce::String current;      // empty: no strip has moved since this log began
        std::vector<Step> steps;   // from[i + 1] == to[i]
        bool readable { true };
    };

    inline Log read (const juce::File& session)
    {
        Log log;
        const auto file = session.getChildFile (kLogName);
        if (! file.existsAsFile()) return log;
        const auto parsed = juce::JSON::parse (file);
        auto* steps = parsed.getProperty ("steps", {}).getArray();
        if (steps == nullptr) { log.readable = false; return log; }
        log.current = parsed.getProperty ("current", "").toString();
        for (const auto& entry : *steps)
        {
            Step step { entry.getProperty ("from", "").toString(), entry.getProperty ("to", "").toString(), {} };
            if (auto* destination = entry.getProperty ("destination", {}).getArray())
                for (const auto& index : *destination) step.destination.push_back ((int) index);
            log.steps.push_back (std::move (step));
        }
        return log;
    }

    inline bool write (const juce::File& session, const Log& log)
    {
        juce::Array<juce::var> steps;
        for (const auto& step : log.steps)
        {
            juce::Array<juce::var> destination;
            for (const int index : step.destination) destination.add (index);
            auto* entry = new juce::DynamicObject();
            entry->setProperty ("from", step.from);
            entry->setProperty ("to", step.to);
            entry->setProperty ("destination", destination);
            steps.add (juce::var (entry));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("current", log.current);
        root->setProperty ("steps", steps);
        return atomicfile::writeText (session.getChildFile (kLogName), juce::JSON::toString (juce::var (root)));
    }

    // Written before a move's journal opens, so a rollback has a copy of it to
    // restore. Without one, an interrupted first move would leave a step that
    // never happened on the primary.
    inline bool ensureLog (const juce::File& session)
    {
        return session.getChildFile (kLogName).existsAsFile() || write (session, {});
    }

    // Inside the move's transaction, after its files moved: the journal's commit
    // is what makes the step true, and a rollback restores the log with the rest.
    inline bool append (const juce::File& session, const std::vector<int>& destination)
    {
        bool moved = false;
        for (size_t i = 0; i < destination.size(); ++i) moved = moved || destination[i] != (int) i;
        if (! moved) return true;
        auto log = read (session);
        if (! log.readable) return false;
        const auto to = juce::Uuid().toString();
        log.steps.push_back ({ log.current, to, destination });
        log.current = to;
        return write (session, log);
    }

    // Every take file a step renames in one Audio Files folder: all containers
    // and continuation parts move together, and removed audio is archived, never
    // deleted. Sidecars stay out of it. They belong to a punch's recovery, which
    // runs under the old names first.
    inline std::vector<TrackFileTransaction::Move> takeMoves (const juce::File& audio, const juce::File& removedDir,
                                                              const std::vector<int>& destination)
    {
        std::vector<TrackFileTransaction::Move> moves;
        for (int old = 0; old < (int) destination.size(); ++old)
        {
            const auto prefix = juce::String::formatted ("Track_%02d", old + 1);
            for (const auto& f : audio.findChildFiles (juce::File::findFiles, false, prefix + ".*;" + prefix + "_part*"))
            {
                if (f.getFileName().containsIgnoreCase (".punchbase")) continue;
                if (! f.hasFileExtension ("wav;flac;aif;aiff")) continue;
                const int dest = destination[(size_t) old];
                const auto target = dest >= 0
                    ? audio.getChildFile (juce::String::formatted ("Track_%02d", dest + 1)
                                           + f.getFileName().substring (prefix.length()))
                    : removedDir.getChildFile (f.getFileName());
                if (target != f) moves.emplace_back (f, target);
            }
        }
        return moves;
    }
}
