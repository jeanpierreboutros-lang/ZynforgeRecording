// Clip + take subsystems on AudioEngine. Extracted from
// AudioEngine.cpp as part of the 2026-05-24 god-class split.
//
// Clip-edit surface: tryClipsFor, splitTrackAtPlayhead, editClip,
// setClipFades, setClipMuted, setClipLocked, setClipGainDb,
// deleteClip, duplicateClip.
//
// Takes / playlist surface: getTakeCount, getActiveTakeIdx,
// getTakeName, setActiveTake, newTakeFromCurrent, deleteTake,
// renameTake, playlistsToJson, loadPlaylistsFromJson,
// seedDefaultClips.

#include "AudioEngine.h"

namespace zynforge
{
    void AudioEngine::seedDefaultClips()
    {
        // Give every track a single full-range clip so the EDIT tools
        // (Trim / Grabber / Fade / Selector / Scrubber) have something
        // to grab. Without an active clip list, TrackRow's clip-edit
        // hit-test short-circuits and the tools feel dead.
        const int n = player.getNumTracks();
        if (n <= 0) return;
        trackClips.clear();
        trackClips.resize ((size_t) n);
        trackPlaylists.clear();
        trackPlaylists.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            Clip c;
            c.name                 = juce::String::formatted ("Track_%02d", i + 1);
            c.timelineStartSamples = 0;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = player.getTrackLengthSamples (i);
            if (c.fileLengthSamples <= 0) continue;
            trackClips[(size_t) i].push_back (c);
            player.setTrackClips (i, trackClips[(size_t) i]);

            // Seed Take 1 = current single-clip layout so the engineer
            // can immediately stamp "Take 2 from current" mid-comp.
            Take t1;
            t1.name  = "Take 1";
            t1.clips = trackClips[(size_t) i];
            trackPlaylists[(size_t) i].takes.push_back (std::move (t1));
            trackPlaylists[(size_t) i].activeTake = 0;
        }
    }

    int AudioEngine::cropToRange (juce::int64 startSample, juce::int64 endSample)
    {
        const juce::int64 len = endSample - startSample;
        if (len <= 0) return 0;

        const int n = (int) trackClips.size();
        int tracksWithAudio = 0;

        for (int t = 0; t < n; ++t)
        {
            // An empty clip list means "play the whole file" -- synthesise
            // a full-range clip so the crop has something to intersect,
            // otherwise the track would be silently dropped.
            std::vector<Clip> src = trackClips[(size_t) t];
            if (src.empty())
            {
                Clip full;
                full.timelineStartSamples = 0;
                full.fileStartSamples     = 0;
                full.fileLengthSamples    = player.getTrackLengthSamples (t);
                if (full.fileLengthSamples > 0) src.push_back (full);
            }

            std::vector<Clip> out;
            for (const auto& c : src)
            {
                const juce::int64 cs = c.timelineStartSamples;
                const juce::int64 ce = cs + c.fileLengthSamples;
                const juce::int64 is = juce::jmax (cs, startSample);   // intersection
                const juce::int64 ie = juce::jmin (ce, endSample);
                if (ie <= is) continue;                                 // clip outside range

                Clip nc = c;
                const juce::int64 lead = is - cs;                       // trimmed off the front
                nc.fileStartSamples     = c.fileStartSamples + lead;
                nc.fileLengthSamples    = ie - is;
                nc.timelineStartSamples = is - startSample;             // range start -> timeline 0
                nc.fadeInSamples  = juce::jmin (nc.fadeInSamples,  nc.fileLengthSamples);
                nc.fadeOutSamples = juce::jmin (nc.fadeOutSamples, nc.fileLengthSamples);
                out.push_back (nc);
            }

            trackClips[(size_t) t] = out;
            player.setTrackClips (t, out);

            // Mirror into the active take so it persists + survives a take
            // swap (matches setActiveTake's contract).
            if (t < (int) trackPlaylists.size())
            {
                auto& p = trackPlaylists[(size_t) t];
                if (p.activeTake >= 0 && p.activeTake < (int) p.takes.size())
                    p.takes[(size_t) p.activeTake].clips = out;
            }

            if (! out.empty()) ++tracksWithAudio;
        }
        return tracksWithAudio;
    }

    int AudioEngine::getTakeCount (int track) const
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return 0;
        return (int) trackPlaylists[(size_t) track].takes.size();
    }

    int AudioEngine::getActiveTakeIdx (int track) const
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return -1;
        return trackPlaylists[(size_t) track].activeTake;
    }

    juce::String AudioEngine::getTakeName (int track, int takeIdx) const
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return {};
        const auto& p = trackPlaylists[(size_t) track];
        if (takeIdx < 0 || takeIdx >= (int) p.takes.size()) return {};
        return p.takes[(size_t) takeIdx].name;
    }

    void AudioEngine::setActiveTake (int track, int takeIdx)
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return;
        auto& p = trackPlaylists[(size_t) track];
        if (takeIdx < 0 || takeIdx >= (int) p.takes.size()) return;

        // Save the current trackClips into the OUTGOING take so a
        // mid-edit switch doesn't lose work, THEN load the incoming
        // take into trackClips + republish to the player.
        if (p.activeTake >= 0 && p.activeTake < (int) p.takes.size())
            p.takes[(size_t) p.activeTake].clips = trackClips[(size_t) track];

        p.activeTake = takeIdx;
        trackClips[(size_t) track] = p.takes[(size_t) takeIdx].clips;
        player.setTrackClips (track, trackClips[(size_t) track]);
    }

    int AudioEngine::newTakeFromCurrent (int track, const juce::String& name)
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return -1;
        auto& p = trackPlaylists[(size_t) track];
        Take t;
        t.name  = name.isNotEmpty() ? name : ("Take " + juce::String ((int) p.takes.size() + 1));
        t.clips = trackClips[(size_t) track];
        p.takes.push_back (std::move (t));
        p.activeTake = (int) p.takes.size() - 1;
        return p.activeTake;
    }

    void AudioEngine::deleteTake (int track, int takeIdx)
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return;
        auto& p = trackPlaylists[(size_t) track];
        if (takeIdx < 0 || takeIdx >= (int) p.takes.size()) return;
        if (p.takes.size() <= 1) return;   // never delete the last take

        p.takes.erase (p.takes.begin() + takeIdx);
        if (p.activeTake >= (int) p.takes.size()) p.activeTake = (int) p.takes.size() - 1;
        trackClips[(size_t) track] = p.takes[(size_t) p.activeTake].clips;
        player.setTrackClips (track, trackClips[(size_t) track]);
    }

    void AudioEngine::renameTake (int track, int takeIdx, const juce::String& name)
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return;
        auto& p = trackPlaylists[(size_t) track];
        if (takeIdx < 0 || takeIdx >= (int) p.takes.size()) return;
        p.takes[(size_t) takeIdx].name = name.isNotEmpty() ? name : ("Take " + juce::String (takeIdx + 1));
    }

    // Serialise the full per-track playlist (every Take + every Clip
    // inside it) so .zfproj round-trips the engineer's comping work.
    // Without this Takes are RAM-only and lost on app quit -- real
    // data-loss bug fixed here.
    juce::var AudioEngine::playlistsToJson() const
    {
        juce::Array<juce::var> tracks;
        for (size_t t = 0; t < trackPlaylists.size(); ++t)
        {
            const auto& pl = trackPlaylists[t];
            juce::DynamicObject::Ptr pObj (new juce::DynamicObject());
            pObj->setProperty ("track",      (int) t);
            pObj->setProperty ("activeTake", pl.activeTake);

            juce::Array<juce::var> takes;
            for (const auto& tk : pl.takes)
            {
                juce::DynamicObject::Ptr tObj (new juce::DynamicObject());
                tObj->setProperty ("name", tk.name);
                juce::Array<juce::var> clips;
                for (const auto& c : tk.clips)
                {
                    juce::DynamicObject::Ptr cObj (new juce::DynamicObject());
                    cObj->setProperty ("name",        c.name);
                    cObj->setProperty ("file",        c.audioFile.getFullPathName());
                    cObj->setProperty ("tlStart",     (juce::int64) c.timelineStartSamples);
                    cObj->setProperty ("fileStart",   (juce::int64) c.fileStartSamples);
                    cObj->setProperty ("fileLen",     (juce::int64) c.fileLengthSamples);
                    cObj->setProperty ("fadeIn",      (juce::int64) c.fadeInSamples);
                    cObj->setProperty ("fadeOut",     (juce::int64) c.fadeOutSamples);
                    cObj->setProperty ("gainDb",      (double) c.gainDb);
                    cObj->setProperty ("muted",       c.muted);
                    cObj->setProperty ("locked",      c.locked);
                    clips.add (juce::var (cObj.get()));
                }
                tObj->setProperty ("clips", juce::var (clips));
                takes.add (juce::var (tObj.get()));
            }
            pObj->setProperty ("takes", juce::var (takes));
            tracks.add (juce::var (pObj.get()));
        }
        return juce::var (tracks);
    }

    void AudioEngine::loadPlaylistsFromJson (const juce::var& v)
    {
        auto* arr = v.getArray();
        if (arr == nullptr) return;

        // Resize storage to match (seedDefaultClips has already
        // populated this on session load; we overwrite per-track here).
        for (const auto& item : *arr)
        {
            auto* pObj = item.getDynamicObject();
            if (pObj == nullptr) continue;
            const int t = (int) pObj->getProperty ("track");
            if (t < 0 || t >= (int) trackPlaylists.size()) continue;
            auto& pl = trackPlaylists[(size_t) t];

            pl.takes.clear();
            pl.activeTake = (int) pObj->getProperty ("activeTake");

            auto* takesArr = pObj->getProperty ("takes").getArray();
            if (takesArr == nullptr) continue;
            for (const auto& takeVar : *takesArr)
            {
                auto* tObj = takeVar.getDynamicObject();
                if (tObj == nullptr) continue;
                Take tk;
                tk.name = tObj->getProperty ("name").toString();
                auto* clipsArr = tObj->getProperty ("clips").getArray();
                if (clipsArr != nullptr)
                {
                    for (const auto& clipVar : *clipsArr)
                    {
                        auto* cObj = clipVar.getDynamicObject();
                        if (cObj == nullptr) continue;
                        Clip c;
                        c.name                 = cObj->getProperty ("name").toString();
                        c.audioFile            = juce::File (cObj->getProperty ("file").toString());
                        c.timelineStartSamples = (juce::int64) (double) cObj->getProperty ("tlStart");
                        c.fileStartSamples     = (juce::int64) (double) cObj->getProperty ("fileStart");
                        c.fileLengthSamples    = (juce::int64) (double) cObj->getProperty ("fileLen");
                        c.fadeInSamples        = (juce::int64) (double) cObj->getProperty ("fadeIn");
                        c.fadeOutSamples       = (juce::int64) (double) cObj->getProperty ("fadeOut");
                        c.gainDb               = (float)        (double) cObj->getProperty ("gainDb");
                        c.muted                = (bool)         cObj->getProperty ("muted");
                        c.locked               = (bool)         cObj->getProperty ("locked");
                        tk.clips.push_back (std::move (c));
                    }
                }
                pl.takes.push_back (std::move (tk));
            }

            // Reapply the active take to the live trackClips +
            // SessionPlayer so playback matches the persisted state.
            if (pl.activeTake >= 0 && pl.activeTake < (int) pl.takes.size())
            {
                trackClips[(size_t) t] = pl.takes[(size_t) pl.activeTake].clips;
                player.setTrackClips (t, trackClips[(size_t) t]);
            }
        }
    }

    const std::vector<Clip>* AudioEngine::tryClipsFor (int track) const
    {
        if (track < 0 || track >= (int) trackClips.size()) return nullptr;
        const auto& v = trackClips[(size_t) track];
        return v.empty() ? nullptr : &v;
    }

    bool AudioEngine::splitTrackAtPlayhead (int track)
    {
        if (track < 0 || track >= recorder.getNumTracks()) return false;
        auto pos = player.isLoaded() ? player.getPositionSamples() : juce::int64 (0);
        // Honour the active snap grid. Bars mode lands the split on
        // the nearest bar boundary so music edits sit cleanly; Markers
        // snaps to the nearest user marker.
        pos = snapSampleToGrid (pos);
        if (pos <= 0) return false;

        auto sessionDir = getActiveSessionDir();
        const auto trackFile = sessionDir.isDirectory()
            ? sessionDir.getChildFile ("Audio Files")
                       .getChildFile (juce::String::formatted ("Track_%02d.wav", track + 1))
            : juce::File();
        if (! trackFile.existsAsFile()) return false;

        // Lazy bootstrap: if the track has no clip list yet, create one
        // full-range clip spanning the whole file.
        auto& list = clipsFor (track);
        if (list.empty())
        {
            Clip c;
            c.name                 = juce::String::formatted ("Track_%02d", track + 1);
            c.audioFile            = trackFile;
            c.timelineStartSamples = 0;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = trackFile.getSize() > 0
                ? (juce::int64) (trackFile.getSize() / 4)  // 24-bit WAV mono ≈ 3 B/sample, rough
                : player.getTotalLengthSamples();
            list.push_back (c);
        }

        // Find the clip whose timeline range contains the playhead.
        bool did = false;
        for (int i = 0; i < (int) list.size(); ++i)
        {
            const auto& c = list[(size_t) i];
            const auto tEnd = c.timelineStartSamples + c.fileLengthSamples;
            if (pos > c.timelineStartSamples && pos < tEnd)
            {
                const auto fileOffset = c.fileStartSamples + (pos - c.timelineStartSamples);
                did = splitClipAt (list, i, fileOffset);
                break;
            }
        }
        // Publish the updated list to the player so playback honours
        // the cut on the next block.
        if (did) player.setTrackClips (track, list);
        return did;
    }

    bool AudioEngine::editClip (int track, int clipIndex, ClipEdit mode, juce::int64 deltaSamples)
    {
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;   // edits refused on locked clips

        // Min length: 1024 samples (~20 ms at 48 k) so a clip never
        // collapses to invisibility on a fast drag.
        constexpr juce::int64 kMinLen = 1024;

        switch (mode)
        {
            case ClipEdit::TrimLeft:
            {
                // Slip-trim: timelineStart + fileStart move together,
                // fileLength contracts/expands by the inverse.
                const juce::int64 newFileStart = juce::jmax<juce::int64> (0,
                                                                          c.fileStartSamples + deltaSamples);
                const juce::int64 realDelta = newFileStart - c.fileStartSamples;
                if (c.fileLengthSamples - realDelta < kMinLen) return false;
                c.fileStartSamples     = newFileStart;
                c.timelineStartSamples = juce::jmax<juce::int64> (0,
                                                                   c.timelineStartSamples + realDelta);
                c.fileLengthSamples   -= realDelta;
                break;
            }
            case ClipEdit::TrimRight:
            {
                const juce::int64 newLen = c.fileLengthSamples + deltaSamples;
                if (newLen < kMinLen) return false;
                c.fileLengthSamples = newLen;
                break;
            }
            case ClipEdit::Move:
            {
                c.timelineStartSamples = juce::jmax<juce::int64> (0,
                                                                   c.timelineStartSamples + deltaSamples);
                break;
            }
        }
        // Publish the updated list to the player so playback honours
        // the edit on the next audio block.
        player.setTrackClips (track, list);
        return true;
    }

    bool AudioEngine::setClipFades (int track, int clipIndex,
                                    juce::int64 fadeInSamples, juce::int64 fadeOutSamples)
    {
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;

        fadeInSamples  = juce::jmax<juce::int64> (0, fadeInSamples);
        fadeOutSamples = juce::jmax<juce::int64> (0, fadeOutSamples);
        if (fadeInSamples + fadeOutSamples > c.fileLengthSamples) return false;

        c.fadeInSamples  = fadeInSamples;
        c.fadeOutSamples = fadeOutSamples;
        player.setTrackClips (track, list);
        return true;
    }

    // ─── Clip-edit helpers ──────────────────────────────────────────
    namespace
    {
        std::vector<Clip>* validClipList (std::vector<std::vector<Clip>>& all,
                                          int track, int clipIndex)
        {
            if (track < 0 || track >= (int) all.size()) return nullptr;
            auto& list = all[(size_t) track];
            if (clipIndex < 0 || clipIndex >= (int) list.size()) return nullptr;
            return &list;
        }
    }

    bool AudioEngine::setClipMuted (int track, int clipIndex, bool muted)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].muted = muted;
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::setClipLocked (int track, int clipIndex, bool locked)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].locked = locked;
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::setClipGainDb (int track, int clipIndex, float dB)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].gainDb = juce::jlimit (-60.0f, 12.0f, dB);
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::deleteClip (int track, int clipIndex)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        list->erase (list->begin() + clipIndex);
        player.setTrackClips (track, *list);
        return true;
    }

    bool AudioEngine::duplicateClip (int track, int clipIndex)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        Clip copy = (*list)[(size_t) clipIndex];
        copy.timelineStartSamples += copy.fileLengthSamples;   // place after the source
        copy.name = copy.name + " (copy)";
        copy.locked = false;
        list->insert (list->begin() + clipIndex + 1, std::move (copy));
        player.setTrackClips (track, *list);
        return true;
    }

}
