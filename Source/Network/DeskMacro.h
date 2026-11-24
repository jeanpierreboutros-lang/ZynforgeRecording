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

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

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
            int     value   { 0 };   // MarkerGoto: the 1-based marker number; 0: the address alone
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

        // The exact address only: no pattern matching, so a wildcard address
        // can't reach any of them.
        inline Command commandFor (const juce::String& path)
        {
            for (const auto& entry : kAddresses)
                if (path == entry.address) return entry.command;
            return Command::None;
        }

        // A desk key can send on press and on release, so integer 0 is the
        // release and means nothing. Only the exact addresses above, with no
        // argument or one integer, are commands: None otherwise. The address
        // alone comes back with value 0, which no integer message can carry
        // (isValueless): the listener refuses a valueless STOP and pairs the
        // rest (ValuelessPairs).
        inline Parsed parse (const juce::OSCMessage& m)
        {
            const auto command = commandFor (m.getAddressPattern().toString());
            if (command == Command::None || m.size() > 1) return {};
            if (m.size() == 1 && ! m[0].isInt32()) return {};
            const int value = m.size() == 1 ? m[0].getInt32() : 0;
            if (m.size() == 1 && value == 0) return {};
            if (command == Command::MarkerGoto && value < 1) return {};
            return { command, value };
        }

        // The address alone: nothing in it says press or release.
        inline bool isValueless (const Parsed& p) noexcept
        {
            return p.command != Command::None && p.value == 0;
        }

        // One of the addresses above with a value parse() does not take: a
        // float (TouchOSC's default 1.0), text, or more than one argument. It
        // is still no command (integers only, decisions.md), but a key set
        // that way did nothing and said nothing, and a RECORD key could leave
        // a show unrecorded. The command it was meant for, or None.
        inline Command commandWithWrongValue (const juce::OSCMessage& m)
        {
            const auto command = commandFor (m.getAddressPattern().toString());
            if (command == Command::None || m.size() == 0) return Command::None;
            if (m.size() == 1 && m[0].isInt32()) return Command::None;
            return command;
        }

        using oscwire::normaliseSourceIp;

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

        // A key set to send the address alone sends the same packet on press
        // AND on release, and nothing in it says which is which. Held past the
        // debounce window, the release was a second command: a second marker,
        // a second jump, a second STOP tap. So a valueless message is a press,
        // and the next valueless one for the same command inside this window is
        // its release and does nothing. A key held longer than the window acts
        // again on release; a desk that sends the address only on press loses
        // a second press inside the window (send 1 on press and every press
        // counts). STOP is never guessed at: a valueless STOP is refused, as on
        // the Generic OSC port. The window is the two-tap STOP's.
        //
        // A key is the command and, on the Generic OSC port, the marker's
        // name: two press-only buttons named Verse and Chorus are two keys, and
        // Chorus pressed a second after Verse is no release of it (it was
        // dropped as one; audit 2026-11-24). The same name again is still the
        // release. The desk macro port has no names: one key per command.
        inline constexpr double kValuelessPairMs = 2000.0;

        class ValuelessPairs
        {
        public:
            // True when this valueless message is the release of the press
            // before it. Either way the next one is a press again.
            bool isRelease (Command c, double nowMs) { return isRelease (c, {}, nowMs); }
            bool isRelease (Command c, const juce::String& name, double nowMs)
            {
                for (auto it = awaiting.begin(); it != awaiting.end(); ++it)
                    if (it->command == c && it->name == name)
                    {
                        const bool release = nowMs - it->pressMs < kValuelessPairMs;
                        awaiting.erase (it);
                        return release;
                    }
                return false;
            }
            void pressed (Command c, double nowMs) { pressed (c, {}, nowMs); }
            void pressed (Command c, const juce::String& name, double nowMs)
            {
                forget (c, name);
                // A press past its window pairs with nothing; dropping it keeps
                // a stream of different names from growing this list.
                awaiting.erase (std::remove_if (awaiting.begin(), awaiting.end(), [nowMs] (const Awaiting& a)
                                                { return nowMs - a.pressMs >= kValuelessPairMs; }),
                                awaiting.end());
                if (awaiting.size() >= kMaxAwaiting) awaiting.erase (awaiting.begin());
                awaiting.push_back ({ c, name, nowMs });
            }
            // This key sends values after all: its next valueless message is a press.
            void forget (Command c) { forget (c, {}); }
            void forget (Command c, const juce::String& name)
            {
                awaiting.erase (std::remove_if (awaiting.begin(), awaiting.end(), [c, &name] (const Awaiting& a)
                                                { return a.command == c && a.name == name; }),
                                awaiting.end());
            }

        private:
            static constexpr size_t kMaxAwaiting = 32;
            struct Awaiting { Command command { Command::None }; juce::String name; double pressMs { 0.0 }; };
            std::vector<Awaiting> awaiting;
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
        void refuseValuelessStop (double nowMs);
        void refuseWrongValue (deskmacro::Command, const juce::OSCMessage&, double nowMs);
        void locateToIndex (int index, const juce::String& refusalIfMissing);

        AudioEngine&        engine;
        deskmacro::Debouncer debouncer;
        deskmacro::ValuelessPairs valueless;
        double              lastValuelessStopNoticeMs { -1.0e9 };
        double              lastWrongValueNoticeMs { -1.0e9 };
        bool                listening { false };
        int                 port { deskmacro::kDefaultPort };
        juce::String        source;
    };
}
