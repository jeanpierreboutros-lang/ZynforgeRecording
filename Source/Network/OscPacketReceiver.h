#pragma once

// OSC over UDP with a parser that cannot be driven into unbounded recursion.
//
// juce::OSCReceiver parses each datagram on its own thread with a recursive
// descent that has no depth cap: every nested #bundle costs a stack frame, so
// ONE datagram of nested bundles (~36-64 KB, from any host on the LAN)
// overflowed the receive thread's 512 KB stack and crashed the app mid-take --
// before OscRemote's token check or a console dialect ever saw a byte. JUCE
// has no public "parse these bytes" entry point, so this owns the receive
// thread and parses with a bounds-checked cursor whose recursion is capped at
// kMaxBundleDepth. It accepts what JUCE accepted (OSC 1.0 messages with
// i / f / s / b / r arguments, bundles, zero padding) and delivers to the same
// juce::OSCReceiver::Listener interface on the message thread, so OscRemote
// and OscUdpTransport keep their listener code unchanged.

#include <juce_osc/juce_osc.h>

#include <cstring>
#include <memory>
#include <optional>

namespace zynforge
{
    namespace oscwire
    {
        // Deepest #bundle nesting accepted. Desks send plain messages or one
        // bundle level; a deeper packet is dropped as malformed.
        inline constexpr int kMaxBundleDepth = 8;

        // Cursor over bytes [pos, end). Every read checks what remains first
        // and fails rather than read past `end`.
        struct Cursor
        {
            const juce::uint8* data;
            size_t pos, end;

            size_t remaining() const noexcept { return end - pos; }

            bool readInt32 (juce::int32& v) noexcept
            {
                if (remaining() < 4) return false;
                v = (juce::int32) juce::ByteOrder::bigEndianInt (data + pos);
                pos += 4;
                return true;
            }

            bool readUint64 (juce::uint64& v) noexcept
            {
                if (remaining() < 8) return false;
                v = juce::ByteOrder::bigEndianInt64 (data + pos);
                pos += 8;
                return true;
            }

            // Bytes [pos + from, pos + to) exist and are all zero.
            bool zeroPadded (size_t from, size_t to) const noexcept
            {
                if (to > remaining()) return false;
                for (auto i = from; i < to; ++i)
                    if (data[pos + i] != 0) return false;
                return true;
            }

            // OSC-string: NUL-terminated, zero-padded to a 4-byte boundary.
            bool readString (juce::String& s)
            {
                const auto* start = data + pos;
                const auto* nul = static_cast<const juce::uint8*> (std::memchr (start, 0, remaining()));
                if (nul == nullptr) return false;
                const auto len = (size_t) (nul - start);
                const auto padded = (len + 4) & ~(size_t) 3;   // the NUL + padding
                if (! zeroPadded (len + 1, padded)) return false;
                const auto* chars = reinterpret_cast<const char*> (start);
                // JUCE took these bytes as UTF-8 regardless. Keep a non-UTF-8
                // channel name readable (as Latin-1) rather than drop the packet.
                if (juce::CharPointer_UTF8::isValidString (chars, (int) len))
                    s = juce::String::fromUTF8 (chars, (int) len);
                else
                {
                    s.clear();
                    for (size_t i = 0; i < len; ++i) s += (juce::juce_wchar) start[i];
                }
                pos += padded;
                return true;
            }

            bool readBlob (juce::MemoryBlock& b)
            {
                juce::int32 size = 0;
                if (! readInt32 (size) || size < 0) return false;
                const auto len = (size_t) size;
                if (! zeroPadded (len, (len + 3) & ~(size_t) 3)) return false;
                b = juce::MemoryBlock (data + pos, len);
                pos += (len + 3) & ~(size_t) 3;
                return true;
            }
        };

        inline std::optional<juce::OSCMessage> readMessage (Cursor& c)
        {
            juce::String address, tags;
            if (! c.readString (address) || ! c.readString (tags) || ! tags.startsWithChar (','))
                return {};
            // Throws juce::OSCFormatError on characters an address can't hold.
            juce::OSCMessage m { juce::OSCAddressPattern (address) };
            for (int i = 1; i < tags.length(); ++i)
            {
                juce::int32 word = 0;
                switch (tags[i])
                {
                    case 'i':
                        if (! c.readInt32 (word)) return {};
                        m.addArgument (juce::OSCArgument (word));
                        break;
                    case 'f':
                    {
                        if (! c.readInt32 (word)) return {};
                        float f;
                        std::memcpy (&f, &word, sizeof (f));
                        m.addArgument (juce::OSCArgument (f));
                        break;
                    }
                    case 's':
                    {
                        juce::String s;
                        if (! c.readString (s)) return {};
                        m.addArgument (juce::OSCArgument (s));
                        break;
                    }
                    case 'b':
                    {
                        juce::MemoryBlock b;
                        if (! c.readBlob (b)) return {};
                        m.addArgument (juce::OSCArgument (std::move (b)));
                        break;
                    }
                    case 'r':
                        if (! c.readInt32 (word)) return {};
                        m.addArgument (juce::OSCArgument (juce::OSCColour::fromInt32 ((juce::uint32) word)));
                        break;
                    default:
                        return {};   // a type JUCE didn't support either
                }
            }
            return m;
        }

