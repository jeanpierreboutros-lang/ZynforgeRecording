// Validates the hardware-accelerated file hash (FastHash.h) against the
// canonical SHA-256 test vectors AND against JUCE's portable SHA256, so we
// know the faster CC_SHA256 path produces byte-identical, `shasum`-verifiable
// digests before it goes anywhere near the integrity manifest.

#include <juce_cryptography/juce_cryptography.h>
#include "../Audio/FastHash.h"
#include "../Audio/ScanInput.h"
#include "../Audio/UncachedFileInput.h"

#include <cstring>

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

            beginTest ("Yielding background reads retain the complete digest");
            {
                juce::MemoryBlock data ((1 << 22) + 123, true);
                auto f = writeTemp (data.getData(), data.getSize());
                hashing::ReadPolicy policy { [] { return true; }, 1 };
                expectEquals (hashing::fileSha256 (f, nullptr, policy),
                              hashing::fileSha256 (f));
                f.deleteFile();
            }

            beginTest ("Superseded scan cancels inside a file without publishing a prefix hash");
            {
                juce::MemoryBlock data ((1 << 23) + 123, true);
                auto f = writeTemp (data.getData(), data.getSize());
                int reads = 0;
                hashing::ReadPolicy policy { [&] { return ++reads < 3; }, 0 };
                expect (hashing::fileSha256 (f, nullptr, policy).isEmpty());
                expect (reads >= 3, "cancellation was not polled during the read");
                std::atomic<bool> cancel { true };
                expect (hashing::fileSha256 (f, &cancel).isEmpty());
                f.deleteFile();
            }

            beginTest ("Missing file hashes to empty string");
            {
                expect (hashing::fileSha256 (juce::File ("/no/such/file_zynforge.bin")).isEmpty());
            }

            beginTest ("Whole-file passes read past the page cache and see every byte");
            {
                juce::Random rng (777);
                const int size = (3 << 20) + 777;
                juce::MemoryBlock data ((size_t) size);
                auto* bytes = static_cast<juce::uint8*> (data.getData());
                for (int i = 0; i < size; ++i) bytes[i] = (juce::uint8) rng.nextInt (256);
                auto f = writeTemp (data.getData(), data.getSize());

                auto in = uncached::FileInput::open (f);
                expect (in != nullptr, "the file did not open");
                if (in != nullptr)
                {
                   #if JUCE_MAC
                    expect (in->bypassesCache() || in->readAheadOff(),
                            "the read-ahead of a session's takes is thrown away before use");
                   #endif
                    expectEquals (in->getTotalLength(), (juce::int64) size);
                    juce::HeapBlock<juce::uint8> got ((size_t) size);
                    int at = 0;
                    for (int chunk : { 1, 1535, 4096, 1 << 19, 3, 1 << 20, 1 << 22 })
                    {
                        const int n = in->read (got.getData() + at, juce::jmin (chunk, size - at));
                        at += n;
                        expectEquals (in->getPosition(), (juce::int64) at);
                    }
                    expectEquals (at, size, "a whole read came up short");
                    expect (std::memcmp (got.getData(), bytes, (size_t) size) == 0, "other bytes came back");
                    expect (in->isExhausted());
                    expectEquals (in->read (got.getData(), 16), 0, "a read past the end");
                    expect (! in->hasFailed(), "the end of a file is not a failure");

                    expect (in->setPosition (size / 2));
                    expectEquals (in->read (got.getData(), 4096), 4096);
                    expect (std::memcmp (got.getData(), bytes + size / 2, 4096) == 0, "a seek read other bytes");
                    in->setPosition (-5);
                    expectEquals (in->getPosition(), (juce::int64) 0);

                    // A file still growing reports its new length, and the new bytes
                    // read back at once: no stale page from before the write.
                    {
                        juce::FileOutputStream out (f);
                        expect (out.openedOk());
                        out.setPosition (size);
                        out.writeRepeatedByte (0x5a, 1000);
                        out.flush();
                    }
                    expectEquals (in->getTotalLength(), (juce::int64) size + 1000);
                    in->setPosition (size);
                    expect (! in->isExhausted());
                    expectEquals (in->read (got.getData(), 2000), 1000);
                    expect (got[0] == 0x5a && got[999] == 0x5a, "an appended block read stale");
                }

                // The scan's buffered stream over it reads the same bytes.
                {
                    auto scan = scanio::openStream (f);
                    expect (scan != nullptr);
                    juce::MemoryBlock all;
                    if (scan != nullptr) scan->readIntoMemoryBlock (all);
                    expectEquals ((int) all.getSize(), size + 1000);
                    expect (all.getSize() >= (size_t) size && std::memcmp (all.getData(), bytes, (size_t) size) == 0,
                            "the scan stream read other bytes");
                }

                expect (uncached::FileInput::open (f.getParentDirectory()) == nullptr, "a folder opened as a file");
                expect (uncached::FileInput::open (juce::File ("/no/such/file_zynforge.bin")) == nullptr);
                f.deleteFile();
            }
        }
    };

    static FastHashTests fastHashTests;
}
