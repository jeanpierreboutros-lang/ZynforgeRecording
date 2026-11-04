#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
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

    // The scan's file, read past the page cache on the scan threads only.
    // Below ~128 samples per pixel JUCE draws a thumbnail by reading its file
    // on the message thread, through this same stream: with two clips of one
    // file on screen, each repaint read both regions again, a disk read per
    // clip per frame once the stream was uncached. Those reads are short and
    // on-screen, so they go through the page cache as they did before.
    class ScanFileInput final : public juce::InputStream
    {
    public:
        static std::unique_ptr<ScanFileInput> open (const juce::File& file)
        {
            auto in = uncached::FileInput::open (file);
            if (in == nullptr) return {};
            return std::unique_ptr<ScanFileInput> (new ScanFileInput (std::move (in)));
        }

        juce::int64 getTotalLength() override { return in->getTotalLength(); }
        int read (void* destination, int bytes) override
        {
            in->usePageCache (juce::MessageManager::existsAndIsCurrentThread());
            return in->read (destination, bytes);
        }
        bool isExhausted() override        { return in->isExhausted(); }
        juce::int64 getPosition() override { return in->getPosition(); }
        bool setPosition (juce::int64 newPosition) override { return in->setPosition (newPosition); }

        bool bypassesCache() const noexcept         { return in->bypassesCache(); }
        bool readsThroughPageCache() const noexcept { return in->readsThroughPageCache(); }

    private:
        explicit ScanFileInput (std::unique_ptr<uncached::FileInput> file) : in (std::move (file)) {}
        std::unique_ptr<uncached::FileInput> in;
    };

    inline std::unique_ptr<juce::InputStream> openStream (const juce::File& file)
    {
        auto in = ScanFileInput::open (file);
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

    // A take must have its drive to itself. Pausing never waits: the slice in
    // flight finishes on its own, and a slice stuck on a drive that stopped
    // answering must not take the message thread with it. Every thumbnail keeps
    // its place, so a resumed scan goes on where it stopped.
    inline void pauseScanThread (juce::TimeSliceThread& thread)
    {
        thread.signalThreadShouldExit();
        thread.notify();
    }

    // False while the slice that was in flight when it paused still runs; try
    // again later rather than wait for it.
    inline bool resumeScanThread (juce::TimeSliceThread& thread)
    {
        if (thread.isThreadRunning()) return ! thread.threadShouldExit();
        startScanThread (thread);
        return true;
    }
}
