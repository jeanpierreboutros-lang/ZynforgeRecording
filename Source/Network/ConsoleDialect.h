#pragma once

// Per-console DIALECT: which address means what, and how to read a reply.
//
// Split out of ConsoleProfile so the capability/UI metadata stays readable and
// the wire details live in one auditable table per manufacturer.
//
// ── VERIFICATION STATUS (read this before trusting any of it) ──────────────
// Only the Behringer X32 / M32 addresses have ever been exercised end-to-end,
// and even those are pinned by unit tests rather than by a desk. Every other
// dialect here is written from the published protocol conventions and is
// marked `verified = false`. ConsoleLink REFUSES every write on an unverified
// dialect until a handshake reply proves the address model matches, so a wrong
// guess degrades to "read-only, no console control" rather than to writing
// garbage gains into someone's show. See decisions.md.

#include <juce_core/juce_core.h>

#include "ConsoleTransport.h"

#include <functional>

namespace zynforge
{
    // The things the RECORDER cares about hearing from a console. Deliberately
    // small: names label the session, scene changes drop markers, and the two
    // write-tier readbacks confirm a repatch / gain capture landed.
    struct ConsoleEvent
    {
        enum class Type
        {
            None,
            ChannelName,        // index = 1-based channel, text = name
            SceneRecalled,      // index = scene/snapshot number, text = its name
            HeadAmpGain,        // index = head-amp, value = RAW wire value
            InputBlockRouting,  // index = block, intValue = routing enum
            HandshakeOk         // the probe reply -- the dialect is confirmed
        };

        Type         type     { Type::None };
        int          index    { -1 };
        float        value    { 0.0f };
        int          intValue { 0 };
        juce::String text;
    };

    // Encoders return an EMPTY ConsoleMessage when the console can't do it.
    struct ConsoleDialect
    {
        // Probe sent on connect. A dialect whose parse() answers HandshakeOk
        // to the reply is considered confirmed, which unlocks the write tier.
        std::function<ConsoleMessage ()>                probe;

        // Read tier -- safe on any desk, never mutates console state.
        std::function<ConsoleMessage (int ch1)>         queryChannelName;
        std::function<ConsoleMessage ()>                subscribe;   // ask for push updates

        // Write tier -- gated behind a confirmed handshake.
        std::function<ConsoleMessage (int block)>       queryInBlock;
        std::function<ConsoleMessage (int block, int routingEnum)> setInBlock;
        std::function<ConsoleMessage (int headamp)>     queryHeadAmp;
        std::function<ConsoleMessage (int headamp, float raw)>     setHeadAmp;

        // Inbound: turn a wire message into something the app understands.
        std::function<ConsoleEvent (const ConsoleMessage&)> parse;
    };

    // ── Behringer X32 / Midas M32 ───────────────────────────────────────────
    // The reference dialect. /xremote subscribes to pushed changes for ~10 s,
    // so the link re-sends it periodically.
    inline ConsoleDialect x32Dialect()
    {
        ConsoleDialect d;
        d.probe            = [] { return ConsoleMessage ("/info"); };
        d.subscribe        = [] { return ConsoleMessage ("/xremote"); };
        d.queryChannelName = [] (int ch1)
        { return ConsoleMessage ("/ch/" + juce::String (ch1).paddedLeft ('0', 2) + "/config/name"); };
        d.queryInBlock = [] (int b)
        {
            static const char* const a[] = { "/config/routing/IN/1-8",   "/config/routing/IN/9-16",
                                             "/config/routing/IN/17-24", "/config/routing/IN/25-32" };
            return (b >= 0 && b < 4) ? ConsoleMessage (a[b]) : ConsoleMessage();
        };
        d.setInBlock = [d] (int b, int v)
        {
            auto m = d.queryInBlock (b);
            if (! m.isEmpty()) m.args.emplace_back (v);
            return m;
        };
        d.queryHeadAmp = [] (int i)
        { return ConsoleMessage ("/headamp/" + juce::String (i).paddedLeft ('0', 3) + "/gain"); };
        d.setHeadAmp = [] (int i, float raw)
        { return ConsoleMessage ("/headamp/" + juce::String (i).paddedLeft ('0', 3) + "/gain",
                                 { (double) raw }); };
        d.parse = [] (const ConsoleMessage& m) -> ConsoleEvent
        {
            const auto& a = m.address;
            if (a.startsWith ("/info"))
                return { ConsoleEvent::Type::HandshakeOk };
            if (a.startsWith ("/headamp/") && a.endsWith ("/gain") && m.hasArgs())
            {
                const int idx = a.fromFirstOccurrenceOf ("/headamp/", false, false)
                                 .upToFirstOccurrenceOf ("/", false, false).getIntValue();
                if (idx < 0 || idx >= 128) return {};
                return { ConsoleEvent::Type::HeadAmpGain, idx, m.floatArg() };
            }
            if (a.startsWith ("/config/routing/IN/") && m.hasArgs())
            {
                static const char* const blk[] = { "1-8", "9-16", "17-24", "25-32" };
                const auto tail = a.fromLastOccurrenceOf ("/", false, false);
                for (int b = 0; b < 4; ++b)
                    if (tail == blk[b])
                        return { ConsoleEvent::Type::InputBlockRouting, b, 0.0f, m.intArg() };
                return {};
            }
            if (a.startsWith ("/ch/") && a.endsWith ("/config/name") && m.hasArgs())
            {
                const int ch = a.fromFirstOccurrenceOf ("/ch/", false, false)
                                .upToFirstOccurrenceOf ("/", false, false).getIntValue();
                if (ch >= 1) return { ConsoleEvent::Type::ChannelName, ch, 0.0f, 0, m.stringArg() };
                return {};
            }
            // Scene / snapshot recall pushes the new cue index.
            if (a.startsWith ("/-show/prepos/current") && m.hasArgs())
                return { ConsoleEvent::Type::SceneRecalled, m.intArg(), 0.0f, 0, {} };
            return {};
        };
        return d;
    }

