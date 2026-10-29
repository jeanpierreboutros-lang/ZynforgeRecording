#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <utility>
#include <vector>

namespace zynforge
{
    // Clip + Playlist data model. Scaffolding for clip-level editing
    // (split / trim / fade / move) and comp playlists per track. The
    // recorder still writes one big Track_NN.wav today; clips index
    // into that file via [fileStartSamples, fileEndSamples].
    //
    // Editor UI lands in a follow-up -- this header is just the shape.
    struct Clip
    {
        juce::String name;
        juce::File   audioFile;
        int          sourceChannel { -1 }; // -1 = track's own channel; explicit files default to 0
        // Position on the timeline.
        juce::int64  timelineStartSamples { 0 };
        // Region of the underlying audio file referenced by this clip.
        juce::int64  fileStartSamples     { 0 };
        juce::int64  fileLengthSamples    { 0 };
        // Per-clip fades + gain -- non-destructive on the audio file.
        juce::int64  fadeInSamples        { 0 };
        juce::int64  fadeOutSamples       { 0 };
        // A fade's RAMP can be longer than the part of it this clip plays: a
        // piece cut out of a fade (split, crop, comp) keeps the original ramp,
        // so the pieces sound exactly as the whole clip did. fadeIn/OutSamples
        // stay the part inside the clip (in + out <= length). 0 everywhere =
        // an ordinary fade whose ramp is the fade itself.
        juce::int64  fadeInRampSamples        { 0 };   // full fade-in ramp length
        juce::int64  fadeInRampOffsetSamples  { 0 };   // ramp before the clip start
        juce::int64  fadeOutRampSamples       { 0 };   // full fade-out ramp length
        juce::int64  fadeOutRampOffsetSamples { 0 };   // ramp after the clip end
        // Fade shape: 0 = linear, 1 = equal-power (constant-power, for
        // butt-spliced crossfades that don't dip in the middle).
        int          fadeCurve            { 0 };
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

    inline juce::int64 fadeInRampLength (const Clip& c) noexcept
    { return c.fadeInRampSamples > 0 ? c.fadeInRampSamples : c.fadeInSamples; }
    inline juce::int64 fadeOutRampLength (const Clip& c) noexcept
    { return c.fadeOutRampSamples > 0 ? c.fadeOutRampSamples : c.fadeOutSamples; }

    // Zero a fade's ramp fields when they describe an ordinary fade (or don't
    // fit it), so equal fades always have equal fields.
    inline void normaliseFadeRamps (Clip& c) noexcept
    {
        if (c.fadeInSamples <= 0 || c.fadeInRampOffsetSamples < 0
            || c.fadeInRampSamples < c.fadeInRampOffsetSamples + c.fadeInSamples
            || (c.fadeInRampOffsetSamples == 0 && c.fadeInRampSamples == c.fadeInSamples))
            c.fadeInRampSamples = c.fadeInRampOffsetSamples = 0;
        if (c.fadeOutSamples <= 0 || c.fadeOutRampOffsetSamples < 0
            || c.fadeOutRampSamples < c.fadeOutRampOffsetSamples + c.fadeOutSamples
            || (c.fadeOutRampOffsetSamples == 0 && c.fadeOutRampSamples == c.fadeOutSamples))
            c.fadeOutRampSamples = c.fadeOutRampOffsetSamples = 0;
    }

    // The fade envelope at clip-relative sample clipPos (1 outside the fades).
    // ONE definition for the real-time player and every offline render, so
    // live playback and the bounce stay sample-identical. For an ordinary fade
    // it is exactly clipPos / fadeIn, or (length - clipPos) / fadeOut.
    inline float clipFadeGain (const Clip& c, juce::int64 clipPos) noexcept
    {
        float t;
        if (c.fadeInSamples > 0 && clipPos < c.fadeInSamples)
            t = (float) (c.fadeInRampOffsetSamples + clipPos) / (float) fadeInRampLength (c);
        else if (c.fadeOutSamples > 0 && clipPos >= c.fileLengthSamples - c.fadeOutSamples)
            t = (float) (c.fileLengthSamples - clipPos + c.fadeOutRampOffsetSamples)
              / (float) fadeOutRampLength (c);
        else
            return 1.0f;
        return c.fadeCurve == 1 ? std::sin (t * 1.57079633f) : t;
    }

