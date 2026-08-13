#pragma once

// Console control TRANSPORT seam.
//
// ConsoleLink used to be welded to juce::DatagramSocket + OSC, which capped
// console support at "OSC desks only" -- so DiGiCo/Yamaha/SSL/A&H could only
// ever be honest stubs. The desks worth reaching don't share a wire protocol:
//
//   * Behringer X32 / M32, Behringer WING, DiGiCo SD / Quantum -> OSC over UDP
//   * Yamaha CL / QL / RIVAGE / DM                             -> SCP, an ASCII
//                                                                 line protocol
//                                                                 over TCP
//   * Allen & Heath dLive / Avantis / SQ                       -> MIDI over TCP
//
// So: a ConsoleMessage is the protocol-neutral unit (an address plus typed
// args), a ConsoleTransport owns bytes + framing + connection lifetime, and the
// ConsoleProfile owns the dialect (which address means what, how to parse a
// reply). ConsoleLink is left as pure state machine and knows none of it.
//
// Threading: transports may read on their own thread but MUST deliver
// onMessage on the message thread, because ConsoleLink and everything it
// notifies are message-thread only.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

namespace zynforge
{
    // One console control message, in or out. `address` is the protocol's
    // addressing string (an OSC path, an SCP parameter name, or a synthetic
    // name for MIDI); `args` carries the typed payload. juce::var covers the
    // int / float / string cases every one of these protocols uses.
    struct ConsoleMessage
    {
        juce::String           address;
        std::vector<juce::var> args;

        ConsoleMessage() = default;
        explicit ConsoleMessage (juce::String a, std::vector<juce::var> v = {})
            : address (std::move (a)), args (std::move (v)) {}

        bool  isEmpty()  const noexcept { return address.isEmpty(); }
        bool  hasArgs()  const noexcept { return ! args.empty(); }
        float floatArg (int i = 0, float dflt = 0.0f) const
        {
            return (i >= 0 && i < (int) args.size()) ? (float) (double) args[(size_t) i] : dflt;
        }
        int intArg (int i = 0, int dflt = 0) const
        {
            return (i >= 0 && i < (int) args.size()) ? (int) args[(size_t) i] : dflt;
        }
        juce::String stringArg (int i = 0) const
        {
            return (i >= 0 && i < (int) args.size()) ? args[(size_t) i].toString() : juce::String();
        }
    };

    class ConsoleTransport
    {
    public:
        ConsoleTransport() { life->owner = this; }
        virtual ~ConsoleTransport() { life->owner = nullptr; }

        virtual bool connect (const juce::String& host, int port) = 0;
        virtual void disconnect() = 0;
        virtual bool isConnected() const = 0;
        virtual bool send (const ConsoleMessage&) = 0;

        // Human-readable transport name, for status lines + the trust dossier.
        virtual juce::String getName() const = 0;

        // Fired on the MESSAGE THREAD for every inbound message.
        std::function<void (const ConsoleMessage&)> onMessage;

    protected:
        // Helper for TCP transports whose reader runs off-thread: hop to the
        // message thread before notifying.
        //
        // The queued lambda must NOT capture `this` (or a pointer to the
        // onMessage member): a transport destroyed between the post and the
        // dispatch would be a use-after-free on the message thread -- the exact
        // class the invariants gate exists to stop. A shared liveness token,
        // cleared in the destructor, makes a late delivery a no-op instead.
        void deliverOnMessageThread (ConsoleMessage m)
        {
            if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
                if (mm->isThisTheMessageThread())
                {
                    if (onMessage) onMessage (m);
                    return;
                }

            std::weak_ptr<Life> weak = life;
            juce::MessageManager::callAsync ([weak, m]
            {
                if (auto strong = weak.lock())
                    if (strong->owner != nullptr && strong->owner->onMessage)
                        strong->owner->onMessage (m);
            });
        }

    private:
        struct Life { ConsoleTransport* owner { nullptr }; };
        std::shared_ptr<Life> life { std::make_shared<Life>() };
    };
}
