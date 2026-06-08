#pragma once

#include "AafTypes.h"
#include <vector>

// ── AAF object `properties` stream codec ─────────────────────────────────────
//
// Every AAF object is a CFB storage containing a stream named "properties" that
// holds that object's property values. Layout (little-endian, byteOrder 'L'):
//
//   header : byteOrder (u8 = 'L')  version (u8)  entryCount (u16)
//   index  : entryCount × { pid (u16)  storedForm (u16)  length (u16) }
//   values : the property values, concatenated in index order
//
// PropertySet collects (pid, storedForm, bytes) tuples and serialises them to
// that stream; parse() reads one back (the round-trip oracle, since no external
// AAF reader is reachable here).
//
// VALIDATION NOTE: the structure above is per the AAF structured-storage spec.
// The one byte not pinned down without a reference AAF is `version` -- we emit
// kFormatVersion and a reader that round-trips it; reconcile this value against
// a pyaaf2 / Pro Tools export before trusting DAW import. The encode/decode are
// proven mutually consistent by AafTests.
namespace zynforge::aaf
{
    constexpr juce::uint8 kByteOrderLE   = 0x4C;   // 'L'
    constexpr juce::uint8 kFormatVersion = 0x05;   // AAF SS property-format version (verify vs reference)

    struct Property
    {
        juce::uint16      pid { 0 };
        juce::uint16      storedForm { SF_DATA };
        juce::MemoryBlock value;
    };

    class PropertySet
    {
    public:
        void add (juce::uint16 pid, juce::uint16 storedForm, juce::MemoryBlock value)
        {
            props.push_back ({ pid, storedForm, std::move (value) });
        }
        void addData (juce::uint16 pid, juce::MemoryBlock value)
        {
            add (pid, SF_DATA, std::move (value));
        }

        int size() const noexcept { return (int) props.size(); }

        juce::MemoryBlock serialise() const
        {
            jassert (props.size() <= 0xFFFF);
            juce::MemoryBlock out;
            const juce::uint8 hdr[2] { kByteOrderLE, kFormatVersion };
            out.append (hdr, 2);
            const auto count = (juce::uint16) props.size();
            appendU16 (out, count);
            for (const auto& p : props)            // index
            {
                appendU16 (out, p.pid);
                appendU16 (out, p.storedForm);
                jassert (p.value.getSize() <= 0xFFFF);
                appendU16 (out, (juce::uint16) p.value.getSize());
            }
            for (const auto& p : props)            // values, in index order
                out.append (p.value.getData(), p.value.getSize());
            return out;
        }

        // Parse a properties stream back into a PropertySet (round-trip oracle).
        static bool parse (const juce::MemoryBlock& mb, PropertySet& outSet)
        {
            const auto* p = (const juce::uint8*) mb.getData();
            const auto n = mb.getSize();
            if (n < 4 || p[0] != kByteOrderLE) return false;
            size_t off = 2;
            const auto count = readU16 (p + off); off += 2;

            std::vector<std::array<juce::uint16, 3>> idx;   // pid, form, length
            for (juce::uint16 i = 0; i < count; ++i)
            {
                if (off + 6 > n) return false;
                idx.push_back ({ readU16 (p + off), readU16 (p + off + 2), readU16 (p + off + 4) });
                off += 6;
            }
            outSet.props.clear();
            for (auto& e : idx)
            {
                const auto len = e[2];
                if (off + len > n) return false;
                outSet.add (e[0], e[1], juce::MemoryBlock (p + off, len));
                off += len;
            }
            return true;
        }

        const std::vector<Property>& all() const noexcept { return props; }
        const Property* find (juce::uint16 pid) const
        {
            for (const auto& p : props) if (p.pid == pid) return &p;
            return nullptr;
        }

    private:
        std::vector<Property> props;

        static void appendU16 (juce::MemoryBlock& mb, juce::uint16 v)
        { const juce::uint8 b[2] { (juce::uint8) v, (juce::uint8) (v >> 8) }; mb.append (b, 2); }
        static juce::uint16 readU16 (const juce::uint8* p) { return (juce::uint16) (p[0] | (p[1] << 8)); }
    };
}
