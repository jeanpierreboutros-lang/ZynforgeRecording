#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <bit>
#include <cmath>
#include <functional>
#include <vector>

namespace zynforge
{
    // JUCE's AIFF writer accepts only integer depths. AIFF-C's standard fl32
    // encoding preserves IEEE float samples while remaining readable by JUCE
    // and other AIFF-C readers. Keep the existing .aif extension/format enum.
    class FloatAiffWriter final : public juce::AudioFormatWriter
    {
    public:
        // Stays inside the recorder's existing conservative AIFF split limit.
        static constexpr juce::uint64 kMaxDataBytes = 0x7ff00000u;

        // Matches JUCE's ownership contract: ownership transfers only on success.
        static juce::AudioFormatWriter* create (juce::OutputStream* stream,
                                               double rate, unsigned int channels)
        {
            if (stream == nullptr || ! std::isfinite (rate) || rate < 1.0
                || rate > 768000.0 || channels == 0 || channels > 256)
                return nullptr;
            auto writer = std::unique_ptr<FloatAiffWriter> (new FloatAiffWriter (stream, rate, channels));
            if (! writer->healthy)
            {
                writer->output = nullptr;
                return nullptr;
            }
            if (onCreateForTests) onCreateForTests (*writer);
            return writer.release();
        }

        ~FloatAiffWriter() override
        {
            if (output != nullptr && headerDirty) flush();
        }

        bool write (const int** samples, int count) override
        {
            if (! healthy || count < 0 || samples == nullptr) return false;
            const auto bytes = (juce::uint64) count * numChannels * 4u;
            if (dataBytes + bytes > kMaxDataBytes) return false;
            for (unsigned int channel = 0; channel < numChannels; ++channel)
                if (samples[channel] == nullptr) return false;
            encoded.resize ((size_t) bytes);
            auto* dest = encoded.data();
            for (int frame = 0; frame < count; ++frame)
                for (unsigned int channel = 0; channel < numChannels; ++channel)
                {
                    // The base writer passes float bit patterns through int**
                    // when usesFloatingPointData is true. memcpy avoids aliasing.
                    juce::uint32 bits;
                    std::memcpy (&bits, samples[channel] + frame, sizeof (bits));
                    *dest++ = (juce::uint8) (bits >> 24);
                    *dest++ = (juce::uint8) (bits >> 16);
                    *dest++ = (juce::uint8) (bits >> 8);
                    *dest++ = (juce::uint8) bits;
                }
            if (! output->write (encoded.data(), encoded.size()))
            {
                healthy = false;
                return false;
            }
            dataBytes += bytes;
            headerDirty = true;
            return true;
        }

        // The periodic flush: header only, no fsync, like the WAV and integer
        // AIFF paths. Every open file on every copy pays for it every 5 s.
        bool flush() override
        {
            const bool headerOk = ! headerDirty || writeHeader();
            if (auto* file = dynamic_cast<juce::FileOutputStream*> (output))
                healthy = healthy && file->getStatus().wasOk();
            healthy = healthy && headerOk;
            if (healthy) headerDirty = false;
            return healthy && ! failFlushForTests;
        }

        // The last chance to hear about a failed write: header, then fsync.
        // Safe to follow with the destructor.
        bool finishBeforeClose()
        {
            const bool headerOk = ! headerDirty || writeHeader();
            output->flush();
            if (auto* file = dynamic_cast<juce::FileOutputStream*> (output))
                healthy = healthy && file->getStatus().wasOk();
            healthy = healthy && headerOk;
            if (healthy) headerDirty = false;
            return healthy && ! failFlushForTests;
        }

    private:
        friend class ReviewFloatFinalizeTests;
        inline static std::function<void (FloatAiffWriter&)> onCreateForTests;
        bool failFlushForTests = false;

