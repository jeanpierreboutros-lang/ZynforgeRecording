#pragma once

#include <juce_core/juce_core.h>

namespace zynforge::preflight
{
    // Measured (not assumed) probes behind the Pre-Flight checklist.
    // Header-only and engine-free so the test harness can drive them
    // headlessly against temp directories.

    // True when `dir` exists and a small file can actually be created,
    // written and deleted there -- catches read-only mounts, vanished
    // externals and permission surprises that File::isDirectory() hides.
    inline bool volumeWritable (const juce::File& dir)
    {
        if (! dir.isDirectory()) return false;
        auto probe = dir.getNonexistentChildFile (".zf-preflight", ".tmp");
        {
            juce::FileOutputStream os (probe);
            if (os.failedToOpen()) return false;
            os.writeInt (0x5A464C54);
            os.flush();
            if (! os.getStatus().wasOk()) { probe.deleteFile(); return false; }
        }
        return probe.deleteFile();
    }

    // Sustained sequential write speed of the volume behind `dir`, in
    // MB/s, measured by streaming `megabytes` of data to a temp file in
    // 1 MB chunks (flushed) and timing it. Returns <= 0 on failure. A
    // 16 MB probe finishes in well under a second on any disk that can
    // host a show; this deliberately measures the *write path the
    // recorder will use* (same folder, same filesystem) rather than
    // trusting the drive's marketing numbers.
    inline double measureWriteSpeedMBps (const juce::File& dir, int megabytes = 16)
    {
        if (megabytes <= 0 || ! dir.isDirectory()) return -1.0;
        auto probe = dir.getNonexistentChildFile (".zf-preflight-speed", ".tmp");

        juce::HeapBlock<char> chunk (1 << 20);
        juce::Random rng (0x5A464C54);                 // incompressible-ish payload
        for (int i = 0; i < (1 << 20); i += 4)
            *reinterpret_cast<juce::uint32*> (chunk.get() + i) = (juce::uint32) rng.nextInt();

        double mbps = -1.0;
        {
            juce::FileOutputStream os (probe);
            if (os.failedToOpen()) return -1.0;
            const auto t0 = juce::Time::getHighResolutionTicks();
            bool ok = true;
            for (int mb = 0; mb < megabytes && ok; ++mb)
                ok = os.write (chunk.get(), 1 << 20);
            os.flush();
            ok = ok && os.getStatus().wasOk();
            const auto t1 = juce::Time::getHighResolutionTicks();
            const double secs = juce::Time::highResolutionTicksToSeconds (t1 - t0);
            if (ok && secs > 0.0)
                mbps = (double) megabytes / secs;
        }
        probe.deleteFile();
        return mbps;
    }

    // Write throughput the recorder needs, in MB/s: sample rate x armed
    // track count x bytes per sample. FLAC callers should pass the
    // uncompressed bytes-per-sample -- it's the worst-case bound.
    inline double requiredWriteMBps (double sampleRate, int armedTracks, int bytesPerSample)
    {
        if (sampleRate <= 0.0 || armedTracks <= 0 || bytesPerSample <= 0) return 0.0;
        return (sampleRate * (double) armedTracks * (double) bytesPerSample) / 1.0e6;
    }

    // Recording headroom in minutes for `freeBytes` at the given demand.
    inline double minutesOfHeadroom (juce::int64 freeBytes, double requiredMBps)
    {
        if (freeBytes <= 0 || requiredMBps <= 0.0) return 0.0;
        return ((double) freeBytes / 1.0e6) / requiredMBps / 60.0;
    }
}
