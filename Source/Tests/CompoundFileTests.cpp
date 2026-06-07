// Round-trip validator for the MS-CFB container writer. Builds a compound
// file with nested storages + a big stream (FAT chain), a small stream and a
// nested small stream (mini-FAT), and an empty stream, then parses the bytes
// back with a self-contained reader (the oracle) and asserts everything
// survives. This is how we de-risk the binary layer without a DAW: the writer
// is correct iff an independent reader reconstructs the exact tree + bytes.

#include <juce_core/juce_core.h>
#include <map>
#include <set>
#include "../Audio/Aaf/CompoundFile.h"

namespace zynforge
{
    // ── Minimal independent CFB reader used only by this test ───────────────
    struct CfbReader
    {
        const juce::uint8* base { nullptr };
        size_t size { 0 };
        std::vector<juce::uint32> fat, miniFat;
        juce::MemoryBlock miniStream;
        std::map<juce::String, juce::MemoryBlock> streams;
        std::set<juce::String> storages;

        struct Entry { juce::String name; int type; juce::uint32 left, right, child, start; juce::uint64 sz; };
        std::vector<Entry> entries;

        static juce::uint16 u16 (const juce::uint8* p) { return (juce::uint16) (p[0] | (p[1] << 8)); }
        static juce::uint32 u32 (const juce::uint8* p)
        { return (juce::uint32) (p[0] | (p[1] << 8) | (p[2] << 16) | ((juce::uint32) p[3] << 24)); }
        static juce::uint64 u64 (const juce::uint8* p)
        { juce::uint64 v = 0; for (int i = 0; i < 8; ++i) v |= (juce::uint64) p[i] << (8 * i); return v; }

        const juce::uint8* sector (juce::uint32 idx) const { return base + 512 + (size_t) idx * 512; }

        juce::MemoryBlock readChain (juce::uint32 start) const
        {
            juce::MemoryBlock mb;
            juce::uint32 s = start; int guard = 0;
            while (s != cfb::ENDOFCHAIN && s != cfb::FREESECT && s < fat.size() && guard++ < 1'000'000)
            { mb.append (sector (s), 512); s = fat[s]; }
            return mb;
        }
        juce::MemoryBlock readMiniChain (juce::uint32 start, juce::uint64 sz) const
        {
            juce::MemoryBlock mb;
            juce::uint32 s = start; int guard = 0;
            while (s != cfb::ENDOFCHAIN && s != cfb::FREESECT && s < miniFat.size() && guard++ < 1'000'000)
            { mb.append ((const juce::uint8*) miniStream.getData() + (size_t) s * 64, 64); s = miniFat[s]; }
            mb.setSize ((size_t) sz);
            return mb;
        }

        bool parse (const juce::MemoryBlock& file)
        {
            base = (const juce::uint8*) file.getData(); size = file.getSize();
            if (size < 512) return false;
            static const juce::uint8 sig[8] = { 0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1 };
            if (std::memcmp (base, sig, 8) != 0) return false;

            const auto numFat   = u32 (base + 44);
            const auto firstDir = u32 (base + 48);
            const auto firstMin = u32 (base + 60);

            for (juce::uint32 i = 0; i < numFat && i < 109; ++i)
            {
                const auto fs = u32 (base + 76 + i * 4);
                if (fs == cfb::FREESECT) break;
                const auto* p = sector (fs);
                for (int j = 0; j < 128; ++j) fat.push_back (u32 (p + j * 4));
            }

            const auto dirBytes = readChain (firstDir);
            const auto* d = (const juce::uint8*) dirBytes.getData();
            const auto n = dirBytes.getSize() / 128;
            for (size_t i = 0; i < n; ++i)
            {
                const auto* p = d + i * 128;
                const int nameLen = u16 (p + 64);
                juce::String name;
                for (int c = 0; c * 2 + 1 < juce::jmax (0, nameLen - 1); ++c)
                    name += (juce::juce_wchar) u16 (p + c * 2);
                entries.push_back ({ name, p[66], u32 (p + 68), u32 (p + 72),
                                     u32 (p + 76), u32 (p + 116), u64 (p + 120) });
            }
            if (entries.empty()) return false;

            // Mini stream lives in the Root Entry's chain; mini-FAT is its own chain.
            const auto& rootE = entries[0];
            if (rootE.start != cfb::ENDOFCHAIN) { miniStream = readChain (rootE.start); miniStream.setSize ((size_t) rootE.sz); }
            if (firstMin != cfb::ENDOFCHAIN)
            {
                const auto mb = readChain (firstMin);
                const auto* p = (const juce::uint8*) mb.getData();
                for (size_t i = 0; i < mb.getSize() / 4; ++i) miniFat.push_back (u32 (p + i * 4));
            }

            walk (entries[0].child, {});
            return true;
        }