        FloatAiffWriter (juce::OutputStream* stream, double rate, unsigned int channels)
            : AudioFormatWriter (stream, "AIFF-C float", rate, channels, 32),
              headerPosition (stream->getPosition())
        {
            usesFloatingPointData = true;
            healthy = writeHeader();
        }

        bool writeHeader()
        {
            const auto end = headerPosition + 72 + (juce::int64) dataBytes;
            if (! output->setPosition (headerPosition)) return false;
            int exponent = 0;
            const double fraction = std::frexp (sampleRate, &exponent);
            const auto mantissa = (juce::uint64) std::ldexp (fraction, 64);
            // FORM / FVER / COMM (24 bytes, empty Pascal compression name) /
            // SSND. Sample-rate is an IEEE 80-bit extended positive number.
            const bool ok = output->write ("FORM", 4)
                && output->writeIntBigEndian ((int) (dataBytes + 64))
                && output->write ("AIFC", 4)
                && output->write ("FVER", 4) && output->writeIntBigEndian (4)
                && output->writeIntBigEndian (std::bit_cast<juce::int32> (juce::uint32 (0xa2805140u)))
                && output->write ("COMM", 4) && output->writeIntBigEndian (24)
                && output->writeShortBigEndian ((short) numChannels)
                && output->writeIntBigEndian ((int) (dataBytes / (numChannels * 4u)))
                && output->writeShortBigEndian (32)
                && output->writeShortBigEndian ((short) (16382 + exponent))
                && output->writeInt64BigEndian (std::bit_cast<juce::int64> (mantissa))
                && output->write ("fl32", 4) && output->writeShort (0)
                && output->write ("SSND", 4) && output->writeIntBigEndian ((int) (dataBytes + 8))
                && output->writeIntBigEndian (0) && output->writeIntBigEndian (0);
            return output->setPosition (end) && ok;
        }

        juce::int64 headerPosition;
        juce::uint64 dataBytes = 0;
        bool healthy = false;
        bool headerDirty = true;
        std::vector<juce::uint8> encoded;
    };

    // JUCE's integer AIFF writer writes its header at open and again only in
    // its destructor, so a crash left a file that says 0 frames however long
    // the take ran. This one writes the same bytes as JUCE's and can rewrite
    // the header while the file is open, which is what the WAV path has always
    // been able to do. The periodic flush stays cheap like WAV's: header only,
    // no fsync, because every open file on every copy pays for it.
    class PcmAiffWriter final : public juce::AudioFormatWriter
    {
    public:
        // Ownership of the stream transfers only on success, as for JUCE's.
        static juce::AudioFormatWriter* create (juce::OutputStream* stream, double rate,
                                                unsigned int channels, int bits)
        {
            if (stream == nullptr || ! std::isfinite (rate) || rate < 1.0 || rate > 768000.0
                || channels == 0 || channels > 256 || (bits != 16 && bits != 24))
                return nullptr;
            auto writer = std::unique_ptr<PcmAiffWriter> (new PcmAiffWriter (stream, rate, channels, bits));
            if (! writer->healthy)
            {
                writer->output = nullptr;
                return nullptr;
            }
            return writer.release();
        }

        ~PcmAiffWriter() override
        {
            if (output != nullptr) closeOut();
        }

        bool write (const int** samples, int count) override
        {
            if (! healthy || count < 0 || samples == nullptr) return false;
            const auto bytesPerSample = bitsPerSample / 8;
            const auto bytes = (juce::uint64) count * numChannels * bytesPerSample;
            // AIFF sizes are signed 32-bit fields. The recorder rolls well below this.
            if (dataBytes + bytes > 0x7ff00000u) return false;
            encoded.resize ((size_t) bytes);
            auto* dest = encoded.data();
            for (int frame = 0; frame < count; ++frame)
                for (unsigned int channel = 0; channel < numChannels; ++channel)
                {
                    // JUCE hands integer writers left-justified 32-bit samples.
                    const auto value = samples[channel] != nullptr
                        ? (juce::uint32) samples[channel][frame] : 0u;
                    *dest++ = (juce::uint8) (value >> 24);
                    *dest++ = (juce::uint8) (value >> 16);
                    if (bytesPerSample == 3) *dest++ = (juce::uint8) (value >> 8);
                }
            if (! output->write (encoded.data(), encoded.size()))
            {
                // A full disk can still take a header: leave what we have readable.
                healthy = false;
                writeHeader();
                return false;
            }
            dataBytes += bytes;
            headerDirty = true;
            return true;
        }

