#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <map>
#include <memory>
#include <vector>

// ── Compound File Binary Format (MS-CFB) writer ─────────────────────────────
//
// AAF files are stored inside a Microsoft Compound File (the same OLE2 /
// "structured storage" container as legacy .doc/.xls). This is the envelope
// layer only: a tree of storages (directories) + streams (files), serialised
// to the exact on-disk layout readers expect (512-byte sectors, FAT, mini-FAT
// for sub-4 KiB streams, a red-black directory tree). The AAF object model is
// built on top of this in a separate layer.
//
// v3 (512-byte sector) container, the universally-supported flavour. Pure +
// header-only so it's unit-testable by reading the bytes straight back.
namespace zynforge::cfb
{
    // Special FAT sector values (MS-CFB §2.2).
    constexpr juce::uint32 FREESECT   = 0xFFFFFFFFu;
    constexpr juce::uint32 ENDOFCHAIN = 0xFFFFFFFEu;
    constexpr juce::uint32 FATSECT    = 0xFFFFFFFDu;
    constexpr juce::uint32 NOSTREAM   = 0xFFFFFFFFu;

    constexpr int    kSectorSize     = 512;
    constexpr int    kMiniSectorSize = 64;
    constexpr juce::uint32 kMiniCutoff = 4096;   // streams smaller than this go in the mini stream
    constexpr int    kDirEntrySize   = 128;
    constexpr int    kDirPerSector   = kSectorSize / kDirEntrySize;   // 4

    enum class NodeType { Storage, Stream };

    // In-memory tree the caller builds; build() serialises it.
    struct Node
    {
        juce::String name;
        NodeType     type { NodeType::Storage };
        juce::MemoryBlock data;                 // streams only
        std::vector<std::unique_ptr<Node>> children;   // storages only

        Node* addStorage (const juce::String& n)
        {
            children.push_back (std::make_unique<Node> (Node { n, NodeType::Storage, {}, {} }));
            return children.back().get();
        }
        Node* addStream (const juce::String& n, const void* d, size_t len)
        {
            auto node = std::make_unique<Node> (Node { n, NodeType::Stream, {}, {} });
            node->data.append (d, len);
            children.push_back (std::move (node));
            return children.back().get();
        }
        Node* addStream (const juce::String& n, const juce::MemoryBlock& mb)
        { return addStream (n, mb.getData(), mb.getSize()); }
    };

    class CompoundFileWriter
    {
    public:
        // The implicit root storage; build a tree of storages/streams under it.
        Node& root() { return rootNode; }

