#pragma once

#include "Markers.h"
#include "TrackExporter.h"

#include <atomic>
#include <functional>
#include <vector>

namespace zynforge
{
    class AudioEngine;

    // Export Songs: cut the show at its markers and write each song as a mix,
    // stems or both, to Export Files/Songs/NN - <Name>/. The cuts come from the
    // pure rangesFromMarkers; the render is the existing streaming path with a
    // timeline range, so every song is the same samples a whole-session render
    // has at that spot, and a long show stays memory-flat.
    namespace songexport
    {
        struct Range
        {
            juce::int64  start { 0 }, end { 0 };
            juce::String name;
        };

        // Each marker to the next, the last to sessionEnd. Markers at the same
        // spot make one song (the first named one names it), and one at or past
        // the end starts nothing. No markers, no songs: a session with no cuts
        // has nothing to divide, and the whole-show bounces already exist.
        std::vector<Range> rangesFromMarkers (std::vector<Marker> markers, juce::int64 sessionEnd);

        // Safe as one path component. '.' goes too: withFileExtension would
        // read "Out 2.1" as a name with extension ".1".
        juce::String safeName (const juce::String& name);

        // What the dialog hands back.
        struct Choice
        {
            std::vector<Range> songs;
            std::vector<int>   numbers;     // the NN of each song, its row in the list
            bool mix { true }, stems { false }, stereoOneFile { true };
            ExportOptions format;
        };

        // One file to write, fully resolved on the message thread.
        struct Target
        {
            enum class Kind { Mix, MonoStem, StereoStem };
            Kind kind { Kind::Mix };
            int  track { -1 };
            juce::File destStem;    // no extension
        };

        struct SongPlan
        {
            juce::int64 start { 0 }, end { 0 };
            juce::File  folder;
            std::vector<Target> targets;
        };

        struct Plan
        {
            std::vector<SongPlan> songs;
            ExportOptions format;
            double renderRate { 48000.0 };
            bool soloActive { false };
            int totalFiles() const noexcept
            {
                int n = 0;
                for (const auto& song : songs) n += (int) song.targets.size();
                return n;
            }
        };

        // Message thread: resolves folders, names and stereo layout from the
        // strips as they stand. A stem is planned only for a strip with an
        // audible clip in that song, so a folder holds the channels that played.
        Plan buildPlan (AudioEngine&, const Choice&, const juce::File& exportFilesDir);

        struct Outcome
        {
            int attempted { 0 }, written { 0 };
            int silentMixes { 0 };     // finished, and nothing in them
            bool soloActive { false };
            bool cancelled { false };
            juce::String firstError;
            bool complete() const noexcept { return ! cancelled && attempted > 0 && written == attempted; }
        };

        // (files finished, files in the plan, what comes next)
        using Progress = std::function<void (int, int, const juce::String&)>;

        // Worker thread, under the session-job reservation. Reads the engine's
        // clips and lanes, never the message thread's UI. Refuses during a take.
        Outcome run (AudioEngine&, const Plan&, const std::atomic<bool>& cancel, const Progress&);

        // What a finished mix render must add to "done": a silent file is not a
        // success, and a solo left on shaped what is in it. Empty when neither.
        juce::String mixRenderWarning (bool silent, bool soloActive);

        // The line the status bar shows, in the wording of a track export.
        juce::String statusLine (const Outcome&, const juce::String& destName);
    }
}
