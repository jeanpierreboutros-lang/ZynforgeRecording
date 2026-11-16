#pragma once

#include <juce_core/juce_core.h>

#include "AtomicFile.h"
#include "TakeNames.h"
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
    // and continuation parts move together, and so do the strip's Consolidate
    // renders, whatever stem they carry, so a render never stays behind to name
    // the next strip at that number; removed audio is archived, never deleted.
    // Clips that name a render follow it through the caller's remap. Sidecars
    // stay out of it. They belong to a punch's recovery, which runs under the
    // old names first.
    //
    // A number two takes claim (TakeNames.h) cannot move: nothing here picks
    // which stem to renumber. Then `clash` (when given) says which files, and
    // nothing at all is returned, so no strip moves. `names` reads a session
    // folder's root, from before Audio Files, as Track_NN only.
    inline std::vector<TrackFileTransaction::Move> takeMoves (const juce::File& audio, const juce::File& removedDir,
                                                              const std::vector<int>& destination,
                                                              juce::String* clash = nullptr,
                                                              takenames::StemIndex::Names names
                                                                  = takenames::StemIndex::Names::any)
    {
        std::vector<TrackFileTransaction::Move> moves;
        const takenames::StemIndex onDisk (audio, names);
        for (int old = 0; old < (int) destination.size(); ++old)
        {
            const int dest = destination[(size_t) old];
            if (dest == old) continue;
            if (onDisk.clashes (old))
            {
                if (clash != nullptr)
                    *clash = "Track " + juce::String (old + 1) + " has " + onDisk.clash (old);
                return {};
            }
            // The number changes and a name stays: "03 SNARE" -> "01 SNARE",
            // "Track_03" -> "Track_01" (TakeNames.h).
            auto files = onDisk.takeAudio (old);
            files.addArray (onDisk.renders (old));
            for (const auto& f : files)
            {
                const auto p = takenames::parse (f.getFileName());
                const auto target = dest >= 0
                    ? audio.getChildFile (takenames::renumbered (p.stem, dest) + p.rest)
                    : removedDir.getChildFile (f.getFileName());
                if (target != f) moves.emplace_back (f, target);
            }
        }
        return moves;
    }
}
