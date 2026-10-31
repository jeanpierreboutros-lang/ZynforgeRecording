#include "SongExport.h"
#include "AtomicFile.h"
#include "AudioEngine.h"

#include <algorithm>

namespace zynforge::songexport
{
    std::vector<Range> rangesFromMarkers (std::vector<Marker> markers, juce::int64 sessionEnd)
    {
        std::stable_sort (markers.begin(), markers.end(), [] (const Marker& a, const Marker& b)
                          { return a.sampleOffset < b.sampleOffset; });

        std::vector<Range> ranges;
        for (const auto& marker : markers)
        {
            const auto start = juce::jmax<juce::int64> (0, marker.sampleOffset);
            if (start >= sessionEnd) break;
            if (! ranges.empty() && ranges.back().start == start)
            {
                if (ranges.back().name.isEmpty()) ranges.back().name = marker.name.trim();
                continue;
            }
            if (! ranges.empty()) ranges.back().end = start;
            ranges.push_back ({ start, sessionEnd, marker.name.trim() });
        }
        for (size_t i = 0; i < ranges.size(); ++i)
            if (ranges[i].name.isEmpty())
                ranges[i].name = "Song " + juce::String ((int) i + 1);
        return ranges;
    }

    juce::String safeName (const juce::String& name)
    {
        const auto legal = juce::File::createLegalFileName (name.replaceCharacter ('.', '_'));
        return legal.trim().isNotEmpty() ? legal.trim() : juce::String ("Untitled");
    }

    Plan buildPlan (AudioEngine& engine, const Choice& choice, const juce::File& exportFilesDir)
    {
        Plan plan;
        plan.format = choice.format;
        const double rate = engine.getPlayer().getSampleRate();
        plan.renderRate = rate > 0.0 ? rate
                        : (engine.getSessionSampleRate() > 0.0 ? engine.getSessionSampleRate()
                                                               : engine.getDeviceSampleRate());

        const auto audibleIn = [&engine] (int track, const Range& song)
        {
            if (const auto* clips = engine.tryClipsFor (track))
                for (const auto& clip : *clips)
                    if (! clip.muted && clip.timelineStartSamples < song.end
                        && clip.timelineStartSamples + clip.fileLengthSamples > song.start)
                        return true;
            return false;
        };

        const auto songsDir = exportFilesDir.getChildFile ("Songs");
        auto& recorder = engine.getRecorder();
        const juce::ScopedLock structure (recorder.getStructureLock());
        const int numTracks = recorder.getNumTracks();

        for (size_t i = 0; i < choice.songs.size(); ++i)
        {
            const auto& song = choice.songs[i];
            const auto title = juce::String::formatted ("%02d - ", choice.numbers[i]) + safeName (song.name);

            SongPlan songPlan;
            songPlan.start  = song.start;
            songPlan.end    = song.end;
            songPlan.folder = atomicfile::firstFreeName (songsDir, title);

            if (choice.mix)
                songPlan.targets.push_back ({ Target::Kind::Mix, -1,
                                              songPlan.folder.getChildFile (title + " - Mix") });
            if (choice.stems)
                for (int t = 0; t < numTracks; ++t)
                {
                    const auto& strip = recorder.getTrack (t);
                    if (t > 0 && recorder.getTrack (t - 1).isStereo.load (std::memory_order_relaxed))
                        continue;   // a pair's right half goes out with its left
                    if (strip.isBus.load (std::memory_order_relaxed)) continue;

                    const auto base = juce::String::formatted ("Track_%02d - ", t + 1)
                                    + safeName (strip.getNameThreadSafe());
                    const bool pair = strip.isStereo.load (std::memory_order_relaxed) && t + 1 < numTracks;
                    if (! pair)
                    {
                        if (audibleIn (t, song))
                            songPlan.targets.push_back ({ Target::Kind::MonoStem, t, songPlan.folder.getChildFile (base) });
                        continue;
                    }
                    if (! audibleIn (t, song) && ! audibleIn (t + 1, song)) continue;
                    if (choice.stereoOneFile)
                    {
                        songPlan.targets.push_back ({ Target::Kind::StereoStem, t, songPlan.folder.getChildFile (base) });
                        continue;
                    }
                    songPlan.targets.push_back ({ Target::Kind::MonoStem, t,     songPlan.folder.getChildFile (base + " L") });
                    songPlan.targets.push_back ({ Target::Kind::MonoStem, t + 1, songPlan.folder.getChildFile (base + " R") });
                }
            plan.songs.push_back (std::move (songPlan));
        }
        return plan;
    }

