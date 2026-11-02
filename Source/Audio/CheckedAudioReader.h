#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>
#include <memory>

#include "FlacRepair.h"

namespace zynforge
{
    // JUCE's WAV, AIFF and FLAC readers fill a short read with zeros and return
    // true, and FileInputStream turns a read error into 0 bytes. So a drive that
    // drops during a render reads as digital silence and the render reports
    // success. The failure can only be seen here, below the format reader.
    class CheckedInputStream final : public juce::InputStream
    {
    public:
        static std::unique_ptr<CheckedInputStream> open (const juce::File& file)
        {
            auto stream = std::make_unique<juce::FileInputStream> (file);
            if (stream->failedToOpen()) return nullptr;
            return std::unique_ptr<CheckedInputStream> (new CheckedInputStream (std::move (stream)));
        }

        // From here on a read is audio. A format reader parsing its header may
        // run off the end quite properly, so nothing latches before this.
        // `everyReadIsAudio`: the reader asks only for what its header promised,
        // so any short read at all is a failure. A streaming decoder asks for a
        // full buffer and takes what is left at the end of the file.
        void watchAudio (bool everyReadIsAudio) noexcept
        {
            watching = true;
            strict = everyReadIsAudio;
        }

        void markFailed() noexcept { failed = true; }
        bool hasFailed() const noexcept { return failed; }

        juce::int64 getTotalLength() override { return sizeAtOpen; }

        // libFLAC keeps asking while the stream says it is not exhausted, and a
        // failed read leaves the position where it was. Without the first test a
        // dead drive would spin that loop for ever.
        bool isExhausted() override { return failed || stream->getPosition() >= sizeAtOpen; }

        juce::int64 getPosition() override { return stream->getPosition(); }

        bool setPosition (juce::int64 position) override
        {
            const bool ok = stream->setPosition (position);
            if (! ok && watching) failed = true;
            return ok;
        }

        int read (void* destination, int bytes) override
        {
            auto* dest = static_cast<char*> (destination);
            int got = 0;
            while (got < bytes)   // one ::read may return less than the file holds
            {
                const int n = stream->read (dest + got, bytes - got);
                if (n <= 0) break;
                got += n;
            }
            if (watching && (stream->getStatus().failed()
                             || (got < bytes && (strict || stream->getPosition() < sizeAtOpen))))
                failed = true;
            return got;
        }

    private:
        explicit CheckedInputStream (std::unique_ptr<juce::FileInputStream> file)
            : stream (std::move (file)), sizeAtOpen (stream->getTotalLength()) {}

        std::unique_ptr<juce::FileInputStream> stream;
        juce::int64 sizeAtOpen;
        bool watching { false }, strict { false }, failed { false };
    };

    // A format reader whose failed read comes back as false. Once a read has
    // failed every later one fails too: nothing downstream should go on to
    // write a file with a hole in it. The destination reads as silence either
    // way, for a caller that does not look at the answer.
    class CheckedAudioReader final : public juce::AudioFormatReader
    {
    public:
        CheckedAudioReader (std::unique_ptr<juce::AudioFormatReader> opened, CheckedInputStream& inside)
            : juce::AudioFormatReader (nullptr, opened->getFormatName()),
              reader (std::move (opened)), stream (inside)
        {
            sampleRate            = reader->sampleRate;
            bitsPerSample         = reader->bitsPerSample;
            lengthInSamples       = reader->lengthInSamples;
            numChannels           = reader->numChannels;
            usesFloatingPointData = reader->usesFloatingPointData;
            metadataValues        = reader->metadataValues;
        }

        // A FLAC file cut short before it was opened gives no short read: every
        // byte asked for is there, and the decoder simply runs out of frames.
        void countFramesOf (const juce::File& flacFile) { flacToCount = flacFile; }

        bool readSamples (int* const* destination, int numDestChannels, int destOffset,
                          juce::int64 startSampleInFile, int numSamples) override
        {
            // On the first read and not at open: session checks open every take
            // for its length, and this reads a megabyte of each.
            if (flacToCount != juce::File())
            {
                const auto frames = flacrepair::wholeFramesEnd (flacToCount);
                flacToCount = juce::File();
                if (frames >= 0) audioEnd = frames;
            }
            if (juce::jmin (startSampleInFile + numSamples, lengthInSamples) > audioEnd) stream.markFailed();

            const bool ok = ! stream.hasFailed()
                         && reader->readSamples (destination, numDestChannels, destOffset, startSampleInFile, numSamples)
                         && ! stream.hasFailed();
            if (! ok)
                for (int channel = 0; channel < numDestChannels; ++channel)
                    if (destination[channel] != nullptr)
                        juce::zeromem (destination[channel] + destOffset, sizeof (int) * (size_t) numSamples);
            return ok;
        }

        juce::AudioChannelSet getChannelLayout() override { return reader->getChannelLayout(); }

    private:
        std::unique_ptr<juce::AudioFormatReader> reader;   // owns the stream
        CheckedInputStream& stream;
        juce::File flacToCount;
        juce::int64 audioEnd { std::numeric_limits<juce::int64>::max() };
    };

    // AudioFormatManager::createReaderFor (File), with the checked stream
    // underneath. Anything that writes or reports from audio it read opens
    // through this. Null when no registered format opens the file.
    // `sessionMedia`: this app wrote it, so its FLAC frames run to the end of
    // the file and can be counted. A file from elsewhere may have a tag there.
    inline std::unique_ptr<juce::AudioFormatReader> openCheckedReader (juce::AudioFormatManager& formats,
                                                                       const juce::File& file,
                                                                       bool sessionMedia = false)
    {
        for (int i = 0; i < formats.getNumKnownFormats(); ++i)
        {
            auto* format = formats.getKnownFormat (i);
            if (! format->canHandleFile (file)) continue;
            auto stream = CheckedInputStream::open (file);
            if (stream == nullptr) continue;
            auto* inside = stream.get();
            std::unique_ptr<juce::AudioFormatReader> reader (format->createReaderFor (stream.release(), true));
            if (reader == nullptr) continue;
            inside->watchAudio (dynamic_cast<juce::WavAudioFormat*> (format) != nullptr
                                || dynamic_cast<juce::AiffAudioFormat*> (format) != nullptr);
            auto checked = std::make_unique<CheckedAudioReader> (std::move (reader), *inside);
            if (sessionMedia && dynamic_cast<juce::FlacAudioFormat*> (format) != nullptr)
                checked->countFramesOf (file);
            return checked;
        }
        return nullptr;
    }
}
