#pragma once

#include <juce_core/juce_core.h>
#include <memory>

#if JUCE_MAC
 #include <cerrno>
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

// A file input stream for whole-file passes that bring their own large buffer:
// the post-STOP waveform scans and the SHA-256 pass. It reads past the page
// cache (F_NOCACHE), or, where a volume refuses that, with read-ahead off.
//
// After a 3-hour, 54-track take on an external exFAT drive (macOS 26, FSKit),
// the four scan threads and the hash pass read about 2.7 GB/s from the drive
// while the scans advanced a few MB/s, for hours. A reader cycling 512 KiB
// reads over the 55 files of a take the same way reproduced it with the app
// closed: 66x the bytes it asked for, 39 MiB/s of audio. The same reads with
// read-ahead off cost 0.96x and moved 1.5 GiB/s; uncached, 1.00x and 1.7 GiB/s.
// The kernel's read-ahead for that many streams no longer fits in memory and is
// thrown away before it is used. Playback and short reads keep the cache.
namespace zynforge::uncached
{
    class FileInput final : public juce::InputStream
    {
    public:
        // Null when the file cannot be opened for reading.
        static std::unique_ptr<FileInput> open (const juce::File& file)
        {
           #if JUCE_MAC
            const int fd = ::open (file.getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) return nullptr;
            struct stat info {};
            if (::fstat (fd, &info) != 0 || ! S_ISREG (info.st_mode)) { ::close (fd); return nullptr; }
            const bool bypassed = ::fcntl (fd, F_NOCACHE, 1) != -1;
            const bool noReadAhead = ! bypassed && ::fcntl (fd, F_RDAHEAD, 0) != -1;
            return std::unique_ptr<FileInput> (new FileInput (fd, bypassed, noReadAhead));
           #else
            auto stream = std::make_unique<juce::FileInputStream> (file);
            if (stream->failedToOpen()) return nullptr;
            return std::unique_ptr<FileInput> (new FileInput (std::move (stream)));
           #endif
        }

       #if JUCE_MAC
        ~FileInput() override { ::close (fd); }

        // The file's size now, as juce::FileInputStream reports it.
        juce::int64 getTotalLength() override
        {
            struct stat info {};
            return ::fstat (fd, &info) == 0 ? (juce::int64) info.st_size : position;
        }

        int read (void* destination, int bytes) override
        {
            if (bytes <= 0) return 0;
            auto* dest = static_cast<char*> (destination);
            int got = 0;
            while (got < bytes)
            {
                const auto n = ::pread (fd, dest + got, (size_t) (bytes - got), (off_t) (position + got));
                if (n < 0)
                {
                    if (errno == EINTR) continue;
                    failed = true;
                    break;
                }
                if (n == 0) break;
                got += (int) n;
            }
            position += got;
            return got;
        }

        bool isExhausted() override        { return position >= getTotalLength(); }
        juce::int64 getPosition() override { return position; }
        bool setPosition (juce::int64 newPosition) override
        {
            position = juce::jmax ((juce::int64) 0, newPosition);
            return true;
        }
       #else
        juce::int64 getTotalLength() override { return stream->getTotalLength(); }
        int read (void* destination, int bytes) override
        {
            const int n = stream->read (destination, bytes);
            failed = failed || stream->getStatus().failed();
            return n;
        }
        bool isExhausted() override        { return stream->isExhausted(); }
        juce::int64 getPosition() override { return stream->getPosition(); }
        bool setPosition (juce::int64 newPosition) override { return stream->setPosition (newPosition); }
       #endif

        // A read the system refused (not the end of the file).
        bool hasFailed() const noexcept { return failed; }
        // Which way the page cache is kept out, for tests and diagnostics.
        bool bypassesCache() const noexcept { return bypassed; }
        bool readAheadOff() const noexcept  { return noReadAhead; }

    private:
       #if JUCE_MAC
        FileInput (int handle, bool bypass, bool rdAheadOff) : fd (handle), bypassed (bypass), noReadAhead (rdAheadOff) {}
        int fd;
        juce::int64 position { 0 };
       #else
        explicit FileInput (std::unique_ptr<juce::FileInputStream> s) : stream (std::move (s)) {}
        std::unique_ptr<juce::FileInputStream> stream;
       #endif
        bool bypassed { false }, noReadAhead { false }, failed { false };
    };
}