        juce::MemoryBlock build()
        {
            // ── Flatten the tree into directory entries (Root = id 0) ───────
            std::vector<DirEntry> dir;
            dir.push_back ({});                       // Root Entry placeholder at id 0
            dir[0].name  = "Root Entry";
            dir[0].type  = 5;                          // root storage
            dir[0].child = NOSTREAM;
            assignChildren (rootNode, 0, dir);

            // ── Split streams: small (mini stream) vs big (regular FAT) ─────
            buildMiniStream (dir);

            sectors.clear(); fat.clear();

            // Allocate every big stream's bytes into 512-byte sectors.
            for (auto& e : dir)
            {
                if (e.type == 2 && ! e.inMini && e.bytes.getSize() > 0)
                    e.start = allocChain (e.bytes);
                else if (e.type == 2 && ! e.inMini)
                    e.start = ENDOFCHAIN;             // empty big stream
            }

            // The mini-stream container is a big stream owned by Root.
            if (miniStream.getSize() > 0)
            {
                dir[0].start = allocChain (miniStream);
                dir[0].size  = (juce::uint64) miniStream.getSize();
            }
            else { dir[0].start = ENDOFCHAIN; dir[0].size = 0; }

            // Mini-FAT as a big stream.
            juce::uint32 firstMiniFat = ENDOFCHAIN; juce::uint32 numMiniFatSectors = 0;
            if (! miniFat.empty())
            {
                juce::MemoryBlock mb (miniFat.size() * 4);
                auto* p = (juce::uint8*) mb.getData();
                for (size_t i = 0; i < miniFat.size(); ++i) putU32 (p + i * 4, miniFat[i]);
                const auto before = (juce::uint32) sectors.size();
                firstMiniFat = allocChain (mb);
                numMiniFatSectors = (juce::uint32) sectors.size() - before;
            }

            // Directory as a big stream (pad final sector with FREE entries).
            const juce::uint32 firstDirSector = allocChain (serialiseDirectory (dir));

            // ── FAT (describes ALL sectors, including itself) ───────────────
            const juce::uint32 dataSectors = (juce::uint32) sectors.size();
            juce::uint32 fatSectors = 1;
            for (;;)   // F = ceil((data + F) * 4 / sectorSize), solved iteratively
            {
                const juce::uint32 entries = dataSectors + fatSectors;
                const juce::uint32 need = (entries * 4 + kSectorSize - 1) / kSectorSize;
                if (need == fatSectors) break;
                fatSectors = need;
            }
            jassert (fatSectors <= 109);   // >109 FAT sectors would need a DIFAT chain (not hit at session scale)

            const juce::uint32 firstFatSector = (juce::uint32) sectors.size();
            for (juce::uint32 i = 0; i < fatSectors; ++i) { sectors.emplace_back(); fat.push_back (FATSECT); }
            for (juce::uint32 i = 0; i < fatSectors; ++i) fat[firstFatSector + i] = FATSECT;

            // Pad FAT to a whole number of sectors with FREESECT, then emit.
            const juce::uint32 fatCapacity = fatSectors * (kSectorSize / 4);
            std::vector<juce::uint32> fatOut = fat;
            fatOut.resize (fatCapacity, FREESECT);
            for (juce::uint32 i = 0; i < fatSectors; ++i)
            {
                auto& s = sectors[firstFatSector + i];
                for (int j = 0; j < kSectorSize / 4; ++j)
                    putU32 (s.data() + j * 4, fatOut[i * (kSectorSize / 4) + j]);
            }

            // ── Header ──────────────────────────────────────────────────────
            std::array<juce::uint8, kSectorSize> header {};
            static const juce::uint8 sig[8] = { 0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1 };
            std::memcpy (header.data(), sig, 8);
            putU16 (header.data() + 24, 0x003E);   // minor version
            putU16 (header.data() + 26, 0x0003);   // major version (v3 = 512-byte sectors)
            putU16 (header.data() + 28, 0xFFFE);   // little-endian
            putU16 (header.data() + 30, 0x0009);   // sector shift (2^9 = 512)
            putU16 (header.data() + 32, 0x0006);   // mini sector shift (2^6 = 64)
            putU32 (header.data() + 44, fatSectors);
            putU32 (header.data() + 48, firstDirSector);
            putU32 (header.data() + 56, kMiniCutoff);
            putU32 (header.data() + 60, firstMiniFat);
            putU32 (header.data() + 64, numMiniFatSectors);
            putU32 (header.data() + 68, ENDOFCHAIN);   // first DIFAT sector (none)
            putU32 (header.data() + 72, 0);            // DIFAT sector count
            for (int i = 0; i < 109; ++i)
                putU32 (header.data() + 76 + i * 4, i < (int) fatSectors ? firstFatSector + i : FREESECT);

            // ── Assemble ────────────────────────────────────────────────────
            juce::MemoryBlock out;
            out.append (header.data(), kSectorSize);
            for (auto& s : sectors) out.append (s.data(), kSectorSize);
            return out;
        }

    private:
        struct DirEntry
        {
            juce::String  name;
            int           type { 0 };          // 0 unalloc, 1 storage, 2 stream, 5 root
            juce::uint32  left { NOSTREAM }, right { NOSTREAM }, child { NOSTREAM };
            juce::uint32  start { ENDOFCHAIN };
            juce::uint64  size { 0 };
            juce::MemoryBlock bytes;            // stream payload (pre-allocation)
            bool          inMini { false };
        };

        Node rootNode { "Root Entry", NodeType::Storage, {}, {} };
        std::vector<std::array<juce::uint8, kSectorSize>> sectors;
        std::vector<juce::uint32> fat;
        juce::MemoryBlock miniStream;
        std::vector<juce::uint32> miniFat;

        static void putU16 (juce::uint8* p, juce::uint16 v) { p[0]=(juce::uint8)v; p[1]=(juce::uint8)(v>>8); }
        static void putU32 (juce::uint8* p, juce::uint32 v)
        { for (int i=0;i<4;++i) p[i]=(juce::uint8)(v>>(8*i)); }
        static void putU64 (juce::uint8* p, juce::uint64 v)
        { for (int i=0;i<8;++i) p[i]=(juce::uint8)(v>>(8*i)); }

        // CFB name order: shorter UTF-16 name first, else upper-cased ordinal.
        static bool nameLess (const juce::String& a, const juce::String& b)
        {
            if (a.length() != b.length()) return a.length() < b.length();
            return a.toUpperCase().compare (b.toUpperCase()) < 0;
        }