        bool flush() override
        {
            const bool headerOk = ! headerDirty || writeHeader();
            if (auto* file = dynamic_cast<juce::FileOutputStream*> (output))
                healthy = healthy && file->getStatus().wasOk();
            healthy = healthy && headerOk;
            if (healthy) headerDirty = false;
            return healthy;
        }

        // The last chance to hear about a failed write: pad, header, fsync.
        // Safe to follow with the destructor.
        bool finishBeforeClose()
        {
            closeOut();
            output->flush();
            if (auto* file = dynamic_cast<juce::FileOutputStream*> (output))
                healthy = healthy && file->getStatus().wasOk();
            return healthy;
        }

    private:
        PcmAiffWriter (juce::OutputStream* stream, double rate, unsigned int channels, int bits)
            : AudioFormatWriter (stream, "AIFF file", rate, channels, (unsigned int) bits),
              headerPosition (stream->getPosition())
        {
            healthy = writeHeader();
        }

        void closeOut()
        {
            const bool needsPad = (dataBytes & 1) != 0 && ! padded;
            if (! headerDirty && ! needsPad) return;
            if (needsPad) padded = output->writeByte (0);
            healthy = writeHeader() && healthy;
            headerDirty = false;
        }

        static constexpr juce::int64 kHeaderBytes = 54;

        bool writeHeader()
        {
            const auto audioBytes = dataBytes + (dataBytes & 1);
            const auto end = headerPosition + kHeaderBytes + (juce::int64) dataBytes + (padded ? 1 : 0);
            const auto frames = (int) (dataBytes / (numChannels * (bitsPerSample / 8)));
            if (! output->setPosition (headerPosition)) return false;
            int exponent = 0;
            const double fraction = std::frexp (sampleRate, &exponent);
            const auto mantissa = (juce::uint64) std::ldexp (fraction, 64);
            const bool ok = output->write ("FORM", 4)
                && output->writeIntBigEndian ((int) (kHeaderBytes + (juce::int64) audioBytes - 8))
                && output->write ("AIFF", 4)
                && output->write ("COMM", 4) && output->writeIntBigEndian (18)
                && output->writeShortBigEndian ((short) numChannels)
                && output->writeIntBigEndian (frames)
                && output->writeShortBigEndian ((short) bitsPerSample)
                && output->writeShortBigEndian ((short) (16382 + exponent))
                && output->writeInt64BigEndian (std::bit_cast<juce::int64> (mantissa))
                && output->write ("SSND", 4) && output->writeIntBigEndian ((int) (audioBytes + 8))
                && output->writeIntBigEndian (0) && output->writeIntBigEndian (0);
            return output->setPosition (end) && ok;
        }

        juce::int64 headerPosition;
        juce::uint64 dataBytes = 0;
        bool healthy = false;
        bool headerDirty = true;
        bool padded = false;
        std::vector<juce::uint8> encoded;
    };

    // JUCE's default flush() means "unsupported" for some other codecs, so
    // check the formats whose explicit finalization contract we implement.
    inline bool flushFloatAiffBeforeClose (juce::AudioFormatWriter* writer)
    {
        if (auto* pcm = dynamic_cast<PcmAiffWriter*> (writer)) return pcm->finishBeforeClose();
        auto* floating = dynamic_cast<FloatAiffWriter*> (writer);
        return floating == nullptr || floating->finishBeforeClose();
    }
}