    // ── Behringer WING ──────────────────────────────────────────────────────
    // Same protocol family, different address model (no zero padding, /$ctl
    // style subscribe). UNVERIFIED.
    inline ConsoleDialect wingDialect()
    {
        ConsoleDialect d;
        d.probe            = [] { return ConsoleMessage ("/?"); };
        d.subscribe        = [] { return ConsoleMessage ("/*S"); };
        d.queryChannelName = [] (int ch1)
        { return ConsoleMessage ("/ch/" + juce::String (ch1) + "/$name"); };
        d.queryHeadAmp     = [] (int i)
        { return ConsoleMessage ("/ch/" + juce::String (i + 1) + "/in/set/g"); };
        d.setHeadAmp       = [] (int i, float raw)
        { return ConsoleMessage ("/ch/" + juce::String (i + 1) + "/in/set/g", { (double) raw }); };
        d.parse = [] (const ConsoleMessage& m) -> ConsoleEvent
        {
            const auto& a = m.address;
            if (a.startsWith ("/?") || a.startsWith ("/WING"))
                return { ConsoleEvent::Type::HandshakeOk };
            if (a.startsWith ("/ch/") && a.endsWith ("/$name") && m.hasArgs())
            {
                const int ch = a.fromFirstOccurrenceOf ("/ch/", false, false)
                                .upToFirstOccurrenceOf ("/", false, false).getIntValue();
                if (ch >= 1) return { ConsoleEvent::Type::ChannelName, ch, 0.0f, 0, m.stringArg() };
            }
            if (a.startsWith ("/ch/") && a.endsWith ("/in/set/g") && m.hasArgs())
            {
                const int ch = a.fromFirstOccurrenceOf ("/ch/", false, false)
                                .upToFirstOccurrenceOf ("/", false, false).getIntValue();
                if (ch >= 1) return { ConsoleEvent::Type::HeadAmpGain, ch - 1, m.floatArg() };
            }
            if (a.startsWith ("/$showctl/scene") && m.hasArgs())
                return { ConsoleEvent::Type::SceneRecalled, m.intArg(), 0.0f, 0, m.stringArg (1) };
            return {};
        };
        return d;
    }

