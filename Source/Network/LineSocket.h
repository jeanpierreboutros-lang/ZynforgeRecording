#pragma once

#include <juce_core/juce_core.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#if JUCE_MAC || JUCE_LINUX
 #include <fcntl.h>
 #include <poll.h>
 #include <sys/socket.h>
#endif

// Newline-framed socket helpers shared by the capture link and the twin link,
// plus the JSON nesting guard every network parser runs first.
namespace zynforge::linesocket
{
    using Clock = std::chrono::steady_clock;

    inline constexpr auto   kDefaultWriteBudget = std::chrono::milliseconds (250);
    inline constexpr size_t kMaxLineBytes       = 1u << 20;   // 1 MiB
    inline constexpr int    kMaxJsonDepth       = 16;

    // juce::JSON::parse recurses once per '[' / '{' with no depth limit, so
    // a 1 MB body of '[' overflowed a worker's stack and took the app down.
    // A command is one flat object; refuse deeper nesting before parsing.
    inline bool jsonDepthWithin (const juce::String& text, int maxDepth)
    {
        int depth = 0;
        bool inString = false, escaped = false;
        for (auto p = text.getCharPointer(); ! p.isEmpty(); ++p)
        {
            const auto c = *p;
            if (inString)
            {
                if (escaped)        escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"')  inString = false;
            }
            else if (c == '"')             inString = true;
            else if (c == '[' || c == '{') { if (++depth > maxDepth) return false; }
            else if (c == ']' || c == '}') depth = juce::jmax (0, depth - 1);
        }
        return true;
    }

    // Read available bytes, accumulate them RAW in `scratch`, and dispatch
    // every COMPLETE newline-terminated line through `onLine`. Buffering at
    // the BYTE level (not the decoded-String level) means a multibyte UTF-8
    // sequence that straddles a 4096-byte read boundary stays intact until
    // its whole line arrives, then decodes correctly -- ASCII is unaffected.
    // Returns false when the socket should be torn down (peer closed /
    // error / oversized line). Blocks up to `waitMs` per call so the caller
    // can re-check its run flag.
    inline bool pumpLines (juce::StreamingSocket& s, juce::MemoryBlock& scratch,
                           const std::function<void (const juce::String&)>& onLine,
                           int waitMs = 200, size_t maxLineBytes = kMaxLineBytes)
    {
        const int ready = s.waitUntilReady (true, waitMs);
        if (ready < 0) return false;       // error
        if (ready == 0) return true;       // timeout -- caller re-checks run flag

        char buf[4096];
        const int got = s.read (buf, sizeof (buf), false);
        if (got <= 0) return false;        // 0 = peer closed cleanly; <0 = error

        scratch.append (buf, (size_t) got);

        // Split on '\n' at the byte level; decode only complete lines.
        const auto* base = static_cast<const char*> (scratch.getData());
        const size_t size = scratch.getSize();
        size_t start = 0;
        for (size_t i = 0; i < size; ++i)
        {
            if (base[i] != '\n') continue;
            // Validate complete lines before decoding or dispatching too:
            // the final read may contain both the excess byte and newline.
            if (i - start > maxLineBytes) return false;
            const auto line = juce::String::fromUTF8 (base + start, (int) (i - start)).trim();
            start = i + 1;
            if (line.isNotEmpty()) onLine (line);
        }

        // Keep the unterminated remainder (partial line / partial multibyte
        // sequence) for the next read.
        if (start > 0)
        {
            const size_t remaining = size - start;
            juce::MemoryBlock rest;
            if (remaining > 0) rest.append (base + start, remaining);
            scratch = std::move (rest);
        }

        // A line that never terminates must not grow the buffer forever.
        if (scratch.getSize() > maxLineBytes) return false;
        return true;
    }

    // Native poll/nonblocking send avoids JUCE's shared read mutex. The
    // same absolute deadline covers lock acquisition, partial sends and
    // the reply; a dozing peer cannot pin the caller indefinitely.
    // All callers serialize socket close with this socket's write lock.
    inline bool writeAll (juce::StreamingSocket& s, const juce::String& line,
                          Clock::time_point deadline = Clock::now() + kDefaultWriteBudget)
    {
       #if JUCE_MAC || JUCE_LINUX
        // MSG_DONTWAIT alone did not bound send() on a fresh macOS socket:
        // a non-reading peer held it beyond the request deadline. Keep the
        // descriptor itself nonblocking, preserving all other status flags.
        // JUCE read(false) uses the same mode; no reader switches it back.
        // Socket close is excluded by the caller's write lock or exclusive
        // ownership of a not-yet-published handshake socket.
        const int handle = s.getRawSocketHandle();
        const int statusFlags = ::fcntl (handle, F_GETFL, 0);
        if (statusFlags < 0
            || ((statusFlags & O_NONBLOCK) == 0
                && ::fcntl (handle, F_SETFL, statusFlags | O_NONBLOCK) < 0))
            return false;
       #endif
        const auto utf8 = line.toRawUTF8();
        const int total = (int) std::strlen (utf8);
        int sent = 0;
        while (sent < total)
        {
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) return false;
           #if JUCE_MAC || JUCE_LINUX
            pollfd descriptor { s.getRawSocketHandle(), POLLOUT, 0 };
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds> (remaining).count();
            const int ready = ::poll (&descriptor, 1, (int) juce::jmax<int64_t> (1, milliseconds));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                return false;
            int flags = MSG_DONTWAIT;
           #ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
           #endif
            const int n = (int) ::send (descriptor.fd, utf8 + sent, (size_t) (total - sent), flags);
           #else
            const int n = s.write (utf8 + sent, total - sent);
           #endif
            if (n <= 0)
            {
               #if JUCE_MAC || JUCE_LINUX
                if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                    continue;
               #endif
                return false;
            }
            sent += n;
        }
        return true;
    }
}
