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

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace zynforge
{
    void AudioEngine::seedDefaultClips (bool preserveEdits)
    {
        // Give every track a single full-range clip so the EDIT tools
        // (Trim / Grabber / Fade / Selector / Scrubber) have something
        // to grab. Without an active clip list, TrackRow's clip-edit
        // hit-test short-circuits and the tools feel dead.
        //
        // preserveEdits guards against silently wiping comp takes / splits /
        // fades on a SAME-session reload (stop-recording, strip reorder). Only
        // the true session-OPEN path passes false, and its .zfproj restore
        // overwrites these seeds anyway. See the seedDefaultClips ADR.
        const int n = player.getNumTracks();
        if (n <= 0)
        {
            if (! preserveEdits) { trackClips.clear(); trackPlaylists.clear(); }
            return;
        }

        // A track is "edited" (must be preserved) when it has been comped
        // (>1 take) or its single clip has been shaped away from the plain
        // full-range default (split, front-trimmed, faded, gained, muted,
        // moved, or repointed at a cross-track file).
        //
        // NOTE: do NOT require the clip length to equal the current file length
        // here. A continue-record / length-changing punch GROWS the file, so an
        // unedited default clip left at the OLD length would then be classified
        // "edited" and preserved short -- silencing the appended audio (a real
        // regression this comment guards against). Treating a plain full-from-
        // zero clip as refreshable reseeds it to the new length. The only cost
        // is that a pure END-trim (single clip, fs=0, ts=0, no fades) reloaded
        // in place is refreshed back to full -- the recorded audio is never
        // lost, and an end-trim is trivially redoable, so for a live recorder
        // that is the correct trade vs. dropping captured audio.
        const auto isPlainDefault = [] (const Clip& c)
        {
            return c.timelineStartSamples == 0
                && c.fileStartSamples     == 0
                && c.fadeInSamples        == 0
                && c.fadeOutSamples       == 0
                && ! c.muted
                && std::abs (c.gainDb) < 0.001f
                && c.audioFile == juce::File();
        };
        const auto trackIsEdited = [&] (int i) -> bool
        {
            if (i >= (int) trackPlaylists.size() || i >= (int) trackClips.size()) return false;
            if (trackPlaylists[(size_t) i].takes.size() > 1) return true;
            const auto& list = trackClips[(size_t) i];
            if (list.empty())    return false;   // nothing worth preserving
            if (list.size() > 1) return true;    // split into multiple clips
            return ! isPlainDefault (list[0]);
        };

        if (! preserveEdits)
        {
            trackClips.clear();
            trackPlaylists.clear();
        }
        trackClips.resize    ((size_t) n);
        trackPlaylists.resize ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            if (preserveEdits && trackIsEdited (i))
            {
                // Keep the engineer's edits verbatim. player.loadSession
                // cleared the player-side clip lists, so republish what we
                // already hold rather than reseeding a default over them.
                player.setTrackClips (i, trackClips[(size_t) i]);
                continue;
            }

            Clip c;
            c.name                 = juce::String::formatted ("Track_%02d", i + 1);
            c.timelineStartSamples = 0;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = player.getTrackLengthSamples (i);

            trackClips[(size_t) i].clear();
            trackPlaylists[(size_t) i] = {};
            if (c.fileLengthSamples <= 0) continue;   // no audio -> whole-file fallback

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

    void AudioEngine::syncActiveTake (int track)
    {
        if (track < 0 || track >= (int) trackClips.size()) return;
        if (track >= (int) trackPlaylists.size())
            trackPlaylists.resize ((size_t) track + 1);

        auto& p = trackPlaylists[(size_t) track];
        if (p.takes.empty())
        {
            Take t1;
            t1.name = "Take 1";
            p.takes.push_back (std::move (t1));
            p.activeTake = 0;
        }
        if (p.activeTake < 0 || p.activeTake >= (int) p.takes.size())
            p.activeTake = 0;

        p.takes[(size_t) p.activeTake].clips = trackClips[(size_t) track];
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
            syncActiveTake (t);   // keep the active take == trackClips

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

    std::vector<Clip> AudioEngine::getTakeClips (int track, int takeIdx) const
    {
        if (track < 0 || track >= (int) trackPlaylists.size()) return {};
        const auto& p = trackPlaylists[(size_t) track];
        if (takeIdx < 0 || takeIdx >= (int) p.takes.size()) return {};
        // The active take's authoritative clips are the live trackClips.
        if (takeIdx == p.activeTake && track < (int) trackClips.size())
            return trackClips[(size_t) track];
        return p.takes[(size_t) takeIdx].clips;
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
                    cObj->setProperty ("fadeCurve",   c.fadeCurve);
                    // Cross-track clips reference another track's file --
                    // store the NAME only so the session stays portable.
                    if (c.audioFile != juce::File())
                        cObj->setProperty ("audioFile", c.audioFile.getFileName());
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
                        c.fadeCurve            = (int) cObj->getProperty ("fadeCurve");   // 0 if absent
                        const auto afName = cObj->getProperty ("audioFile").toString();
                        if (afName.isNotEmpty())
                            c.audioFile = getActiveSessionDir().getChildFile ("Audio Files")
                                                               .getChildFile (afName);
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

    bool AudioEngine::isTrackArrangementEmpty (int track) const noexcept
    {
        // Present in trackClips (seedDefaultClips sized it / an edit created
        // the entry) AND empty -> the arrangement was emptied by the engineer
        // (or the track carries no audio). tryClipsFor can't express this: it
        // collapses "empty" to nullptr, which the EDIT view can't tell apart
        // from a never-edited fresh take that should still show its waveform.
        return track >= 0 && track < (int) trackClips.size()
            && trackClips[(size_t) track].empty();
    }

    namespace
    {
        // Split whichever clip in `list` straddles `timelineSample`.
        // Returns true if a cut landed (sample strictly inside a clip).
        bool splitListAtTimeline (std::vector<Clip>& list, juce::int64 timelineSample)
        {
            for (int i = 0; i < (int) list.size(); ++i)
            {
                const auto& c = list[(size_t) i];
                const auto tEnd = c.timelineStartSamples + c.fileLengthSamples;
                if (timelineSample > c.timelineStartSamples && timelineSample < tEnd)
                {
                    const auto fileOffset = c.fileStartSamples
                                          + (timelineSample - c.timelineStartSamples);
                    return splitClipAt (list, i, fileOffset);
                }
            }
            return false;
        }
    }

    // Lazy bootstrap: ensure `track` has a clip list (one full-range clip
    // spanning the whole file) so edits have something to act on. Returns
    // false when there's no audio file backing the track.
    bool AudioEngine::ensureClipList (int track)
    {
        auto& list = clipsFor (track);
        if (! list.empty()) return true;

        auto sessionDir = getActiveSessionDir();
        const auto trackFile = sessionDir.isDirectory()
            ? sessionDir.getChildFile ("Audio Files")
                       .getChildFile (juce::String::formatted ("Track_%02d.wav", track + 1))
            : juce::File();
        if (! trackFile.existsAsFile()) return false;

        Clip c;
        c.name                 = juce::String::formatted ("Track_%02d", track + 1);
        c.audioFile            = trackFile;
        c.timelineStartSamples = 0;
        c.fileStartSamples     = 0;
        // Sample count from the actual reader -- the old getSize()/4 estimate
        // assumed 4 bytes/sample and under-counted a 24-bit mono file (3 B/frame
        // + WAV header) by ~25%. Open the file and read the real length; fall
        // back to the player's loaded length only if it won't open.
        juce::int64 lengthSamples = 0;
        {
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (trackFile));
            if (rd != nullptr) lengthSamples = (juce::int64) rd->lengthInSamples;
        }
        c.fileLengthSamples = lengthSamples > 0 ? lengthSamples
                                                : player.getTotalLengthSamples();
        if (c.fileLengthSamples <= 0) return false;
        list.push_back (c);
        return true;
    }

    juce::int64 AudioEngine::getArrangementLengthSamples()
    {
        juce::int64 maxEnd = 0;
        for (const auto& list : trackClips)
            for (const auto& c : list)
                maxEnd = juce::jmax (maxEnd, c.timelineStartSamples + c.fileLengthSamples);
        if (maxEnd <= 0) maxEnd = player.getTotalLengthSamples();
        return maxEnd;
    }

    namespace
    {
        // Offline renders run in fixed windows so memory stays O(window)
        // regardless of arrangement length -- a multi-hour show no longer
        // needs a multi-GB flat buffer. 65536 is a multiple of the
        // 512-sample automation step, so windowed and whole-buffer renders
        // evaluate automation at identical sample positions.
        constexpr int kRenderWindowSamples = 1 << 16;

        // One track's offline render source: the resolved clip list plus
        // the opened readers (track file + any cross-track clip files).
        struct ArrangementSource
        {
            juce::AudioFormatManager fm;
            std::unique_ptr<juce::AudioFormatReader> reader;
            std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> extra;
            std::vector<Clip> clips;
            juce::File srcFile;
            // Which channel of srcFile this track reads: -1 = read all (mono
            // file), 0 = LEFT of an interleaved stereo file, 1 = RIGHT of one.
            int readChannel { -1 };

            bool open (const juce::File& audioDir, int track, const std::vector<Clip>* engineClips)
            {
                fm.registerBasicFormats();

                // 1) The track's OWN file (Track_<track+1>). A 2-channel own
                //    file means this index is the LEFT of an interleaved
                //    stereo pair -> read channel 0; a mono file reads all.
                for (auto* ext : { ".wav", ".flac", ".aif", ".aiff" })
                {
                    auto f = audioDir.getChildFile (juce::String::formatted ("Track_%02d", track + 1) + ext);
                    if (f.existsAsFile()) { srcFile = f; break; }
                }

                if (srcFile.existsAsFile())
                {
                    reader.reset (fm.createReaderFor (srcFile));
                    if (reader == nullptr) return false;
                    readChannel = (reader->numChannels >= 2) ? 0 : -1;
                }
                else if (track > 0)
                {
                    // 2) No own file: this may be the RIGHT half of an
                    //    interleaved stereo pair, whose audio lives in the
                    //    LEFT slot's 2-ch file (named Track_<track>, since the
                    //    L partner is index track-1). Read channel 1 from it.
                    for (auto* ext : { ".wav", ".flac", ".aif", ".aiff" })
                    {
                        auto f = audioDir.getChildFile (juce::String::formatted ("Track_%02d", track) + ext);
                        if (f.existsAsFile()) { srcFile = f; break; }
                    }
                    if (! srcFile.existsAsFile()) return false;
                    reader.reset (fm.createReaderFor (srcFile));
                    if (reader == nullptr || reader->numChannels < 2) return false;
                    readChannel = 1;
                }
                else
                {
                    return false;
                }

                // Active-take clips; bootstrap a whole-file clip if the
                // track was never edited so it still renders.
                if (engineClips != nullptr) clips = *engineClips;
                if (clips.empty())
                {
                    Clip c;
                    c.timelineStartSamples = 0;
                    c.fileStartSamples     = 0;
                    c.fileLengthSamples    = reader->lengthInSamples;
                    clips.push_back (c);
                }
                return true;
            }

            // Mix every clip's overlap with [winStart, winStart + winLen)
            // into dst (window-relative, accumulating). Fades, clip gain
            // and clip mute match the real-time player path.
            void mixWindow (float* dst, juce::int64 winStart, int winLen, juce::AudioBuffer<float>& tmp)
            {
                for (const auto& c : clips)
                {
                    if (c.muted || c.fileLengthSamples <= 0) continue;
                    const juce::int64 tlStart  = c.timelineStartSamples;
                    const juce::int64 writeBeg = juce::jmax (winStart, tlStart);
                    const juce::int64 writeEnd = juce::jmin (winStart + winLen, tlStart + c.fileLengthSamples);
                    if (writeEnd <= writeBeg) continue;
                    const int span = (int) (writeEnd - writeBeg);
                    const juce::int64 fileReadStart = c.fileStartSamples + (writeBeg - tlStart);

                    // Cross-track clips read from their own file -- cache one
                    // reader per distinct path so the bounce matches playback.
                    juce::AudioFormatReader* rd = reader.get();
                    if (c.audioFile != juce::File() && c.audioFile != srcFile)
                    {
                        const auto key = c.audioFile.getFullPathName();
                        auto it = extra.find (key);
                        if (it == extra.end())
                            it = extra.emplace (key, std::unique_ptr<juce::AudioFormatReader> (
                                                         fm.createReaderFor (c.audioFile))).first;
                        if (it->second != nullptr) rd = it->second.get();
                    }
                    if (rd == nullptr) continue;

                    // Read both channels (a stereo file fills L+R; a mono file
                    // fills ch 0). For the track's own reader, pick the L/R
                    // half via readChannel; cross-track clips are mono -> ch 0.
                    const int ch = (rd == reader.get()) ? (readChannel < 0 ? 0 : readChannel) : 0;
                    tmp.setSize (2, span, false, false, true);
                    tmp.clear();
                    rd->read (&tmp, 0, span, fileReadStart, true, true);

                    const float clipGain = juce::Decibels::decibelsToGain (c.gainDb, -60.0f);
                    const bool  eq        = (c.fadeCurve == 1);
                    const juce::int64 fIn = c.fadeInSamples, fOut = c.fadeOutSamples;
                    const juce::int64 fOutStart = c.fileLengthSamples - fOut;
                    const juce::int64 spanOffsetInClip = writeBeg - tlStart;
                    const auto* s = tmp.getReadPointer (ch);
                    auto* d = dst + (int) (writeBeg - winStart);
                    for (int i = 0; i < span; ++i)
                    {
                        const juce::int64 clipPos = spanOffsetInClip + i;
                        float g = clipGain;
                        if (fIn > 0 && clipPos < fIn)
                        { const float t = (float) clipPos / (float) fIn; g *= eq ? std::sin (t * 1.57079633f) : t; }
                        else if (fOut > 0 && clipPos >= fOutStart)
                        { const float t = (float) (c.fileLengthSamples - clipPos) / (float) fOut; g *= eq ? std::sin (t * 1.57079633f) : t; }
                        d[i] += s[i] * g;
                    }
                }
            }
        };

        std::unique_ptr<juce::AudioFormatWriter> createWav24Writer (const juce::File& f, double sr, int channels)
        {
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            if (os == nullptr) return nullptr;
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, (unsigned int) channels, 24, {}, 0));
            if (w != nullptr) os.release();
            return w;
        }
    }

    bool AudioEngine::forEachArrangementWindow (int track, juce::int64 startSample, juce::int64 endSample,
                                                const std::function<bool (const float*, juce::int64, int)>& consume)
    {
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return false;
        if (endSample <= startSample) return false;

        auto sessionDir = getActiveSessionDir();
        if (! sessionDir.isDirectory()) return false;

        ArrangementSource src;
        const std::vector<Clip>* engineClips = (track < (int) trackClips.size())
                                                 ? &trackClips[(size_t) track] : nullptr;
        if (! src.open (sessionDir.getChildFile ("Audio Files"), track, engineClips)) return false;

        juce::AudioBuffer<float> window (1, kRenderWindowSamples), tmp (1, 0);
        for (juce::int64 winStart = startSample; winStart < endSample; winStart += kRenderWindowSamples)
        {
            const int winLen = (int) juce::jmin<juce::int64> (kRenderWindowSamples, endSample - winStart);
            window.clear (0, 0, winLen);
            src.mixWindow (window.getWritePointer (0), winStart, winLen, tmp);
            if (! consume (window.getReadPointer (0), winStart, winLen)) return false;
        }
        return true;
    }

    bool AudioEngine::renderTrackArrangement (int track, juce::AudioBuffer<float>& out,
                                              juce::int64 totalSamples)
    {
        if (totalSamples <= 0) return false;
        const int outLen = (int) juce::jmin<juce::int64> (totalSamples, 0x7fffffff);
        bool sized = false;
        return forEachArrangementWindow (track, 0, outLen,
            [&] (const float* data, juce::int64 winStart, int winLen)
            {
                if (! sized)
                {
                    out.setSize (1, outLen, false, true, true);
                    out.clear();
                    sized = true;
                }
                juce::FloatVectorOperations::copy (out.getWritePointer (0) + (int) winStart, data, winLen);
                return true;
            });
    }

    bool AudioEngine::bounceTrackArrangementToWav (int track, const juce::File& dest,
                                                   juce::int64 totalSamples, double sampleRate)
    {
        auto writer = createWav24Writer (dest, sampleRate, 1);
        if (writer == nullptr) return false;
        const bool ok = forEachArrangementWindow (track, 0, totalSamples,
            [&] (const float* data, juce::int64, int winLen)
            {
                const float* chans[1] = { data };
                return writer->writeFromFloatArrays (chans, 1, winLen);
            });
        writer.reset();
        if (! ok) dest.deleteFile();
        return ok;
    }

    bool AudioEngine::forEachStereoMixWindow (juce::int64 totalSamples,
                                              const std::function<bool (const juce::AudioBuffer<float>&, juce::int64, int)>& consume)
    {
        if (totalSamples <= 0) return false;
        const int nTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        const auto audioDir = getActiveSessionDir().getChildFile ("Audio Files");

        bool anySolo = false;
        for (int i = 0; i < nTracks; ++i)
            if (recorder.getTrack (i).soloed.load (std::memory_order_relaxed)) { anySolo = true; break; }
        bool anyVcaSolo = false;
        for (const auto& v : vcas)
            if (v.soloed.load (std::memory_order_relaxed)) { anyVcaSolo = true; break; }

        // Open every audible track's source up front; the tracks then
        // render window-by-window in lockstep, so only O(window) audio is
        // in RAM at once (readers, not buffers, stay open per track).
        struct TrackCtx
        {
            int track {};
            ArrangementSource src;
            float baseDb {}, basePan {}, vcaDb {};
            int   autoCh {};
            TrackState* ts {};
        };
        std::vector<std::unique_ptr<TrackCtx>> live;
        for (int t = 0; t < nTracks; ++t)
        {
            auto& ts = recorder.getTrack (t);
            const int grp = ts.vcaGroup.load (std::memory_order_relaxed);

            bool audible;
            if (grp >= 0 && grp < kNumVcas)
            {
                if (vcas[(size_t) grp].muted.load (std::memory_order_relaxed)) audible = false;
                else if (anyVcaSolo && ! vcas[(size_t) grp].soloed.load (std::memory_order_relaxed)) audible = false;
                else if (anySolo) audible = ts.soloed.load (std::memory_order_relaxed);
                else audible = ! ts.muted.load (std::memory_order_relaxed);
            }
            else if (anyVcaSolo) audible = false;
            else if (anySolo)    audible = ts.soloed.load (std::memory_order_relaxed);
            else                 audible = ! ts.muted.load (std::memory_order_relaxed);
            if (! audible) continue;

            auto ctx = std::make_unique<TrackCtx>();
            const std::vector<Clip>* engineClips = (t < (int) trackClips.size())
                                                     ? &trackClips[(size_t) t] : nullptr;
            if (! ctx->src.open (audioDir, t, engineClips)) continue;
            ctx->track   = t;
            ctx->baseDb  = ts.gainDb.load (std::memory_order_relaxed);
            ctx->basePan = ts.pan.load (std::memory_order_relaxed);
            ctx->vcaDb   = (grp >= 0 && grp < kNumVcas)
                             ? vcas[(size_t) grp].gainDb.load (std::memory_order_relaxed) : 0.0f;
            // Stereo pair reads its left partner's volume + mute lanes.
            ctx->autoCh  = (t > 0 && recorder.getTrack (t - 1).isStereo.load (std::memory_order_relaxed))
                             ? t - 1 : t;
            ctx->ts      = &ts;
            live.push_back (std::move (ctx));
        }

        const double halfPi = juce::MathConstants<double>::halfPi;
        const int step = 512;   // re-evaluate automation every ~10 ms

        const bool   mMute = masterState.muted.load (std::memory_order_relaxed);
        const double mGain = mMute ? 0.0
                                   : juce::Decibels::decibelsToGain (
                                         (double) masterState.gainDb.load (std::memory_order_relaxed), -60.0);

        juce::AudioBuffer<float> stereo (2, kRenderWindowSamples), mono (1, kRenderWindowSamples), tmp (1, 0);
        for (juce::int64 winStart = 0; winStart < totalSamples; winStart += kRenderWindowSamples)
        {
            const int winLen = (int) juce::jmin<juce::int64> (kRenderWindowSamples, totalSamples - winStart);
            stereo.clear();
            auto* oL = stereo.getWritePointer (0);
            auto* oR = stereo.getWritePointer (1);

            for (auto& cp : live)
            {
                mono.clear (0, 0, winLen);
                cp->src.mixWindow (mono.getWritePointer (0), winStart, winLen, tmp);
                const auto* s = mono.getReadPointer (0);

                for (int i = 0; i < winLen; i += step)
                {
                    const int n = juce::jmin (step, winLen - i);
                    const juce::int64 absPos = winStart + i;
                    const float volDb = automationValueAt (cp->autoCh, AutomationParam::Volume, absPos, cp->baseDb);
                    const float panV  = automationValueAt (cp->track,  AutomationParam::Pan,    absPos, cp->basePan);
                    const float muteV = automationValueAt (cp->autoCh, AutomationParam::Mute,   absPos,
                                                           cp->ts->muted.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
                    if (muteV > 0.5f) continue;

                    const double gain = juce::Decibels::decibelsToGain ((double) (volDb + cp->vcaDb), -60.0);
                    const double pn   = ((double) juce::jlimit (-1.0f, 1.0f, panV) + 1.0) * 0.5;
                    const double gL   = gain * std::cos (pn * halfPi);
                    const double gR   = gain * std::sin (pn * halfPi);
                    for (int k = 0; k < n; ++k)
                    {
                        const float v = s[i + k];
                        oL[i + k] += (float) (v * gL);
                        oR[i + k] += (float) (v * gR);
                    }
                }
            }

            if (std::abs (mGain - 1.0) > 1.0e-6)
                stereo.applyGain (0, winLen, (float) mGain);
            if (! consume (stereo, winStart, winLen)) return false;
        }
        return true;
    }

    bool AudioEngine::renderStereoMix (juce::AudioBuffer<float>& outStereo, juce::int64 totalSamples)
    {
        if (totalSamples <= 0) return false;
        const int len = (int) juce::jmin<juce::int64> (totalSamples, 0x7fffffff);
        outStereo.setSize (2, len, false, true, true);
        outStereo.clear();
        return forEachStereoMixWindow (len,
            [&] (const juce::AudioBuffer<float>& w, juce::int64 winStart, int winLen)
            {
                for (int ch = 0; ch < 2; ++ch)
                    outStereo.copyFrom (ch, (int) winStart, w, ch, 0, winLen);
                return true;
            });
    }

    bool AudioEngine::bounceStereoMixToWav (const juce::File& dest,
                                            juce::int64 totalSamples, double sampleRate)
    {
        auto writer = createWav24Writer (dest, sampleRate, 2);
        if (writer == nullptr) return false;
        const bool ok = forEachStereoMixWindow (totalSamples,
            [&] (const juce::AudioBuffer<float>& w, juce::int64, int winLen)
            { return writer->writeFromAudioSampleBuffer (w, 0, winLen); });
        writer.reset();
        if (! ok) dest.deleteFile();
        return ok;
    }

    bool AudioEngine::bounceStereoPairToWav (int trackL, const juce::File& dest,
                                             juce::int64 totalSamples, double sampleRate)
    {
        if (totalSamples <= 0) return false;
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (trackL < 0 || trackL + 1 >= maxTracks) return false;

        auto sessionDir = getActiveSessionDir();
        if (! sessionDir.isDirectory()) return false;
        const auto audioDir = sessionDir.getChildFile ("Audio Files");

        ArrangementSource L, R;
        const std::vector<Clip>* clipsL = (trackL     < (int) trackClips.size()) ? &trackClips[(size_t) trackL]     : nullptr;
        const std::vector<Clip>* clipsR = (trackL + 1 < (int) trackClips.size()) ? &trackClips[(size_t) trackL + 1] : nullptr;
        const bool okL = L.open (audioDir, trackL,     clipsL);
        const bool okR = R.open (audioDir, trackL + 1, clipsR);
        if (! okL && ! okR) return false;   // neither side has audio

        auto writer = createWav24Writer (dest, sampleRate, 2);
        if (writer == nullptr) return false;

        const int len = (int) juce::jmin<juce::int64> (totalSamples, 0x7fffffff);
        juce::AudioBuffer<float> stereo (2, kRenderWindowSamples), tmp (1, 0);
        bool ok = true;
        for (juce::int64 winStart = 0; winStart < len && ok; winStart += kRenderWindowSamples)
        {
            const int winLen = (int) juce::jmin<juce::int64> (kRenderWindowSamples, len - winStart);
            stereo.clear (0, 0, winLen);
            stereo.clear (1, 0, winLen);
            if (okL) L.mixWindow (stereo.getWritePointer (0), winStart, winLen, tmp);
            if (okR) R.mixWindow (stereo.getWritePointer (1), winStart, winLen, tmp);
            ok = writer->writeFromAudioSampleBuffer (stereo, 0, winLen);
        }
        writer.reset();
        if (! ok) dest.deleteFile();
        return ok;
    }

    bool AudioEngine::splitTrackAtPlayhead (int track)
    {
        auto pos = player.isLoaded() ? player.getPositionSamples() : juce::int64 (0);
        // Honour the active snap grid (Markers snaps to the nearest user
        // marker); the arbitrary-sample variant does NOT snap.
        pos = snapSampleToGrid (pos);
        return splitTrackAtSample (track, pos);
    }

    namespace
    {
        // Two adjacent clips can be healed (rejoined) only if they're a clean
        // split: same source file, butt-jointed in BOTH the file and the
        // timeline, neither locked. (A moved / slip-trimmed clip fails these.)
        bool clipsAreHealable (const Clip& a, const Clip& b)
        {
            return ! a.locked && ! b.locked
                && a.audioFile == b.audioFile
                && a.fileStartSamples + a.fileLengthSamples == b.fileStartSamples
                && a.timelineStartSamples + a.fileLengthSamples == b.timelineStartSamples;
        }

        void mergeClipPair (std::vector<Clip>& list, int i)
        {
            auto& a = list[(size_t) i];
            const auto& b = list[(size_t) (i + 1)];
            a.fileLengthSamples += b.fileLengthSamples;
            a.fadeOutSamples     = b.fadeOutSamples;   // the join had a hard cut; keep the tail fade
            list.erase (list.begin() + i + 1);
        }
    }

    bool AudioEngine::healSeparationAt (int track, juce::int64 timelineSample)
    {
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = clipsFor (track);
        int    best = -1;
        juce::int64 bestDist = std::numeric_limits<juce::int64>::max();
        for (int i = 0; i + 1 < (int) list.size(); ++i)
        {
            if (! clipsAreHealable (list[(size_t) i], list[(size_t) (i + 1)])) continue;
            const auto boundary = list[(size_t) i].timelineStartSamples
                                + list[(size_t) i].fileLengthSamples;
            const auto dist = std::abs (boundary - timelineSample);
            if (dist < bestDist) { bestDist = dist; best = i; }
        }
        if (best < 0) return false;
        mergeClipPair (list, best);
        player.setTrackClips (track, list);
        syncActiveTake (track);
        return true;
    }

    int AudioEngine::healSeparationRange (int track, juce::int64 start, juce::int64 end)
    {
        if (track < 0 || track >= (int) trackClips.size()) return 0;
        auto& list = clipsFor (track);
        int healed = 0;
        for (int i = 0; i + 1 < (int) list.size(); )
        {
            const auto boundary = list[(size_t) i].timelineStartSamples
                                + list[(size_t) i].fileLengthSamples;
            if (boundary > start && boundary < end
                && clipsAreHealable (list[(size_t) i], list[(size_t) (i + 1)]))
            { mergeClipPair (list, i); ++healed; }   // don't advance: heal a run in one pass
            else ++i;
        }
        if (healed > 0) { player.setTrackClips (track, list); syncActiveTake (track); }
        return healed;
    }

    int AudioEngine::stripSilence (int track, float thresholdDb,
                                   juce::int64 minSilenceSamples, juce::int64 minClipSamples,
                                   juce::int64 padSamples)
    {
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return -1;

        auto sessionDir = getActiveSessionDir();
        if (! sessionDir.isDirectory()) return -1;
        const auto audioDir = sessionDir.getChildFile ("Audio Files");
        juce::File src;
        for (auto* ext : { ".wav", ".flac", ".aif", ".aiff" })
        {
            auto f = audioDir.getChildFile (juce::String::formatted ("Track_%02d", track + 1) + ext);
            if (f.existsAsFile()) { src = f; break; }
        }
        if (! src.existsAsFile()) return -1;

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (src));
        if (reader == nullptr || reader->lengthInSamples <= 0) return -1;

        const juce::int64 total = reader->lengthInSamples;
        const float thresh = juce::Decibels::decibelsToGain (thresholdDb, -120.0f);

        // Scan a peak envelope in ~10 ms windows; a window is "silent" when its
        // peak sits below the threshold.
        const int win = juce::jmax (64, (int) (reader->sampleRate * 0.010));
        juce::AudioBuffer<float> buf (1, win);
        std::vector<char> loud;                         // 1 = audible window
        loud.reserve ((size_t) (total / win + 1));
        for (juce::int64 p = 0; p < total; p += win)
        {
            const int n = (int) juce::jmin<juce::int64> (win, total - p);
            buf.clear();
            reader->read (&buf, 0, n, p, true, true);
            loud.push_back (buf.getMagnitude (0, 0, n) >= thresh ? 1 : 0);
        }

        // Build audible runs, requiring a silent gap of >= minSilenceSamples to
        // break, and dropping audible runs shorter than minClipSamples.
        const juce::int64 minSilWin  = juce::jmax<juce::int64> (1, minSilenceSamples / win);
        std::vector<std::pair<juce::int64, juce::int64>> runs;   // [startWin, endWin)
        int i = 0; const int nw = (int) loud.size();
        while (i < nw)
        {
            while (i < nw && ! loud[(size_t) i]) ++i;            // skip leading silence
            if (i >= nw) break;
            int runStart = i;
            while (i < nw)
            {
                if (loud[(size_t) i]) { ++i; continue; }
                int sil = i; while (sil < nw && ! loud[(size_t) sil]) ++sil;   // measure gap
                if ((juce::int64) (sil - i) >= minSilWin) break;               // real gap -> end run
                i = sil;                                                        // short gap -> keep going
            }
            runs.emplace_back ((juce::int64) runStart, (juce::int64) i);
        }

        // Apply the start/end pad (PT "Clip Start/End Pad"): extend each kept
        // run outward by padSamples so transients aren't clipped, then merge
        // any runs the padding made overlap.
        std::vector<std::pair<juce::int64, juce::int64>> ranges;
        for (auto& r : runs)
        {
            juce::int64 a = juce::jmax<juce::int64> (0,     r.first  * win - padSamples);
            juce::int64 b = juce::jmin<juce::int64> (total, r.second * win + padSamples);
            if (! ranges.empty() && a <= ranges.back().second)
                ranges.back().second = juce::jmax (ranges.back().second, b);   // merge
            else
                ranges.emplace_back (a, b);
        }

        std::vector<Clip> out;
        const juce::int64 fade = juce::jmin<juce::int64> ((juce::int64) (reader->sampleRate * 0.005),
                                                          (juce::int64) win);
        for (auto& r : ranges)
        {
            const juce::int64 a = r.first;
            const juce::int64 b = r.second;
            if (b - a < juce::jmax<juce::int64> (1, minClipSamples)) continue;
            Clip c;
            c.name                 = juce::String::formatted ("Track_%02d", track + 1);
            c.timelineStartSamples = a;
            c.fileStartSamples     = a;
            c.fileLengthSamples    = b - a;
            c.fadeInSamples        = juce::jmin (fade, c.fileLengthSamples / 2);
            c.fadeOutSamples       = juce::jmin (fade, c.fileLengthSamples / 2);
            out.push_back (c);
        }
        if (out.empty()) return 0;

        if (! ensureClipList (track)) { /* still proceed -- we're replacing */ }
        clipsFor (track) = out;
        player.setTrackClips (track, out);
        syncActiveTake (track);
        return (int) out.size();
    }

    bool AudioEngine::consolidateRange (int track, juce::int64 start, juce::int64 end)
    {
        if (end <= start) return false;
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return false;

        const juce::int64 arrLen = juce::jmax (end, getArrangementLengthSamples());
        const juce::int64 a = juce::jmin (start, arrLen);
        const juce::int64 b = juce::jmin (end,   arrLen);
        if (b <= a) return false;

        auto sessionDir = getActiveSessionDir();
        const auto audioDir = sessionDir.getChildFile ("Audio Files");
        audioDir.createDirectory();
        juce::File dst;
        for (int k = 1; k < 1000; ++k)
        {
            dst = audioDir.getChildFile (juce::String::formatted ("Track_%02d_consolidated_%d.wav", track + 1, k));
            if (! dst.existsAsFile()) break;
        }

        const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
        {
            auto w = createWav24Writer (dst, sr, 1);
            if (w == nullptr) return false;
            // Stream the edited arrangement for just [a, b) straight to the
            // writer -- no whole-track buffer.
            const bool ok = forEachArrangementWindow (track, a, b,
                [&] (const float* data, juce::int64, int winLen)
                {
                    const float* chans[1] = { data };
                    return w->writeFromFloatArrays (chans, 1, winLen);
                });
            if (! ok) { w.reset(); dst.deleteFile(); return false; }
        }

        // Replace the clips in [start, end) with one clip referencing dst.
        clearTrackRange (track, start, end);
        auto& list = clipsFor (track);
        Clip c;
        c.name                 = dst.getFileNameWithoutExtension();
        c.audioFile            = dst;
        c.timelineStartSamples = start;
        c.fileStartSamples     = 0;
        c.fileLengthSamples    = b - a;
        // Insert in timeline order.
        int ins = 0; while (ins < (int) list.size() && list[(size_t) ins].timelineStartSamples < start) ++ins;
        list.insert (list.begin() + ins, c);
        player.setTrackClips (track, list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::splitTrackAtSample (int track, juce::int64 timelineSample)
    {
        // Bound against whichever subsystem currently knows about tracks:
        // the recorder (just-captured, not yet reloaded) OR the player (a
        // loaded / reopened session). The ensureClipList file check is the
        // real gate on whether there's audio to split.
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return false;
        if (timelineSample <= 0) return false;
        if (! ensureClipList (track)) return false;

        auto& list = clipsFor (track);
        const bool did = splitListAtTimeline (list, timelineSample);
        if (did)
        {
            player.setTrackClips (track, list);   // playback honours the cut next block
            syncActiveTake (track);               // persist + make undoable
        }
        return did;
    }

    bool AudioEngine::clearTrackRange (int track, juce::int64 start, juce::int64 end)
    {
        if (end <= start) return false;
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return false;
        if (! ensureClipList (track)) return false;

        auto& list = clipsFor (track);
        // Clean edges so clips inside the range have exact boundaries.
        splitListAtTimeline (list, start);
        splitListAtTimeline (list, end);

        // Drop every clip fully inside [start, end). Locked clips survive.
        bool changed = false;
        for (int i = (int) list.size() - 1; i >= 0; --i)
        {
            const auto& c  = list[(size_t) i];
            const auto  cs = c.timelineStartSamples;
            const auto  ce = cs + c.fileLengthSamples;
            if (cs >= start && ce <= end && ! c.locked)
            {
                list.erase (list.begin() + i);
                changed = true;
            }
        }
        if (changed)
        {
            player.setTrackClips (track, list);
            syncActiveTake (track);
        }
        return changed;
    }

    bool AudioEngine::rippleDeleteRange (int track, juce::int64 start, juce::int64 end)
    {
        if (end <= start) return false;
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return false;
        if (! ensureClipList (track)) return false;

        auto& list = clipsFor (track);
        splitListAtTimeline (list, start);
        splitListAtTimeline (list, end);

        const juce::int64 shift = end - start;
        std::vector<Clip> out;
        out.reserve (list.size());
        bool changed = false;
        for (auto& c : list)
        {
            const auto cs = c.timelineStartSamples;
            const auto ce = cs + c.fileLengthSamples;
            if (cs >= start && ce <= end && ! c.locked)   // inside the gap -> drop
            {
                changed = true;
                continue;
            }
            if (cs >= end)                                // after the gap -> slide left
            {
                c.timelineStartSamples -= shift;
                changed = true;
            }
            out.push_back (c);
        }
        if (changed)
        {
            list = std::move (out);
            player.setTrackClips (track, list);
            syncActiveTake (track);
        }
        return changed;
    }

    bool AudioEngine::compRangeFromTake (int track, int sourceTakeIdx,
                                         juce::int64 start, juce::int64 end)
    {
        if (end <= start) return false;
        if (track < 0 || track >= (int) trackPlaylists.size()) return false;
        auto& p = trackPlaylists[(size_t) track];
        if (sourceTakeIdx < 0 || sourceTakeIdx >= (int) p.takes.size()) return false;
        // Keep the active take == trackClips before we splice.
        syncActiveTake (track);
        if (track >= (int) trackClips.size()) return false;
        auto& dst = trackClips[(size_t) track];

        // 1. Clear [start, end) in the active comp.
        splitListAtTimeline (dst, start);
        splitListAtTimeline (dst, end);
        for (int i = (int) dst.size() - 1; i >= 0; --i)
        {
            const auto cs = dst[(size_t) i].timelineStartSamples;
            const auto ce = cs + dst[(size_t) i].fileLengthSamples;
            if (cs >= start && ce <= end && ! dst[(size_t) i].locked)
                dst.erase (dst.begin() + i);
        }

        // 2. Splice in the source take's clips intersected with [start, end).
        const auto srcClips = p.takes[(size_t) sourceTakeIdx].clips;
        for (const auto& c : srcClips)
        {
            const auto cs = c.timelineStartSamples;
            const auto ce = cs + c.fileLengthSamples;
            const auto is = juce::jmax (cs, start);
            const auto ie = juce::jmin (ce, end);
            if (ie <= is) continue;
            Clip nc = c;
            const auto lead = is - cs;
            nc.fileStartSamples     = c.fileStartSamples + lead;
            nc.fileLengthSamples    = ie - is;
            nc.timelineStartSamples = is;
            nc.fadeInSamples  = juce::jmin (nc.fadeInSamples,  nc.fileLengthSamples);
            nc.fadeOutSamples = juce::jmin (nc.fadeOutSamples, nc.fileLengthSamples);
            dst.push_back (nc);
        }

        std::sort (dst.begin(), dst.end(),
                   [] (const Clip& a, const Clip& b)
                   { return a.timelineStartSamples < b.timelineStartSamples; });
        player.setTrackClips (track, dst);
        syncActiveTake (track);
        return true;
    }

    int AudioEngine::pasteClip (int track, juce::int64 timelineStart,
                                juce::int64 fileStart, juce::int64 fileLength,
                                juce::int64 fadeIn, juce::int64 fadeOut, float gainDb,
                                const juce::String& name, const juce::File& audioFile)
    {
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return -1;
        if (fileLength <= 0) return -1;
        ensureClipList (track);
        auto& list = clipsFor (track);

        Clip c;
        c.audioFile            = audioFile;   // empty = same-track (track reader)
        c.name                 = name.isNotEmpty() ? name : juce::String ("paste");
        c.timelineStartSamples = juce::jmax ((juce::int64) 0, timelineStart);
        c.fileStartSamples     = fileStart;
        c.fileLengthSamples    = fileLength;
        c.fadeInSamples        = juce::jlimit ((juce::int64) 0, fileLength, fadeIn);
        c.fadeOutSamples       = juce::jlimit ((juce::int64) 0, fileLength, fadeOut);
        c.gainDb               = gainDb;

        // Insert sorted by timeline position so the list stays ordered.
        int insertAt = (int) list.size();
        for (int i = 0; i < (int) list.size(); ++i)
            if (list[(size_t) i].timelineStartSamples > c.timelineStartSamples) { insertAt = i; break; }
        list.insert (list.begin() + insertAt, std::move (c));
        player.setTrackClips (track, list);
        syncActiveTake (track);
        return insertAt;
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
        syncActiveTake (track);
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
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipFadeCurve (int track, int clipIndex, int curve)
    {
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;
        c.fadeCurve = juce::jlimit (0, 1, curve);   // 0 linear, 1 equal-power
        player.setTrackClips (track, list);
        syncActiveTake (track);
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
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipLocked (int track, int clipIndex, bool locked)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].locked = locked;
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipName (int track, int clipIndex, const juce::String& name)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].name = name;
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipGainDb (int track, int clipIndex, float dB)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].gainDb = juce::jlimit (-60.0f, 12.0f, dB);
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

    float AudioEngine::clipNormalizeGainDb (int track, int clipIndex, float targetDbFS)
    {
        const float kFail = std::numeric_limits<float>::quiet_NaN();

        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return kFail;
        const auto& c = (*list)[(size_t) clipIndex];
        if (c.locked) return kFail;

        // The clip's source audio. Fall back to the track's Track_NN.wav when
        // the clip doesn't carry an explicit file (seeded full-range clip).
        // For a native-stereo L slot this resolves to the 2-channel file, so
        // the peak below spans BOTH channels -> one gain covers the pair.
        auto file = c.audioFile;
        if (! file.existsAsFile())
            file = getActiveSessionDir().getChildFile ("Audio Files")
                       .getChildFile (juce::String::formatted ("Track_%02d.wav", track + 1));
        if (! file.existsAsFile()) return kFail;

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->numChannels == 0) return kFail;

        const juce::int64 start = juce::jmax ((juce::int64) 0, c.fileStartSamples);
        const juce::int64 len   = juce::jmin (c.fileLengthSamples,
                                              (juce::int64) reader->lengthInSamples - start);
        if (len <= 0) return kFail;

        // Read-only offline scan for the peak magnitude over the clip's region.
        float peak = 0.0f;
        const int block = 1 << 16;
        juce::AudioBuffer<float> buf ((int) reader->numChannels, block);
        for (juce::int64 p = 0; p < len; p += block)
        {
            const int n = (int) juce::jmin ((juce::int64) block, len - p);
            if (! reader->read (&buf, 0, n, start + p, true, true)) break;
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                peak = juce::jmax (peak, buf.getMagnitude (ch, 0, n));
        }
        if (peak <= 0.0f) return kFail;   // silent -> nothing to normalize

        return targetDbFS - juce::Decibels::gainToDecibels (peak);
    }

    bool AudioEngine::normalizeClip (int track, int clipIndex, float targetDbFS)
    {
        const float gainDb = clipNormalizeGainDb (track, clipIndex, targetDbFS);
        if (! std::isfinite (gainDb)) return false;
        // Non-destructive: set the clip gain so the peak hits the target.
        // setClipGainDb clamps to +12 dB, so a very quiet clip boosts as far
        // as the clip-gain range allows (the audio file is never altered).
        return setClipGainDb (track, clipIndex, gainDb);
    }

    juce::int64 AudioEngine::nearestZeroCrossing (int track, juce::int64 timelineSample,
                                                  int windowSamples)
    {
        const auto* list = tryClipsFor (track);
        if (list == nullptr) return timelineSample;

        // The clip under the cut.
        const Clip* clip = nullptr;
        for (const auto& c : *list)
            if (timelineSample >= c.timelineStartSamples
                && timelineSample <  c.timelineStartSamples + c.fileLengthSamples)
            { clip = &c; break; }
        if (clip == nullptr) return timelineSample;

        auto file = clip->audioFile.existsAsFile()
            ? clip->audioFile
            : getActiveSessionDir().getChildFile ("Audio Files")
                  .getChildFile (juce::String::formatted ("Track_%02d.wav", track + 1));
        if (! file.existsAsFile()) return timelineSample;

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        if (reader == nullptr || reader->numChannels == 0) return timelineSample;

        const juce::int64 fileSample = clip->fileStartSamples
                                     + (timelineSample - clip->timelineStartSamples);
        const juce::int64 w = juce::jlimit ((juce::int64) 0, (juce::int64) 4800,
                                            (juce::int64) windowSamples);
        const juce::int64 readStart = juce::jmax ((juce::int64) 0, fileSample - w);
        const juce::int64 readEnd   = juce::jmin ((juce::int64) reader->lengthInSamples,
                                                  fileSample + w);
        const int n = (int) (readEnd - readStart);
        if (n <= 2) return timelineSample;

        juce::AudioBuffer<float> buf ((int) reader->numChannels, n);
        if (! reader->read (&buf, 0, n, readStart, true, true)) return timelineSample;

        const int centre = (int) (fileSample - readStart);
        const int chans  = buf.getNumChannels();
        auto mono = [&] (int i) { float s = 0.0f; for (int ch = 0; ch < chans; ++ch) s += buf.getSample (ch, i); return s; };
        auto crosses = [&] (int i) { return i > 0 && i < n
            && ((mono (i - 1) <= 0.0f && mono (i) > 0.0f)
             || (mono (i - 1) >= 0.0f && mono (i) < 0.0f)); };

        int best = -1;
        for (int d = 0; d < n && best < 0; ++d)
        {
            if (crosses (centre + d)) best = centre + d;
            else if (crosses (centre - d)) best = centre - d;
        }
        if (best < 0) return timelineSample;   // no crossing in window -> leave the cut where it is

        const juce::int64 adjustedFile = readStart + best;
        return clip->timelineStartSamples + (adjustedFile - clip->fileStartSamples);
    }

    bool AudioEngine::deleteClip (int track, int clipIndex)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        if ((*list)[(size_t) clipIndex].locked) return false;   // lock protects against delete
        list->erase (list->begin() + clipIndex);
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::duplicateClip (int track, int clipIndex)
    {
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        if ((*list)[(size_t) clipIndex].locked) return false;   // lock protects against duplicate
        Clip copy = (*list)[(size_t) clipIndex];
        copy.timelineStartSamples += copy.fileLengthSamples;   // place after the source
        copy.name = copy.name + " (copy)";
        copy.locked = false;
        list->insert (list->begin() + clipIndex + 1, std::move (copy));
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

}
