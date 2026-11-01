#pragma once

#include <atomic>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <vector>
#include "ClipModel.h"
#include "MultiPartReader.h"

namespace zynforge::songs
{
    // Post-show song-boundary detection. Live recordings can't use naive
    // whole-mix silence detection -- ambient / vocal mics stay hot with
    // crowd noise between songs. Instead each track gets its own 100 ms
    // peak envelope, and a window counts as "music" only when at least
    // `quorumTracks` tracks are simultaneously above the threshold: the
    // band playing lights up many channels at once, applause on a couple
    // of ambient mics doesn't.
    //
    // Engine-free + streamed (per-track envelopes are bit flags, never
    // audio), so the test harness drives it headlessly.

    struct Params
    {
        float  thresholdDb   { -40.0f };  // per-track "this channel is active"
        double minGapSec     { 4.0 };     // quiet span that separates songs
        double minSongSec    { 60.0 };    // runs shorter than this aren't songs
        int    quorumTracks  { 2 };       // tracks that must be active at once
        double windowSec     { 0.100 };
    };

    struct Song
    {
        juce::int64 startSample { 0 };
        juce::int64 endSample   { 0 };
    };

    // One voice of the quorum: a strip's own take file and what plays of it.
    // A clip with a file of its own reads that file instead.
    struct Voice
    {
        juce::File ownFile;
        std::vector<Clip> clips;
    };

    // Per-file peak-envelope flags: flags[w] = 1 when window w's peak is
    // above threshold. Streams the file; never holds more than one block.
    inline std::vector<char> envelopeFlags (const juce::File& f, float thresholdDb,
                                            double windowSec, double& srOut,
                                            const std::atomic<bool>* cancel = nullptr)
    {
        std::vector<char> flags;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        auto reader = zynforge::ConcatReader::create (fm, zynforge::findTakeParts (f));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            return flags;

        srOut = reader->sampleRate;
        const float thresh = juce::Decibels::decibelsToGain (thresholdDb, -120.0f);
        const int win = juce::jmax (64, (int) (reader->sampleRate * windowSec));
        juce::AudioBuffer<float> buf (1, win);
        flags.reserve ((size_t) (reader->lengthInSamples / win + 1));
        for (juce::int64 p = 0; p < reader->lengthInSamples; p += win)
        {
            if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) return {};
            const int n = (int) juce::jmin ((juce::int64) win, reader->lengthInSamples - p);
            buf.clear();
            if (! reader->read (&buf, 0, n, p, true, false)) break;
            flags.push_back (buf.getMagnitude (0, 0, n) >= thresh ? 1 : 0);
        }
        return flags;
    }

    // Moves a file's envelope to where its clips play it: the timeline the
    // markers land on. Audio cut from the arrangement or muted makes no sound
    // and counts as nothing here.
    inline std::vector<char> timelineFlags (const std::vector<Clip>& clips, int win,
                                            const std::function<const std::vector<char>* (const Clip&)>& flagsFor)
    {
        std::vector<char> timeline;
        for (const auto& clip : clips)
        {
            if (clip.muted || clip.fileLengthSamples <= 0) continue;
            const auto* flags = flagsFor (clip);
            if (flags == nullptr) continue;
            const auto firstWindow = clip.fileStartSamples / win;
            const auto lastWindow  = (clip.fileStartSamples + clip.fileLengthSamples - 1) / win;
            for (auto w = firstWindow; w <= lastWindow && w < (juce::int64) flags->size(); ++w)
            {
                if (! (*flags)[(size_t) w]) continue;
                const auto at = (size_t) ((clip.timelineStartSamples + w * win - clip.fileStartSamples
                                           + (juce::int64) win / 2) / win);
                if (at >= timeline.size()) timeline.resize (at + 1, 0);
                timeline[at] = 1;
            }
        }
        return timeline;
    }

    // Every voice's envelope on the timeline. Each file is read once however
    // many clips use it. Empty when cancelled.
    inline std::vector<std::vector<char>> scanVoices (const std::vector<Voice>& voices, const Params& params,
                                                      double& srOut, const std::atomic<bool>* cancel = nullptr)
    {
        std::vector<std::vector<char>> tracks;
        std::map<juce::String, std::vector<char>> perFile;
        for (const auto& voice : voices)
        {
            const auto flagsFor = [&] (const Clip& clip) -> const std::vector<char>*
            {
                const auto file = clip.audioFile != juce::File() ? clip.audioFile : voice.ownFile;
                const auto key  = file.getFullPathName();
                auto known = perFile.find (key);
                if (known == perFile.end())
                {
                    double fileRate = 0.0;
                    known = perFile.emplace (key, envelopeFlags (file, params.thresholdDb,
                                                                 params.windowSec, fileRate, cancel)).first;
                    if (fileRate > 0.0) srOut = fileRate;
                }
                return &known->second;
            };
            // A first read may set the rate the windows are counted in.
            for (const auto& clip : voice.clips) flagsFor (clip);
            if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) return {};
            const int win = juce::jmax (64, (int) (srOut * params.windowSec));
            auto flags = timelineFlags (voice.clips, win, flagsFor);
            if (! flags.empty()) tracks.push_back (std::move (flags));
        }
        return tracks;
    }

    // Combine per-track envelopes -> song spans. `sr` is the session
    // sample rate (window positions convert back through it).
    inline std::vector<Song> detect (const std::vector<std::vector<char>>& trackFlags,
                                     double sr, const Params& p = {})
    {
        std::vector<Song> songs;
        if (sr <= 0.0 || trackFlags.empty()) return songs;

        size_t numWindows = 0;
        for (const auto& f : trackFlags) numWindows = std::max (numWindows, f.size());
        if (numWindows == 0) return songs;

        const int win = juce::jmax (64, (int) (sr * p.windowSec));
        const int quorum = juce::jmax (1, p.quorumTracks);

        // music[w] = enough tracks active at once.
        std::vector<char> music (numWindows, 0);
        for (size_t w = 0; w < numWindows; ++w)
        {
            int active = 0;
            for (const auto& f : trackFlags)
                if (w < f.size() && f[w]) ++active;
            music[w] = active >= quorum ? 1 : 0;
        }

        // Runs of music, breaking only on gaps >= minGapSec; drop runs
        // shorter than minSongSec.
        const auto minGapWin  = juce::jmax ((juce::int64) 1, (juce::int64) (p.minGapSec  * sr) / win);
        const auto minSongWin = juce::jmax ((juce::int64) 1, (juce::int64) (p.minSongSec * sr) / win);

        size_t i = 0; const size_t nw = numWindows;
        while (i < nw)
        {
            while (i < nw && ! music[i]) ++i;
            if (i >= nw) break;
            const size_t runStart = i;
            size_t runEnd = i;
            while (i < nw)
            {
                if (music[i]) { runEnd = ++i; continue; }
                size_t sil = i; while (sil < nw && ! music[sil]) ++sil;
                if ((juce::int64) (sil - i) >= minGapWin) break;      // real gap -> song over
                i = sil;                                              // short dip -> same song
            }
            if ((juce::int64) (runEnd - runStart) >= minSongWin)
                songs.push_back ({ (juce::int64) runStart * win, (juce::int64) runEnd * win });
        }
        return songs;
    }
}