        juce::MemoryBlock streamBytes (const Entry& e) const
        {
            if (e.sz == 0) return {};
            if (e.sz < cfb::kMiniCutoff) return readMiniChain (e.start, e.sz);
            auto mb = readChain (e.start); mb.setSize ((size_t) e.sz); return mb;
        }

        void walk (juce::uint32 id, const juce::String& prefix)
        {
            if (id == cfb::NOSTREAM) return;
            const auto& e = entries[id];
            walk (e.left, prefix);
            const auto path = prefix + "/" + e.name;
            if (e.type == 2) streams[path] = streamBytes (e);
            else { storages.insert (path); if (e.child != cfb::NOSTREAM) walk (e.child, path); }
            walk (e.right, prefix);
        }
    };

    class CompoundFileTests final : public juce::UnitTest
    {
    public:
        CompoundFileTests() : UnitTest ("Compound file (CFB)", "zynforge") {}

        static juce::MemoryBlock pattern (size_t n, juce::uint8 seed)
        {
            juce::MemoryBlock mb (n);
            auto* p = (juce::uint8*) mb.getData();
            for (size_t i = 0; i < n; ++i) p[i] = (juce::uint8) ((i * 31 + seed) & 0xFF);
            return mb;
        }

        void runTest() override
        {
            beginTest ("Streams + nested storages round-trip through FAT and mini-FAT");

            const auto big   = pattern (5000, 1);   // >= 4096 -> FAT chain
            const auto small = pattern (100,  2);    // <  4096 -> mini-FAT
            const auto inner = pattern (70,   3);    // <  4096 -> mini-FAT, nested

            cfb::CompoundFileWriter w;
            w.root().addStream ("Big",   big);
            w.root().addStream ("Small", small);
            w.root().addStream ("Empty", nullptr, 0);
            auto* stg = w.root().addStorage ("Stg");
            stg->addStream ("Inner", inner);

            const auto bytes = w.build();
            expect (bytes.getSize() % 512 == 0, "file is not a whole number of sectors");
            expect (bytes.getSize() >= 512 * 4, "file unexpectedly tiny");

            CfbReader r;
            expect (r.parse (bytes), "independent reader could not parse the container");

            // The big + small + empty + nested streams all survive byte-exact.
            expect (r.streams.count ("/Big")   == 1, "Big missing");
            expect (r.streams.count ("/Small") == 1, "Small missing");
            expect (r.streams.count ("/Empty") == 1, "Empty missing");
            expect (r.storages.count ("/Stg")  == 1, "Stg storage missing");
            expect (r.streams.count ("/Stg/Inner") == 1, "nested Inner missing");

            expect (r.streams["/Big"]   == big,   "Big bytes differ");
            expect (r.streams["/Small"] == small, "Small bytes differ");
            expect (r.streams["/Empty"].getSize() == 0, "Empty not empty");
            expect (r.streams["/Stg/Inner"] == inner, "nested Inner bytes differ");
        }
    };

    static CompoundFileTests compoundFileTests;
}
