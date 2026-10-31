#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstring>

// Broadcast WAV timing and channel names for captured files.
//
// A take's bext TimeReference is the number of samples between midnight and
// the file's first sample. JUCE keeps the bext block it was given at open and
// rewrites it on every header flush, and writers open before the first live
// block, so a file opens with an ESTIMATE (a crash survivor keeps a usable
// time) and finalization patches the exact value after the writer closes.
// That 8-byte patch is the only write ever made to a closed capture file.
namespace zynforge::bwf
{
    inline constexpr juce::int64 kTimeReferenceOffset = 338;  // into the bext data
    inline constexpr juce::int64 kBextFixedBytes      = 602;  // before CodingHistory
    inline constexpr int         kDescriptionBytes    = 256;
    inline constexpr int         kMaxNameBytes        = 64;

    enum class StampResult { ok, missingFile, notWav, noBext, badBext, unexpectedValue, ioError };

    inline juce::String toString (StampResult r)
    {
        switch (r)
        {
            case StampResult::ok:              return "ok";
            case StampResult::missingFile:     return "file missing";
            case StampResult::notWav:          return "not a RIFF/RF64 WAVE file";
            case StampResult::noBext:          return "no bext chunk";
            case StampResult::badBext:         return "bext chunk is damaged";
            case StampResult::unexpectedValue: return "bext holds an unexpected time reference";
            case StampResult::ioError:         return "read or write failed";
        }
        return "unknown";
    }

    // Samples since midnight. Pre-roll can step back over midnight and a long
    // take can roll past it, so both ends wrap into one day.
    inline juce::int64 wrapToDay (juce::int64 samples, double sampleRate) noexcept
    {
        const auto day = (juce::int64) std::llround (sampleRate * 86400.0);
        if (day <= 0) return 0;
        const auto wrapped = samples % day;
        return wrapped < 0 ? wrapped + day : wrapped;
    }

    // A time of day pinned to the high-resolution clock, so the moment the
    // first live block lands can be turned into a time without calling
    // localtime on the audio thread. Timecode keeps running through the record
    // start work, so this holds for a hint taken before it too. The ticks are
    // mach_absolute_time, shared by the GUI and the capture daemon.
    struct TimeAnchor
    {
        double       secondsSinceMidnight { 0.0 };
        juce::int64  ticks { 0 };
        juce::String source { "clock" };   // "ltc", "mtc" or "clock"

        double secondsAt (juce::int64 atTicks) const noexcept
        {
            return secondsSinceMidnight
                 + juce::Time::highResolutionTicksToSeconds (atTicks - ticks);
        }

        static TimeAnchor fromWallClock()
        {
            const auto ticks = juce::Time::getHighResolutionTicks();
            const auto now   = juce::Time::getCurrentTime();
            return { now.getHours() * 3600.0 + now.getMinutes() * 60.0 + now.getSeconds()
                         + now.getMilliseconds() / 1000.0,
                     ticks, "clock" };
        }

        // Carried from the GUI to the capture daemon in the Start command.
        juce::var toJson() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("seconds", secondsSinceMidnight);
            o->setProperty ("ticks", ticks);
            o->setProperty ("source", source);
            return juce::var (o);
        }