    namespace
    {
        bool renderTarget (AudioEngine& engine, const Target& target, const SongPlan& song,
                           ExportFileStream& out, const std::atomic<bool>& cancel)
        {
            const auto sink = [&] (const juce::AudioBuffer<float>& block, int numSamples)
            {
                return ! cancel.load (std::memory_order_relaxed) && out.write (block, numSamples);
            };
            switch (target.kind)
            {
                case Target::Kind::Mix:
                    return engine.forEachStereoMixWindow (song.start, song.end,
                        [&] (const juce::AudioBuffer<float>& w, juce::int64, int n) { return sink (w, n); });
                case Target::Kind::StereoStem:
                    return engine.forEachStereoPairWindow (target.track, song.start, song.end,
                        [&] (const juce::AudioBuffer<float>& w, juce::int64, int n) { return sink (w, n); });
                case Target::Kind::MonoStem:
                    break;
            }
            return engine.forEachArrangementWindow (target.track, song.start, song.end,
                [&] (const float* data, juce::int64, int n)
                {
                    float* channels[1] = { const_cast<float*> (data) };
                    return sink (juce::AudioBuffer<float> (channels, 1, n), n);
                });
        }
    }

    Outcome run (AudioEngine& engine, const Plan& plan, const std::atomic<bool>& cancel, const Progress& progress)
    {
        Outcome outcome;
        const int total = plan.totalFiles();
        if (const auto refusal = engine.exportRefusal(); refusal.isNotEmpty())
        {
            outcome.attempted  = total;
            outcome.firstError = refusal;
            return outcome;
        }

        TrackExporter exporter;
        for (const auto& song : plan.songs)
        {
            if (! song.targets.empty() && song.folder.createDirectory().failed())
            {
                outcome.attempted += (int) song.targets.size();
                if (outcome.firstError.isEmpty())
                    outcome.firstError = "Cannot create " + song.folder.getFileName();
                continue;
            }
            for (const auto& target : song.targets)
            {
                if (cancel.load (std::memory_order_relaxed)) break;
                if (progress != nullptr)
                    progress (outcome.attempted, total, song.folder.getFileName());

                juce::String error;
                bool ok = false;
                {
                    const int channels = target.kind == Target::Kind::MonoStem ? 1 : 2;
                    ExportFileStream out (exporter, target.destStem, plan.format, plan.renderRate, channels);
                    ok = out.isOpen() && renderTarget (engine, target, song, out, cancel);
                    if (cancel.load (std::memory_order_relaxed)) break;
                    ok = ok ? out.commit (error) : false;
                    if (! ok && error.isEmpty())
                        error = "Could not render " + target.destStem.getFileName();
                }
                ++outcome.attempted;
                if (ok) ++outcome.written;
                else if (outcome.firstError.isEmpty()) outcome.firstError = error;
            }
            if (song.folder.isDirectory()
                && song.folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
                song.folder.deleteFile();
            if (cancel.load (std::memory_order_relaxed)) break;
        }
        outcome.cancelled = cancel.load (std::memory_order_relaxed);
        return outcome;
    }

    juce::String statusLine (const Outcome& outcome, const juce::String& destName)
    {
        if (outcome.cancelled)
            return "Song export cancelled: " + juce::String (outcome.written) + " file(s) finished";
        const auto why = outcome.firstError.isNotEmpty() ? " (" + outcome.firstError + ")" : juce::String();
        if (outcome.attempted == 0)
            return "Export failed: no songs to write";
        if (outcome.written == 0)
            return "Export failed" + (outcome.firstError.isNotEmpty() ? ": " + outcome.firstError : juce::String());
        if (outcome.written < outcome.attempted)
            return "Export INCOMPLETE: " + juce::String (outcome.attempted - outcome.written)
                 + " of " + juce::String (outcome.attempted) + " failed or missing" + why;
        return "Exported " + juce::String (outcome.written) + " file(s) -> " + destName;
    }
}
