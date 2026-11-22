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
// notifies are message-thread only. A reader thread's messages wait in a
// bounded backlog (deliverOnMessageThread), never one posted message each.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
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

        // Filled by a transport for the traffic log. Nothing decides on these:
        // `source` is "ip:port", `wire` is the message as it arrived, for when
        // `args` can't show all of it.
        juce::String           source;
        juce::String           wire;

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

        // Fired on the MESSAGE THREAD for traffic nothing will act on (an OSC
        // bundle, a datagram the parser refused, messages dropped because the
        // message thread was too far behind), so the log can still show it.
        std::function<void (const juce::String& source, const juce::String& description)> onUnhandledTraffic;

        // Inbound messages a reader thread dropped because the message thread
        // was this far behind (the backlog was full). Since connect().
        juce::int64 getInboundOverflowCount() const noexcept
        { return life->overflowed.load (std::memory_order_relaxed); }

    protected:
        // Helper for TCP transports whose reader runs off-thread: hop to the
        // message thread before notifying.
        //
        // Messages wait in a bounded backlog, as OSC packets do in
        // OscPacketReceiver. They used to go into the message queue one posted
        // message each, with nothing to say how many: a desk (or anything on
        // its port) sending faster than the message thread put timers, meters
        // and STOP behind an ever longer line. One drain is posted at a time;
        // it delivers for one turn (kDrainTurnMs), then goes to the back of the
        // queue behind whatever else waits there. Past the caps the message is
        // dropped, counted (getInboundOverflowCount) and reported through
        // onUnhandledTraffic, thinned to one report per kReportMs. False when
        // it was dropped.
        //
        // The queued lambda must NOT capture `this` (or a pointer to the
        // onMessage member): a transport destroyed between the post and the
        // dispatch would be a use-after-free on the message thread -- the exact
        // class the invariants gate exists to stop. A shared liveness token,
        // cleared in the destructor, makes a late delivery a no-op instead.
        bool deliverOnMessageThread (ConsoleMessage m)
        {
            if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
                if (mm->isThisTheMessageThread())
                {
                    bool waiting = false;
                    { const std::lock_guard<std::mutex> g (life->lock); waiting = ! life->backlog.empty(); }
                    if (! waiting)   // nothing ahead of it to overtake
                    {
                        if (auto handler = onMessage) handler (m);
                        return true;
                    }
                }

            const auto bytes = approximateBytes (m);
            bool post = false;
            juce::int64 droppedSoFar = 0;   // > 0: this one was dropped, and a report is due
            bool dropped = false;
            {
                const std::lock_guard<std::mutex> g (life->lock);
                if (life->backlog.size() >= kMaxBacklogMessages || life->backlogBytes + bytes > kMaxBacklogBytes)
                {
                    dropped = true;
                    const auto count = life->overflowed.fetch_add (1, std::memory_order_relaxed) + 1;
                    const auto now = juce::Time::getMillisecondCounterHiRes();
                    if (now >= life->nextOverflowReportMs)
                    {
                        life->nextOverflowReportMs = now + kReportMs;
                        droppedSoFar = count;
                    }
                }
                else
                {
                    life->backlogBytes += bytes;
                    life->backlog.emplace_back (std::move (m), bytes);
                    post = ! life->drainPosted;
                    life->drainPosted = true;
                }
            }
            if (droppedSoFar > 0)
                reportOnMessageThread (overflowSource, "dropped: arriving faster than they can be handled ("
                                                           + juce::String (droppedSoFar) + " so far)");
            if (post) postDrain (life);
            return ! dropped;
        }

        // What still waits is dropped (a link closing must deliver nothing
        // more); with resetCount, the overflow count starts again (connect).
        void clearInbound (bool resetCount)
        {
            const std::lock_guard<std::mutex> g (life->lock);
            life->backlog.clear();
            life->backlogBytes = 0;
            if (resetCount)
            {
                life->overflowed.store (0, std::memory_order_relaxed);
                life->nextOverflowReportMs = 0.0;
            }
        }

        // onUnhandledTraffic, from any thread. Callers thin their own reports.
        void reportOnMessageThread (const juce::String& source, const juce::String& description)
        {
            if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
                if (mm->isThisTheMessageThread())
                {
                    if (auto handler = onUnhandledTraffic) handler (source, description);
                    return;
                }
            std::weak_ptr<Life> weak = life;
            juce::MessageManager::callAsync ([weak, source, description]
            {
                if (auto strong = weak.lock())
                    if (strong->owner != nullptr)
                        if (auto handler = strong->owner->onUnhandledTraffic) handler (source, description);
            });
        }

        // Named in a dropped-message report: "ip:port" of the far end. Set
        // before the reader starts.
        juce::String overflowSource { "(console)" };

    private:
        // Well past a desk's name dump (hundreds of short lines); the byte
        // limit is for a stream of long ones.
        static constexpr size_t kMaxBacklogMessages = 2048;
        static constexpr size_t kMaxBacklogBytes    = 4 * 1024 * 1024;
        static constexpr double kDrainTurnMs        = 4.0;
        static constexpr double kReportMs           = 100.0;

        struct Life
        {
            ConsoleTransport* owner { nullptr };    // message thread
            std::mutex        lock;
            std::deque<std::pair<ConsoleMessage, size_t>> backlog;   // guarded by lock
            size_t            backlogBytes { 0 };                     // guarded by lock
            bool              drainPosted { false };                  // guarded by lock
            double            nextOverflowReportMs { 0.0 };           // guarded by lock
            std::atomic<juce::int64> overflowed { 0 };
        };

        static size_t approximateBytes (const ConsoleMessage& m)
        {
            size_t bytes = sizeof (ConsoleMessage) + m.address.getNumBytesAsUTF8()
                         + m.wire.getNumBytesAsUTF8() + m.source.getNumBytesAsUTF8();
            for (const auto& a : m.args)
                bytes += sizeof (juce::var) + (a.isString() ? a.toString().getNumBytesAsUTF8() : 0);
            return bytes;
        }

        static void postDrain (std::weak_ptr<Life> weak)
        {
            if (! juce::MessageManager::callAsync ([weak] { drain (weak); }))
                if (auto strong = weak.lock())
                {
                    const std::lock_guard<std::mutex> g (strong->lock);
                    strong->drainPosted = false;   // nothing will run it: the next message posts again
                }
        }

        // Message thread. A handler can close the link, and with it the
        // transport: the owner is asked again before every message, and the
        // handler is a copy, never the member being cleared under it.
        static void drain (std::weak_ptr<Life> weak)
        {
            const auto turnEnds = juce::Time::getMillisecondCounterHiRes() + kDrainTurnMs;
            for (;;)
            {
                const auto strong = weak.lock();
                if (strong == nullptr) return;
                std::function<void (const ConsoleMessage&)> handler;
                ConsoleMessage next;
                {
                    const std::lock_guard<std::mutex> g (strong->lock);
                    if (strong->owner == nullptr || strong->backlog.empty())
                    {
                        strong->backlog.clear();
                        strong->backlogBytes = 0;
                        strong->drainPosted = false;
                        return;
                    }
                    next = std::move (strong->backlog.front().first);
                    strong->backlogBytes -= strong->backlog.front().second;
                    strong->backlog.pop_front();
                    handler = strong->owner->onMessage;
                }
                if (handler) handler (next);
                if (juce::Time::getMillisecondCounterHiRes() >= turnEnds) break;
            }
            postDrain (weak);
        }

        std::shared_ptr<Life> life { std::make_shared<Life>() };
    };
}