        // DFS the caller's tree, appending DirEntries and linking each storage's
        // children into a balanced binary search tree (a valid CFB red-black
        // tree shape; colours are all black, which readers accept).
        void assignChildren (Node& parent, juce::uint32 parentId, std::vector<DirEntry>& dir)
        {
            std::vector<Node*> kids;
            for (auto& c : parent.children) kids.push_back (c.get());
            std::sort (kids.begin(), kids.end(),
                       [] (Node* x, Node* y) { return nameLess (x->name, y->name); });

            std::vector<juce::uint32> ids;
            for (auto* k : kids)
            {
                DirEntry e;
                e.name = k->name;
                if (k->type == NodeType::Stream)
                {
                    e.type = 2; e.bytes = k->data; e.size = (juce::uint64) k->data.getSize();
                }
                else { e.type = 1; e.child = NOSTREAM; }
                ids.push_back ((juce::uint32) dir.size());
                dir.push_back (std::move (e));
            }
            // Recurse into storages now that ids are assigned.
            for (size_t i = 0; i < kids.size(); ++i)
                if (kids[i]->type == NodeType::Storage)
                    assignChildren (*kids[i], ids[i], dir);

            // Build the balanced BST over the (already sorted) ids.
            dir[parentId].child = buildTree (ids, 0, (int) ids.size() - 1, dir);
        }

        juce::uint32 buildTree (const std::vector<juce::uint32>& ids, int lo, int hi,
                                std::vector<DirEntry>& dir)
        {
            if (lo > hi) return NOSTREAM;
            const int mid = (lo + hi) / 2;
            const juce::uint32 id = ids[(size_t) mid];
            dir[id].left  = buildTree (ids, lo, mid - 1, dir);
            dir[id].right = buildTree (ids, mid + 1, hi, dir);
            return id;
        }

        // Pack every small (< cutoff, non-empty) stream into the mini stream and
        // build the mini-FAT; mark those entries inMini with a mini-sector start.
        void buildMiniStream (std::vector<DirEntry>& dir)
        {
            for (auto& e : dir)
            {
                if (e.type != 2 || e.bytes.getSize() == 0) continue;
                if (e.bytes.getSize() >= kMiniCutoff) continue;     // big stream
                e.inMini = true;
                const juce::uint32 firstMini = (juce::uint32) miniFat.size();
                const auto numMini = (int) ((e.bytes.getSize() + kMiniSectorSize - 1) / kMiniSectorSize);
                for (int i = 0; i < numMini; ++i)
                {
                    const auto idx = (juce::uint32) miniFat.size();
                    miniFat.push_back (i + 1 < numMini ? idx + 1 : ENDOFCHAIN);
                }
                e.start = firstMini;
                e.size  = (juce::uint64) e.bytes.getSize();
                // Append padded to a whole number of 64-byte mini sectors.
                miniStream.append (e.bytes.getData(), e.bytes.getSize());
                const auto pad = numMini * kMiniSectorSize - (int) e.bytes.getSize();
                if (pad > 0) { std::vector<juce::uint8> z ((size_t) pad, 0); miniStream.append (z.data(), z.size()); }
            }
        }

        // Append ceil(bytes/512) sectors holding `data`, link their FAT chain,
        // and return the first sector index.
        juce::uint32 allocChain (const juce::MemoryBlock& data)
        {
            const auto total = data.getSize();
            const auto n = (juce::uint32) ((total + kSectorSize - 1) / kSectorSize);
            const auto first = (juce::uint32) sectors.size();
            for (juce::uint32 i = 0; i < n; ++i)
            {
                std::array<juce::uint8, kSectorSize> s {};
                const auto off = (size_t) i * kSectorSize;
                const auto len = juce::jmin ((size_t) kSectorSize, total - off);
                std::memcpy (s.data(), (const juce::uint8*) data.getData() + off, len);
                sectors.push_back (s);
                fat.push_back (i + 1 < n ? first + i + 1 : ENDOFCHAIN);
            }
            return first;
        }

        juce::MemoryBlock serialiseDirectory (const std::vector<DirEntry>& dir)
        {
            // Pad to a whole number of 4-entry sectors with empty (unalloc) entries.
            const auto count = ((dir.size() + kDirPerSector - 1) / kDirPerSector) * kDirPerSector;
            juce::MemoryBlock mb (count * kDirEntrySize);
            mb.fillWith (0);
            auto* base = (juce::uint8*) mb.getData();
            for (size_t i = 0; i < count; ++i)
            {
                auto* p = base + i * kDirEntrySize;
                if (i >= dir.size()) { putU32 (p + 68, NOSTREAM); putU32 (p + 72, NOSTREAM); putU32 (p + 76, NOSTREAM); continue; }
                const auto& e = dir[i];
                // Name as UTF-16LE incl. terminator.
                const auto utf16len = juce::jmin (31, e.name.length());
                for (int c = 0; c < utf16len; ++c)
                {
                    const auto ch = (juce::uint16) e.name[c];
                    putU16 (p + c * 2, ch);
                }
                putU16 (p + utf16len * 2, 0);                       // null terminator
                putU16 (p + 64, (juce::uint16) ((utf16len + 1) * 2));
                p[66] = (juce::uint8) e.type;
                p[67] = 1;                                          // colour = black
                putU32 (p + 68, e.left);
                putU32 (p + 72, e.right);
                putU32 (p + 76, e.child);
                putU32 (p + 116, e.start);
                putU64 (p + 120, e.size);
            }
            return mb;
        }
    };
}
