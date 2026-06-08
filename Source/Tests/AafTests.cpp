// Phase-2 AAF primitives: AUID / MobID / value encoders and the object
// `properties`-stream codec. No external AAF reader is reachable here, so these
// assert internal consistency (round-trip) + the documented byte structure.

#include <juce_core/juce_core.h>
#include "../Audio/Aaf/AafObject.h"

namespace zynforge
{
    class AafTests final : public juce::UnitTest
    {
    public:
        AafTests() : UnitTest ("AAF primitives", "zynforge") {}

        static const cfb::Node* findChild (const cfb::Node& n, const juce::String& name)
        {
            for (const auto& c : n.children) if (c->name == name) return c.get();
            return nullptr;
        }

        void runTest() override
        {
            using namespace aaf;

            beginTest ("AUID serialises LE and round-trips");
            {
                Auid a; a.data1 = 0x0D010101; a.data2 = 0x0101; a.data3 = 0x0200;
                a.data4 = { 0x06,0x0E,0x2B,0x34,0x01,0x01,0x01,0x02 };
                const auto bytes = a.toBytes();
                expectEquals ((int) bytes.size(), 16);
                expectEquals ((int) bytes[0], 0x01);   // data1 low byte first (LE)
                expectEquals ((int) bytes[3], 0x0D);   // data1 high byte
                expect (Auid::fromBytes (bytes.data()) == a, "AUID byte round-trip failed");

                // fromUuid preserves the canonical UUID ordering.
                const auto u = juce::Uuid();
                const auto a2 = Auid::fromUuid (u);
                expect (Auid::fromBytes (a2.toBytes().data()) == a2, "fromUuid round-trip failed");
            }

            beginTest ("MobID is a 32-byte UMID, unique per generate()");
            {
                const auto m1 = MobID::generate();
                const auto m2 = MobID::generate();
                expectEquals ((int) m1.bytes.size(), 32);
                expectEquals ((int) m1.bytes[0], 0x06);   // SMPTE UMID label
                expectEquals ((int) m1.bytes[12], 0x13);  // length byte
                expect (! (m1 == m2), "two MobIDs collided");
            }

            beginTest ("Little-endian value encoders");
            {
                auto u32 = leU32 (0x11223344);
                const auto* p = (const juce::uint8*) u32.getData();
                expectEquals ((int) u32.getSize(), 4);
                expectEquals ((int) p[0], 0x44); expectEquals ((int) p[3], 0x11);

                auto i64 = leI64 ((juce::int64) 1);
                expectEquals ((int) i64.getSize(), 8);
                expectEquals ((int) ((const juce::uint8*) i64.getData())[0], 1);

                // UTF-16LE + null terminator.
                auto s = encString ("AB");
                expectEquals ((int) s.getSize(), 6);   // 2 chars * 2 + 2-byte null
                const auto* sp = (const juce::uint8*) s.getData();
                expectEquals ((int) sp[0], (int) 'A'); expectEquals ((int) sp[1], 0);
                expectEquals ((int) sp[4], 0); expectEquals ((int) sp[5], 0);
            }

            beginTest ("properties stream: header structure + property round-trip");
            {
                Auid cls; cls.data1 = 0xDEADBEEF;
                const auto mob = MobID::generate();

                PropertySet set;
                set.addData (0x0101, leI32 (48000));                 // a direct int
                set.addData (0x4401, encString ("Kick"));            // a UTF-16 name
                set.add     (0x0102, SF_UNIQUE_OBJECT_ID, encMobID (mob));
                set.add     (0x0103, SF_STRONG_OBJECT_REFERENCE, encAuid (cls));

                const auto stream = set.serialise();
                const auto* s = (const juce::uint8*) stream.getData();
                expectEquals ((int) s[0], 0x4C, "byteOrder not 'L'");
                expectEquals ((int) s[1], (int) aaf::kFormatVersion);
                expectEquals ((int) (s[2] | (s[3] << 8)), 4, "entry count wrong");

                PropertySet back;
                expect (PropertySet::parse (stream, back), "parse failed");
                expectEquals (back.size(), 4);

                expect (back.find (0x0101) != nullptr && back.find (0x0101)->value == leI32 (48000),
                        "int property lost");
                expect (back.find (0x4401) != nullptr && back.find (0x4401)->value == encString ("Kick"),
                        "name property lost");
                const auto* mp = back.find (0x0102);
                expect (mp != nullptr && mp->storedForm == SF_UNIQUE_OBJECT_ID
                        && mp->value == encMobID (mob), "MobID property lost");
                const auto* cp = back.find (0x0103);
                expect (cp != nullptr && cp->storedForm == SF_STRONG_OBJECT_REFERENCE,
                        "strong-ref storedForm lost");
            }

            beginTest ("empty property set serialises to a valid header");
            {
                PropertySet empty;
                const auto stream = empty.serialise();
                expectEquals ((int) stream.getSize(), 4);   // header only
                PropertySet back;
                expect (PropertySet::parse (stream, back) && back.size() == 0);
            }

            beginTest ("AAF object graph emits to a CFB storage tree + round-trips");
            {
                Auid header;   header.data1 = 0x0D010101; header.data3 = 0x2F00;
                Auid mobClass; mobClass.data1 = 0x0D010101; mobClass.data3 = 0x3500;

                AafObject root (header);
                root.props.addData (0x4001, leI32 (1));
                auto* cs = root.addChild ("ContentStorage", mobClass);
                cs->props.addData (0x4002, encString ("Comp"));

                cfb::CompoundFileWriter w;
                root.emitInto (w.root());

                // Root storage carries a "properties" stream + a "ContentStorage" sub-storage.
                const auto* propsStream = findChild (w.root(), "properties");
                const auto* csStorage   = findChild (w.root(), "ContentStorage");
                expect (propsStream != nullptr && propsStream->type == cfb::NodeType::Stream,
                        "root properties stream missing");
                expect (csStorage != nullptr && csStorage->type == cfb::NodeType::Storage,
                        "child storage missing");

                // Root properties parse back, carrying the class AUID + the property.
                PropertySet ps;
                expect (PropertySet::parse (propsStream->data, ps), "root properties unparsable");
                expect (ps.find (kPidObjClass) != nullptr
                        && ps.find (kPidObjClass)->value == encAuid (header), "root class AUID lost");
                expect (ps.find (0x4001) != nullptr, "root property lost");

                // The child object's own properties survive in its sub-storage.
                const auto* childProps = findChild (*csStorage, "properties");
                expect (childProps != nullptr, "child properties stream missing");
                PropertySet cps;
                expect (PropertySet::parse (childProps->data, cps), "child properties unparsable");
                expect (cps.find (kPidObjClass) != nullptr
                        && cps.find (kPidObjClass)->value == encAuid (mobClass), "child class AUID lost");
                expect (cps.find (0x4002) != nullptr
                        && cps.find (0x4002)->value == encString ("Comp"), "child property lost");

                // And the whole thing serialises to a valid CFB envelope.
                expect (w.build().getSize() % 512 == 0, "assembled CFB not sector-aligned");
            }
        }
    };

    static AafTests aafTests;
}
