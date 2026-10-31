#pragma once

#include "ClipModel.h"
#include "MixKernel.h"
#include "MultiPartReader.h"

#include <atomic>
#include <map>
#include <optional>
#include <vector>

namespace zynforge::interchange
{
    // The session as another DAW should see it, shared by the Reaper and
    // DAWproject writers. Two steps, because the first must not touch the disk
    // and the second must not touch the engine:
    //
    //   SessionInput  plain values copied on the message thread
    //                 (AudioEngine::captureInterchange), before a job starts.
    //   Project       what resolve() makes of them on the worker: every clip
    //                 mapped to the part file that holds it, every stereo pair
    //                 collapsed to one track, every missing file refused.
    //
    // Nothing here reads a TrackState, so tests can build a SessionInput by hand.

    // ── Captured on the message thread ───────────────────────────────────────
    struct StripInput
    {
        juce::String name;
        juce::uint32 colourArgb { 0 };      // 0 = no colour chosen
        bool  stereoLeft { false };         // L half of a pair; the next strip is its R
        bool  isBus      { false };
        float faderDb    { 0.0f };
        float pan        { 0.0f };
        bool  muted      { false };
        float vcaDb      { 0.0f };          // 0 = no VCA
        bool  vcaAssigned { false };
        bool  vcaMuted   { false };
        bool  hasSends   { false };
        int   automationLanes { 0 };        // lanes holding points
        // nullopt: never given a clip list, so it plays its whole file. An
        // empty list is different: the engineer deleted every clip, and the
        // strip stays silent however much media sits on disk.
        std::optional<std::vector<Clip>> clips;
    };

    struct MarkerInput { juce::int64 sample { 0 }; juce::String name; };

    struct SessionInput
    {
        juce::File   sessionDir;
        juce::String sessionName;
        double sampleRate { 48000.0 };
        double tempoBpm   { 120.0 };
        int    timeSigNum { 4 }, timeSigDen { 4 };
        bool   hasTempoMap { false };
        bool   anySolo     { false };
        float  masterDb    { 0.0f };
        bool   masterMuted { false };
        std::vector<StripInput>  strips;
        std::vector<MarkerInput> markers;
    };

    // ── Resolved on the worker ───────────────────────────────────────────────
    struct Item
    {
        juce::String name;
        juce::File   file;
        // Which channel of `file` plays: -1 all of it, 0 left, 1 right.
        int          channel { -1 };
        int          fileChannels { 1 };
        double       fileRate { 48000.0 };
        juce::int64  fileLength { 0 };
        juce::int64  position { 0 };        // timeline, session samples
        juce::int64  sourceOffset { 0 };    // into `file`, in its own samples
        juce::int64  length { 0 };
        float        gainDb { 0.0f };
        juce::int64  fadeIn { 0 }, fadeOut { 0 };
        int          fadeCurve { 0 };       // as Clip: 0 linear, 1 equal power
        bool         muted { false };
    };

    struct Track
    {
        juce::String name;
        juce::uint32 colourArgb { 0 };
        int    channels { 1 };              // 2 = one stereo track from a pair
        double gain { 1.0 };                // linear, with the VCA folded in
        float  pan { 0.0f };                // mono tracks
        float  panLeft { -1.0f }, panRight { 1.0f };   // stereo tracks, per side
        bool   muted { false };
        std::vector<Item> items;
    };

    struct Project
    {
        juce::String sessionName;
        double sampleRate { 48000.0 };
        double tempoBpm { 120.0 };
        int    timeSigNum { 4 }, timeSigDen { 4 };
        double masterGain { 1.0 };
        bool   masterMuted { false };
        std::vector<Track> tracks;
        std::vector<MarkerInput> markers;
        // True for both formats; each writer adds what only it cannot carry.
        juce::StringArray notes;
    };

    struct Resolved
    {
        Project project;
        juce::String error;   // non-empty: nothing may be written
        bool ok() const noexcept { return error.isEmpty(); }
    };

    // Names can come off a networked console. Both formats are text, so a
    // control character is a way to break out of the field it sits in: a
    // newline in an .RPP starts a new statement, and XML 1.0 has no legal
    // spelling for most of them.
    inline juce::String cleanText (const juce::String& text)
    {
        juce::String out;
        out.preallocateBytes (text.getNumBytesAsUTF8());
        for (auto p = text.getCharPointer(); ! p.isEmpty();)
        {
            const auto c = p.getAndAdvance();
            out << ((c < 0x20 || c == 0x7f) ? juce::juce_wchar (' ') : c);
        }
        return out;
    }

