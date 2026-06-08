#pragma once

#include "AafProperties.h"
#include "CompoundFile.h"
#include <memory>
#include <utility>
#include <vector>

// ── AAF object graph -> CFB storage tree ─────────────────────────────────────
//
// An AAF file is a tree of interchange objects. Each object maps to a CFB
// storage that contains:
//   - a stream named "properties" (its property index + values, AafProperties.h)
//   - one sub-storage per strongly-referenced contained object
//
// AafObject models that node; emitInto() writes it (and its children) into a
// cfb::Node storage. This is the generic assembly mechanism that turns the
// object model into the on-disk container; the concrete AAF graph (Header ->
// ContentStorage -> Mobs -> Slots -> Sequences -> SourceClips -> Descriptors)
// is built on top of it.
//
// The object's CLASS is recorded as a property (pid kPidObjClass, the AUID of
// its class). NOTE: AAF can also carry the class in the storage's CLSID; which
// of the two a given reader requires must be reconciled against a reference
// AAF (phase 2c). The emit/readback mechanism here is round-trip-tested.
namespace zynforge::aaf
{
    // Well-known PID for the object's class AUID (per AAF; verify vs reference).
    constexpr juce::uint16 kPidObjClass = 0x0001;

    struct AafObject
    {
        Auid        classId;
        PropertySet props;
        std::vector<std::pair<juce::String, std::unique_ptr<AafObject>>> children;

        explicit AafObject (const Auid& cls = {}) : classId (cls) {}

        // Add a strongly-referenced child stored in a sub-storage named `name`.
        AafObject* addChild (const juce::String& name, const Auid& childClass)
        {
            children.emplace_back (name, std::make_unique<AafObject> (childClass));
            return children.back().second.get();
        }

        // Serialise into a CFB storage node: the class AUID + properties go into
        // the "properties" stream; each child becomes a sub-storage.
        void emitInto (cfb::Node& storage) const
        {
            PropertySet out = props;                       // copy + prepend the class id
            out.add (kPidObjClass, SF_DATA, encAuid (classId));
            storage.addStream ("properties", out.serialise());

            for (const auto& [name, child] : children)
                child->emitInto (*storage.addStorage (name));
        }
    };
}
