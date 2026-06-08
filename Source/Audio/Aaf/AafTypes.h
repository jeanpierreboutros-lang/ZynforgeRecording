#pragma once

#include <juce_core/juce_core.h>
#include <array>

// ── AAF primitive types + value encoders ────────────────────────────────────
//
// The building blocks of the AAF object model (phase 2), layered on the MS-CFB
// container (phase 1, CompoundFile.h). These are the spec-structural pieces:
//
//  - AUID   : a 16-byte identifier (a GUID) -- AAF classes, properties, types,
//             and definitions are all keyed by AUID.
//  - MobID  : a 32-byte SMPTE UMID identifying a Mob (composition / file).
//  - storedForm constants: how a property value is encoded in the object's
//             `properties` stream (direct data, strong/weak references, sets).
//  - little-endian value encoders for the scalar types AAF stores.
//
// AAF on disk is little-endian (byteOrder 'L'). All multi-byte integers and the
// AUID Data1/2/3 fields are written LE.
//
// NOTE (validation): the *structure* here follows the AAF structured-storage
// spec; the exact registry of class/property AUIDs (phase 2b) and the precise
// `properties`-stream header byte (see AafProperties.h) must be reconciled
// against a reference AAF (pyaaf2 / a Pro Tools export) before output imports
// into a DAW. Everything here is round-trip-tested for internal consistency.
namespace zynforge::aaf
{
    // ── storedForm (AAF SS property encodings) ──────────────────────────────
    enum StoredForm : juce::uint16
    {
        SF_DATA                           = 0x82,  // direct property value bytes
        SF_DATA_STREAM                    = 0x42,  // value is in a named sub-stream
        SF_STRONG_OBJECT_REFERENCE        = 0x22,  // contained object (sub-storage)
        SF_STRONG_OBJECT_REFERENCE_VECTOR = 0x32,  // ordered contained objects
        SF_STRONG_OBJECT_REFERENCE_SET    = 0x3A,  // keyed contained objects
        SF_WEAK_OBJECT_REFERENCE          = 0x02,  // reference by id (not contained)
        SF_WEAK_OBJECT_REFERENCE_VECTOR   = 0x0A,
        SF_WEAK_OBJECT_REFERENCE_SET      = 0x12,
        SF_UNIQUE_OBJECT_ID               = 0x86,  // the object's own id (MobID/AUID)
    };

    // ── AUID: 16-byte identifier (GUID layout) ──────────────────────────────
    struct Auid
    {
        juce::uint32 data1 { 0 };
        juce::uint16 data2 { 0 };
        juce::uint16 data3 { 0 };
        std::array<juce::uint8, 8> data4 {};

        bool operator== (const Auid& o) const noexcept
        { return data1 == o.data1 && data2 == o.data2 && data3 == o.data3 && data4 == o.data4; }

        // Serialise LE (Data1/2/3 little-endian; Data4 as-is) -- 16 bytes.
        std::array<juce::uint8, 16> toBytes() const noexcept
        {
            std::array<juce::uint8, 16> b {};
            for (int i = 0; i < 4; ++i) b[(size_t) i]      = (juce::uint8) (data1 >> (8 * i));
            for (int i = 0; i < 2; ++i) b[(size_t) 4 + i]  = (juce::uint8) (data2 >> (8 * i));
            for (int i = 0; i < 2; ++i) b[(size_t) 6 + i]  = (juce::uint8) (data3 >> (8 * i));
            for (int i = 0; i < 8; ++i) b[(size_t) 8 + i]  = data4[(size_t) i];
            return b;
        }
        static Auid fromBytes (const juce::uint8* p) noexcept
        {
            Auid a;
            for (int i = 0; i < 4; ++i) a.data1 |= (juce::uint32) p[i]     << (8 * i);
            for (int i = 0; i < 2; ++i) a.data2 |= (juce::uint16) p[4 + i] << (8 * i);
            for (int i = 0; i < 2; ++i) a.data3 |= (juce::uint16) p[6 + i] << (8 * i);
            for (int i = 0; i < 8; ++i) a.data4[(size_t) i] = p[8 + i];
            return a;
        }
        // Build from a juce::Uuid's 16 bytes (Data1/2/3 read big-endian from the
        // Uuid, so the canonical UUID string round-trips).
        static Auid fromUuid (const juce::Uuid& u) noexcept
        {
            const auto* r = u.getRawData();
            Auid a;
            a.data1 = ((juce::uint32) r[0] << 24) | ((juce::uint32) r[1] << 16)
                    | ((juce::uint32) r[2] << 8)  |  (juce::uint32) r[3];
            a.data2 = (juce::uint16) ((r[4] << 8) | r[5]);
            a.data3 = (juce::uint16) ((r[6] << 8) | r[7]);
            for (int i = 0; i < 8; ++i) a.data4[(size_t) i] = r[8 + i];
            return a;
        }
    };

