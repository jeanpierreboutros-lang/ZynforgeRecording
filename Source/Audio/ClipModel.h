#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace zynforge
{
    // Clip + Playlist data model. Scaffolding for clip-level editing
    // (split / trim / fade / move) and comp playlists per track. The
    // recorder still writes one big Track_NN.wav today; clips index
    // into that file via [fileStartSamples, fileEndSamples].
    //
    // Editor UI lands in a follow-up — this header is just the shape.
    struct Clip
    {
        juce::String name;
        juce::File   audioFile;
        // Position on the timeline.
        juce::int64  timelineStartSamples { 0 };
        // Region of the underlying audio file referenced by this clip.
        juce::int64  fileStartSamples     { 0 };
        juce::int64  fileLengthSamples    { 0 };
        // Per-clip fades + gain — non-destructive on the audio file.
        juce::int64  fadeInSamples        { 0 };
        juce::int64  fadeOutSamples       { 0 };
        float        gainDb               { 0.0f };
        // Editing flags.
        //   muted  → clip outputs silence (still claims its timeline range)
        //   locked → engine refuses trim / move / fade edits; UI greys
        //            the handles. Use to pin a 'keeper' take during
        //            virtual soundcheck so a stray drag can't ruin it.
        bool         muted                { false };
        bool         locked               { false };
    };

    // A 'take' is a named clip list. A track can hold many takes
    // (different attempts at the same song); only one is the active
    // playback take at any time. Engineer swipes between takes for
    // comping.
    struct Take
    {
        juce::String name;
        std::vector<Clip> clips;
    };

    // Playlist = container of takes belonging to one track. The active
    // take is what the SessionPlayer actually plays back. The non-
    // active takes stay on disk and are reachable via the comp editor.
    struct Playlist
    {
        std::vector<Take> takes;
        int               activeTake { 0 };
    };

    // Split one clip in two at the given file-relative sample offset.
    // Returns true if the split landed inside the clip bounds.
    inline bool splitClipAt (std::vector<Clip>& list, int clipIdx, juce::int64 sampleInFile)
    {
        if (clipIdx < 0 || clipIdx >= (int) list.size()) return false;
        auto& src = list[(size_t) clipIdx];
        if (sampleInFile <= src.fileStartSamples
            || sampleInFile >= src.fileStartSamples + src.fileLengthSamples)
            return false;

        Clip right = src;
        const juce::int64 splitOffset = sampleInFile - src.fileStartSamples;
        // Left half keeps its start, shrinks to splitOffset.
        src.fileLengthSamples = splitOffset;
        src.fadeOutSamples    = juce::jmin (src.fadeOutSamples, splitOffset);
        // Right half starts where left ended, on the same timeline.
        right.fileStartSamples     = sampleInFile;
        right.fileLengthSamples    = right.fileLengthSamples - splitOffset;
        right.timelineStartSamples = src.timelineStartSamples + splitOffset;
        right.fadeInSamples        = 0;   // a fresh edit has a hard cut at the split
        right.name                 = src.name + " (b)";
        src.name                   = src.name + " (a)";

        list.insert (list.begin() + clipIdx + 1, right);
        return true;
    }
}
