#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>

#include "CheckedAudioReader.h"

// Input for waveform scans. JUCE's AudioThumbnail asks its reader for one
// thumbnail column (512 samples) at a time, and a WAV/AIFF reader passes each
// request straight to its stream. On a bare file stream that is a 1.5 KB read()
// per column of a mono 24-bit track. After a 4-hour, 55-track take on an
// external exFAT drive, each of those cost about a full disk transfer: the
// post-STOP rescan moved under 2 MB/s of audio while the drive ran at 2.7 GB/s,
// and the report's SHA-256 pass crawled behind it. A large buffer turns each
// stretch of columns into one sequential read.
namespace zynforge::scanio
{
    inline constexpr int kReadBufferBytes = 1 << 19;   // 512 KiB

    inline std::unique_ptr<juce::InputStream> openStream (const juce::File& file)
    {
        auto in = file.createInputStream();
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
    // playback reader: its decoder reads whole frames, not one column.
    inline std::unique_ptr<juce::AudioFormatReader> openReader (juce::AudioFormatManager& formats,
                                                               const juce::File& file)
    {
        if (file.hasFileExtension ("flac")) return openPlaybackReader (formats, file);
        auto in = openStream (file);
        if (in == nullptr) return {};
        return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (std::move (in)));
    }
}
