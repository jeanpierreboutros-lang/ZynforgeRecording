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
#include <map>

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
        c.fileLengthSamples    = trackFile.getSize() > 0
            ? (juce::int64) (trackFile.getSize() / 4)  // 24-bit WAV mono ≈ 3 B/sample, rough
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

    bool AudioEngine::renderTrackArrangement (int track, juce::AudioBuffer<float>& out,
                                              juce::int64 totalSamples)
    {
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return false;
        if (totalSamples <= 0) return false;

        auto sessionDir = getActiveSessionDir();
        if (! sessionDir.isDirectory()) return false;
        const auto audioDir = sessionDir.getChildFile ("Audio Files");
        juce::File src;
        for (auto* ext : { ".wav", ".flac", ".aif", ".aiff" })
        {
            auto f = audioDir.getChildFile (juce::String::formatted ("Track_%02d", track + 1) + ext);
            if (f.existsAsFile()) { src = f; break; }
        }
        if (! src.existsAsFile()) return false;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (src));
        if (reader == nullptr) return false;

        out.setSize (1, (int) juce::jmin<juce::int64> (totalSamples, 0x7fffffff),
                     false, true, true);
        out.clear();
        const int outLen = out.getNumSamples();

        // Active-take clips for this track; bootstrap a whole-file clip if
        // the track was never edited so it still renders.
        std::vector<Clip> clips = (track < (int) trackClips.size())
                                    ? trackClips[(size_t) track] : std::vector<Clip>{};
        if (clips.empty())
        {
            Clip c;
            c.timelineStartSamples = 0;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = reader->lengthInSamples;
            clips.push_back (c);
        }

        // Cross-track clips read from their own file -- cache one reader
        // per distinct path so the bounce matches what playback renders.
        std::map<juce::String, std::unique_ptr<juce::AudioFormatReader>> extra;

        juce::AudioBuffer<float> tmp (1, 0);
        auto* dst = out.getWritePointer (0);
        for (const auto& c : clips)
        {
            if (c.muted || c.fileLengthSamples <= 0) continue;
            const juce::int64 tlStart  = c.timelineStartSamples;
            const juce::int64 writeBeg = juce::jmax<juce::int64> (0, tlStart);
            const juce::int64 writeEnd = juce::jmin<juce::int64> (outLen, tlStart + c.fileLengthSamples);
            if (writeEnd <= writeBeg) continue;
            const int span = (int) (writeEnd - writeBeg);
            const juce::int64 fileReadStart = c.fileStartSamples + (writeBeg - tlStart);

            juce::AudioFormatReader* rd = reader.get();
            if (c.audioFile != juce::File() && c.audioFile != src)
            {
                const auto key = c.audioFile.getFullPathName();
                auto it = extra.find (key);
                if (it == extra.end())
                    it = extra.emplace (key, std::unique_ptr<juce::AudioFormatReader> (
                                                 fm.createReaderFor (c.audioFile))).first;
                if (it->second != nullptr) rd = it->second.get();
            }
            if (rd == nullptr) continue;

            tmp.setSize (1, span, false, false, true);
            tmp.clear();
            rd->read (&tmp, 0, span, fileReadStart, true, true);

            const float clipGain = juce::Decibels::decibelsToGain (c.gainDb, -60.0f);
            const bool  eq        = (c.fadeCurve == 1);
            const juce::int64 fIn = c.fadeInSamples, fOut = c.fadeOutSamples;
            const juce::int64 fOutStart = c.fileLengthSamples - fOut;
            const juce::int64 spanOffsetInClip = writeBeg - tlStart;
            const auto* s = tmp.getReadPointer (0);
            for (int i = 0; i < span; ++i)
            {
                const juce::int64 clipPos = spanOffsetInClip + i;
                float g = clipGain;
                if (fIn > 0 && clipPos < fIn)
                { const float t = (float) clipPos / (float) fIn; g *= eq ? std::sin (t * 1.57079633f) : t; }
                else if (fOut > 0 && clipPos >= fOutStart)
                { const float t = (float) (c.fileLengthSamples - clipPos) / (float) fOut; g *= eq ? std::sin (t * 1.57079633f) : t; }
                dst[(int) writeBeg + i] += s[i] * g;
            }
        }
        return true;
    }

    bool AudioEngine::renderStereoMix (juce::AudioBuffer<float>& outStereo, juce::int64 totalSamples)
    {
        if (totalSamples <= 0) return false;
        const int len = (int) juce::jmin<juce::int64> (totalSamples, 0x7fffffff);
        outStereo.setSize (2, len, false, true, true);
        outStereo.clear();
        auto* oL = outStereo.getWritePointer (0);
        auto* oR = outStereo.getWritePointer (1);

        const int nTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());

        bool anySolo = false;
        for (int i = 0; i < nTracks; ++i)
            if (recorder.getTrack (i).soloed.load (std::memory_order_relaxed)) { anySolo = true; break; }
        bool anyVcaSolo = false;
        for (const auto& v : vcas)
            if (v.soloed.load (std::memory_order_relaxed)) { anyVcaSolo = true; break; }

        const double halfPi = juce::MathConstants<double>::halfPi;
        const int step = 512;   // re-evaluate automation every ~10 ms
        juce::AudioBuffer<float> mono;

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

            if (! renderTrackArrangement (t, mono, totalSamples)) continue;
            const auto* s = mono.getReadPointer (0);
            const float baseDb  = ts.gainDb.load (std::memory_order_relaxed);
            const float basePan = ts.pan.load (std::memory_order_relaxed);
            const float vcaDb   = (grp >= 0 && grp < kNumVcas)
                                    ? vcas[(size_t) grp].gainDb.load (std::memory_order_relaxed) : 0.0f;
            // Stereo pair reads its left partner's volume + mute lanes.
            const int autoCh = (t > 0 && recorder.getTrack (t - 1).isStereo.load (std::memory_order_relaxed))
                                 ? t - 1 : t;

            for (int i = 0; i < len; i += step)
            {
                const int n = juce::jmin (step, len - i);
                const float volDb = automationValueAt (autoCh, AutomationParam::Volume, i, baseDb);
                const float panV  = automationValueAt (t,      AutomationParam::Pan,    i, basePan);
                const float muteV = automationValueAt (autoCh, AutomationParam::Mute,   i,
                                                       ts.muted.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
                if (muteV > 0.5f) continue;

                const double gain = juce::Decibels::decibelsToGain ((double) (volDb + vcaDb), -60.0);
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

        const bool   mMute = masterState.muted.load (std::memory_order_relaxed);
        const double mGain = mMute ? 0.0
                                   : juce::Decibels::decibelsToGain (
                                         (double) masterState.gainDb.load (std::memory_order_relaxed), -60.0);
        if (std::abs (mGain - 1.0) > 1.0e-6) outStereo.applyGain ((float) mGain);
        return true;
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

        // Render the whole edited track, then slice out [start, end).
        juce::AudioBuffer<float> full;
        const juce::int64 arrLen = juce::jmax (end, getArrangementLengthSamples());
        if (! renderTrackArrangement (track, full, arrLen)) return false;
        const int a = (int) juce::jmin<juce::int64> (start, full.getNumSamples());
        const int b = (int) juce::jmin<juce::int64> (end,   full.getNumSamples());
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
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (dst.createOutputStream());
            if (os == nullptr) return false;
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, 1, 24, {}, 0));
            if (w == nullptr) return false;
            os.release();
            juce::AudioBuffer<float> slice (full.getArrayOfWritePointers(), 1, a, b - a);
            w->writeFromAudioSampleBuffer (slice, 0, b - a);
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