    // A session's media as resolve() needs it: one take is a sequence of files.
    struct TakeMedia
    {
        std::vector<juce::File>  parts;
        std::vector<juce::int64> starts;    // take position of each part
        juce::int64 total { 0 };
        int         channels { 0 };
        double      rate { 0.0 };
        juce::String problem;
    };

    class MediaCache
    {
    public:
        MediaCache() { formats.registerBasicFormats(); }

        const TakeMedia& take (const juce::File& mainFile)
        {
            const auto key = mainFile.getFullPathName();
            const auto found = takes.find (key);
            if (found != takes.end()) return found->second;
            return takes.emplace (key, open (mainFile)).first->second;
        }

    private:
        TakeMedia open (const juce::File& mainFile)
        {
            TakeMedia media;
            media.parts = findTakeParts (mainFile);
            if (media.parts.empty()) media.problem = "missing " + mainFile.getFileName();
            for (const auto& part : media.parts)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (
                    part.existsAsFile() ? formats.createReaderFor (part) : nullptr);
                if (reader == nullptr)
                {
                    media.problem = "missing or unreadable "
                        + (part == juce::File() ? mainFile.getFileName() + " part" : part.getFileName());
                    return media;
                }
                if (media.starts.empty())
                {
                    media.channels = (int) reader->numChannels;
                    media.rate     = reader->sampleRate;
                }
                else if ((int) reader->numChannels != media.channels
                         || ! juce::exactlyEqual (reader->sampleRate, media.rate))
                {
                    media.problem = part.getFileName() + " does not match the rest of its take";
                    return media;
                }
                media.starts.push_back (media.total);
                media.total += reader->lengthInSamples;
            }
            return media;
        }

