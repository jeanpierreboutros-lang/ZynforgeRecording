// Validates the hardware-accelerated file hash (FastHash.h) against the
// canonical SHA-256 test vectors AND against JUCE's portable SHA256, so we
// know the faster CC_SHA256 path produces byte-identical, `shasum`-verifiable
// digests before it goes anywhere near the integrity manifest.

#include <juce_cryptography/juce_cryptography.h>
#include "../Audio/FastHash.h"

namespace zynforge
{
    class FastHashTests final : public juce::UnitTest
    {
    public:
        FastHashTests() : UnitTest ("Fast hash", "zynforge") {}

        static juce::File writeTemp (const void* data, size_t len)
        {
            auto f = juce::File::createTempFile (".bin");
            if (len == 0) f.create();                 // replaceWithData(_, 0) is a no-op -> make a real empty file
            else          f.replaceWithData (data, len);
            return f;
        }

        void runTest() override
        {
            beginTest ("Known SHA-256 vectors");
            {
                // "abc" and the empty string -- the standard FIPS-180 vectors.
                auto fa = writeTemp ("abc", 3);
                expectEquals (hashing::fileSha256 (fa),
                    juce::String ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
                fa.deleteFile();

                auto fe = writeTemp ("", 0);
                expectEquals (hashing::fileSha256 (fe),
                    juce::String ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
                fe.deleteFile();
            }

            beginTest ("Fast hash matches juce::SHA256 across buffer boundaries");
            {
                juce::Random rng (12345);
                // Sizes straddle the 4 MiB read chunk so we exercise multi-block
                // updates, not just a single read.
                for (int sz : { 1, 64, 4096, (1 << 22) - 1, (1 << 22), (1 << 22) + 7, (1 << 23) + 123 })
                {
                    juce::MemoryBlock mb ((size_t) sz);
                    auto* p = (juce::uint8*) mb.getData();
                    for (int i = 0; i < sz; ++i) p[i] = (juce::uint8) rng.nextInt (256);

                    auto f = writeTemp (mb.getData(), mb.getSize());
                    const auto fast = hashing::fileSha256 (f);

                    juce::FileInputStream in (f);
                    const juce::String ref = juce::SHA256 (in).toHexString();
                    f.deleteFile();

                    expectEquals (fast, ref, "fast hash != juce::SHA256 at size " + juce::String (sz));
                    expectEquals (fast.length(), 64, "digest is not 64 hex chars");
                }
            }

            beginTest ("Missing file hashes to empty string");
            {
                expect (hashing::fileSha256 (juce::File ("/no/such/file_zynforge.bin")).isEmpty());
            }
        }
    };

    static FastHashTests fastHashTests;
}
