#pragma once

#include "SecureToken.h"
#include <juce_cryptography/juce_cryptography.h>
#include <array>
#include <filesystem>
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
            claimsName = juce::String (port) + ".claims";
            const int claims = ::openat (directory, claimsName.toRawUTF8(),
                                         O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (claims >= 0) ::close (claims);
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
                ::unlinkat (directory, claimsName.toRawUTF8(), 0);
            }
            if (descriptor >= 0) ::close (descriptor);
            if (directory >= 0) ::close (directory);
           #endif
            descriptor = directory = -1;
            ownsFile = false;
            key.clear();
        }

        // A descriptor on a file in the same private folder, for a lock the
        // GUI and the daemon share. -1 if it can't be opened.
        static int openSharedFile (const char* name, bool create = true)
        {
           #if JUCE_MAC || JUCE_LINUX
            const int folder = openDirectory (create);
            if (folder < 0) return -1;
            const int file = ::openat (folder, name, O_RDWR | (create ? O_CREAT : 0) | O_NOFOLLOW | O_CLOEXEC, 0600);
            ::close (folder);
            return file;
           #else
            juce::ignoreUnused (name, create);
            return -1;
           #endif
        }

        // Someone holds this file's lock. The kernel drops one when its process
        // ends, however it ends, so the answer needs nothing from that process.
        static bool lockIsHeld (const char* name)
        {
           #if JUCE_MAC || JUCE_LINUX
            const int file = openSharedFile (name, false);
            if (file < 0) return false;
            const bool held = ::flock (file, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
            ::close (file);
            return held;
           #else
            juce::ignoreUnused (name);
            return false;
           #endif
        }

        // A helper is alive on this port and publishes no TakeClaim: a build from
        // before they existed, so nothing says which session it is writing.
        static bool helperWithoutClaims (int port)
        {
            if (! lockIsHeld ((juce::String (port) + ".key").toRawUTF8())) return false;
            const int claims = openSharedFile ((juce::String (port) + ".claims").toRawUTF8(), false);
            if (claims < 0) return true;
           #if JUCE_MAC || JUCE_LINUX
            ::close (claims);
           #endif
            return false;
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
        juce::String key, filename, claimsName;
        JUCE_DECLARE_NON_COPYABLE (Endpoint)
    };

    // One flock per session folder in the private capture folder, held while a
    // process works on that session. The kernel drops it when the process
    // ends, however it ends, so another process can ask with no help from it.
    class SessionClaim
    {
    public:
        ~SessionClaim() { release(); }

        // False when someone else kept it through the brief retry.
        bool hold (const juce::File& session)
        {
            release();
           #if JUCE_MAC || JUCE_LINUX
            descriptor = Endpoint::openSharedFile (nameFor (kind, session).toRawUTF8());
            // Only a probe holds it otherwise, for an instant.
            for (int attempt = 0; descriptor >= 0 && attempt < 50; ++attempt)
            {
                if (::flock (descriptor, LOCK_EX | LOCK_NB) == 0) return true;
                juce::Thread::sleep (1);
            }
            release();
           #else
            juce::ignoreUnused (session);
           #endif
            return false;
        }

        void release()
        {
           #if JUCE_MAC || JUCE_LINUX
            if (descriptor >= 0) ::close (descriptor);   // closing releases it
           #endif
            descriptor = -1;
        }

    protected:
        explicit SessionClaim (const char* kindIn) : kind (kindIn) {}

        static bool heldAt (const char* kind, const juce::File& session)
        {
            return Endpoint::lockIsHeld (nameFor (kind, session).toRawUTF8());
        }

    private:
        // One folder has one name here, however the path was spelled.
        static juce::String nameFor (const char* kind, const juce::File& session)
        {
            std::error_code error;
            const auto path = std::filesystem::weakly_canonical (session.getFullPathName().toStdString(), error);
            const auto text = error ? session.getFullPathName() : juce::String (path.string());
            return juce::String (kind) + "-" + juce::SHA256 (text.toUTF8()).toHexString().substring (0, 32) + ".lock";
        }

        const char* kind;
        int descriptor { -1 };
        JUCE_DECLARE_NON_COPYABLE (SessionClaim)
    };

    // A capture helper holds this from before it touches a session until that
    // session's files are final. A window can then tell a live session from a
    // crashed one with no help from the helper: a handshake can fail, or never
    // be tried, while the helper goes on writing.
    class TakeClaim final : public SessionClaim
    {
    public:
        TakeClaim() : SessionClaim ("take") {}
        static bool isHeld (const juce::File& session) { return heldAt ("take", session); }
    };

    // A hash pass over a session's report holds this for as long as it reads.
    // The count of running passes is per process: a helper's pass after STOP
    // and a window's resumed one would otherwise read every take file twice,
    // side by side, on the same drive.
    class ScanClaim final : public SessionClaim
    {
    public:
        ScanClaim() : SessionClaim ("scan") {}
        static bool isHeld (const juce::File& session) { return heldAt ("scan", session); }
    };
}
