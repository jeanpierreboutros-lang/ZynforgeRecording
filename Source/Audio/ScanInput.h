#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>

#include "CheckedAudioReader.h"
#include "UncachedFileInput.h"

// Input for waveform scans. JUCE's AudioThumbnail asks its reader for one
// thumbnail column (512 samples) at a time, and a WAV/AIFF reader passes each
// request straight to its stream: a 1.5 KB read() per column of a mono 24-bit
// track. A 512 KiB buffer turns each stretch of columns into one read.
//
// The buffer alone did not fix the slow post-STOP rescan on an external exFAT
// drive: the kernel's read-ahead for 55 files scanned side by side was thrown
// away before use, so the drive read 2.7 GB/s for a few MB/s of audio. Under
// the buffer the file is now read past the page cache (UncachedFileInput.h).
namespace zynforge::scanio
{
    inline constexpr int kReadBufferBytes = 1 << 19;   // 512 KiB

    inline std::unique_ptr<juce::InputStream> openStream (const juce::File& file)
    {
        auto in = uncached::FileInput::open (file);
        if (in == nullptr) return {};
        return std::make_unique<juce::BufferedInputStream> (in.release(), kReadBufferBytes, true);
    }

    // juce::FileInputSource with a buffered stream. hashCode() matches
    // juce::FileInputSource (file).hashCode(): the thumbnail cache, its removeThumb
    // calls and WaveCache.wfm are all keyed by it, so peaks saved by an earlier
    // build still match.
    class FileSource final : public juce::InputSource
    {
    public:
        explicit FileSource (const juce::File& f) : file (f) {}

        juce::InputStream* createInputStream() override { return openStream (file).release(); }
        juce::InputStream* createInputStreamFor (const juce::String& relatedItemPath) override
        {
            return openStream (file.getSiblingFile (relatedItemPath)).release();
        }
        juce::int64 hashCode() const override { return file.hashCode(); }

    private:
        juce::File file;
    };

    // A reader for drawing a part of a multi-part take. FLAC keeps the
    // playback reader, and with it the page cache: its decoder reads whole
    // frames, not one column, and that stream also carries JUCE's FLAC seeks
    // past 2 GiB, which a BufferedInputStream would clamp to 0.
    inline std::unique_ptr<juce::AudioFormatReader> openReader (juce::AudioFormatManager& formats,
                                                               const juce::File& file)
    {
        if (file.hasFileExtension ("flac")) return openPlaybackReader (formats, file);
        auto in = openStream (file);
        if (in == nullptr) return {};
        return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (std::move (in)));
    }

    // Waveform scan threads run at utility priority. Background priority
    // throttled their reads, but macOS lets a background thread's timers fire
    // up to ~64 ms late, and JUCE's TimeSliceThread waits 1 ms after every pass
    // over its thumbnails: on a 55-track take each scan thread slept about 90%
    // of the time. Utility still puts the reads in a throttled, low-priority
    // disk tier below recording, playback and session saves, and leaves the
    // CPU to them. Never call setiopolicy_np or a legacy priority call on
    // these threads: it takes a thread out of QoS for good (measured: the QoS
    // class reads back unspecified and can not be set again).
    inline void startScanThread (juce::TimeSliceThread& thread)
    {
        thread.stopThread (-1);
        thread.startThread (juce::Thread::Priority::low);
    }
}