    // ── DiGiCo SD / Quantum ─────────────────────────────────────────────────
    // OSC. The recorder only wants names + snapshot markers; the desk's own
    // Virtual Soundcheck does the repatch, so no write tier. UNVERIFIED.
    inline ConsoleDialect digicoDialect()
    {
        ConsoleDialect d;
        d.probe            = [] { return ConsoleMessage ("/info"); };
        d.subscribe        = [] { return ConsoleMessage ("/Console/Subscribe", { 1 }); };
        d.queryChannelName = [] (int ch1)
        { return ConsoleMessage ("/Console/Channels/" + juce::String (ch1) + "/name"); };
        d.parse = [] (const ConsoleMessage& m) -> ConsoleEvent
        {
            const auto& a = m.address;
            if (a.startsWith ("/info") || a.startsWith ("/Console/Info"))
                return { ConsoleEvent::Type::HandshakeOk };
            if (a.startsWith ("/Console/Channels/") && a.endsWith ("/name") && m.hasArgs())
            {
                const int ch = a.fromFirstOccurrenceOf ("/Console/Channels/", false, false)
                                .upToFirstOccurrenceOf ("/", false, false).getIntValue();
                if (ch >= 1) return { ConsoleEvent::Type::ChannelName, ch, 0.0f, 0, m.stringArg() };
            }
            if (a.startsWith ("/Console/Snapshots/recall") && m.hasArgs())
                return { ConsoleEvent::Type::SceneRecalled, m.intArg(), 0.0f, 0, m.stringArg (1) };
            return {};
        };
        return d;
    }

    // ── Yamaha CL / QL / RIVAGE / DM (SCP over TCP) ─────────────────────────
    // SCP is `verb Parameter x y [value]`, replies lead with OK/NOTIFY. The
    // transport strips the verb, so `address` is the parameter. UNVERIFIED.
    inline ConsoleDialect yamahaScpDialect()
    {
        ConsoleDialect d;
        d.probe     = [] { return ConsoleMessage ("devinfo", { juce::String ("productname") }); };
        d.subscribe = [] { return ConsoleMessage ("ssrecall_ex", { juce::String ("scene") }); };
        d.queryChannelName = [] (int ch1)
        { return ConsoleMessage ("get", { juce::String ("MIXER:Current/InCh/Label/Name"),
                                          ch1 - 1, 0 }); };
        d.parse = [] (const ConsoleMessage& m) -> ConsoleEvent
        {
            const auto& a = m.address;
            if (a.containsIgnoreCase ("devinfo") || a.containsIgnoreCase ("productname"))
                return { ConsoleEvent::Type::HandshakeOk };
            if (a.containsIgnoreCase ("InCh/Label/Name") && m.args.size() >= 3)
                return { ConsoleEvent::Type::ChannelName, m.intArg (0) + 1, 0.0f, 0,
                         m.stringArg ((int) m.args.size() - 1) };
            // "NOTIFY sscurrent_ex scene <bank> <index>" -- the transport keeps
            // the trailing numbers as args.
            if (a.containsIgnoreCase ("scene") && m.hasArgs())
                return { ConsoleEvent::Type::SceneRecalled,
                         m.intArg ((int) m.args.size() - 1), 0.0f, 0, {} };
            return {};
        };
        return d;
    }

    // ── Allen & Heath dLive / Avantis / SQ (MIDI over TCP) ──────────────────
    // A&H uses NRPN-style CC pairs and SysEx. The transport hands us raw MIDI
    // bytes under the synthetic address "midi". UNVERIFIED.
    inline ConsoleDialect allenHeathMidiDialect()
    {
        ConsoleDialect d;
        // A&H SysEx probe: F0 00 00 1A 50 <model> 01 00 F7 (system enquiry).
        d.probe = []
        { return ConsoleMessage ("midi", { 0xF0, 0x00, 0x00, 0x1A, 0x50, 0x10, 0x01, 0x00, 0xF7 }); };
        d.parse = [] (const ConsoleMessage& m) -> ConsoleEvent
        {
            if (m.address != "midi" || m.args.empty()) return {};
            const int b0 = m.intArg (0);

            // SysEx reply to the enquiry confirms we're talking to an A&H desk.
            if (b0 == 0xF0 && m.args.size() >= 5
                && m.intArg (1) == 0x00 && m.intArg (2) == 0x00 && m.intArg (3) == 0x1A)
                return { ConsoleEvent::Type::HandshakeOk };

            // Scene recall arrives as Bank Select (CC0) + Program Change.
            if ((b0 & 0xF0) == 0xC0 && m.args.size() >= 2)
                return { ConsoleEvent::Type::SceneRecalled, m.intArg (1) + 1 };
            return {};
        };
        return d;
    }

    // A dialect that reads nothing and writes nothing -- the honest default
    // for "generic OSC / manual", so the link still connects for transport use.
    inline ConsoleDialect passiveDialect()
    {
        ConsoleDialect d;
        d.parse = [] (const ConsoleMessage&) -> ConsoleEvent { return {}; };
        return d;
    }
}
