#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace zynforge::flacrepair
{
    enum class Result { untouched, repaired, failed };

    namespace detail
    {
        inline juce::uint8 crc8 (const juce::uint8* data, size_t size) noexcept
        {
            juce::uint8 crc = 0;
            for (size_t i = 0; i < size; ++i)
            {
                crc = (juce::uint8) (crc ^ data[i]);
                for (int bit = 0; bit < 8; ++bit)
                    crc = (juce::uint8) ((crc & 0x80) != 0 ? (crc << 1) ^ 0x07 : crc << 1);
            }
            return crc;
        }

        inline juce::uint16 crc16 (const juce::uint8* data, size_t size) noexcept
        {
            juce::uint16 crc = 0;
            for (size_t i = 0; i < size; ++i)
            {
                crc = (juce::uint16) (crc ^ (data[i] << 8));
                for (int bit = 0; bit < 8; ++bit)
                    crc = (juce::uint16) ((crc & 0x8000) != 0 ? (crc << 1) ^ 0x8005 : crc << 1);
            }
            return crc;
        }

        struct Frame { size_t offset {}; juce::int64 number {}; int samples {}; };

        // A fixed-blocksize frame header at p, CRC-8 checked. Returns false for
        // anything else, which in compressed audio is nearly every position.
        inline bool parseFrame (const juce::uint8* p, size_t available, juce::int64& number, int& samples) noexcept
        {
            if (available < 6 || p[0] != 0xff || p[1] != 0xf8) return false;
            const int blockCode = p[2] >> 4, rateCode = p[2] & 0x0f;
            if (blockCode == 0 || rateCode == 15) return false;
            if ((p[3] >> 4) > 10 || (p[3] & 1) != 0) return false;

            size_t at = 4;
            const auto first = p[at++];
            int extra = 0;
            juce::int64 value = first;
            if      ((first & 0x80) == 0)    extra = 0;
            else if ((first & 0xe0) == 0xc0) { extra = 1; value = first & 0x1f; }
            else if ((first & 0xf0) == 0xe0) { extra = 2; value = first & 0x0f; }
            else if ((first & 0xf8) == 0xf0) { extra = 3; value = first & 0x07; }
            else if ((first & 0xfc) == 0xf8) { extra = 4; value = first & 0x03; }
            else if ((first & 0xfe) == 0xfc) { extra = 5; value = first & 0x01; }
            else return false;
            if (at + (size_t) extra + 5 > available) return false;
            for (int i = 0; i < extra; ++i)
            {
                if ((p[at] & 0xc0) != 0x80) return false;
                value = (value << 6) | (p[at++] & 0x3f);
            }
            number = value;

            if      (blockCode == 1)  samples = 192;
            else if (blockCode <= 5)  samples = 576 << (blockCode - 2);
            else if (blockCode == 6)  samples = p[at++] + 1;
            else if (blockCode == 7)  { samples = ((p[at] << 8) | p[at + 1]) + 1; at += 2; }
            else                      samples = 256 << (blockCode - 8);

            if (rateCode == 12)                     at += 1;
            else if (rateCode == 13 || rateCode == 14) at += 2;
            return at < available && crc8 (p, at) == p[at];
        }

        // Where the last frame whose CRC-16 holds ends, in samples, from the
        // bytes after `in`'s STREAMINFO. 0 when no whole frame can be found.
        inline juce::int64 lastWholeFrameEnd (juce::FileInputStream& in, int blockSize)
        {
            constexpr juce::int64 kTailBytes = 1 << 20;
            const auto start = juce::jmax ((juce::int64) 42, in.getTotalLength() - kTailBytes);
            std::vector<juce::uint8> tail ((size_t) (in.getTotalLength() - start));
            if (! in.setPosition (start)
                || in.read (tail.data(), (int) tail.size()) != (int) tail.size())
                return 0;

            std::vector<Frame> frames;
            for (size_t i = 0; i + 1 < tail.size(); ++i)
            {
                juce::int64 number = 0;
                int samples = 0;
                if (parseFrame (tail.data() + i, tail.size() - i, number, samples))
                    frames.push_back ({ i, number, samples });
            }

            const auto whole = [&tail] (size_t from, size_t to)
            {
                if (to < from + 8) return false;
                const auto stored16 = (juce::uint16) ((tail[to - 2] << 8) | tail[to - 1]);
                return crc16 (tail.data() + from, to - from - 2) == stored16;
            };
            for (auto it = frames.rbegin(); it != frames.rend(); ++it)
            {
                bool ends = whole (it->offset, tail.size());
                for (auto next = frames.rbegin(); ! ends && next != it; ++next)
                    ends = next->number == it->number + 1 && whole (it->offset, next->offset);
                if (ends) return it->number * blockSize + it->samples;
            }
            return 0;
        }

        // STREAMINFO's maximum block size, or 0 for anything but FLAC.
        inline int streamInfoBlockSize (const juce::uint8* head) noexcept
        {
            if (std::memcmp (head, "fLaC", 4) != 0 || (head[4] & 0x7f) != 0) return 0;
            return (head[10] << 8) | head[11];
        }
    }

    // How many samples the file's frames really hold. A file cut short after it
    // was finished keeps STREAMINFO's full length, and the decoder fills what it
    // cannot find with zeros and says nothing. -1 when it cannot be told.
    inline juce::int64 wholeFramesEnd (const juce::File& file)
    {
        juce::FileInputStream in (file);
        if (in.failedToOpen() || in.getTotalLength() < 42) return -1;
        juce::uint8 head[42];
        if (in.read (head, 42) != 42) return -1;
        const int blockSize = detail::streamInfoBlockSize (head);
        if (blockSize <= 0) return -1;
        const auto end = detail::lastWholeFrameEnd (in, blockSize);
        return end > 0 ? end : -1;
    }

    // A FLAC encoder writes STREAMINFO's total length when it finishes, so a
    // crash leaves 0, and JUCE then decodes the whole file to learn the length,
    // every time a reader opens one. Each frame carries its own number, so the
    // last whole frame gives the length from the tail alone. A frame counts
    // only if its CRC-16 holds, up to the next frame or the end of the file: a
    // crash usually leaves one torn. Nothing else in the file is touched.
    inline Result repairStaleLength (const juce::File& file)
    {
        juce::int64 total = 0;
        juce::uint8 lengthByte = 0;
        {
            juce::FileInputStream in (file);
            if (in.failedToOpen() || in.getTotalLength() < 42) return Result::untouched;
            juce::uint8 head[42];
            if (in.read (head, 42) != 42) return Result::untouched;
            const int blockSize = detail::streamInfoBlockSize (head);   // STREAMINFO's maximum
            if (blockSize <= 0) return Result::untouched;
            const auto stored = ((juce::int64) (head[21] & 0x0f) << 32)
                              | ((juce::int64) head[22] << 24) | ((juce::int64) head[23] << 16)
                              | ((juce::int64) head[24] << 8)  |  (juce::int64) head[25];
            if (stored != 0) return Result::untouched;
            lengthByte = head[21];
            total = detail::lastWholeFrameEnd (in, blockSize);
        }
        if (total <= 0) return Result::untouched;

        juce::FileOutputStream out (file);
        if (! out.openedOk()) return Result::failed;
        const auto bytes = (juce::uint32) (total & 0xffffffff);
        const juce::uint8 patch[] { (juce::uint8) ((lengthByte & 0xf0) | ((total >> 32) & 0x0f)),
                                    (juce::uint8) (bytes >> 24), (juce::uint8) (bytes >> 16),
                                    (juce::uint8) (bytes >> 8),  (juce::uint8) bytes };
        const bool ok = out.setPosition (21) && out.write (patch, sizeof (patch));
        out.flush();
        return ok && out.getStatus().wasOk() ? Result::repaired : Result::failed;
    }

    inline void repairTakeFolder (const juce::File& audioDir, int* repaired = nullptr, int* failed = nullptr)
    {
        for (const auto& file : audioDir.findChildFiles (juce::File::findFiles, false, "Track_*.flac"))
        {
            const auto result = repairStaleLength (file);
            if (result == Result::repaired && repaired != nullptr) ++*repaired;
            if (result == Result::failed && failed != nullptr) ++*failed;
        }
    }
}