        static bool fromJson (const juce::var& v, TimeAnchor& anchor)
        {
            const auto seconds = (double) v.getProperty ("seconds", -1.0);
            const auto when    = (juce::int64) v.getProperty ("ticks", 0);
            const auto from    = v.getProperty ("source", "").toString();
            if (seconds < 0.0 || seconds > 86400.0 || when <= 0 || (from != "ltc" && from != "mtc"))
                return false;
            anchor = { seconds, when, from };
            return true;
        }
    };

    // ── Channel names in the Description field ──────────────────────────────
    // The longest prefix of `text` that fits maxBytes of UTF-8 without cutting
    // a character in half.
    inline juce::String fitUtf8 (const juce::String& text, int maxBytes)
    {
        int used = 0, chars = 0;
        for (auto p = text.getCharPointer(); ! p.isEmpty();)
        {
            const auto bytes = (int) juce::CharPointer_UTF8::getBytesRequiredFor (p.getAndAdvance());
            if (used + bytes > maxBytes) break;
            used += bytes;
            ++chars;
        }
        return text.substring (0, chars);
    }

    // Names arrive from console OSC too, so a control character must not be
    // able to break out of its sTRK line.
    inline juce::String channelNameField (const juce::String& name)
    {
        juce::String clean;
        for (auto p = name.getCharPointer(); ! p.isEmpty();)
        {
            const auto c = p.getAndAdvance();
            clean += c < 0x20 || c == 0x7f ? (juce::juce_wchar) ' ' : c;
        }
        return fitUtf8 (clean.trim(), kMaxNameBytes);
    }

    // sTRK1=<left or mono name> and sTRK2=<right name>, one CRLF-ended line
    // each, then the take text, cut to what the fixed 256-byte field holds.
    // kMaxNameBytes leaves the take text room however long the names are.
    inline juce::String channelDescription (const juce::String& takeText,
                                            const juce::String& firstName,
                                            const juce::String& secondName = {})
    {
        juce::String lines;
        const auto first = channelNameField (firstName), second = channelNameField (secondName);
        if (first.isNotEmpty())  lines << "sTRK1=" << first  << "\r\n";
        if (second.isNotEmpty()) lines << "sTRK2=" << second << "\r\n";
        return lines + fitUtf8 (takeText, kDescriptionBytes - (int) lines.getNumBytesAsUTF8());
    }

    // ── Chunk walker + patch ────────────────────────────────────────────────
    namespace detail
    {
        inline bool readAt (juce::InputStream& in, juce::int64 position, void* dest, int bytes)
        {
            return in.setPosition (position) && in.read (dest, bytes) == bytes;
        }

        inline juce::uint32 le32 (const char* p) noexcept
        {
            return juce::ByteOrder::littleEndianInt (p);
        }

        inline juce::uint64 le64 (const char* p) noexcept
        {
            return juce::ByteOrder::littleEndianInt64 (p);
        }

        inline bool tagIs (const char* p, const char* tag) noexcept
        {
            return std::memcmp (p, tag, 4) == 0;
        }
    }

    struct BextLocation
    {
        StampResult result { StampResult::noBext };
        juce::int64 dataStart { -1 }, size { 0 };
    };

    // Walks the top-level RIFF chunks. An RF64 data chunk stores -1 as its
    // size and keeps the real one in ds64.
    inline BextLocation findBext (juce::InputStream& in)
    {
        using namespace detail;
        constexpr int kMaxChunks = 4096;   // a damaged size must not spin here
        const auto total = in.getTotalLength();

        char header[12];
        if (! readAt (in, 0, header, 12)) return { StampResult::notWav };
        const bool rf64 = tagIs (header, "RF64");
        if ((! rf64 && ! tagIs (header, "RIFF")) || ! tagIs (header + 8, "WAVE"))
            return { StampResult::notWav };

        juce::int64 position = 12, ds64DataSize = -1;
        for (int chunk = 0; chunk < kMaxChunks && position + 8 <= total; ++chunk)
        {
            char head[8];
            if (! readAt (in, position, head, 8)) return { StampResult::ioError };
            const auto start = position + 8;
            auto size = (juce::int64) le32 (head + 4);

            if (tagIs (head, "ds64"))
            {
                char sizes[16];
                if (size < 16 || ! readAt (in, start, sizes, 16)) return { StampResult::notWav };
                ds64DataSize = (juce::int64) le64 (sizes + 8);
            }
            else if (tagIs (head, "data") && rf64 && size == 0xffffffffLL)
            {
                if (ds64DataSize < 0) return { StampResult::notWav };
                size = ds64DataSize;
            }
            else if (tagIs (head, "bext"))
            {
                if (size < kBextFixedBytes || start + size > total)
                    return { StampResult::badBext };
                return { StampResult::ok, start, size };
            }
            position = start + size + (size & 1);
        }
        return { StampResult::noBext };
    }

    inline StampResult readTimeReference (const juce::File& wav, juce::uint64& value)
    {
        if (! wav.existsAsFile()) return StampResult::missingFile;
        juce::FileInputStream in (wav);
        if (in.failedToOpen()) return StampResult::ioError;
        const auto bext = findBext (in);
        if (bext.result != StampResult::ok) return bext.result;
        char bytes[8];
        if (! detail::readAt (in, bext.dataStart + kTimeReferenceOffset, bytes, 8))
            return StampResult::ioError;
        value = detail::le64 (bytes);
        return StampResult::ok;
    }

    // Replaces the 8 TimeReference bytes and nothing else. `expected` is what
    // this recorder wrote at open: anything else means the walker is not
    // looking at the block it thinks it is, so the file is left alone.
    inline StampResult stampTimeReference (const juce::File& wav,
                                           juce::uint64 expected, juce::uint64 value)
    {
        if (! wav.existsAsFile()) return StampResult::missingFile;

        // One walk: the bytes compared are the bytes written.
        juce::int64 offset = -1;
        {
            juce::FileInputStream in (wav);
            if (in.failedToOpen()) return StampResult::ioError;
            const auto bext = findBext (in);
            if (bext.result != StampResult::ok) return bext.result;
            offset = bext.dataStart + kTimeReferenceOffset;

            char bytes[8];
            if (! detail::readAt (in, offset, bytes, 8)) return StampResult::ioError;
            const auto current = detail::le64 (bytes);
            if (current == value) return StampResult::ok;
            if (current != expected) return StampResult::unexpectedValue;
        }

        const auto sizeBefore = wav.getSize();
        {
            juce::FileOutputStream out (wav);
            if (! out.openedOk() || ! out.setPosition (offset)) return StampResult::ioError;
            const auto littleEndian = juce::ByteOrder::swapIfBigEndian (value);
            if (! out.write (&littleEndian, 8)) return StampResult::ioError;
            out.flush();   // includes fsync: the hash that follows must describe the disk
            if (! out.getStatus().wasOk()) return StampResult::ioError;
        }

        juce::uint64 written = 0;
        if (readTimeReference (wav, written) != StampResult::ok
            || written != value || wav.getSize() != sizeBefore)
            return StampResult::ioError;
        return StampResult::ok;
    }
}
