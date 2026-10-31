#pragma once

// Opt-in transport control from a desk macro.
//
// Generic OSC needs a per-start token as its last argument, which a desk's macro
// keys can't carry, and the console dialects deliberately can't drive transport.
// This listener is the narrow door between them: its own port, one pinned source
// address, a fixed list of addresses, and every command ends in an entry point
// that already refuses for itself (RECORD's pre-flight, the two-tap STOP, LOCK).
// A source address can be spoofed on a LAN; decisions.md says why that residual
// risk is accepted for this listener and for nothing wider.

#include <juce_osc/juce_osc.h>

#include "OscPacketReceiver.h"

#include <array>
#include <functional>

namespace zynforge
{
    class AudioEngine;

    namespace deskmacro
    {
        inline constexpr int    kDefaultPort = 9301;
        inline constexpr double kDebounceMs  = 250.0;

        enum class Command : int
        {
            None, Record, Stop, Play, Pause, Marker, MarkerNext, MarkerPrev, MarkerGoto, Count
        };

        struct Parsed
        {
            Command command { Command::None };
            int     value   { 0 };   // MarkerGoto: the 1-based marker number
        };

        struct AddressEntry { const char* address; Command command; };

        inline constexpr std::array<AddressEntry, 8> kAddresses {{
            { "/zynforge/macro/record",      Command::Record },
            { "/zynforge/macro/stop",        Command::Stop },
            { "/zynforge/macro/play",        Command::Play },
            { "/zynforge/macro/pause",       Command::Pause },
            { "/zynforge/macro/marker",      Command::Marker },
            { "/zynforge/macro/marker/next", Command::MarkerNext },
            { "/zynforge/macro/marker/prev", Command::MarkerPrev },
            { "/zynforge/macro/marker/goto", Command::MarkerGoto },
        }};

        // A desk key can send on press and on release, so integer 0 is the
        // release and means nothing. Only the exact addresses above, with no
        // argument or one integer, are commands: None otherwise. No pattern
        // matching, so a wildcard address can't reach any of them.
        inline Parsed parse (const juce::OSCMessage& m)
        {
            const auto path = m.getAddressPattern().toString();
            auto command = Command::None;
            for (const auto& entry : kAddresses)
                if (path == entry.address) command = entry.command;
            if (command == Command::None || m.size() > 1) return {};
            if (m.size() == 1 && ! m[0].isInt32()) return {};
            const int value = m.size() == 1 ? m[0].getInt32() : 0;
            if (m.size() == 1 && value == 0) return {};
            if (command == Command::MarkerGoto && value < 1) return {};
            return { command, value };
        }

        // A strict dotted quad, normalised so it compares equal to what the
        // socket reports. Empty for anything that couldn't be one desk: no
        // hostnames, and no wildcard or broadcast that would widen the pin.
        inline juce::String normaliseSourceIp (const juce::String& text)
        {
            const auto parts = juce::StringArray::fromTokens (text.trim(), ".", "");
            if (parts.size() != 4) return {};
            int octets[4] {};
            for (int i = 0; i < 4; ++i)
            {
                if (parts[i].isEmpty() || parts[i].length() > 3 || ! parts[i].containsOnly ("0123456789"))
                    return {};
                octets[i] = parts[i].getIntValue();
                if (octets[i] > 255) return {};
            }
            if (octets[0] == 0 || octets[0] >= 224) return {};
            return juce::String (octets[0]) + "." + juce::String (octets[1]) + "."
                 + juce::String (octets[2]) + "." + juce::String (octets[3]);
        }

        // Desk macros can double-fire, and a held key can repeat. Measured from
        // the last SIGHTING, not the last command acted on: otherwise a repeat
        // every 200 ms would act every 400 ms, and one held STOP key would
        // complete the two-tap guard by itself.
        class Debouncer
        {
        public:
            bool admit (const Parsed& p, double nowMs)
            {
                auto& slot = slots[(size_t) p.command];
                const bool identical = slot.seen && slot.value == p.value
                                    && nowMs - slot.lastMs < kDebounceMs;
                slot = { nowMs, p.value, true };
                return ! identical;
            }

        private:
            struct Slot { double lastMs { 0.0 }; int value { 0 }; bool seen { false }; };
            std::array<Slot, (size_t) Command::Count> slots;
        };
    }

    class DeskMacroListener final : private OscPacketReceiver,
                                    private juce::OSCReceiver::Listener<
                                        juce::OSCReceiver::MessageLoopCallback>
    {
    public:
        explicit DeskMacroListener (AudioEngine&);
        ~DeskMacroListener() override;

        // `sourceIp` is the desk: false, and not listening, unless it is one
        // usable address and the port binds.
        bool start (int udpPort, const juce::String& sourceIp);
        void stop();

        bool         isListening() const noexcept { return listening; }
        int          getPort() const noexcept     { return port; }
        juce::String getSourceIp() const          { return source; }
        using OscPacketReceiver::getDroppedSourceCount;
        using OscPacketReceiver::getLastDroppedSource;

        // Delivers as the socket would, past the source filter, at a chosen
        // time. The source filter itself is exercised over a real socket.
        void dispatchForTest (const juce::OSCMessage&, double nowMs);

    private:
        void oscMessageReceived (const juce::OSCMessage&) override;
        void oscBundleReceived (const juce::OSCBundle&) override;
        void handle (const juce::OSCMessage&, double nowMs);
        void perform (const deskmacro::Parsed&);
        void report (const juce::String& message);
        void locateToIndex (int index, const juce::String& refusalIfMissing);

        AudioEngine&        engine;
        deskmacro::Debouncer debouncer;
        bool                listening { false };
        int                 port { deskmacro::kDefaultPort };
        juce::String        source;
    };
}
