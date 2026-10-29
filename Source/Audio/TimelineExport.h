#pragma once

#include <juce_core/juce_core.h>
#include "MultiPartReader.h"
#include <functional>
#include <vector>

namespace zynforge::timelineexport
{
    // Portable session-timeline sidecar (CSV/EDL-style). Hands an editor the
    // track→file map plus timecode-stamped markers + cues so the session can
    // be rebuilt in another DAW (Pro Tools / Nuendo / Reaper). Not AAF -- a
    // bounded, universally-ingestable first step. Pure + headless-testable:
    // buildCsv takes plain data and returns the file text.

    // One [Tracks] row per media file a strip reads. channel1 is the channel
    // inside `file` (1 = left, 2 = right of an interleaved stereo take; 0 = a
    // mono file, all of it). part is 1 for the main file, 2.. for its
    // Track_NN_partXX continuations in playback order. An empty `file` is a
    // strip with no recorded media.
    struct TrackEntry { int index1 { 0 }; juce::String name, file; int channel1 { 0 }; int part { 1 }; };

    // Resolves strip `track` (0-based) to the take file it reads and which
    // channel of it (AudioEngine::getTrackAudioFile).
    using MediaResolver = std::function<juce::File (int track, int& channel)>;

    // The rows for every strip. The right strip of a native-stereo pair has no
    // Track_NN file of its own -- its audio is channel 2 of the left strip's
    // file -- so naming its own stem pointed the editor at a file that doesn't
    // exist, and listing only the main file dropped every continuation part.
    inline std::vector<TrackEntry> trackEntries (const juce::StringArray& names, const MediaResolver& resolve)
    {
        std::vector<juce::File> files;
        std::vector<int> channels;
        for (int i = 0; i < names.size(); ++i)
        {
            int channel = 0;
            files.push_back (resolve (i, channel));
            channels.push_back (channel);
        }

        std::vector<TrackEntry> out;
        for (int i = 0; i < names.size(); ++i)
        {
            const auto& file = files[(size_t) i];
            if (! file.existsAsFile()) { out.push_back ({ i + 1, names[i], {}, 0, 1 }); continue; }
            // A strip reading channel 0 is the LEFT of a stereo file when the
            // next strip reads channel 1 of the same file; otherwise mono.
            const bool left = channels[(size_t) i] == 0 && i + 1 < names.size()
                           && files[(size_t) i + 1] == file && channels[(size_t) i + 1] == 1;
            const int channel1 = channels[(size_t) i] == 1 ? 2 : (left ? 1 : 0);
            int part = 1;
            for (const auto& f : findTakeParts (file))
                out.push_back ({ i + 1, names[i], f.getFileName(), channel1, part++ });
        }
        return out;
    }

    // Timecode frame rate for the export: the house rate learned from LTC/MTC
    // chase (nominal integer rate, so 29.97 -> 30), else 30.
    inline int frameRateFor (float learnedFps)
    {
        return learnedFps > 0.0f ? juce::roundToInt (learnedFps) : 30;
    }

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

    // RFC-4180-ish field: wrap in quotes, double any internal quote. Also
    // neutralise spreadsheet formulas. Track/cue names may originate on a
    // networked console; opening an export in Excel/Numbers must not execute a
    // value beginning with =, +, -, @, tab, CR or LF as a formula/directive.
    inline juce::String csvField (const juce::String& s)
    {
        auto safe = s;
        if (safe.isNotEmpty()
            && (safe[0] == '=' || safe[0] == '+' || safe[0] == '-'
                || safe[0] == '@' || safe[0] == '\t'
                || safe[0] == '\r' || safe[0] == '\n'))
            safe = "'" + safe;
        return "\"" + safe.replace ("\"", "\"\"") + "\"";
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

        out << "[Tracks]\r\nIndex,Name,File,Channel,Part\r\n";
        for (const auto& t : tracks)
            out << t.index1 << "," << csvField (t.name) << "," << csvField (t.file) << ","
                << (t.channel1 > 0 ? juce::String (t.channel1) : juce::String()) << ","
                << t.part << "\r\n";

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
