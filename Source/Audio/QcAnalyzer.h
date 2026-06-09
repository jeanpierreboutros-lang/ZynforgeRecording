#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "LoudnessMeter.h"

#include <algorithm>
#include <vector>

namespace zynforge::qc
{
    // Post-show QC scan: for every recorded track -- peak, BS.1770
    // integrated loudness, clipping events with their timecode, and the
    // between-song noise floor. Streams the file in blocks (never loads
    // a track into RAM), engine-free, headless-testable.

    struct ClipEvent
    {
        juce::int64 startSample { 0 };
        int         lengthSamples { 0 };
    };

    struct TrackQc
    {
        juce::String name;                  // engineer-facing strip name
        juce::File   file;
        int          trackIndex { 0 };      // 0-based
        double       sampleRate { 0.0 };
        juce::int64  lengthSamples { 0 };
        float        peakDb         { -120.0f };
        float        integratedLufs { -120.0f };
        float        noiseFloorDb   { -120.0f };  // 10th percentile of 100 ms RMS windows
        int          clipEventCount { 0 };
        std::vector<ClipEvent> clipEvents;  // first kMaxStoredClipEvents only
    };

    // A clip = a run of >= kClipRunSamples consecutive samples at (or
    // beyond) kClipThreshold of full scale -- the classic flat-top test.
    // One run = one event regardless of length.
    constexpr float kClipThreshold        = 0.999f;
    constexpr int   kClipRunSamples       = 3;
    constexpr int   kMaxStoredClipEvents  = 200;

    inline TrackQc analyzeFile (const juce::File& f)
    {
        TrackQc qc;
        qc.file = f;
        if (! f.existsAsFile()) return qc;

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            return qc;

        qc.sampleRate    = reader->sampleRate;
        qc.lengthSamples = reader->lengthInSamples;

        zynforge::LoudnessMeter loudness;
        loudness.prepare (reader->sampleRate);

        const int block = 1 << 16;
        juce::AudioBuffer<float> buf ((int) reader->numChannels, block);
        std::vector<double> dL ((size_t) block), dR ((size_t) block, 0.0);  // R stays 0: single-channel BS.1770

        // Noise floor: RMS per 100 ms window, then the 10th percentile.
        const int winLen = juce::jmax (1, (int) (reader->sampleRate * 0.100));
        std::vector<float> windowsDb;
        windowsDb.reserve ((size_t) (qc.lengthSamples / winLen + 1));
        double winAcc = 0.0; int winFill = 0;

        float peak = 0.0f;
        int   clipRun = 0; juce::int64 clipRunStart = 0;

        auto endClipRun = [&qc, &clipRun, &clipRunStart]()
        {
            if (clipRun >= kClipRunSamples)
            {
                ++qc.clipEventCount;
                if ((int) qc.clipEvents.size() < kMaxStoredClipEvents)
                    qc.clipEvents.push_back ({ clipRunStart, clipRun });
            }
            clipRun = 0;
        };

        for (juce::int64 pos = 0; pos < qc.lengthSamples; pos += block)
        {
            const int n = (int) juce::jmin ((juce::int64) block, qc.lengthSamples - pos);
            if (! reader->read (&buf, 0, n, pos, true, false)) break;
            const float* s = buf.getReadPointer (0);     // track files are mono; ch 0 for safety

            for (int i = 0; i < n; ++i)
            {
                const float v  = s[i];
                const float av = std::abs (v);
                peak = juce::jmax (peak, av);

                if (av >= kClipThreshold)
                {
                    if (clipRun == 0) clipRunStart = pos + i;
                    ++clipRun;
                }
                else
                    endClipRun();

                winAcc += (double) v * (double) v;
                if (++winFill == winLen)
                {
                    windowsDb.push_back (juce::Decibels::gainToDecibels (
                        (float) std::sqrt (winAcc / (double) winLen), -120.0f));
                    winAcc = 0.0; winFill = 0;
                }

                dL[(size_t) i] = (double) v;
            }
            loudness.processMasterDouble (dL.data(), dR.data(), 1.0, n);
        }
        endClipRun();

        qc.peakDb         = juce::Decibels::gainToDecibels (peak, -120.0f);
        qc.integratedLufs = loudness.getIntegratedLufs();
        if (! windowsDb.empty())
        {
            auto sorted = windowsDb;
            std::sort (sorted.begin(), sorted.end());
            qc.noiseFloorDb = sorted[(size_t) ((double) (sorted.size() - 1) * 0.10)];
        }
        return qc;
    }

    inline juce::String timecode (juce::int64 sample, double sr)
    {
        if (sr <= 0.0) return "--";
        const auto totalMs = (juce::int64) (1000.0 * (double) sample / sr);
        const int h  = (int) (totalMs / 3600000);
        const int m  = (int) ((totalMs / 60000) % 60);
        const int s  = (int) ((totalMs / 1000) % 60);
        const int ms = (int) (totalMs % 1000);
        return juce::String::formatted ("%02d:%02d:%02d.%03d", h, m, s, ms);
    }

    // Plain-text report -- what gets written to Export Files/ so the
    // engineer can read it the morning after (or hand it to the band).
    inline juce::String reportText (const juce::String& sessionName,
                                    const std::vector<TrackQc>& tracks,
                                    juce::int64 missedSamples = -1)
    {
        juce::String t;
        t << "ZynForge Recording -- Post-Show QC Report\n"
          << "Session: " << sessionName << "\n"
          << "Generated: " << juce::Time::getCurrentTime().toString (true, true) << "\n";
        if (missedSamples >= 0)
            t << "Recorder missed samples: " << juce::String (missedSamples)
              << (missedSamples == 0 ? "  [OK]" : "  [!!] DROPOUTS") << "\n";
        t << "\n";

        for (const auto& q : tracks)
        {
            t << juce::String::formatted ("Track %02d  ", q.trackIndex + 1) << q.name << "\n"
              << "  Peak: "        << juce::String (q.peakDb, 1)         << " dBFS"
              << "   LUFS-I: "     << juce::String (q.integratedLufs, 1)
              << "   Noise floor: "<< juce::String (q.noiseFloorDb, 1)   << " dBFS"
              << "   Length: "     << timecode (q.lengthSamples, q.sampleRate) << "\n";
            if (q.clipEventCount > 0)
            {
                t << "  CLIPPING: " << q.clipEventCount << " event"
                  << (q.clipEventCount == 1 ? "" : "s");
                const int show = juce::jmin (10, (int) q.clipEvents.size());
                for (int i = 0; i < show; ++i)
                    t << (i == 0 ? " at " : ", ") << timecode (q.clipEvents[(size_t) i].startSample, q.sampleRate);
                if (q.clipEventCount > show) t << ", ...";
                t << "\n";
            }
            t << "\n";
        }
        if (tracks.empty()) t << "(no recorded tracks found)\n";
        return t;
    }
}
