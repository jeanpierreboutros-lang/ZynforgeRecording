#pragma once

#include "AtomicFile.h"
#include "DawProject.h"
#include "ReaperProject.h"
#include "SessionInterchange.h"

#include <algorithm>

namespace zynforge::projectexport
{
    enum class Format { Reaper, DawProject };

    struct Result
    {
        bool ok { false };
        juce::String error;
        juce::File file;
        juce::StringArray notes;   // what was approximated or left out
    };

    inline juce::String extensionFor (Format format)
    {
        return format == Format::Reaper ? ".RPP" : ".dawproject";
    }

    // What this format cannot carry, on top of what no format can.
    inline juce::StringArray notesFor (const interchange::Project& project, Format format)
    {
        auto notes = project.notes;
        int clipGains = 0, equalPower = 0, oneSide = 0, wideOrNarrow = 0, overlaps = 0;
        for (const auto& track : project.tracks)
        {
            if (track.channels == 2
                && ! (juce::exactlyEqual (track.panLeft, -1.0f) && juce::exactlyEqual (track.panRight, 1.0f)))
                ++wideOrNarrow;

            std::vector<std::pair<juce::int64, juce::int64>> spans;
            for (const auto& item : track.items)
            {
                if (std::abs (item.gainDb) > 0.0001f) ++clipGains;
                if (item.fadeCurve == 1 && (item.fadeIn > 0 || item.fadeOut > 0)) ++equalPower;
                if (item.channel >= 0 && item.fileChannels >= 2) ++oneSide;
                spans.emplace_back (item.position, item.position + item.length);
            }
            std::sort (spans.begin(), spans.end());
            juce::int64 reached = 0;
            for (const auto& [start, end] : spans)
            {
                if (start < reached) ++overlaps;
                reached = juce::jmax (reached, end);
            }
        }
        using interchange::detail::plural;
        if (format == Format::DawProject)
        {
            if (clipGains > 0)
                notes.add ("DAWproject has no clip gain, so these play at unity: " + plural (clipGains, "clip")
                           + ". Bounce stems if that level matters.");
            if (equalPower > 0)
                notes.add ("DAWproject has no fade shape, so the importing DAW's default curve replaces"
                           " equal-power fades on: " + plural (equalPower, "clip") + ".");
            if (oneSide > 0)
                notes.add ("DAWproject cannot pick one channel of a stereo file, so these play both"
                           " sides on a mono track: " + plural (oneSide, "clip") + ".");
            if (wideOrNarrow > 0)
                notes.add ("DAWproject has one pan per track, so only the balance is carried for tracks"
                           " not panned hard left and right: " + plural (wideOrNarrow, "stereo track") + ".");
            if (overlaps > 0)
                notes.add ("Overlapping clips sum in the session; the importing DAW may treat them its"
                           " own way: " + plural (overlaps, "clip") + ".");
            notes.add ("Pan laws differ between DAWs: levels of panned tracks may not match the session mix.");
        }
        return notes;
    }

    // `dest` must not exist; the caller picks it. Nothing is written unless
    // every clip resolved, and the file appears whole or not at all.
    inline Result write (const interchange::SessionInput& input, Format format, const juce::File& dest,
                         const juce::String& appVersion, const std::atomic<bool>& cancel)
    {
        Result result;
        const auto resolved = interchange::resolve (input, cancel);
        if (! resolved.ok()) { result.error = resolved.error; return result; }

        const auto& project = resolved.project;
        if (format == Format::Reaper)
            for (const auto& track : project.tracks)
                for (const auto& item : track.items)
                    if (! reaperproject::isQuotable (reaperproject::relativePath (item.file, dest)))
                    {
                        result.error = item.file.getFullPathName()
                                     + " has a path an .RPP cannot hold (line breaks, or all three quote marks)";
                        return result;
                    }
        if (cancel.load (std::memory_order_relaxed)) { result.error = "Cancelled"; return result; }

        result.notes = notesFor (project, format);
        bool wrote = false;
        if (format == Format::Reaper)
        {
            wrote = atomicfile::writeText (dest, reaperproject::build (
                project, dest, result.notes, juce::Time::currentTimeMillis() / 1000));
        }
        else
        {
            const auto archive = dawproject::buildArchive (
                dawproject::buildProjectXml (project, dest, appVersion),
                dawproject::buildMetadataXml (project.sessionName, result.notes));
            wrote = atomicfile::writeBytes (dest, archive.getData(), archive.getSize());
        }
        if (! wrote) { result.error = "Could not write " + dest.getFileName(); return result; }
        result.ok   = true;
        result.file = dest;
        return result;
    }

    // Export Files/<Session>.RPP, numbered rather than ever replaced.
    inline juce::File uniqueDestination (const juce::File& directory, const juce::String& sessionName,
                                         Format format)
    {
        const auto base = juce::File::createLegalFileName (sessionName).trim();
        return atomicfile::firstFreeName (directory, base.isNotEmpty() ? base : "Session", extensionFor (format));
    }

    // The session as it will sit under a handoff copy: same strips and clips,
    // media paths moved with the folder.
    inline interchange::SessionInput rebased (interchange::SessionInput input, const juce::File& newDir)
    {
        for (auto& strip : input.strips)
            if (strip.clips.has_value())
                for (auto& clip : *strip.clips)
                    if (clip.audioFile != juce::File() && clip.audioFile.isAChildOf (input.sessionDir))
                        clip.audioFile = newDir.getChildFile (clip.audioFile.getRelativePathFrom (input.sessionDir));
        input.sessionDir = newDir;
        return input;
    }

    // A clip reading a file outside the session. A handoff copies the session
    // folder only, so that clip would be silent on the receiving disk.
    inline juce::File firstMediaOutsideSession (const interchange::SessionInput& input)
    {
        for (const auto& strip : input.strips)
            if (strip.clips.has_value())
                for (const auto& clip : *strip.clips)
                    if (clip.audioFile != juce::File() && ! clip.audioFile.isAChildOf (input.sessionDir))
                        return clip.audioFile;
        return {};
    }

    // Both project files at the root of a handoff copy, reading the copy's own
    // Audio Files, so the folder opens as it stands on the mix engineer's disk.
    inline std::vector<juce::File> writeForHandoff (const interchange::SessionInput& input,
                                                    const juce::File& handoffDir,
                                                    const juce::String& appVersion,
                                                    const std::atomic<bool>& cancel,
                                                    juce::String& warning)
    {
        std::vector<juce::File> written;
        juce::StringArray missing;
        juce::String reason;
        const auto copied = rebased (input, handoffDir);
        for (const auto format : { Format::Reaper, Format::DawProject })
        {
            const auto dest = uniqueDestination (handoffDir, input.sessionName, format);
            const auto result = write (copied, format, dest, appVersion, cancel);
            if (result.ok) { written.push_back (result.file); continue; }
            missing.add (dest.getFileName());
            if (reason.isEmpty()) reason = result.error;
        }
        if (! missing.isEmpty()) warning = missing.joinIntoString (" and ") + " not written: " + reason;
        return written;
    }
}
