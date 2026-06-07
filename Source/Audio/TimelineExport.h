#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace zynforge::timelineexport
{
    // Portable session-timeline sidecar (CSV/EDL-style). Hands an editor the
    // track→file map plus timecode-stamped markers + cues so the session can
    // be rebuilt in another DAW (Pro Tools / Nuendo / Reaper). Not AAF -- a
    // bounded, universally-ingestable first step. Pure + headless-testable:
    // buildCsv takes plain data and returns the file text.

    struct TrackEntry { int index1 { 0 }; juce::String name, file; };
    struct MarkEntry  { juce::int64 sample { 0 }; juce::String name, type; };

    // SMPTE HH:MM:SS:FF for a sample position at the given audio rate + fps.
    inline juce::String samplesToTimecode (juce::int64 samples, double sampleRate, int fps)
    {
        if (sampleRate <= 0.0 || fps <= 0) return "00:00:00:00";
        const juce::int64 totalFrames = (juce::int64) llround ((double) samples / sampleRate * (double) fps);
        const int ff = (int) (totalFrames % fps);
        const juce::int64 totalSec = totalFrames / fps;
        const int ss = (int) (totalSec % 60);
        const int mm = (int) ((totalSec / 60) % 60);
        const int hh = (int) (totalSec / 3600);
        return juce::String::formatted ("%02d:%02d:%02d:%02d", hh, mm, ss, ff);
    }

    // RFC-4180-ish field: wrap in quotes, double any internal quote.
    inline juce::String csvField (const juce::String& s)
    {
        return "\"" + s.replace ("\"", "\"\"") + "\"";
    }

    inline juce::String buildCsv (const juce::String& sessionName,
                                  double sampleRate, int fps,
                                  const std::vector<TrackEntry>& tracks,
                                  const std::vector<MarkEntry>&  markers,
                                  const std::vector<MarkEntry>&  cues)
    {
        juce::String out;
        out << "# Zynforge Recording -- session timeline export\r\n";
        out << "Session," << csvField (sessionName) << "\r\n";
        out << "SampleRate," << juce::String (sampleRate, 0) << "\r\n";
        out << "FrameRate," << fps << "\r\n\r\n";

        out << "[Tracks]\r\nIndex,Name,File\r\n";
        for (const auto& t : tracks)
            out << t.index1 << "," << csvField (t.name) << "," << csvField (t.file) << "\r\n";

        out << "\r\n[Markers]\r\nTimecode,Samples,Type,Name\r\n";
        for (const auto& m : markers)
            out << samplesToTimecode (m.sample, sampleRate, fps) << ","
                << m.sample << "," << csvField (m.type) << "," << csvField (m.name) << "\r\n";

        out << "\r\n[Cues]\r\nTimecode,Samples,Name\r\n";
        for (const auto& c : cues)
            out << samplesToTimecode (c.sample, sampleRate, fps) << ","
                << c.sample << "," << csvField (c.name) << "\r\n";

        return out;
    }
}
