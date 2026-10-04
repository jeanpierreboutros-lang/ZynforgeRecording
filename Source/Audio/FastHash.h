#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
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
    // Optional cooperative read policy for whole-session background scans.
    // The predicate is checked between reads, including after each yield, so a
    // superseded report stops consuming disk bandwidth part-way through a file.
    struct ReadPolicy
    {
        std::function<bool()> shouldContinue;
        int pausePerChunkMs = 0; // yield after each 4 MiB, not each tiny SHA read
    };

    class HashInputStream final : public juce::InputStream
    {
    public:
        HashInputStream (juce::FileInputStream& sourceIn, const std::atomic<bool>* cancelIn,
                         const ReadPolicy& policyIn)
            : source (sourceIn), cancel (cancelIn), policy (policyIn) {}

        juce::int64 getTotalLength() override { return source.getTotalLength(); }
        juce::int64 getPosition() override { return source.getPosition(); }
        bool setPosition (juce::int64 pos) override { return source.setPosition (pos); }
        bool isExhausted() override { return failed || source.isExhausted(); }

        int read (void* dest, int count) override
        {
            if (failed || ! canContinue()) { failed = true; return 0; }
            if (bytesSinceYield >= (1 << 22) && policy.pausePerChunkMs > 0
                && ! source.isExhausted())
            {
                juce::Thread::sleep (policy.pausePerChunkMs);
                bytesSinceYield = 0;
                if (! canContinue()) { failed = true; return 0; }
            }
            const int n = source.read (dest, count);
            if (n < 0 || (n == 0 && ! source.isExhausted()))
            { failed = true; return 0; }
            bytesSinceYield += n;
            return n;
        }

        bool hasFailed() const { return failed; }

    private:
        bool canContinue() const
        {
            return (cancel == nullptr || ! cancel->load (std::memory_order_relaxed))
                && (! policy.shouldContinue || policy.shouldContinue());
        }
        juce::FileInputStream& source;
        const std::atomic<bool>* cancel;
        const ReadPolicy& policy;
        juce::int64 bytesSinceYield = 0;
        bool failed = false;
    };

    inline juce::String fileSha256 (const juce::File& f,
                                  const std::atomic<bool>* cancel = nullptr,
                                  const ReadPolicy& policy = {})
    {
        if (! f.existsAsFile()) return {};
        juce::FileInputStream source (f);
        if (! source.openedOk()) return {};
        HashInputStream in (source, cancel, policy);

       #if JUCE_MAC
        CC_SHA256_CTX ctx;
        CC_SHA256_Init (&ctx);
        constexpr int bufBytes = 1 << 22;            // 4 MiB reads -> few syscalls
        juce::HeapBlock<char> buf (bufBytes);
        for (;;)
        {
            if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) return {};
            const int n = in.read (buf.getData(), bufBytes);
            if (in.hasFailed()) return {};                      // read error -> fail, don't hash a partial prefix
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
        if (cancel != nullptr && cancel->load (std::memory_order_relaxed)) return {};
        const juce::SHA256 digest (in);
        return in.hasFailed() ? juce::String() : digest.toHexString();
       #endif
    }
}
