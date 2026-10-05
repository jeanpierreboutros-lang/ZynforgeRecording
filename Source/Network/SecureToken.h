#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstddef>
#include <functional>

#if JUCE_MAC || JUCE_IOS
 #include <Security/SecRandom.h>
#elif JUCE_LINUX || JUCE_ANDROID
 #include <cerrno>
 #include <sys/random.h>
#endif

namespace zynforge::securetoken
{
    using EntropyProvider = std::function<bool(void*, size_t)>;

    inline bool fillOSRandom (void* destination, size_t size)
    {
        if (destination == nullptr && size != 0) return false;
       #if JUCE_MAC || JUCE_IOS
        return SecRandomCopyBytes (kSecRandomDefault, size, destination) == errSecSuccess;
       #elif JUCE_LINUX || JUCE_ANDROID
        auto* bytes = static_cast<unsigned char*> (destination);
        while (size != 0)
        {
            const auto count = ::getrandom (bytes, size, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            bytes += count;
            size -= static_cast<size_t> (count);
        }
        return true;
       #else
        // Unsupported platforms must add an OS CSPRNG, never a PRNG fallback.
        juce::ignoreUnused (destination, size);
        return false;
       #endif
    }

    inline juce::String generate (const EntropyProvider& provider = {})
    {
        std::array<unsigned char, 32> bytes {};
        if (! (provider ? provider (bytes.data(), bytes.size())
                        : fillOSRandom (bytes.data(), bytes.size())))
            return {};
        return juce::String::toHexString (bytes.data(), static_cast<int> (bytes.size()), 0);
    }
}
