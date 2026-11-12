#pragma once

// Which clips one clip command reaches. A stereo pair is one logical strip and
// an edit group moves together, so every clip command -- mouse, menu or
// keyboard -- resolves its targets here. A command that took its own (track,
// index) moved L against R, and the row paints only L's list, so EDIT looked
// right while the pair played out of time.

#include "../Audio/AudioEngine.h"
#include "EditTimeline.h"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace zynforge::clipedit
{
    using ClipRef = std::pair<int, int>;   // (track, clip index)

    inline bool areStereoPartners (AudioEngine& engine, int a, int b)
    {
        auto& rec = engine.getRecorder();
        const int left = juce::jmin (a, b), right = juce::jmax (a, b);
        return left >= 0 && right == left + 1 && right < rec.getNumTracks()
            && rec.getTrack (left).isStereo.load (std::memory_order_relaxed);
    }

    // The physical tracks an edit on `track` must reach: its strip (both
    // halves of a pair) and every member of its edit group, each with its
    // partner. An ungrouped mono track returns just itself.
    inline std::vector<int> peerTracks (AudioEngine& engine, int track)
    {
        auto& rec = engine.getRecorder();
        const int n = rec.getNumTracks();
        const bool isRightHalf = track > 0 && areStereoPartners (engine, track - 1, track);
        const int strip = isRightHalf ? track - 1 : track;

        std::vector<int> members;
        if (strip >= 0 && strip < n)
            if (const int group = engine.getTrackEditGroup (strip); group >= 0)
                members = engine.getStripsInEditGroup (group);
        if (members.empty()) members.push_back (strip);

        std::vector<int> peers;
        const auto add = [&peers] (int t)
        {
            if (std::find (peers.begin(), peers.end(), t) == peers.end())
                peers.push_back (t);
        };
        for (int t : members)
        {
            if (t > 0 && areStereoPartners (engine, t - 1, t)) add (t - 1);
            add (t);
            if (areStereoPartners (engine, t, t + 1)) add (t + 1);
        }
        return peers;
    }

    // 0 a mono strip, 1 a stereo L, 2 a stereo R.
    inline int stereoHalf (AudioEngine& engine, int track)
    {
        if (areStereoPartners (engine, track, track + 1)) return 1;
        return areStereoPartners (engine, track - 1, track) ? 2 : 0;
    }

    // Peer's own clip for clip clipIdx of `track`; -1 when it has nothing there.
    inline int peerClipIndex (AudioEngine& engine, int track, int clipIdx, int peer)
    {
        if (peer == track) return clipIdx;
        const auto* source = engine.tryClipsFor (track);
        const auto* target = engine.tryClipsFor (peer);
        if (source == nullptr || target == nullptr) return -1;
        return matchingClipIndex (*source, clipIdx, *target, areStereoPartners (engine, track, peer));
    }

    inline std::vector<ClipRef> peerClips (AudioEngine& engine, int track, int clipIdx)
    {
        std::vector<ClipRef> clips;
        for (int peer : peerTracks (engine, track))
            if (const int i = peerClipIndex (engine, track, clipIdx, peer); i >= 0)
                clips.emplace_back (peer, i);
        return clips;
    }

    // A selection grown to everything it must act on. Resolve this BEFORE
    // mutating anything: a delete or duplicate shifts the indices it reads.
    // A clip already reached as a peer adds no peers of its own: with group
    // members split differently, one more hop lands on a clip nobody chose.
    inline std::vector<ClipRef> closure (AudioEngine& engine, const std::set<ClipRef>& selection,
                                         ClipRef primary)
    {
        std::set<ClipRef> all;
        const auto reach = [&] (const ClipRef& clip)
        {
            if (all.count (clip) > 0) return;
            for (const auto& ref : peerClips (engine, clip.first, clip.second))
                all.insert (ref);
        };
        if (selection.count (primary) > 0) reach (primary);
        for (const auto& clip : selection) reach (clip);
        return { all.begin(), all.end() };
    }

    // A clip as it was when a prompt opened on it. A list index goes stale
    // while the prompt waits (an undo from the menu bar, a remote edit), so the
    // answer finds the clip again by what it is, not where it was.
    struct ClipPrint
    {
        int          track { -1 };
        juce::int64  start { 0 }, fileStart { 0 }, length { 0 };
        juce::File   file;
        int          channel { -1 };
    };

    inline std::vector<ClipPrint> printsOf (AudioEngine& engine, const std::vector<ClipRef>& clips)
    {
        std::vector<ClipPrint> prints;
        for (const auto& [track, idx] : clips)
            if (const auto* list = engine.tryClipsFor (track); list != nullptr && idx >= 0 && idx < (int) list->size())
            {
                const auto& c = (*list)[(size_t) idx];
                prints.push_back ({ track, c.timelineStartSamples, c.fileStartSamples, c.fileLengthSamples,
                                    c.audioFile, c.sourceChannel });
            }
        return prints;
    }

    // Where each printed clip is now. Nothing when any of them is gone, moved,
    // trimmed or no longer one clip: the answer then lands on none of them,
    // never on whatever sits at the old index.
    inline std::optional<std::vector<ClipRef>> findPrinted (AudioEngine& engine,
                                                            const std::vector<ClipPrint>& prints)
    {
        std::vector<ClipRef> found;
        for (const auto& p : prints)
        {
            const auto* list = engine.tryClipsFor (p.track);
            if (list == nullptr) return std::nullopt;
            int at = -1;
            for (int i = 0; i < (int) list->size(); ++i)
            {
                const auto& c = (*list)[(size_t) i];
                if (c.timelineStartSamples != p.start || c.fileStartSamples != p.fileStart
                    || c.fileLengthSamples != p.length || c.audioFile != p.file || c.sourceChannel != p.channel)
                    continue;
                if (at >= 0) return std::nullopt;   // two alike: neither is surely the one
                at = i;
            }
            if (at < 0) return std::nullopt;
            found.emplace_back (p.track, at);
        }
        if (found.empty()) return std::nullopt;
        return found;
    }

    // Refusing the whole command is what keeps a group together: skipping one
    // locked clip would leave its peers changed and it behind.
    inline bool anyLocked (AudioEngine& engine, const std::vector<ClipRef>& clips)
    {
        for (const auto& [track, clip] : clips)
            if (const auto* list = engine.tryClipsFor (track);
                list != nullptr && clip >= 0 && clip < (int) list->size()
                && (*list)[(size_t) clip].locked)
                return true;
        return false;
    }

    // One gap a ripple closes on these tracks.
    struct RippleSpan
    {
        std::vector<int> tracks;
        juce::int64 start, end;
    };

    // What a ripple of these clips removes, last span first so that nothing
    // moves before it is applied. Every track of a strip or edit group takes
    // the SAME spans (the clicked track's own clips), or later audio would
    // slide by different amounts on different tracks.
    inline std::vector<RippleSpan> ripplePlan (AudioEngine& engine,
                                               const std::vector<ClipRef>& clips, int primaryTrack)
    {
        std::vector<RippleSpan> plan;
        std::set<int> planned;
        const auto planFamily = [&] (int leader)
        {
            if (planned.count (leader) > 0) return;
            const auto family = peerTracks (engine, leader);
            planned.insert (family.begin(), family.end());

            const auto* list = engine.tryClipsFor (leader);
            if (list == nullptr) return;
            std::vector<std::pair<juce::int64, juce::int64>> spans;
            for (const auto& [track, clip] : clips)
                if (track == leader && clip >= 0 && clip < (int) list->size())
                {
                    const auto& c = (*list)[(size_t) clip];
                    spans.emplace_back (c.timelineStartSamples,
                                        c.timelineStartSamples + c.fileLengthSamples);
                }
            std::sort (spans.begin(), spans.end());
            std::vector<std::pair<juce::int64, juce::int64>> merged;
            for (const auto& span : spans)
                if (! merged.empty() && span.first <= merged.back().second)
                    merged.back().second = juce::jmax (merged.back().second, span.second);
                else
                    merged.push_back (span);
            for (auto it = merged.rbegin(); it != merged.rend(); ++it)
                plan.push_back ({ family, it->first, it->second });
        };
        planFamily (primaryTrack);
        for (const auto& clip : clips)
            planFamily (clip.first);
        return plan;
    }
}