    // Where each visible fade starts and ends on its ramp (0 silent, 1 full),
    // for drawing: an ordinary fade-in is {0, 1}, a fade-out {1, 0}.
    inline std::pair<float, float> fadeInLevels (const Clip& c) noexcept
    {
        const auto ramp = (float) juce::jmax<juce::int64> (1, fadeInRampLength (c));
        return { (float) c.fadeInRampOffsetSamples / ramp,
                 (float) (c.fadeInRampOffsetSamples + c.fadeInSamples) / ramp };
    }
    inline std::pair<float, float> fadeOutLevels (const Clip& c) noexcept
    {
        const auto ramp = (float) juce::jmax<juce::int64> (1, fadeOutRampLength (c));
        return { (float) (c.fadeOutRampOffsetSamples + c.fadeOutSamples) / ramp,
                 (float) c.fadeOutRampOffsetSamples / ramp };
    }

    // The part [from, from + length) of clip c (clip-relative), placed where
    // it sat on the timeline. A piece that takes part of a fade keeps that
    // fade's ORIGINAL ramp, so the pieces of a clip play exactly what the
    // whole clip did; a piece outside the fades gets hard cuts.
    inline Clip clipSlice (const Clip& c, juce::int64 from, juce::int64 length)
    {
        Clip s = c;
        const auto to = from + length;
        s.timelineStartSamples = c.timelineStartSamples + from;
        s.fileStartSamples     = c.fileStartSamples + from;
        s.fileLengthSamples    = length;
        s.fadeInSamples = s.fadeInRampSamples = s.fadeInRampOffsetSamples = 0;
        s.fadeOutSamples = s.fadeOutRampSamples = s.fadeOutRampOffsetSamples = 0;
        if (from < c.fadeInSamples)
        {
            s.fadeInSamples           = juce::jmin (c.fadeInSamples, to) - from;
            s.fadeInRampSamples       = fadeInRampLength (c);
            s.fadeInRampOffsetSamples = c.fadeInRampOffsetSamples + from;
        }
        const auto outStart = c.fileLengthSamples - c.fadeOutSamples;
        if (c.fadeOutSamples > 0 && to > outStart)
        {
            s.fadeOutSamples           = to - juce::jmax (from, outStart);
            s.fadeOutRampSamples       = fadeOutRampLength (c);
            s.fadeOutRampOffsetSamples = c.fadeOutRampOffsetSamples + (c.fileLengthSamples - to);
        }
        normaliseFadeRamps (s);
        return s;
    }

    // Split one clip in two at the given file-relative sample offset.
    // Returns true if the split landed inside the clip bounds.
    inline bool splitClipAt (std::vector<Clip>& list, int clipIdx, juce::int64 sampleInFile)
    {
        if (clipIdx < 0 || clipIdx >= (int) list.size()) return false;
        const auto& src = list[(size_t) clipIdx];
        if (src.locked) return false;
        if (sampleInFile <= src.fileStartSamples
            || sampleInFile >= src.fileStartSamples + src.fileLengthSamples)
            return false;

        // A split outside the fades is a HARD cut on both sides -- no new fade
        // into or out of the seam (that would dip to silence there). The left
        // half keeps the fade-in and the right half the fade-out. A split
        // INSIDE a fade leaves both halves on the original ramp: clamping it
        // squeezed the left half's fade-in and started the right half at full
        // level, so the cut changed the audio on both sides.
        const juce::int64 splitOffset = sampleInFile - src.fileStartSamples;
        Clip left  = clipSlice (src, 0, splitOffset);
        Clip right = clipSlice (src, splitOffset, src.fileLengthSamples - splitOffset);
        left.name  = src.name + " (a)";
        right.name = src.name + " (b)";

        list[(size_t) clipIdx] = std::move (left);
        list.insert (list.begin() + clipIdx + 1, std::move (right));
        return true;
    }
}
