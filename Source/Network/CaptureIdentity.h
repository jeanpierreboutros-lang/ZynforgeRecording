#pragma once

#include "SecureToken.h"
#include <juce_cryptography/juce_cryptography.h>
#include <array>
#if JUCE_MAC || JUCE_LINUX
 #include <fcntl.h>
 #include <sys/file.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace zynforge::capture::identity
{
    // HMAC-SHA256 (RFC 2104). The capability never travels on the socket.
    inline juce::String authenticate (const juce::String& key, const juce::String& message)
    {
        std::array<unsigned char, 64> inner {}, outer {};
        juce::MemoryBlock material (key.toRawUTF8(), key.getNumBytesAsUTF8());
        if (material.getSize() > inner.size())
            material = juce::SHA256 (material).getRawData();
        for (size_t i = 0; i < inner.size(); ++i)
        {
            const auto byte = i < material.getSize() ? ((const unsigned char*) material.getData())[i] : 0;
            inner[i] = (unsigned char) (byte ^ 0x36);
            outer[i] = (unsigned char) (byte ^ 0x5c);
        }
        juce::MemoryBlock first (inner.data(), inner.size());
        first.append (message.toRawUTF8(), message.getNumBytesAsUTF8());
        const auto digest = juce::SHA256 (first).getRawData();
        juce::MemoryBlock second (outer.data(), outer.size());
        second.append (digest.getData(), digest.getSize());
        return juce::SHA256 (second).toHexString();
    }

    inline bool equalProof (const juce::String& a, const juce::String& b)
    {
        if (a.length() != 64 || b.length() != 64) return false;
        unsigned int difference = 0;
        for (int i = 0; i < 64; ++i) difference |= (unsigned int) (a[i] ^ b[i]);
        return difference == 0;
    }

    inline bool validNonce (const juce::String& nonce)
    { return nonce.length() == 64 && nonce.containsOnly ("0123456789abcdef"); }

    // One capability per listening endpoint, readable only by its OS user.
    // A held file lock protects restart/cleanup; old files from a crashed
    // process are replaced only after obtaining that lock. No path is followed
    // through a symlink, and ownership/mode/link count are checked on open fds.
    class Endpoint final
    {
    public:
        Endpoint() = default;
        ~Endpoint() { reset(); }
        bool create (int port)
        {
            reset();
           #if JUCE_MAC || JUCE_LINUX
            directory = openDirectory (true);
            if (directory < 0) return false;
            filename = juce::String (port) + ".key";
            descriptor = ::openat (directory, filename.toRawUTF8(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
            struct stat status {};
            if (descriptor < 0 || ::fstat (descriptor, &status) != 0 || ! validFile (status)
                || ::flock (descriptor, LOCK_EX | LOCK_NB) != 0)
            { reset(); return false; }
            ownsFile = true;
            key = securetoken::generate();
            if (! validNonce (key) || ::ftruncate (descriptor, 0) != 0
                || ::pwrite (descriptor, key.toRawUTF8(), 64, 0) != 64)
            { reset(); return false; }
            return true;
           #else
            juce::ignoreUnused (port);
            return false;
           #endif
        }

        const juce::String& secret() const noexcept { return key; }

        void reset()
        {
           #if JUCE_MAC || JUCE_LINUX
            if (ownsFile && directory >= 0 && descriptor >= 0)
            {
                struct stat opened {}, named {};
                if (::fstat (descriptor, &opened) == 0
                    && ::fstatat (directory, filename.toRawUTF8(), &named, AT_SYMLINK_NOFOLLOW) == 0
                    && opened.st_dev == named.st_dev && opened.st_ino == named.st_ino)
                    ::unlinkat (directory, filename.toRawUTF8(), 0);
            }
            if (descriptor >= 0) ::close (descriptor);
            if (directory >= 0) ::close (directory);
           #endif
            descriptor = directory = -1;
            ownsFile = false;
            key.clear();
        }

        static juce::String load (int port)
        {
           #if JUCE_MAC || JUCE_LINUX
            const int folder = openDirectory (false);
            if (folder < 0) return {};
            const juce::ScopeGuard closeFolder { [folder] { ::close (folder); } };
            const auto name = juce::String (port) + ".key";
            const int file = ::openat (folder, name.toRawUTF8(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (file < 0) return {};
            const juce::ScopeGuard closeFile { [file] { ::close (file); } };
            struct stat status {};
            std::array<char, 64> bytes {};
            if (::fstat (file, &status) != 0 || ! validFile (status) || status.st_size != 64
                || ::pread (file, bytes.data(), bytes.size(), 0) != (ssize_t) bytes.size()) return {};
            const auto result = juce::String::fromUTF8 (bytes.data(), (int) bytes.size());
            return validNonce (result) ? result : juce::String();
           #else
            juce::ignoreUnused (port);
            return {};
           #endif
        }

    private:
       #if JUCE_MAC || JUCE_LINUX
        static bool validFile (const struct stat& status)
        { return S_ISREG (status.st_mode) && status.st_uid == ::geteuid()
                 && (status.st_mode & 0777) == 0600 && status.st_nlink == 1; }

        static int openDirectory (bool create)
        {
            const auto path = juce::String ("/tmp/zynforge-capture-") + juce::String ((int) ::geteuid());
            if (create && ::mkdir (path.toRawUTF8(), 0700) != 0 && errno != EEXIST) return -1;
            const int fd = ::open (path.toRawUTF8(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) return -1;
            struct stat status {};
            if (::fstat (fd, &status) == 0 && S_ISDIR (status.st_mode)
                && status.st_uid == ::geteuid() && (status.st_mode & 0777) == 0700) return fd;
            ::close (fd);
            return -1;
        }
       #endif
        int directory { -1 }, descriptor { -1 };
        bool ownsFile { false };
        juce::String key, filename;
        JUCE_DECLARE_NON_COPYABLE (Endpoint)
    };
}
