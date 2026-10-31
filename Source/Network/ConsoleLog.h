#pragma once

// One line per console message for the OSC debug log: the address as it was on
// the wire, an OSC-style type-tag string, then the values. Every path that logs
// (the console link, the OSC listener) formats here, so
// credential redaction has one definition. RemoteControl::logOsc is the sink: it
// scrubs the live token and only then cuts a line to length, so a cut can't leave
// part of a token behind.

#include <juce_osc/juce_osc.h>

#include "ConsoleTransport.h"

namespace zynforge::consolelog
{
    // The final Generic OSC argument is a credential, including on a failed
    // attempt.
    inline bool isCredential (const juce::String& path, int index, int count, bool isString)
    {
        return path.startsWith ("/zynforge/") && index == count - 1;
    }

    inline juce::String describe (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();
        juce::String tags (","), values;
        for (int i = 0; i < m.size(); ++i)
        {
            const auto& a = m[i];
            const bool secret = isCredential (path, i, m.size(), a.isString());
            if (a.isInt32())        { tags << "i"; values << " " << (secret ? juce::String ("[redacted]") : juce::String (a.getInt32())); }
            else if (a.isFloat32()) { tags << "f"; values << " " << (secret ? juce::String ("[redacted]") : juce::String (a.getFloat32(), 6)); }
            else if (a.isString())  { tags << "s"; values << " " << (secret ? juce::String ("[redacted]") : a.getString().quoted()); }
            else if (a.isBlob())    { tags << "b"; values << " [" << (int) a.getBlob().getSize() << " byte blob]"; }
            else if (a.isColour())  { tags << "r"; values << " " << (secret ? juce::String ("[redacted]") : juce::String::toHexString ((int) a.getColour().toInt32())); }
        }
        return path + " " + tags + values;
    }

    // Outbound messages, and the TCP transports' inbound ones, whose payload is
    // exactly what `args` holds. i / f / s match how OscUdpTransport encodes them.
    inline juce::String describe (const ConsoleMessage& m)
    {
        juce::String tags (","), values;
        const int count = (int) m.args.size();
        for (int i = 0; i < count; ++i)
        {
            const auto& a = m.args[(size_t) i];
            const bool isText = ! (a.isInt() || a.isInt64() || a.isDouble());
            const bool secret = isCredential (m.address, i, count, isText);
            if (a.isInt() || a.isInt64()) tags << "i";
            else if (a.isDouble())        tags << "f";
            else                          tags << "s";
            values << " " << (secret ? juce::String ("[redacted]")
                                     : isText ? a.toString().quoted() : a.toString());
        }
        return m.address + " " + tags + values;
    }

    // A bundle is logged message by message: neither listener acts on one, and
    // a desk that sends only bundles must not look like a desk that sends
    // nothing.
    inline void describeBundle (const juce::OSCBundle& bundle, juce::StringArray& lines)
    {
        for (const auto& element : bundle)
        {
            if (element.isMessage())    lines.add ("[bundle, not acted on] " + describe (element.getMessage()));
            else if (element.isBundle()) describeBundle (element.getBundle(), lines);
        }
    }
}
