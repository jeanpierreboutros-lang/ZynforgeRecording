#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <vector>

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

    // Per-file peak-envelope flags: flags[w] = 1 when window w's peak is
    // above threshold. Streams the file; never holds more than one block.
    inline std::vector<char> envelopeFlags (const juce::File& f, float thresholdDb,
                                            double windowSec, double& srOut)
    {
        std::vector<char> flags;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            return flags;

        srOut = reader->sampleRate;
        const float thresh = juce::Decibels::decibelsToGain (thresholdDb, -120.0f);
        const int win = juce::jmax (64, (int) (reader->sampleRate * windowSec));
        juce::AudioBuffer<float> buf (1, win);
        flags.reserve ((size_t) (reader->lengthInSamples / win + 1));
        for (juce::int64 p = 0; p < reader->lengthInSamples; p += win)
        {
            const int n = (int) juce::jmin ((juce::int64) win, reader->lengthInSamples - p);
            buf.clear();
            if (! reader->read (&buf, 0, n, p, true, false)) break;
            flags.push_back (buf.getMagnitude (0, 0, n) >= thresh ? 1 : 0);
        }
        return flags;
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