        juce::AudioFormatManager formats;
        std::map<juce::String, TakeMedia> takes;
    };

    namespace detail
    {
        // The file and channel a strip reads when a clip names no file of its
        // own: the same resolution ArrangementSource::open makes for a render.
        struct OwnMedia { juce::File file; int channel { -1 }; };

        inline OwnMedia ownMedia (const juce::File& audioDir, int strip, MediaCache& media)
        {
            const auto own = findTrackMedia (audioDir, strip);
            if (own.existsAsFile())
                return { own, media.take (own).channels >= 2 ? 0 : -1 };
            if (strip > 0)
            {
                const auto left = findTrackMedia (audioDir, strip - 1);
                if (left.existsAsFile() && media.take (left).channels >= 2)
                    return { left, 1 };
            }
            return {};
        }

        // A DAW fade runs from silence to full level. A piece that starts or
        // ends part-way along a ramp (a split inside a fade, or a fade across
        // a part boundary) has no such spelling. The piece that touches
        // silence keeps a fade over its own span and the others play at full
        // level, so the ramp finishes early rather than dipping at the seam.
        inline void applyFades (const Clip& piece, Item& item, int& approximated)
        {
            item.fadeIn  = piece.fadeInRampOffsetSamples  > 0 ? 0 : piece.fadeInSamples;
            item.fadeOut = piece.fadeOutRampOffsetSamples > 0 ? 0 : piece.fadeOutSamples;
            item.fadeCurve = piece.fadeCurve;
            if (piece.fadeInRampSamples > 0)  ++approximated;
            if (piece.fadeOutRampSamples > 0) ++approximated;
        }

        inline juce::String stripLabel (const StripInput& strip, int index)
        {
            return "Track " + juce::String (index + 1)
                 + (strip.name.isNotEmpty() ? " (" + strip.name + ")" : juce::String());
        }

        // Everything one strip plays, cut at part boundaries so each piece is
        // one file. Empty string on success, else why the export must stop.
        inline juce::String stripItems (const SessionInput& in, int index, const juce::File& audioDir,
                                        MediaCache& media, std::vector<Item>& out, int& approximated)
        {
            const auto& strip = in.strips[(size_t) index];
            const auto own = ownMedia (audioDir, index, media);

            std::vector<Clip> clips;
            if (strip.clips.has_value())
                clips = *strip.clips;
            else if (own.file.existsAsFile())
            {
                const auto& take = media.take (own.file);
                if (take.problem.isNotEmpty())
                    return stripLabel (strip, index) + ": " + take.problem;
                Clip whole;
                whole.name = cleanText (strip.name);
                whole.fileLengthSamples = take.total;
                clips.push_back (whole);
            }

            for (const auto& clip : clips)
            {
                if (clip.fileLengthSamples <= 0) continue;
                const bool fromOwn = clip.audioFile == juce::File() || clip.audioFile == own.file;
                const auto file = fromOwn ? own.file : clip.audioFile;
                if (! file.existsAsFile())
                    return stripLabel (strip, index) + ": clip \"" + clip.name + "\" has no media"
                         + (file == juce::File() ? juce::String() : " (" + file.getFileName() + " is missing)");

                const auto& take = media.take (file);
                if (take.problem.isNotEmpty())
                    return stripLabel (strip, index) + ": " + take.problem;
                if (clip.fileStartSamples < 0
                    || clip.fileStartSamples + clip.fileLengthSamples > take.total)
                    return stripLabel (strip, index) + ": clip \"" + clip.name
                         + "\" runs past the end of " + file.getFileName();

                const int side = clip.sourceChannel >= 0 ? juce::jlimit (0, 1, clip.sourceChannel)
                                                         : (fromOwn ? juce::jmax (0, own.channel) : 0);
                if (side >= take.channels)
                    return stripLabel (strip, index) + ": clip \"" + clip.name
                         + "\" reads a channel " + file.getFileName() + " does not have";

                for (juce::int64 from = 0; from < clip.fileLengthSamples;)
                {
                    const auto takePos = clip.fileStartSamples + from;
                    size_t part = take.parts.size() - 1;
                    while (take.starts[part] > takePos) --part;
                    const auto partEnd = part + 1 < take.parts.size() ? take.starts[part + 1] : take.total;
                    const auto length = juce::jmin (clip.fileLengthSamples - from, partEnd - takePos);
                    const auto piece = clipSlice (clip, from, length);

                    Item item;
                    item.name         = cleanText (clip.name);
                    item.file         = take.parts[part];
                    item.channel      = take.channels >= 2 ? side : -1;
                    item.fileChannels = take.channels;
                    item.fileRate     = take.rate;
                    item.fileLength   = partEnd - take.starts[part];
                    item.position     = piece.timelineStartSamples;
                    item.sourceOffset = takePos - take.starts[part];
                    item.length       = length;
                    item.gainDb       = clip.gainDb;
                    item.muted        = clip.muted;
                    applyFades (piece, item, approximated);
                    out.push_back (std::move (item));
                    from += length;
                }
            }
            return {};
        }

        // The two halves of one stereo clip: the same span of the same file,
        // left and right, edited identically.
        inline bool sameStereoClip (const Item& l, const Item& r) noexcept
        {
            return l.file == r.file && l.fileChannels == 2
                && l.channel == 0 && r.channel == 1
                && l.position == r.position && l.sourceOffset == r.sourceOffset
                && l.length == r.length && juce::exactlyEqual (l.gainDb, r.gainDb)
                && l.fadeIn == r.fadeIn && l.fadeOut == r.fadeOut
                && l.fadeCurve == r.fadeCurve && l.muted == r.muted;
        }

        inline std::optional<std::vector<Item>> mergeStereo (const std::vector<Item>& left,
                                                             const std::vector<Item>& right)
        {
            if (left.size() != right.size()) return std::nullopt;
            std::vector<Item> merged;
            merged.reserve (left.size());
            for (size_t i = 0; i < left.size(); ++i)
            {
                if (! sameStereoClip (left[i], right[i])) return std::nullopt;
                merged.push_back (left[i]);
                merged.back().channel = -1;
            }
            return merged;
        }

        inline Track trackFrom (const StripInput& strip)
        {
            Track track;
            track.name       = cleanText (strip.name);
            track.colourArgb = strip.colourArgb;
            track.gain  = juce::Decibels::decibelsToGain ((double) mix::stripDb (strip.faderDb, strip.vcaDb), -60.0);
            track.pan   = strip.pan;
            track.muted = strip.muted || strip.vcaMuted;
            return track;
        }

        inline juce::String plural (int n, const juce::String& noun)
        {
            return juce::String (n) + " " + noun + (n == 1 ? "" : "s");
        }
    }

    inline Resolved resolve (const SessionInput& in, const std::atomic<bool>& cancel)
    {
        using namespace detail;
        Resolved result;
        auto& project = result.project;
        project.sessionName = cleanText (in.sessionName);
        project.sampleRate  = in.sampleRate;
        project.tempoBpm    = in.tempoBpm;
        project.timeSigNum  = in.timeSigNum;
        project.timeSigDen  = in.timeSigDen;
        project.masterGain  = juce::Decibels::decibelsToGain ((double) in.masterDb, -60.0);
        project.masterMuted = in.masterMuted;
        for (const auto& marker : in.markers)
            if (marker.sample >= 0)
                project.markers.push_back ({ marker.sample, cleanText (marker.name) });

        auto audioDir = in.sessionDir.getChildFile ("Audio Files");
        if (! audioDir.isDirectory()) audioDir = in.sessionDir;

        MediaCache media;
        int approximatedFades = 0, automation = 0, splitPairs = 0, sends = 0, vcas = 0;
        const int count = (int) in.strips.size();
        for (int index = 0; index < count; ++index)
        {
            if (cancel.load (std::memory_order_relaxed)) { result.error = "Cancelled"; return result; }
            const auto& strip = in.strips[(size_t) index];
            automation += strip.automationLanes;
            sends      += strip.hasSends ? 1 : 0;
            vcas       += strip.vcaAssigned ? 1 : 0;

            // A pair's right half travels with its left.
            if (index > 0 && in.strips[(size_t) index - 1].stereoLeft) continue;

            // A bus mixes other strips and plays no media of its own, even with
            // a stereo take sitting in the slot before it.
            if (strip.isBus)
            {
                project.tracks.push_back (trackFrom (strip));
                continue;
            }

            std::vector<Item> left;
            if (auto problem = stripItems (in, index, audioDir, media, left, approximatedFades);
                problem.isNotEmpty())
            { result.error = problem; return result; }

            if (! strip.stereoLeft || index + 1 >= count)
            {
                auto track = trackFrom (strip);
                track.items = std::move (left);
                project.tracks.push_back (std::move (track));
                continue;
            }

            const auto& rightStrip = in.strips[(size_t) index + 1];
            std::vector<Item> right;
            int rightFades = 0;
            if (auto problem = stripItems (in, index + 1, audioDir, media, right, rightFades);
                problem.isNotEmpty())
            { result.error = problem; return result; }

            auto track = trackFrom (strip);
            if (auto merged = mergeStereo (left, right))
            {
                track.channels = 2;
                track.panLeft  = strip.pan;
                track.panRight = rightStrip.pan;
                track.items    = std::move (*merged);
                project.tracks.push_back (std::move (track));
                continue;
            }

            // One stereo clip cannot read two files, nor two different edits
            // (a pair recorded before stereo was one file, or halves edited
            // apart), so these stay two tracks rather than lose a side.
            ++splitPairs;
            approximatedFades += rightFades;
            auto rightTrack = trackFrom (rightStrip);
            rightTrack.name = track.name + " R";
            track.name << " L";
            track.items      = std::move (left);
            rightTrack.items = std::move (right);
            project.tracks.push_back (std::move (track));
            project.tracks.push_back (std::move (rightTrack));
        }

        auto& notes = project.notes;
        if (automation > 0)
            notes.add ("Automation is not exported (" + plural (automation, "lane")
                       + "): tracks carry their static fader, pan and mute.");
        if (sends > 0)
            notes.add ("Aux sends and bus routing are not exported (" + plural (sends, "strip")
                       + " with sends): bus tracks arrive empty.");
        if (vcas > 0)
            notes.add ("VCA groups are not exported: " + plural (vcas, "strip")
                       + " carry the VCA level in the track fader.");
        if (approximatedFades > 0)
            notes.add (plural (approximatedFades, "fade") + " cut by a split or a file boundary"
                       " became ordinary fades on the piece that reaches silence.");
        if (splitPairs > 0)
            notes.add (plural (splitPairs, "stereo pair") + " exported as two mono tracks"
                       " (L and R): the sides read different files or were edited apart.");
        if (in.hasTempoMap)
            notes.add ("Tempo changes are not exported: the project runs at the session tempo.");
        if (in.anySolo)
            notes.add ("Solo is not exported: every track plays unless it is muted.");
        return result;
    }
}
