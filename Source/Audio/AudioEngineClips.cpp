// Clip + take subsystems on AudioEngine. Extracted from
// AudioEngine.cpp as part of the 2026-05-24 god-class split.
//
// Clip-edit surface: tryClipsFor, splitTrackAtPlayhead, editClip,
// setClipFades, setClipMuted, setClipLocked, setClipGainDb,
// deleteClip, duplicateClip.
//
// Takes / playlist surface: getActiveTakeIdx, getTakeName,
// playlistsToJson, loadPlaylistsFromJson, seedDefaultClips.

#include "AudioEngine.h"
#include "CheckedFileStream.h"
#include "MultiPartReader.h"
#include "SessionInterchange.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace zynforge
{
    namespace
    {
        // Nothing but the track's own recording, whole, from timeline zero.
        bool isPlainDefaultClip (const Clip& c, int track, juce::int64 sourceLength)
        {
            return c.timelineStartSamples == 0
                && c.fileStartSamples     == 0
                && c.fileLengthSamples == sourceLength
                && c.fadeInSamples        == 0
                && c.fadeOutSamples       == 0
                && ! c.muted
                && ! c.locked
                && std::abs (c.gainDb) < 0.001f
                && c.audioFile == juce::File()
                && c.name == juce::String::formatted ("Track_%02d", track + 1);
        }
    }

    juce::File AudioEngine::getTrackAudioFile (int track, int* channel) const
    {
        if (channel != nullptr) *channel = 0;
        if (! getActiveSessionDir().isDirectory()) return {};
        auto dir = getActiveSessionDir().getChildFile ("Audio Files");
        if (! dir.isDirectory()) dir = getActiveSessionDir();
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        for (int offset = 0; offset <= 1 && track - offset >= 0; ++offset)
        {
            const auto f = findTrackMedia (dir, track - offset);
            if (! f.existsAsFile()) continue;
            if (offset == 1)
            {
                auto r = ConcatReader::create (fm, findTakeParts (f));
                if (r == nullptr || r->numChannels < 2) continue;
            }
            if (channel != nullptr) *channel = offset;
            return f;
        }
        return {};
    }

    void AudioEngine::seedDefaultClips (bool preserveEdits, bool appendRecordedAudio)
    {
        // Give every track a single full-range clip so the EDIT tools
        // (Trim / Grabber / Fade / Selector) have something
        // to grab. Without an active clip list, TrackRow's clip-edit
        // hit-test short-circuits and the tools feel dead.
        //
        // preserveEdits guards against silently wiping takes / splits /
        // fades on a SAME-session reload (stop-recording, strip reorder). Only
        // the true session-OPEN path passes false, and its .zfproj restore
        // overwrites these seeds anyway. See the seedDefaultClips ADR.
        const int n = player.getNumTracks();
        if (n <= 0 && ! preserveEdits)
        {
            trackClips.clear(); trackPlaylists.clear(); sourceTrackLengths.clear();
            return;
        }

        // A track is "edited" (must be preserved) when it holds several
        // takes or its single clip has been shaped away from the plain
        // full-range default (split, front-trimmed, faded, gained, muted,
        // moved, or repointed at a cross-track file).
        //
        // Compare against the PREVIOUS source length, not the newly loaded
        // file length. This distinguishes a default clip from a deliberate
        // end trim, even when a continuation has grown the source file.
        const auto isPlainDefault = [&] (const Clip& c, int i)
        {
            return isPlainDefaultClip (c, i, i < (int) sourceTrackLengths.size()
                                                 ? sourceTrackLengths[(size_t) i] : c.fileLengthSamples);
        };
        const auto trackIsEdited = [&] (int i) -> bool
        {
            if (i >= (int) trackPlaylists.size() || i >= (int) trackClips.size()) return false;
            if (trackPlaylists[(size_t) i].takes.size() > 1) return true;
            const auto& list = trackClips[(size_t) i];
            if (list.empty())    return ! trackPlaylists[(size_t) i].takes.empty();
            if (list.size() > 1) return true;    // split into multiple clips
            return ! isPlainDefault (list[0], i);
        };

        if (! preserveEdits)
        {
            trackClips.clear();
            trackPlaylists.clear();
            sourceTrackLengths.clear();
        }
        // GROW ONLY. A plain resize(n) SHRANK the lists to the player's track
        // count, destroying the clip list of every strip above the highest
        // recorded Track_NN -- e.g. a cross-track clip pasted onto a strip that
        // was never recorded to. That ran on every stop-record / reload, so the
        // edit silently vanished even though preserveEdits was true.
        if ((int) trackClips.size()     < n) trackClips.resize     ((size_t) n);
        if ((int) trackPlaylists.size() < n) trackPlaylists.resize ((size_t) n);
        if ((int) sourceTrackLengths.size() < n) sourceTrackLengths.resize ((size_t) n);

        // A take that landed away from its file position lands there on every
        // strip, or a plain strip growing in place would drift from the others.
        const auto shift = appendRecordedAudio ? continueShift.exchange (0, std::memory_order_acq_rel) : (juce::int64) 0;

        for (int i = 0; i < n; ++i)
        {
            const auto oldLength = sourceTrackLengths[(size_t) i];
            const auto newLength = player.getTrackLengthSamples (i);
            if (preserveEdits && (trackIsEdited (i) || shift != 0))
            {
                // A continuation adds source media after the prior end. Keep
                // every existing split/trim verbatim and expose only the
                // newly recorded span as a new clip on the active take. A
                // strip with no media before (oldLength 0) whose arrangement
                // holds pasted clips gets its whole new take the same way --
                // requiring oldLength > 0 left that recording out entirely.
                if (appendRecordedAudio && newLength > oldLength)
                    addRecordedSpan (i, shift != 0 ? juce::jmax (oldLength, continueFileBase) : oldLength,
                                     newLength, shift);
                // Keep the engineer's edits verbatim. player.loadSession
                // cleared the player-side clip lists, so republish what we
                // already hold rather than reseeding a default over them.
                player.setTrackClips (i, trackClips[(size_t) i]);
                sourceTrackLengths[(size_t) i] = newLength;
                continue;
            }

            Clip c;
            c.name                 = juce::String::formatted ("Track_%02d", i + 1);
            c.timelineStartSamples = 0;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = player.getTrackLengthSamples (i);
            sourceTrackLengths[(size_t) i] = c.fileLengthSamples;

            trackClips[(size_t) i].clear();
            trackPlaylists[(size_t) i] = {};
            if (c.fileLengthSamples <= 0) continue;   // no audio -> whole-file fallback

            trackClips[(size_t) i].push_back (c);
            player.setTrackClips (i, trackClips[(size_t) i]);

            // Seed Take 1 = current single-clip layout.
            Take t1;
            t1.name  = "Take 1";
            t1.clips = trackClips[(size_t) i];
            trackPlaylists[(size_t) i].takes.push_back (std::move (t1));
            trackPlaylists[(size_t) i].activeTake = 0;
        }

        // Strips above the highest recorded Track_NN keep their clip lists
        // (grow-only above), but the player's track count was just reset to
        // the files on disk, so it no longer renders them. Republish, or a
        // clip pasted there goes silent in playback after every take stop or
        // reload while EDIT still shows it and the bounce still renders it.
        if (preserveEdits)
            for (int i = juce::jmax (0, n); i < (int) trackClips.size(); ++i)
                if (! trackClips[(size_t) i].empty()
                    || (i < (int) trackPlaylists.size() && ! trackPlaylists[(size_t) i].takes.empty()))
                    player.setTrackClips (i, trackClips[(size_t) i]);
    }

    void AudioEngine::addRecordedSpan (int track, juce::int64 from, juce::int64 to, juce::int64 shift)
    {
        if (to <= from) return;
        Clip appended;
        appended.name = juce::String::formatted ("Track_%02d continued", track + 1);
        appended.timelineStartSamples = from - shift;
        appended.fileStartSamples = from;
        appended.fileLengthSamples = to - from;
        trackClips[(size_t) track].push_back (std::move (appended));
        syncActiveTake (track);
    }

    juce::int64 AudioEngine::continueLandsAt()
    {
        juce::int64 end = 0;
        const int strips = juce::jmax (recorder.getNumTracks(), (int) trackClips.size());
        for (int t = 0; t < strips; ++t)
        {
            if (t < recorder.getNumTracks() && recorder.getTrack (t).referenceMedia.load (std::memory_order_relaxed))
                continue;
            const bool arranged = t < (int) trackClips.size()
                               && (! trackClips[(size_t) t].empty()
                                   || (t < (int) trackPlaylists.size() && ! trackPlaylists[(size_t) t].takes.empty()));
            if (! arranged) end = juce::jmax (end, player.getTrackLengthSamples (t));
            else
                for (const auto& c : trackClips[(size_t) t])
                    end = juce::jmax (end, c.timelineStartSamples + c.fileLengthSamples);
        }
        return end;
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

    namespace
    {
        // The part of a clip list inside [start, end), moved to timeline 0.
        std::vector<Clip> croppedClips (const std::vector<Clip>& src, juce::int64 start, juce::int64 end)
        {
            std::vector<Clip> out;
            for (const auto& c : src)
            {
                const juce::int64 cs = c.timelineStartSamples;
                const juce::int64 ce = cs + c.fileLengthSamples;
                const juce::int64 is = juce::jmax (cs, start);   // intersection
                const juce::int64 ie = juce::jmin (ce, end);
                if (ie <= is) continue;                           // clip outside range

                // The piece plays exactly what the clip played there, fades
                // and all (a fresh fade at the range edge changed the audio).
                Clip piece = clipSlice (c, is - cs, ie - is);
                piece.timelineStartSamples = is - start;          // range start -> timeline 0
                out.push_back (piece);
            }
            return out;
        }
    }

    int AudioEngine::cropToRange (juce::int64 startSample, juce::int64 endSample)
    {
        if (endSample <= startSample) return 0;
        const timelinecut::Cut cut { startSample, endSample, true };
        const auto tracks = allTimelineTracks();
        if (timelineCutRefusal (cut, tracks).isNotEmpty()) return -1;

        const int n = (int) trackClips.size();
        int tracksWithAudio = 0;

        for (int t = 0; t < n; ++t)
        {
            // An empty clip list means "play the whole file" -- synthesise
            // a full-range clip so the crop has something to intersect,
            // otherwise the track would be silently dropped.
            std::vector<Clip> src = trackClips[(size_t) t];
            if (src.empty() && (t >= (int) trackPlaylists.size() || trackPlaylists[(size_t) t].takes.empty()))
            {
                Clip full;
                full.timelineStartSamples = 0;
                full.fileStartSamples     = 0;
                full.fileLengthSamples    = player.getTrackLengthSamples (t);
                if (full.fileLengthSamples > 0) src.push_back (full);
            }

            const auto out = croppedClips (src, startSample, endSample);
            trackClips[(size_t) t] = out;
            player.setTrackClips (t, out);
            syncActiveTake (t);   // keep the active take == trackClips

            // A take not playing now still has to line up with the one that
            // is, on the session's one timeline.
            auto& playlist = trackPlaylists[(size_t) t];
            for (int k = 0; k < (int) playlist.takes.size(); ++k)
                if (k != playlist.activeTake)
                    playlist.takes[(size_t) k].clips = croppedClips (playlist.takes[(size_t) k].clips,
                                                                     startSample, endSample);

            if (! out.empty()) ++tracksWithAudio;
        }
        cutSessionTimeline (cut);
        return tracksWithAudio;
    }

    void AudioEngine::cutSessionTimeline (const timelinecut::Cut& cut)
    {
        std::vector<Marker> kept;
        for (const auto& marker : markers.getAll())
            if (const auto pos = cut.map (marker.sampleOffset))
            {
                kept.push_back (marker);
                kept.back().sampleOffset = *pos;
            }
        markers.replaceAll (std::move (kept));
    }

    std::vector<int> AudioEngine::allTimelineTracks() const
    {
        std::vector<int> tracks ((size_t) juce::jmax (recorder.getNumTracks(), player.getNumTracks(),
                                                      (int) trackClips.size()));
        std::iota (tracks.begin(), tracks.end(), 0);
        return tracks;
    }

    bool AudioEngine::namesEveryStrip (const std::vector<int>& tracks) const
    {
        for (int t = 0; t < recorder.getNumTracks(); ++t)
            if (std::find (tracks.begin(), tracks.end(), t) == tracks.end()) return false;
        return true;
    }

    juce::String AudioEngine::timelineCutRefusal (const timelinecut::Cut& cut, const std::vector<int>& tracks) const
    {
        if (isRecording())         return "Stop recording before changing the timeline";
        if (isCaptureFinalizing()) return "Wait for the take to finish finalizing";
        if (isSessionTransitionActive())   return "Wait for the session operation to finish";

        const auto inTheWay = [&cut] (const std::vector<Clip>& clips)
        {
            return std::any_of (clips.begin(), clips.end(), [&cut] (const Clip& c)
            {
                return c.locked && (cut.crop || c.timelineStartSamples + c.fileLengthSamples > cut.start);
            });
        };
        for (const int t : tracks)
        {
            bool locked = t >= 0 && t < (int) trackClips.size() && inTheWay (trackClips[(size_t) t]);
            if (t >= 0 && t < (int) trackPlaylists.size())
                for (const auto& take : trackPlaylists[(size_t) t].takes)
                    locked = locked || inTheWay (take.clips);
            if (locked)
                return "A locked clip on track " + juce::String (t + 1)
                     + " would have to move -- unlock it first";
        }
        return {};
    }

    AudioEngine::TimelineState AudioEngine::captureTimelineState() const
    {
        return { playlistsToJson(), markers.getAll() };
    }

    void AudioEngine::restoreTimelineState (const TimelineState& state)
    {
        restorePlaylistSnapshot (state.playlists);
        markers.replaceAll (state.markers);
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

    // Serialise the full per-track playlist (every Take + every Clip
    // inside it) so .zfproj round-trips the engineer's work.
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
            // How much recorded media this arrangement knew about. An undo
            // step restores it after a later take: see restorePlaylistSnapshot.
            pObj->setProperty ("sourceLength", (juce::int64) (t < sourceTrackLengths.size()
                                                                  ? sourceTrackLengths[t] : 0));

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
                    cObj->setProperty ("sourceChannel", c.sourceChannel);
                    cObj->setProperty ("tlStart",     (juce::int64) c.timelineStartSamples);
                    cObj->setProperty ("fileStart",   (juce::int64) c.fileStartSamples);
                    cObj->setProperty ("fileLen",     (juce::int64) c.fileLengthSamples);
                    cObj->setProperty ("fadeIn",      (juce::int64) c.fadeInSamples);
                    cObj->setProperty ("fadeOut",     (juce::int64) c.fadeOutSamples);
                    cObj->setProperty ("fadeCurve",   c.fadeCurve);
                    cObj->setProperty ("fadeInRamp",        (juce::int64) c.fadeInRampSamples);
                    cObj->setProperty ("fadeInRampOffset",  (juce::int64) c.fadeInRampOffsetSamples);
                    cObj->setProperty ("fadeOutRamp",       (juce::int64) c.fadeOutRampSamples);
                    cObj->setProperty ("fadeOutRampOffset", (juce::int64) c.fadeOutRampOffsetSamples);
                    // Cross-track clips reference another track's file --
                    // store the NAME only so the session stays portable.
                    if (c.audioFile != juce::File() && c.audioFile.isAChildOf (getActiveSessionDir()))
                        cObj->setProperty ("sessionAudioFile", c.audioFile.getRelativePathFrom (getActiveSessionDir()));
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

    juce::var AudioEngine::mediaLengthsToJson() const
    {
        juce::Array<juce::var> lengths;
        for (int i = 0; i < player.getNumTracks(); ++i)
            lengths.add (juce::var (player.getTrackLengthSamples (i)));
        return juce::var (lengths);
    }

    void AudioEngine::reconcileClipsWithMedia (const juce::var& savedLengths, bool afterCrash)
    {
        const auto* saved = savedLengths.getArray();
        if (saved == nullptr && ! afterCrash) return;

        const int n = player.getNumTracks();
        if ((int) sourceTrackLengths.size() < n) sourceTrackLengths.resize ((size_t) n);
        if ((int) trackClips.size() < n) return;

        bool grew = false;
        std::vector<juce::int64> known ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const auto media = player.getTrackLengthSamples (i);
            juce::int64 covered = 0;
            for (const auto& clip : trackClips[(size_t) i])
                if (clip.audioFile == juce::File())
                    covered = juce::jmax (covered, clip.fileStartSamples + clip.fileLengthSamples);
            const auto before = saved != nullptr ? (i < saved->size() ? (juce::int64) (*saved)[i] : 0) : covered;
            known[(size_t) i] = covered >= media ? media : juce::jmin (before, media);
            grew = grew || known[(size_t) i] < media;
        }
        if (! grew) return;

        // STOP's own path, from the length these clips knew: a plain clip
        // grows, an edited arrangement keeps every edit and gains a new clip.
        for (int i = 0; i < n; ++i) sourceTrackLengths[(size_t) i] = known[(size_t) i];
        seedDefaultClips (/*preserveEdits*/ true, /*appendRecordedAudio*/ true);
    }

    namespace
    {
        bool readSampleCoordinate (const juce::var& value, juce::int64& result)
        {
            // Missing legacy fields retain their zero default. Integer JSON
            // coordinates never pass through double, which loses samples >2^53.
            if (value.isVoid()) { result = 0; return true; }
            if (value.isInt() || value.isInt64())
            {
                result = (juce::int64) value;
                return result >= 0;
            }
            if (! value.isDouble()) return false;
            const double number = (double) value;
            // INT64_MAX rounds to 2^63 as a double: use an exclusive upper bound.
            if (! std::isfinite (number) || number < 0.0 || number >= 0x1p63
                || std::floor (number) != number)
                return false;
            result = (juce::int64) number;
            return true;
        }
    }

    void AudioEngine::loadPlaylistsFromJson (const juce::var& v)
    {
        auto* arr = v.getArray();
        if (arr == nullptr) return;

        // Resize storage to match (seedDefaultClips has already
        // populated this on session load; we overwrite per-track here).
        std::vector<char> hasTakes;   // tracks the JSON gives at least one take
        for (const auto& item : *arr)
        {
            auto* pObj = item.getDynamicObject();
            if (pObj == nullptr) continue;
            const int t = (int) pObj->getProperty ("track");
            if (t < 0 || t >= 256) continue;
            if (t >= (int) trackPlaylists.size()) trackPlaylists.resize ((size_t) t + 1);
            if (t >= (int) trackClips.size()) trackClips.resize ((size_t) t + 1);
            auto& pl = trackPlaylists[(size_t) t];

            pl.takes.clear();
            pl.activeTake = 0;

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
                        if (! readSampleCoordinate (cObj->getProperty ("tlStart"), c.timelineStartSamples)
                            || ! readSampleCoordinate (cObj->getProperty ("fileStart"), c.fileStartSamples)
                            || ! readSampleCoordinate (cObj->getProperty ("fileLen"), c.fileLengthSamples)
                            || ! readSampleCoordinate (cObj->getProperty ("fadeIn"), c.fadeInSamples)
                            || ! readSampleCoordinate (cObj->getProperty ("fadeOut"), c.fadeOutSamples))
                            continue;
                        constexpr auto maxSample = std::numeric_limits<juce::int64>::max();
                        if (c.fileLengthSamples > maxSample - c.timelineStartSamples
                            || c.fileLengthSamples > maxSample - c.fileStartSamples)
                            continue;
                        c.fadeCurve            = (int) cObj->getProperty ("fadeCurve");   // 0 if absent
                        // Fade ramps (absent in older saves = ordinary fades);
                        // a malformed one degrades to an ordinary fade.
                        for (auto [key, field] : { std::pair { "fadeInRamp",        &c.fadeInRampSamples },
                                                   std::pair { "fadeInRampOffset",  &c.fadeInRampOffsetSamples },
                                                   std::pair { "fadeOutRamp",       &c.fadeOutRampSamples },
                                                   std::pair { "fadeOutRampOffset", &c.fadeOutRampOffsetSamples } })
                            if (! readSampleCoordinate (cObj->getProperty (key), *field)) *field = 0;
                        normaliseFadeRamps (c);
                        c.sourceChannel        = cObj->hasProperty ("sourceChannel") ? (int) cObj->getProperty ("sourceChannel") : -1;
                        const auto afName = cObj->getProperty ("audioFile").toString();
                        if (afName.isNotEmpty())
                            c.audioFile = getActiveSessionDir().getChildFile ("Audio Files")
                                                               .getChildFile (afName);
                        const auto relativeFile = cObj->getProperty ("sessionAudioFile").toString();
                        if (relativeFile.isNotEmpty())
                            c.audioFile = getActiveSessionDir().getChildFile (relativeFile);
                        c.gainDb               = (float)        (double) cObj->getProperty ("gainDb");
                        c.muted                = (bool)         cObj->getProperty ("muted");
                        c.locked               = (bool)         cObj->getProperty ("locked");
                        tk.clips.push_back (std::move (c));
                    }
                }
                pl.takes.push_back (std::move (tk));
            }
            if (! pl.takes.empty())
            {
                if (t >= (int) hasTakes.size()) hasTakes.resize ((size_t) t + 1, 0);
                hasTakes[(size_t) t] = 1;

                // The app only writes a valid index. A hand-edited one must
                // not index outside the takes, or leave the live clips apart
                // from the active take so the next edit overwrites a take.
                pl.activeTake = juce::jlimit (0, (int) pl.takes.size() - 1,
                                              (int) pObj->getProperty ("activeTake"));

                // Reapply the active take to the live trackClips +
                // SessionPlayer so playback matches the persisted state.
                trackClips[(size_t) t] = pl.takes[(size_t) pl.activeTake].clips;
                player.setTrackClips (t, trackClips[(size_t) t]);
            }
        }

        // A strip the JSON gives no take -- absent (it had no playlist when an
        // undo snapshot was taken) or an empty take list -- used to keep the
        // clips it holds now. Undoing a paste onto a never-recorded strip so
        // left the clip visible, audible and bounced while the playlist, and
        // so the next save, no longer had it. Return such a strip to "no
        // arrangement", but only when it has no media of its own: a take
        // recorded after an older undo snapshot must keep its clips.
        const auto tracks = juce::jmax (trackClips.size(), trackPlaylists.size());
        for (size_t t = 0; t < tracks; ++t)
        {
            if (t < hasTakes.size() && hasTakes[t] != 0) continue;
            if (player.getTrackLengthSamples ((int) t) > 0)
            {
                // The loop above may have emptied this playlist; put the
                // clips it keeps back in, or the next save drops them.
                if (t < trackClips.size() && ! trackClips[t].empty()) syncActiveTake ((int) t);
                continue;
            }
            if (t < trackPlaylists.size()) trackPlaylists[t] = {};
            if (t < trackClips.size() && ! trackClips[t].empty())
            {
                trackClips[t].clear();
                player.setTrackClips ((int) t, trackClips[t]);
            }
        }
    }

    void AudioEngine::restorePlaylistSnapshot (const juce::var& snapshot)
    {
        loadPlaylistsFromJson (snapshot);

        // A snapshot can be older than the last take. Recording is not
        // undoable, so the media recorded since joins the restored
        // arrangement the way the take's own stop added it. Redo comes through
        // here too: its snapshot is just as old.
        auto* entries = snapshot.getArray();
        if (entries == nullptr) return;
        for (const auto& item : *entries)
        {
            auto* entry = item.getDynamicObject();
            if (entry == nullptr || ! entry->hasProperty ("sourceLength")) continue;
            const int t = (int) entry->getProperty ("track");
            if (t < 0 || t >= (int) sourceTrackLengths.size() || t >= (int) trackClips.size()) continue;

            // No takes in the snapshot: loadPlaylistsFromJson kept today's clips.
            const auto* takes = entry->getProperty ("takes").getArray();
            if (takes == nullptr || takes->isEmpty()) continue;

            const auto known = (juce::int64) entry->getProperty ("sourceLength");
            const auto now = sourceTrackLengths[(size_t) t];
            if (now <= known) continue;

            auto& list = trackClips[(size_t) t];
            if (list.size() == 1 && isPlainDefaultClip (list[0], t, known))
            {
                list[0].fileLengthSamples = now;
                syncActiveTake (t);
            }
            else
            {
                addRecordedSpan (t, known, now);
            }
            player.setTrackClips (t, list);
        }
    }

    const std::vector<Clip>* AudioEngine::tryClipsFor (int track) const
    {
        if (track < 0 || track >= (int) trackClips.size()) return nullptr;
        const auto& v = trackClips[(size_t) track];
        return v.empty() ? nullptr : &v;
    }

    juce::String AudioEngine::punchRefusal (juce::int64 start, juce::int64 end)
    {
        const auto last = end >= 0 ? end : std::numeric_limits<juce::int64>::max();
        const juce::ScopedLock structure (recorder.getStructureLock());
        const int strips = recorder.getNumTracks();

        const auto refused = [this] (int track, const juce::String& why)
        {
            return "Punch refused -- " + recorder.getTrack (track).getNameThreadSafe() + ": " + why;
        };

        for (int t = 0; t < strips; ++t)
        {
            // A stereo strip's punch rewrites one file, so its right half
            // answers for that file too, armed or not.
            const bool leftArmed = t > 0 && recorder.getTrack (t - 1).isStereo.load (std::memory_order_relaxed)
                                && recorder.getTrack (t - 1).armed.load (std::memory_order_relaxed);
            if (! leftArmed && ! recorder.getTrack (t).armed.load (std::memory_order_relaxed)) continue;

            static const std::vector<Clip> none;
            const auto& clips = t < (int) trackClips.size() ? trackClips[(size_t) t] : none;
            const auto media  = player.getTrackLengthSamples (t);
            int channel = 0;
            const auto take = getTrackAudioFile (t, &channel);
            const auto playsTake = [&] (const Clip& c)
            {
                return (c.audioFile == juce::File() || c.audioFile == take)
                    && (c.sourceChannel < 0 || c.sourceChannel == channel);
            };

            std::vector<std::pair<juce::int64, juce::int64>> spans;
            juce::int64 clipsEnd = 0;
            for (const auto& c : clips)
            {
                const auto from = c.timelineStartSamples, to = from + c.fileLengthSamples;
                clipsEnd = juce::jmax (clipsEnd, to);
                if (to <= start || from >= last) continue;
                if (! playsTake (c))                 return refused (t, "the clip here plays another file");
                if (from != c.fileStartSamples)      return refused (t, "the clip here has moved, so the timeline is not the take here");
                spans.emplace_back (from, to);
            }

            // No clip list at all is not an edit: the file plays whole, from zero.
            const bool cleared = clips.empty() && t < (int) trackPlaylists.size()
                              && ! trackPlaylists[(size_t) t].takes.empty();
            if (! clips.empty() || cleared)
            {
                std::sort (spans.begin(), spans.end());
                auto reached = start;
                for (const auto& [from, to] : spans)
                {
                    if (from > reached) break;
                    reached = juce::jmax (reached, to);
                }
                // Past the strip's end there is nothing to lose. An unedited
                // strip that simply ended early still takes the punch there.
                const auto stripEnd = juce::jmax (media, clipsEnd);
                const bool plain = clips.size() == 1 && clips[0].timelineStartSamples == 0
                                && clips[0].fileStartSamples == 0 && playsTake (clips[0])
                                && clips[0].fileLengthSamples >= media;
                if (reached < juce::jmin (last, stripEnd) || (start >= stripEnd && ! plain))
                    return refused (t, "no clip plays here, so the punch would go unheard into the take");
            }

            // The same file audio can also sound at another place on the
            // timeline: a duplicate, or a clip pasted to another strip. A punch
            // would change it there. One playing it right here hears the punch
            // where it was played, on whichever strip.
            for (size_t other = 0; other < trackClips.size(); ++other)
                for (const auto& c : trackClips[other])
                {
                    const bool readsTake = c.audioFile == juce::File() ? (int) other == t
                                                                       : take != juce::File() && c.audioFile == take;
                    if (! readsTake || c.timelineStartSamples == c.fileStartSamples
                        || c.fileStartSamples >= last || c.fileStartSamples + c.fileLengthSamples <= start)
                        continue;
                    return refused (t, "this audio also plays elsewhere on the timeline, which the punch would change");
                }
        }
        return {};
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
        // Split EVERY clip in `list` that straddles `timelineSample`.
        // Returns true if at least one cut landed (sample strictly inside a
        // clip). Clips may overlap (a paste or move over a neighbour), so
        // stopping at the first one left the others crossing a range edge:
        // Clear / Ripple kept them playing inside the range, and
        // Consolidate played them again on top of its own render.
        bool splitListAtTimeline (std::vector<Clip>& list, juce::int64 timelineSample)
        {
            bool cut = false;
            for (int i = 0; i < (int) list.size(); ++i)
            {
                const auto& c = list[(size_t) i];
                const auto tEnd = c.timelineStartSamples + c.fileLengthSamples;
                if (! c.locked && timelineSample > c.timelineStartSamples && timelineSample < tEnd)
                {
                    const auto fileOffset = c.fileStartSamples
                                          + (timelineSample - c.timelineStartSamples);
                    // The right half is inserted at i + 1 and starts exactly
                    // at the cut, so step over it.
                    if (splitClipAt (list, i, fileOffset)) { cut = true; ++i; }
                }
            }
            return cut;
        }
    }

    // Lazy bootstrap: ensure `track` has a clip list (one full-range clip
    // spanning the whole file) so edits have something to act on. Returns
    // false when there's no audio file backing the track.
    bool AudioEngine::ensureClipList (int track)
    {
        // Existing empty arrangements are deliberately silent.
        if (track >= 0 && track < (int) trackPlaylists.size()
            && ! trackPlaylists[(size_t) track].takes.empty())
            return true;
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
            auto rd = ConcatReader::create (fm, findTakeParts (trackFile));
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
        // needs a multi-GB flat buffer.
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
                if (engineClips != nullptr) clips = *engineClips;

                // 1) The track's OWN file (Track_<track+1>). A 2-channel own
                //    file means this index is the LEFT of an interleaved
                //    stereo pair -> read channel 0; a mono file reads all.
                srcFile = findTrackMedia (audioDir, track);

                if (srcFile.existsAsFile())
                {
                    reader = ConcatReader::create (fm, findTakeParts (srcFile));
                    if (reader == nullptr) return engineClips != nullptr;
                    readChannel = (reader->numChannels >= 2) ? 0 : -1;
                }
                else if (track > 0)
                {
                    // 2) No own file: this may be the RIGHT half of an
                    //    interleaved stereo pair, whose audio lives in the
                    //    LEFT slot's 2-ch file (named Track_<track>, since the
                    //    L partner is index track-1). Read channel 1 from it.
                    srcFile = findTrackMedia (audioDir, track - 1);
                    if (! srcFile.existsAsFile()) return true;
                    reader = ConcatReader::create (fm, findTakeParts (srcFile));
                    if (reader == nullptr || reader->numChannels < 2)
                    { reader.reset(); srcFile = juce::File(); return true; }
                    readChannel = 1;
                }
                else
                {
                    return true;
                }

                // Active-take clips; bootstrap a whole-file clip if the
                // track was never edited so it still renders.
                if (engineClips != nullptr) clips = *engineClips;
                if (engineClips == nullptr)
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
            bool mixWindow (float* dst, juce::int64 winStart, int winLen, juce::AudioBuffer<float>& tmp)
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
                        rd = nullptr;
                        const auto key = c.audioFile.getFullPathName();
                        auto it = extra.find (key);
                        if (it == extra.end())
                            it = extra.emplace (key, ConcatReader::create (
                                                         fm, findTakeParts (c.audioFile))).first;
                        if (it->second != nullptr) rd = it->second.get();
                    }
                    if (rd == nullptr || fileReadStart < 0
                        || fileReadStart + span > rd->lengthInSamples) return false;

                    // Read both channels (a stereo file fills L+R; a mono file
                    // fills ch 0). For the track's own reader, pick the L/R
                    // half via readChannel; cross-track clips are mono -> ch 0.
                    const int ch = c.sourceChannel >= 0 ? juce::jlimit (0, 1, c.sourceChannel)
                                 : (rd == reader.get() ? juce::jmax (0, readChannel) : 0);
                    tmp.setSize (2, span, false, false, true);
                    tmp.clear();
                    if ((unsigned) ch >= rd->numChannels
                        || ! rd->read (&tmp, 0, span, fileReadStart, true, true)) return false;

                    const float clipGain = juce::Decibels::decibelsToGain (c.gainDb, -60.0f);
                    const juce::int64 spanOffsetInClip = writeBeg - tlStart;
                    const auto* s = tmp.getReadPointer (ch);
                    auto* d = dst + (int) (writeBeg - winStart);
                    // The player's fade envelope (ClipModel.h), so render == monitor.
                    for (int i = 0; i < span; ++i)
                        d[i] += s[i] * (clipGain * clipFadeGain (c, spanOffsetInClip + i));
                }
                return true;
            }
        };

        bool sameClipList (const std::vector<Clip>& a, const std::vector<Clip>& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                const auto& x = a[i];
                const auto& y = b[i];
                if (x.name != y.name || x.audioFile != y.audioFile
                    || x.sourceChannel != y.sourceChannel
                    || x.timelineStartSamples != y.timelineStartSamples
                    || x.fileStartSamples != y.fileStartSamples
                    || x.fileLengthSamples != y.fileLengthSamples
                    || x.fadeInSamples != y.fadeInSamples
                    || x.fadeOutSamples != y.fadeOutSamples
                    || x.fadeInRampSamples != y.fadeInRampSamples
                    || x.fadeInRampOffsetSamples != y.fadeInRampOffsetSamples
                    || x.fadeOutRampSamples != y.fadeOutRampSamples
                    || x.fadeOutRampOffsetSamples != y.fadeOutRampOffsetSamples
                    || x.fadeCurve != y.fadeCurve || x.gainDb != y.gainDb
                    || x.muted != y.muted || x.locked != y.locked)
                    return false;
            }
            return true;
        }

        std::unique_ptr<juce::AudioFormatWriter> createWav24Writer (const juce::File& f, double sr, int channels,
                                                                    const CheckedFileStream::Failed& closeFailed)
        {
            f.deleteFile();
            juce::WavAudioFormat wav;
            auto os = CheckedFileStream::open (f, closeFailed);
            if (os == nullptr) return nullptr;
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, (unsigned int) channels, 24, {}, 0));
            if (w != nullptr) os.release();
            return w;
        }

        juce::File bounceTemporary (const juce::File& destination)
        {
            return destination.getSiblingFile ("." + destination.getFileNameWithoutExtension()
                                               + "-" + juce::Uuid().toString()
                                               + ".partial.wav");
        }

        bool installBounce (const juce::File& temporary, const juce::File& destination)
        {
            if (temporary.replaceFileIn (destination)) return true;
            temporary.deleteFile();
            return false;
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
        auto audioDir = sessionDir.getChildFile ("Audio Files");
        if (! audioDir.isDirectory()) audioDir = sessionDir;
        if (! src.open (audioDir, track, engineClips)) return false;

        juce::AudioBuffer<float> window (1, kRenderWindowSamples), tmp (1, 0);
        for (juce::int64 winStart = startSample; winStart < endSample; winStart += kRenderWindowSamples)
        {
            const int winLen = (int) juce::jmin<juce::int64> (kRenderWindowSamples, endSample - winStart);
            window.clear (0, 0, winLen);
            if (! src.mixWindow (window.getWritePointer (0), winStart, winLen, tmp)) return false;
            if (! consume (window.getReadPointer (0), winStart, winLen)) return false;
        }
        return true;
    }

    bool AudioEngine::renderTrackArrangement (int track, juce::AudioBuffer<float>& out,
                                              juce::int64 totalSamples)
    {
        if (totalSamples <= 0 || totalSamples > std::numeric_limits<int>::max()) return false;
        const int outLen = (int) totalSamples;
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
                                                   juce::int64 totalSamples, double sampleRate,
                                                   const std::atomic<bool>* cancel)
    {
        const auto temporary = bounceTemporary (dest);
        const auto closeFailed = CheckedFileStream::makeFailedFlag();
        auto writer = createWav24Writer (temporary, sampleRate, 1, closeFailed);
        if (writer == nullptr) { temporary.deleteFile(); return false; }
        const bool ok = forEachArrangementWindow (track, 0, totalSamples,
            [&] (const float* data, juce::int64, int winLen)
            {
                if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) return false;
                const float* chans[1] = { data };
                return writer->writeFromFloatArrays (chans, 1, winLen);
            });
        writer.reset();
        if (! ok || closeFailed->load()) { temporary.deleteFile(); return false; }
        return installBounce (temporary, dest);
    }

    bool AudioEngine::forEachStereoMixWindow (juce::int64 totalSamples,
                                              const std::function<bool (const juce::AudioBuffer<float>&, juce::int64, int)>& consume)
    {
        return forEachStereoMixWindow (0, totalSamples, consume);
    }

    bool AudioEngine::forEachStereoMixWindow (juce::int64 startSample, juce::int64 endSample,
                                              const std::function<bool (const juce::AudioBuffer<float>&, juce::int64, int)>& consume)
    {
        if (startSample < 0 || endSample <= startSample) return false;
        // The MIXER is authoritative for the stereo mix: every loop below reads
        // a TrackState (solo / mute / gain / pan), so the bound MUST be
        // the recorder's track count. It used to be jmax(recorder, player),
        // which indexed `recorder.getTrack()` -- an unchecked `*tracks[i]` --
        // past the end whenever the session had more Track_NN files on disk
        // than strips in the mixer (delete a strip after recording, then Bounce
        // Stereo Mix). A file with no strip has no fader/pan/mute, so it isn't
        // part of the mix -- matching onBounceStems, which already bounds on
        // the recorder count.
        const auto audioDir = getActiveSessionDir().getChildFile ("Audio Files");

        // Everything the mix reads from a strip is SNAPSHOTTED here, under a
        // BRIEF structure lock, and the windows below render from the
        // snapshot alone. The render used to keep a pointer to each strip's
        // TrackState and re-read mute every window, so the caller
        // had to hold the structure lock for the WHOLE bounce -- and the UI
        // timer's captureStatus() takes that lock, freezing the app until the
        // bounce finished. The snapshot is also the honest semantic: the file
        // is the mix as it stood when the bounce began, not a blend of
        // mid-bounce fader and mute moves.
        struct TrackCtx
        {
            int track {};
            bool audible {};
            ArrangementSource src;
            float baseDb {}, basePan {};
        };
        std::vector<std::unique_ptr<TrackCtx>> live;
        int nTracks = 0;
        {
            const juce::ScopedLock structure (recorder.getStructureLock());
            // The MIXER is authoritative for the stereo mix: every strip read
            // below is a TrackState (solo / mute / gain / pan), so the
            // bound MUST be the recorder's track count. It used to be
            // jmax(recorder, player), which indexed `recorder.getTrack()` --
            // an unchecked `*tracks[i]` -- past the end whenever the session
            // had more Track_NN files on disk than strips in the mixer. A file
            // with no strip has no fader/pan/mute, so it isn't part of the mix
            // -- matching onBounceStems, which also bounds on the recorder.
            nTracks = recorder.getNumTracks();

            bool anySolo = false;
            for (int i = 0; i < nTracks; ++i)
                if (recorder.getTrack (i).soloed.load (std::memory_order_relaxed)) { anySolo = true; break; }

            for (int t = 0; t < nTracks; ++t)
            {
                const auto& ts = recorder.getTrack (t);
                auto ctx = std::make_unique<TrackCtx>();
                ctx->audible  = mix::audible (ts.muted.load (std::memory_order_relaxed),
                                              ts.soloed.load (std::memory_order_relaxed), anySolo);
                ctx->track    = t;
                ctx->baseDb   = ts.gainDb.load (std::memory_order_relaxed);
                ctx->basePan  = ts.pan.load (std::memory_order_relaxed);
                live.push_back (std::move (ctx));
            }
        }

        for (auto& cp : live)
            if (cp->audible)
            {
                const auto* clips = cp->track < (int) trackClips.size()
                    ? &trackClips[(size_t) cp->track] : nullptr;
                if (! cp->src.open (audioDir, cp->track, clips)) return false;
            }

        // Master MUTE keeps playback out of the PA. It is a monitoring switch,
        // not part of the mix, so a render follows the master fader alone.
        const double mGain = mix::masterGain (false, masterState.gainDb.load (std::memory_order_relaxed));

        juce::AudioBuffer<float> stereo (2, kRenderWindowSamples), raw (juce::jmax (1, nTracks), kRenderWindowSamples), tmp (1, 0);
        // Windows stay on the absolute grid wherever a range starts, so a range
        // cut anywhere plays exactly the samples a whole render puts there. Only
        // the part of the first window that lies inside the range reaches
        // `consume`.
        for (juce::int64 winStart = startSample - startSample % kRenderWindowSamples;
             winStart < endSample; winStart += kRenderWindowSamples)
        {
            const int winLen = (int) juce::jmin<juce::int64> (kRenderWindowSamples, endSample - winStart);
            stereo.clear();
            auto* oL = stereo.getWritePointer (0);
            auto* oR = stereo.getWritePointer (1);

            raw.clear();
            for (auto& cp : live)
                if (cp->audible
                    && ! cp->src.mixWindow (raw.getWritePointer (cp->track), winStart, winLen, tmp)) return false;

            for (auto& cp : live)
            {
                if (! cp->audible) continue;
                const double gain = juce::Decibels::decibelsToGain ((double) mix::stripDb (cp->baseDb), -60.0);
                const auto [gL, gR] = mix::constantPowerPan (gain, cp->basePan);
                const auto* s = raw.getReadPointer (cp->track);
                for (int k = 0; k < winLen; ++k)
                {
                    const float v = s[k];
                    oL[k] += (float) (v * gL);
                    oR[k] += (float) (v * gR);
                }
            }

            if (std::abs (mGain - 1.0) > 1.0e-6)
                stereo.applyGain (0, winLen, (float) mGain);
            if (const int skip = (int) juce::jmax<juce::int64> (0, startSample - winStart); skip > 0)
            {
                const juce::AudioBuffer<float> inRange (stereo.getArrayOfWritePointers(), 2, skip, winLen - skip);
                if (! consume (inRange, winStart + skip, winLen - skip)) return false;
            }
            else if (! consume (stereo, winStart, winLen)) return false;
        }
        return true;
    }

    bool AudioEngine::renderStereoMix (juce::AudioBuffer<float>& outStereo, juce::int64 totalSamples)
    {
        // AudioBuffer uses an int sample count. The file-bounce path below is
        // the streaming API for longer timelines; refuse instead of silently
        // truncating or overflowing this in-memory convenience render.
        if (totalSamples <= 0 || totalSamples > std::numeric_limits<int>::max()) return false;
        const int len = (int) totalSamples;
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
                                            juce::int64 totalSamples, double sampleRate,
                                            const std::atomic<bool>* cancel, float* peak)
    {
        const auto temporary = bounceTemporary (dest);
        const auto closeFailed = CheckedFileStream::makeFailedFlag();
        auto writer = createWav24Writer (temporary, sampleRate, 2, closeFailed);
        if (writer == nullptr) { temporary.deleteFile(); return false; }
        if (peak != nullptr) *peak = 0.0f;
        const bool ok = forEachStereoMixWindow (totalSamples,
            [&] (const juce::AudioBuffer<float>& w, juce::int64, int winLen)
            {
                if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) return false;
                if (peak != nullptr) *peak = juce::jmax (*peak, w.getMagnitude (0, winLen));
                return writer->writeFromAudioSampleBuffer (w, 0, winLen);
            });
        writer.reset();
        if (! ok || closeFailed->load()) { temporary.deleteFile(); return false; }
        return installBounce (temporary, dest);
    }

    bool AudioEngine::forEachStereoPairWindow (int trackL, juce::int64 startSample, juce::int64 endSample,
                                               const std::function<bool (const juce::AudioBuffer<float>&, juce::int64, int)>& consume)
    {
        if (startSample < 0 || endSample <= startSample) return false;
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

        juce::AudioBuffer<float> stereo (2, kRenderWindowSamples), tmp (1, 0);
        for (juce::int64 winStart = startSample; winStart < endSample; winStart += kRenderWindowSamples)
        {
            const int winLen = (int) juce::jmin<juce::int64> (kRenderWindowSamples, endSample - winStart);
            stereo.clear (0, 0, winLen);
            stereo.clear (1, 0, winLen);
            if ((okL && ! L.mixWindow (stereo.getWritePointer (0), winStart, winLen, tmp))
                || (okR && ! R.mixWindow (stereo.getWritePointer (1), winStart, winLen, tmp)))
                return false;
            if (! consume (stereo, winStart, winLen)) return false;
        }
        return true;
    }

    bool AudioEngine::bounceStereoPairToWav (int trackL, const juce::File& dest,
                                             juce::int64 totalSamples, double sampleRate,
                                             const std::atomic<bool>* cancel)
    {
        const auto temporary = bounceTemporary (dest);
        const auto closeFailed = CheckedFileStream::makeFailedFlag();
        auto writer = createWav24Writer (temporary, sampleRate, 2, closeFailed);
        if (writer == nullptr) { temporary.deleteFile(); return false; }
        const bool ok = forEachStereoPairWindow (trackL, 0, totalSamples,
            [&] (const juce::AudioBuffer<float>& stereo, juce::int64, int winLen)
            {
                if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) return false;
                return writer->writeFromAudioSampleBuffer (stereo, 0, winLen);
            });
        writer.reset();
        if (! ok || closeFailed->load()) { temporary.deleteFile(); return false; }
        return installBounce (temporary, dest);
    }

    std::vector<songs::Voice> AudioEngine::songScanVoices()
    {
        std::vector<songs::Voice> voices;
        const juce::ScopedLock structure (recorder.getStructureLock());
        for (int t = 0; t < recorder.getNumTracks(); ++t)
        {
            if (t > 0 && recorder.getTrack (t - 1).isStereo.load (std::memory_order_relaxed)) continue;
            if (isTrackArrangementEmpty (t)) continue;

            songs::Voice voice;
            voice.ownFile = getTrackAudioFile (t);
            if (const auto* clips = tryClipsFor (t)) voice.clips = *clips;
            else
            {
                Clip whole;
                whole.fileLengthSamples = player.getTrackLengthSamples (t);
                if (whole.fileLengthSamples <= 0) continue;   // nothing recorded here
                voice.clips.push_back (whole);
            }
            voices.push_back (std::move (voice));
        }
        return voices;
    }

    bool AudioEngine::isAnySoloActive()
    {
        const juce::ScopedLock structure (recorder.getStructureLock());
        for (int t = 0; t < recorder.getNumTracks(); ++t)
            if (recorder.getTrack (t).soloed.load (std::memory_order_relaxed)) return true;
        return false;
    }

    juce::String AudioEngine::exportRefusal() const
    {
        if (isRecording())         return "Stop recording before exporting";
        if (isCaptureFinalizing()) return "Wait for the take to finish finalizing before exporting";
        return {};
    }

    interchange::SessionInput AudioEngine::captureInterchange()
    {
        interchange::SessionInput input;
        input.sessionDir  = getActiveSessionDir();
        input.sessionName = input.sessionDir.getFileName();
        const double playerRate = player.getSampleRate();
        input.sampleRate = playerRate > 0.0 ? playerRate
                         : (sessionSampleRate.load() > 0.0 ? sessionSampleRate.load() : getDeviceSampleRate());
        input.masterDb    = getMasterGainDb();
        input.masterMuted = getMasterMuted();
        input.anySolo     = isAnySoloActive();

        for (const auto& marker : markers.getAll())
            input.markers.push_back ({ marker.sampleOffset, marker.name });

        {
            const juce::ScopedLock structure (recorder.getStructureLock());
            for (int t = 0; t < recorder.getNumTracks(); ++t)
            {
                const auto& ts = recorder.getTrack (t);
                interchange::StripInput strip;
                strip.name       = ts.getNameThreadSafe();
                strip.colourArgb = ts.colourARGB.load (std::memory_order_relaxed);
                strip.stereoLeft = ts.isStereo.load (std::memory_order_relaxed);
                strip.faderDb    = ts.gainDb.load (std::memory_order_relaxed);
                strip.pan        = ts.pan.load (std::memory_order_relaxed);
                strip.muted      = ts.muted.load (std::memory_order_relaxed);

                if (t < (int) trackClips.size())
                    strip.clips = trackClips[(size_t) t];
                input.strips.push_back (std::move (strip));
            }
        }

        return input;
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
        // The one clip that adjacent clips a + b would be: a's start and
        // fade-in, b's end and fade-out, and a fade that one half's ramp
        // carries across the join continuing into the other half.
        Clip joinedClip (const Clip& a, const Clip& b)
        {
            Clip m = a;
            m.fileLengthSamples = a.fileLengthSamples + b.fileLengthSamples;
            if (a.fadeInSamples == a.fileLengthSamples && b.fadeInSamples > 0)
                m.fadeInSamples = a.fadeInSamples + b.fadeInSamples;
            m.fadeOutSamples           = b.fadeOutSamples;
            m.fadeOutRampSamples       = b.fadeOutRampSamples;
            m.fadeOutRampOffsetSamples = b.fadeOutRampOffsetSamples;
            if (b.fadeOutSamples == b.fileLengthSamples && a.fadeOutSamples > 0)
                m.fadeOutSamples = a.fadeOutSamples + b.fadeOutSamples;
            normaliseFadeRamps (m);
            return m;
        }

        bool sameFades (const Clip& x, const Clip& y)
        {
            return x.fadeInSamples == y.fadeInSamples && x.fadeOutSamples == y.fadeOutSamples
                && x.fadeInRampSamples == y.fadeInRampSamples
                && x.fadeInRampOffsetSamples == y.fadeInRampOffsetSamples
                && x.fadeOutRampSamples == y.fadeOutRampSamples
                && x.fadeOutRampOffsetSamples == y.fadeOutRampOffsetSamples;
        }

        // Two adjacent clips can be healed (rejoined) only if they're a clean
        // split: same source file, butt-jointed in BOTH the file and the
        // timeline, neither locked, and cutting the joined clip at the join
        // gives both back fades included -- a hard cut on both sides, or one
        // fade's ramp running across the join. (A moved / slip-trimmed clip,
        // or a fade added at the seam, fails these.)
        bool clipsAreHealable (const Clip& a, const Clip& b)
        {
            if (a.locked || b.locked
                || a.audioFile != b.audioFile
                || a.sourceChannel != b.sourceChannel || a.gainDb != b.gainDb
                || a.muted != b.muted || a.fadeCurve != b.fadeCurve
                || a.fileStartSamples + a.fileLengthSamples != b.fileStartSamples
                || a.timelineStartSamples + a.fileLengthSamples != b.timelineStartSamples)
                return false;
            const auto m = joinedClip (a, b);
            return sameFades (clipSlice (m, 0, a.fileLengthSamples), a)
                && sameFades (clipSlice (m, a.fileLengthSamples, b.fileLengthSamples), b);
        }

        void mergeClipPair (std::vector<Clip>& list, int i)
        {
            list[(size_t) i] = joinedClip (list[(size_t) i], list[(size_t) (i + 1)]);
            list.erase (list.begin() + i + 1);
        }
    }

    bool AudioEngine::healSeparationAt (int track, juce::int64 timelineSample)
    {
        if (isArrangementFrozen()) return false;
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = clipsFor (track);
        int    best = -1;
        // "At the playhead" means near it: reaching for the nearest split at
        // any distance healed a different far-away split on each track.
        constexpr double kHealReachSeconds = 0.25;
        const double rate = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
        juce::int64 bestDist = (juce::int64) (rate * kHealReachSeconds) + 1;
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
        if (isArrangementFrozen()) return 0;
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

    bool AudioEngine::consolidateRange (int track, juce::int64 start, juce::int64 end)
    {
        if (end <= start) return false;
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return false;

        if (const auto* clips = tryClipsFor (track))
            for (const auto& clip : *clips)
                if (clip.locked && clip.timelineStartSamples < end
                    && clip.timelineStartSamples + clip.fileLengthSamples > start) return false;

        const juce::int64 arrLen = juce::jmax (end, getArrangementLengthSamples());
        const juce::int64 a = juce::jmin (start, arrLen);
        const juce::int64 b = juce::jmin (end,   arrLen);
        if (b <= a) return false;

        auto sessionDir = getActiveSessionDir();
        const auto audioDir = sessionDir.getChildFile ("Audio Files");
        audioDir.createDirectory();
        juce::File dst;
        for (int k = 1; ; ++k)
        {
            dst = audioDir.getChildFile (juce::String::formatted ("Track_%02d_consolidated_%d.wav", track + 1, k));
            if (! dst.existsAsFile()) break;
        }

        const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
        {
            const auto closeFailed = CheckedFileStream::makeFailedFlag();
            auto w = createWav24Writer (dst, sr, 1, closeFailed);
            if (w == nullptr) return false;
            // Stream the edited arrangement for just [a, b) straight to the
            // writer -- no whole-track buffer.
            const bool ok = forEachArrangementWindow (track, a, b,
                [&] (const float* data, juce::int64, int winLen)
                {
                    const float* chans[1] = { data };
                    return w->writeFromFloatArrays (chans, 1, winLen);
                });
            w.reset();
            if (! ok || closeFailed->load()) { dst.deleteFile(); return false; }
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

    void AudioEngine::consolidateRangeAsync (std::vector<int> tracks,
                                             juce::int64 start, juce::int64 end,
                                             std::function<void (int, bool)> completion)
    {
        if (isRecording() || end <= start)
        {
            if (completion) completion (0, false);
            return;
        }

        for (int track : tracks)
            if (const auto* clips = tryClipsFor (track))
                for (const auto& clip : *clips)
                    if (clip.locked && clip.timelineStartSamples < end
                        && clip.timelineStartSamples + clip.fileLengthSamples > start)
                    { if (completion) completion (0, false); return; }

        std::sort (tracks.begin(), tracks.end());
        tracks.erase (std::unique (tracks.begin(), tracks.end()), tracks.end());

        struct Job
        {
            int track { -1 };
            std::vector<Clip> original;
            juce::File temporary;
            bool rendered { false };
        };

        const auto sessionDir = getActiveSessionDir();
        if (! sessionDir.isDirectory())
        {
            if (completion) completion (0, false);
            return;
        }
        auto audioDir = sessionDir.getChildFile ("Audio Files");
        if (! audioDir.isDirectory() && ! audioDir.createDirectory().wasOk())
        {
            if (completion) completion (0, false);
            return;
        }
        const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());

        std::vector<Job> jobs;
        for (const int track : tracks)
        {
            if (track < 0 || track >= maxTracks || ! ensureClipList (track)) continue;
            Job job;
            job.track = track;
            job.original = clipsFor (track);
            job.temporary = audioDir.getChildFile ("." + juce::Uuid().toString()
                                                   + ".consolidating.wav");
            jobs.push_back (std::move (job));
        }
        if (jobs.empty())
        {
            if (completion) completion (0, false);
            return;
        }

        const auto handle = asyncHandle;
        const auto layout = recorder.getTrackGeneration();
        juce::Thread::launch (
            [handle, sessionDir, audioDir, sr, start, end, layout,
             jobs = std::move (jobs), completion = std::move (completion)] () mutable
        {
            bool allRendered = true;
            for (auto& job : jobs)
            {
                ArrangementSource src;
                if (! src.open (audioDir, job.track, &job.original))
                {
                    allRendered = false;
                    continue;
                }
                const auto closeFailed = CheckedFileStream::makeFailedFlag();
                auto writer = createWav24Writer (job.temporary, sr, 1, closeFailed);
                if (writer == nullptr)
                {
                    allRendered = false;
                    continue;
                }

                juce::AudioBuffer<float> window (1, kRenderWindowSamples), tmp (1, 0);
                job.rendered = true;
                for (juce::int64 winStart = start; winStart < end; winStart += kRenderWindowSamples)
                {
                    const int winLen = (int) juce::jmin<juce::int64> (
                        kRenderWindowSamples, end - winStart);
                    window.clear (0, 0, winLen);
                    if (! src.mixWindow (window.getWritePointer (0), winStart, winLen, tmp))
                    { job.rendered = false; break; }
                    const float* channels[1] = { window.getReadPointer (0) };
                    if (! writer->writeFromFloatArrays (channels, 1, winLen))
                    {
                        job.rendered = false;
                        break;
                    }
                }
                writer.reset();
                job.rendered = job.rendered && ! closeFailed->load();
                if (! job.rendered) job.temporary.deleteFile();
                allRendered = allRendered && job.rendered;
            }

            juce::MessageManager::callAsync (
                [handle, sessionDir, start, end, layout, jobs = std::move (jobs), allRendered,
                 completion = std::move (completion)] () mutable
            {
                auto* engine = handle->load (std::memory_order_acquire);
                if (engine == nullptr)
                {
                    for (auto& job : jobs) job.temporary.deleteFile();
                    return;
                }

                const bool current = engine->getActiveSessionDir() == sessionDir
                                  && ! engine->isArrangementFrozen()
                                  && engine->recorder.getTrackGeneration() == layout;
                int completed = 0;
                for (auto& job : jobs)
                {
                    if (! current || ! job.rendered)
                    {
                        job.temporary.deleteFile();
                        continue;
                    }
                    if (job.track < 0 || job.track >= (int) engine->trackClips.size()
                        || ! sameClipList (engine->trackClips[(size_t) job.track],
                                          job.original))
                    {
                        job.temporary.deleteFile();
                        continue;
                    }

                    const auto audioDirNow = sessionDir.getChildFile ("Audio Files");
                    juce::File destination;
                    for (int k = 1; ; ++k)
                    {
                        destination = audioDirNow.getChildFile (
                            juce::String::formatted ("Track_%02d_consolidated_%d.wav",
                                                     job.track + 1, k));
                        if (! destination.existsAsFile()) break;
                    }
                    if (! job.temporary.moveFileTo (destination))
                    {
                        job.temporary.deleteFile();
                        continue;
                    }

                    engine->clearTrackRange (job.track, start, end);
                    auto& list = engine->clipsFor (job.track);
                    Clip clip;
                    clip.name = destination.getFileNameWithoutExtension();
                    clip.audioFile = destination;
                    clip.timelineStartSamples = start;
                    clip.fileStartSamples = 0;
                    clip.fileLengthSamples = end - start;
                    auto where = std::lower_bound (list.begin(), list.end(), start,
                        [] (const Clip& c, juce::int64 pos)
                        { return c.timelineStartSamples < pos; });
                    list.insert (where, std::move (clip));
                    engine->player.setTrackClips (job.track, list);
                    engine->syncActiveTake (job.track);
                    ++completed;
                }
                if (completion)
                    completion (completed, current && allRendered
                                             && completed == (int) jobs.size());
            });
        });
    }

    bool AudioEngine::splitTrackAtSample (int track, juce::int64 timelineSample)
    {
        if (isArrangementFrozen()) return false;
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
        if (isArrangementFrozen()) return false;
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

    namespace
    {
        // Removes [start, end) from a clip list and closes the gap. Returns
        // false when no clip was touched.
        bool rippleClips (std::vector<Clip>& list, juce::int64 start, juce::int64 end)
        {
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
                if (cs >= start && ce <= end)   // inside the gap -> drop
                {
                    changed = true;
                    continue;
                }
                if (cs >= end)
                {
                    c.timelineStartSamples -= shift;
                    changed = true;
                }
                out.push_back (c);
            }
            if (changed) list = std::move (out);
            return changed;
        }
    }

    bool AudioEngine::rippleDeleteRange (int track, juce::int64 start, juce::int64 end)
    {
        return rippleDeleteTracks ({ track }, start, end) > 0;
    }

    int AudioEngine::rippleDeleteTracks (const std::vector<int>& tracks, juce::int64 start, juce::int64 end)
    {
        if (end <= start) return 0;
        const timelinecut::Cut cut { start, end, false };
        if (timelineCutRefusal (cut, tracks).isNotEmpty()) return -1;

        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        int changed = 0;
        for (const int track : tracks)
        {
            if (track < 0 || track >= maxTracks || ! ensureClipList (track)) continue;
            auto& list = clipsFor (track);
            if (! rippleClips (list, start, end)) continue;
            ++changed;
            player.setTrackClips (track, list);
            syncActiveTake (track);

            auto& playlist = trackPlaylists[(size_t) track];
            for (int k = 0; k < (int) playlist.takes.size(); ++k)
                if (k != playlist.activeTake)
                    rippleClips (playlist.takes[(size_t) k].clips, start, end);
        }
        if (changed == 0) return 0;

        // On some strips only, the rest of the session stays where it was, so
        // the markers stay with it.
        if (namesEveryStrip (tracks)) cutSessionTimeline (cut);
        return changed;
    }

    int AudioEngine::pasteClip (int track, juce::int64 timelineStart,
                                juce::int64 fileStart, juce::int64 fileLength,
                                juce::int64 fadeIn, juce::int64 fadeOut, float gainDb,
                                const juce::String& name, const juce::File& audioFile,
                                int sourceChannel, int fadeCurve)
    {
        if (isArrangementFrozen()) return -1;
        const int maxTracks = juce::jmax (recorder.getNumTracks(), player.getNumTracks());
        if (track < 0 || track >= maxTracks) return -1;
        if (fileLength <= 0) return -1;
        // A session reads only its own media: a file elsewhere has no place in
        // a Save As, a handoff or a DAW project, and may not be there tomorrow.
        if (audioFile != juce::File() && ! audioFile.isAChildOf (getActiveSessionDir())) return -1;
        ensureClipList (track);
        auto& list = clipsFor (track);

        Clip c;
        c.audioFile            = audioFile;   // empty = same-track (track reader)
        c.sourceChannel        = sourceChannel;
        c.fadeCurve            = fadeCurve;
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

    juce::int64 AudioEngine::constrainClipEditDelta (int track, int clipIndex, ClipEdit mode,
                                                    juce::int64 delta) const
    {
        const auto* clips = tryClipsFor (track);
        if (clips == nullptr || clipIndex < 0 || clipIndex >= (int) clips->size()) return 0;
        const auto& c = (*clips)[(size_t) clipIndex];
        if (c.locked) return 0;
        constexpr juce::int64 minLength = 1024;
        if (mode == ClipEdit::Move) return juce::jmax (delta, -c.timelineStartSamples);
        if (mode == ClipEdit::TrimLeft)
            return juce::jlimit (-juce::jmin (c.fileStartSamples, c.timelineStartSamples),
                                juce::jmax ((juce::int64) 0, c.fileLengthSamples - minLength), delta);
        if (delta <= 0) return juce::jmax (delta, juce::jmin ((juce::int64) 0, minLength - c.fileLengthSamples));
        juce::int64 sourceLength = 0;
        if (c.audioFile == juce::File()) sourceLength = player.getTrackLengthSamples (track);
        else
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            if (auto reader = ConcatReader::create (fm, findTakeParts (c.audioFile)))
                sourceLength = reader->lengthInSamples;
        }
        return juce::jmin (delta, juce::jmax ((juce::int64) 0,
                                           sourceLength - c.fileStartSamples - c.fileLengthSamples));
    }

    bool AudioEngine::editClip (int track, int clipIndex, ClipEdit mode, juce::int64 deltaSamples)
    {
        if (isArrangementFrozen()) return false;
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;   // edits refused on locked clips
        deltaSamples = constrainClipEditDelta (track, clipIndex, mode, deltaSamples);
        if (deltaSamples == 0) return false;

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
        // Each fade's ramp moves with its own edge. A ramp longer than the
        // fade it shows (a piece cut out of a longer fade) shows as much of
        // itself as now fits, so extending that piece brings the original
        // ramp back; an ordinary fade keeps its length.
        if (c.fadeInRampSamples > 0)
            c.fadeInSamples = juce::jmin (c.fadeInRampSamples - c.fadeInRampOffsetSamples,
                                          c.fileLengthSamples);
        if (c.fadeOutRampSamples > 0)
            c.fadeOutSamples = juce::jmin (c.fadeOutRampSamples - c.fadeOutRampOffsetSamples,
                                           c.fileLengthSamples);
        // A trim can leave the clip shorter than its fades. Shrink both in
        // proportion (as ordinary fades) so fade-in + fade-out fit again: a
        // fade longer than the clip never reaches full gain, and setClipFades
        // (in + out <= length) then refused every fade-handle drag on it.
        if (const auto fades = c.fadeInSamples + c.fadeOutSamples; fades > c.fileLengthSamples)
        {
            c.fadeInSamples  = (juce::int64) ((double) c.fadeInSamples * (double) c.fileLengthSamples
                                              / (double) fades);
            c.fadeOutSamples = c.fileLengthSamples - c.fadeInSamples;
            c.fadeInRampSamples = c.fadeInRampOffsetSamples = 0;
            c.fadeOutRampSamples = c.fadeOutRampOffsetSamples = 0;
        }
        normaliseFadeRamps (c);
        // Publish the updated list to the player so playback honours
        // the edit on the next audio block.
        player.setTrackClips (track, list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipFades (int track, int clipIndex,
                                    juce::int64 fadeInSamples, juce::int64 fadeOutSamples)
    {
        if (isArrangementFrozen()) return false;
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;

        fadeInSamples  = juce::jmax<juce::int64> (0, fadeInSamples);
        fadeOutSamples = juce::jmax<juce::int64> (0, fadeOutSamples);
        if (fadeInSamples + fadeOutSamples > c.fileLengthSamples) return false;

        // A fade the engineer changes becomes an ordinary fade of that length;
        // the one left alone keeps its ramp (a piece of a split fade).
        if (fadeInSamples != c.fadeInSamples)   c.fadeInRampSamples  = c.fadeInRampOffsetSamples  = 0;
        if (fadeOutSamples != c.fadeOutSamples) c.fadeOutRampSamples = c.fadeOutRampOffsetSamples = 0;
        c.fadeInSamples  = fadeInSamples;
        c.fadeOutSamples = fadeOutSamples;
        player.setTrackClips (track, list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipFadeRamps (int track, int clipIndex,
                                        juce::int64 inRamp, juce::int64 inRampOffset,
                                        juce::int64 outRamp, juce::int64 outRampOffset)
    {
        if (track < 0 || track >= (int) trackClips.size()) return false;
        auto& list = trackClips[(size_t) track];
        if (clipIndex < 0 || clipIndex >= (int) list.size()) return false;
        auto& c = list[(size_t) clipIndex];
        if (c.locked) return false;
        c.fadeInRampSamples        = inRamp;
        c.fadeInRampOffsetSamples  = inRampOffset;
        c.fadeOutRampSamples       = outRamp;
        c.fadeOutRampOffsetSamples = outRampOffset;
        normaliseFadeRamps (c);
        player.setTrackClips (track, list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipFadeCurve (int track, int clipIndex, int curve)
    {
        if (isArrangementFrozen()) return false;
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
        if (isArrangementFrozen()) return false;
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].muted = muted;
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipLocked (int track, int clipIndex, bool locked)
    {
        if (isArrangementFrozen()) return false;
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].locked = locked;
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipName (int track, int clipIndex, const juce::String& name)
    {
        if (isArrangementFrozen()) return false;
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].name = name;
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

    bool AudioEngine::setClipGainDb (int track, int clipIndex, float dB)
    {
        if (isArrangementFrozen()) return false;
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        (*list)[(size_t) clipIndex].gainDb = juce::jlimit (-60.0f, 12.0f, dB);
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
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

        int channel = juce::jmax (0, clip->sourceChannel);
        auto file = clip->audioFile;
        if (file == juce::File()) file = getTrackAudioFile (track, &channel);
        if (! file.existsAsFile()) return timelineSample;

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto reader = ConcatReader::create (fm, findTakeParts (file));
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
        channel = juce::jlimit (0, buf.getNumChannels() - 1, channel);
        auto mono = [&] (int i) { return buf.getSample (channel, i); };
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
        if (isArrangementFrozen()) return false;
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
        if (isArrangementFrozen()) return false;
        auto* list = validClipList (trackClips, track, clipIndex);
        if (list == nullptr) return false;
        if ((*list)[(size_t) clipIndex].locked) return false;   // lock protects against duplicate
        Clip copy = (*list)[(size_t) clipIndex];
        copy.timelineStartSamples += copy.fileLengthSamples;   // place after the source
        copy.name = copy.name + " (copy)";
        copy.locked = false;
        copy.selectionId = 0;
        list->insert (list->begin() + clipIndex + 1, std::move (copy));
        player.setTrackClips (track, *list);
        syncActiveTake (track);
        return true;
    }

}
