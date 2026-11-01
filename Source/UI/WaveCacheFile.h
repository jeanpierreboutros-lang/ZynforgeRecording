#pragma once

// WaveCache.wfm, the peaks EDIT saves so a session reopens without rescanning.
// It reaches hundreds of megabytes on a long multitrack show, so both directions
// run on EditPage's own thread. `stop` is polled between chunks: a take starting
// or the session changing gives up at once, and only a complete cache ever
// replaces the file, so a full disk cannot leave truncated peaks behind.

#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AtomicFile.h"
#include "WaveCacheFreshness.h"

#include <functional>
#include <memory>
#include <vector>

namespace zynforge::wavecache
{
    // Waveform thumbnail resolution: samples averaged per drawn min/max
    // point. Lower = finer detail (and a bigger cache). 128 stays crisp
    // when a finalized take is zoomed in well past its overview scale. The
    // WaveCache.wfm on disk bakes thumbnails AT this resolution, so any
    // change here must invalidate older caches -- hence kVersion, which is
    // written as a header and re-checked on load. Bump the version whenever
    // the resolution (or anything else affecting the baked data) changes, so
    // stale coarse caches are discarded and re-scanned.
    inline constexpr int kThumbResolution = 128;
    inline constexpr int kMagic = 0x5A574332; // 'ZWC2'
    // Low byte is a revision: bump it to discard previously-saved caches even
    // when the resolution is unchanged. rev 2 dropped caches written by the
    // build that froze thumbnails at a fraction of a second. rev 3 (2026-06-14)
    // changes the on-disk layout to N length-prefixed shard sections (parallel
    // scan caches), so pre-rev-3 single-blob caches are dropped + re-scanned.
    inline constexpr int kVersion = (kThumbResolution << 8) | 3;

    using Caches    = std::vector<std::unique_ptr<juce::AudioThumbnailCache>>;
    using StopCheck = std::function<bool()>;

    inline juce::File fileIn (const juce::File& sessionDir)
    {
        return sessionDir.getChildFile ("WaveCache.wfm");
    }

    // One length-prefixed section per shard, so each loads back into the same
    // shard on reopen. False the moment a write fails or `stop` says so.
    inline bool write (juce::OutputStream& out, Caches& caches, const StopCheck& stop)
    {
        if (! out.writeInt (kMagic) || ! out.writeInt (kVersion)
            || ! out.writeInt ((int) caches.size()))
            return false;

        constexpr size_t chunk = 1 << 20;
        for (auto& cache : caches)
        {
            juce::MemoryOutputStream section;
            cache->writeToStream (section);
            if (stop() || ! out.writeInt ((int) section.getDataSize())) return false;
            const auto* data = static_cast<const char*> (section.getData());
            for (size_t done = 0; done < section.getDataSize();)
            {
                const auto count = juce::jmin (chunk, section.getDataSize() - done);
                if (stop() || ! out.write (data + done, count)) return false;
                done += count;
            }
        }
        return true;
    }

    inline bool save (const juce::File& sessionDir, Caches& caches, const StopCheck& stop)
    {
        return atomicfile::writeWith (fileIn (sessionDir), [&] (juce::OutputStream& out)
                                      { return write (out, caches, stop); });
    }

    // Pulls the session's peaks into the caches. Anything that cannot be
    // trusted is deleted and those thumbnails re-scan: worst case a slow first
    // paint, never wrong audio.
    inline void load (const juce::File& sessionDir, Caches& caches, const StopCheck& stop)
    {
        const auto cacheFile = fileIn (sessionDir);
        if (! cacheFile.existsAsFile() || cacheFile.getSize() < 16) return;

        // A normal take may reuse Track_NN paths while an earlier waveform
        // cache still exists. Never load those stale peaks into the new rows.
        if (waveCachePredatesAudio (cacheFile, sessionDir))
        {
            cacheFile.deleteFile();
            return;
        }

        // Versioned header: magic + (resolution<<8|rev). A cache baked at a
        // different thumbnail resolution would re-paint as coarse stair-step
        // blocks (its min/max points are too sparse for the new draw), so a
        // mismatch -- including pre-header caches whose first int isn't our
        // magic -- means delete the stale file and let the thumbnails
        // re-scan. The stream is scoped so its file handle is released before
        // we delete.
        bool stale = false;
        {
            juce::FileInputStream in (cacheFile);
            if (! in.openedOk()) return;

            const int magic  = in.readInt();
            const int ver    = in.readInt();
            const int shards = in.readInt();
            stale = magic != kMagic || ver != kVersion || shards != (int) caches.size();
            for (auto it = caches.begin(); ! stale && it != caches.end(); ++it)
            {
                if (stop()) return;
                const int length = in.readInt();
                if (length <= 0) continue;
                // A section running past the end is a file cut short by a
                // failed write: its peaks would load as flat, finished lanes.
                if (length > in.getNumBytesRemaining()) { stale = true; break; }
                juce::MemoryBlock section;
                in.readIntoMemoryBlock (section, length);
                juce::MemoryInputStream stream (section, false);
                (*it)->readFromStream (stream);
            }
        }
        if (stale)
            cacheFile.deleteFile();
    }
}