    // ── MobID: 32-byte SMPTE UMID identifying a Mob ─────────────────────────
    // Layout: 12-byte UMID label + length(0x13) + instance(3) + 16-byte material
    // number. We fill the material number from a UUID so each Mob is unique.
    struct MobID
    {
        std::array<juce::uint8, 32> bytes {};

        static MobID generate()
        {
            MobID m;
            // SMPTE UMID base label for an AAF MobID (per the AAF spec).
            static const juce::uint8 label[12] =
                { 0x06,0x0A,0x2B,0x34,0x01,0x01,0x01,0x01,0x01,0x01,0x0F,0x00 };
            std::memcpy (m.bytes.data(), label, 12);
            m.bytes[12] = 0x13;        // length
            m.bytes[13] = 0x00;        // instance hi
            m.bytes[14] = 0x00;
            m.bytes[15] = 0x00;        // instance lo
            const auto u = juce::Uuid();
            std::memcpy (m.bytes.data() + 16, u.getRawData(), 16);   // material number
            return m;
        }
        bool operator== (const MobID& o) const noexcept { return bytes == o.bytes; }
    };

    // ── Little-endian value encoders -> MemoryBlock ─────────────────────────
    inline juce::MemoryBlock leU8  (juce::uint8 v)  { return { &v, 1 }; }
    inline juce::MemoryBlock leU16 (juce::uint16 v) { juce::uint8 b[2]; for (int i=0;i<2;++i) b[i]=(juce::uint8)(v>>(8*i)); return { b, 2 }; }
    inline juce::MemoryBlock leU32 (juce::uint32 v) { juce::uint8 b[4]; for (int i=0;i<4;++i) b[i]=(juce::uint8)(v>>(8*i)); return { b, 4 }; }
    inline juce::MemoryBlock leI32 (juce::int32 v)  { return leU32 ((juce::uint32) v); }
    inline juce::MemoryBlock leU64 (juce::uint64 v) { juce::uint8 b[8]; for (int i=0;i<8;++i) b[i]=(juce::uint8)(v>>(8*i)); return { b, 8 }; }
    inline juce::MemoryBlock leI64 (juce::int64 v)  { return leU64 ((juce::uint64) v); }
    inline juce::MemoryBlock encAuid (const Auid& a)   { auto b = a.toBytes();  return { b.data(), b.size() }; }
    inline juce::MemoryBlock encMobID (const MobID& m) { return { m.bytes.data(), m.bytes.size() }; }

    // AAF strings are UTF-16LE, null-terminated.
    inline juce::MemoryBlock encString (const juce::String& s)
    {
        juce::MemoryBlock mb;
        for (auto c : s)            { const auto u = (juce::uint16) c; juce::uint8 b[2]{ (juce::uint8) u, (juce::uint8)(u>>8) }; mb.append (b, 2); }
        const juce::uint8 nul[2] { 0, 0 }; mb.append (nul, 2);
        return mb;
    }
}
