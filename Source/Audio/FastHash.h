#pragma once

#include <juce_core/juce_core.h>
#if JUCE_MAC
 #include <CommonCrypto/CommonDigest.h>
#endif

// Fast SHA-256 of a file. On macOS this uses CommonCrypto's CC_SHA256, which
// runs on the CPU's hardware SHA extensions (ARMv8 crypto / SHA-NI) -- many
// times faster than a portable software SHA-256, so a multi-GB integrity pass
// becomes disk-bound instead of CPU-bound. Output is lowercase 64-hex,
// identical to `shasum -a 256` and to juce::SHA256::toHexString(), so the
// manifest stays verifiable with standard tools. Falls back to juce::SHA256
// off macOS.
namespace zynforge::hashing
{
    inline juce::String fileSha256 (const juce::File& f)
    {
        if (! f.existsAsFile()) return {};
        juce::FileInputStream in (f);
        if (! in.openedOk()) return {};

       #if JUCE_MAC
        CC_SHA256_CTX ctx;
        CC_SHA256_Init (&ctx);
        constexpr int bufBytes = 1 << 22;            // 4 MiB reads -> few syscalls
        juce::HeapBlock<char> buf (bufBytes);
        for (;;)
        {
            const int n = in.read (buf.getData(), bufBytes);
            if (n < 0) return {};                      // read error -> fail, don't hash a partial prefix
            if (n == 0)
            {
                if (! in.isExhausted()) return {};     // 0 bytes but not at EOF -> mid-file I/O error
                break;                                 // clean EOF
            }
            CC_SHA256_Update (&ctx, buf.getData(), (CC_LONG) n);
        }
        unsigned char out[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final (out, &ctx);

        juce::String hex;
        hex.preallocateBytes (CC_SHA256_DIGEST_LENGTH * 2 + 1);
        for (auto b : out) hex += juce::String::formatted ("%02x", (int) b);
        return hex;
       #else
        const juce::SHA256 digest (in);
        return digest.toHexString();
       #endif
    }
}