        inline std::optional<juce::OSCBundle::Element> readElement (Cursor& c, int depth);

        // `depth` counts this bundle and every bundle around it. Recursion is
        // bounded by kMaxBundleDepth, never by the packet's contents.
        inline std::optional<juce::OSCBundle> readBundle (Cursor& c, int depth)
        {
            if (depth > kMaxBundleDepth) return {};
            juce::String tag;
            juce::uint64 timeTag = 0;
            if (! c.readString (tag) || tag != "#bundle" || ! c.readUint64 (timeTag)) return {};
            juce::OSCBundle bundle { juce::OSCTimeTag (timeTag) };
            while (c.remaining() > 0)
            {
                juce::int32 size = 0;
                if (! c.readInt32 (size) || size < 4 || (size_t) size > c.remaining()) return {};
                // Each element is parsed inside its declared size and must fill it.
                Cursor inner { c.data, c.pos, c.pos + (size_t) size };
                const auto element = readElement (inner, depth);
                if (! element || inner.remaining() != 0) return {};
                bundle.addElement (*element);
                c.pos = inner.end;
            }
            return bundle;
        }

        inline std::optional<juce::OSCBundle::Element> readElement (Cursor& c, int depth)
        {
            if (c.remaining() == 0) return {};
            if (c.data[c.pos] == '/')
            {
                if (auto m = readMessage (c)) return juce::OSCBundle::Element (std::move (*m));
            }
            else if (c.data[c.pos] == '#')
            {
                if (auto b = readBundle (c, depth + 1)) return juce::OSCBundle::Element (std::move (*b));
            }
            return {};
        }

        // One datagram -> one message or bundle. nullopt when it is malformed,
        // nests bundles deeper than kMaxBundleDepth, or has trailing bytes.
        inline std::optional<juce::OSCBundle::Element> parsePacket (const void* data, size_t size)
        {
            if (data == nullptr || size < 4) return {};
            try
            {
                Cursor c { static_cast<const juce::uint8*> (data), 0, size };
                auto element = readElement (c, 0);
                if (element && c.remaining() == 0) return element;
            }
            catch (const juce::OSCException&) {}   // e.g. an illegal address pattern
            return {};
        }
    }

    // Stands in for the part of juce::OSCReceiver this app uses: bind a port or
    // share a socket, add message-thread listeners, disconnect.
    class OscPacketReceiver : private juce::Thread,
                              private juce::MessageListener
    {
    public:
        using Listener = juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>;

        OscPacketReceiver() : juce::Thread ("ZynForge OSC receiver") {}
        ~OscPacketReceiver() override { disconnect(); }

        // Binds a socket of our own to `port` on every interface, as
        // juce::OSCReceiver::connect did.
        bool connect (int port)
        {
            disconnect();
            auto s = std::make_unique<juce::DatagramSocket> (false);
            if (! s->bindToPort (port)) return false;
            owned = std::move (s);
            socket = owned.get();
            startThread();
            return true;
        }

        // Reads a socket the caller owns and also sends from (a desk replies to
        // the request's source port). It must outlive disconnect().
        bool connectToSocket (juce::DatagramSocket& shared)
        {
            disconnect();
            socket = &shared;
            startThread();
            return true;
        }

        bool disconnect()
        {
            if (socket == nullptr) return true;
            signalThreadShouldExit();
            // Our own socket can be shut to wake the reader at once; a shared
            // one is polled with a 100 ms timeout. Parsing is bounded, so the
            // wait always ends -- and never frees a socket the reader is on.
            if (owned != nullptr) owned->shutdown();
            waitForThreadToExit (-1);
            socket = nullptr;
            owned.reset();
            return true;
        }

        void addListener (Listener* l)    { listeners.add (l); }
        void removeListener (Listener* l) { listeners.remove (l); }

    private:
        struct Packet final : juce::Message
        {
            explicit Packet (const juce::OSCBundle::Element& e) : content (e) {}
            juce::OSCBundle::Element content;
        };

        void run() override
        {
            constexpr int kBufferSize = 65535;
            juce::HeapBlock<char> buffer (kBufferSize);
            while (! threadShouldExit())
            {
                const int ready = socket->waitUntilReady (true, 100);
                if (ready < 0 || threadShouldExit()) return;
                if (ready == 0) continue;
                const int got = socket->read (buffer.getData(), kBufferSize, false);
                if (got < 4) continue;
                if (const auto element = oscwire::parsePacket (buffer.getData(), (size_t) got))
                    postMessage (new Packet (*element));   // MessageListener: dropped if we're gone
            }
        }

        void handleMessage (const juce::Message& message) override
        {
            const auto* packet = dynamic_cast<const Packet*> (&message);
            if (packet == nullptr) return;
            const auto& content = packet->content;
            if (content.isMessage())
                listeners.call ([&] (Listener& l) { l.oscMessageReceived (content.getMessage()); });
            else
                listeners.call ([&] (Listener& l) { l.oscBundleReceived (content.getBundle()); });
        }

        juce::DatagramSocket*                 socket { nullptr };
        std::unique_ptr<juce::DatagramSocket> owned;
        juce::ListenerList<Listener>          listeners;
    };
}
