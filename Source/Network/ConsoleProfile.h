#pragma once

#include <juce_core/juce_core.h>

#include "ConsoleDialect.h"

#include <functional>
#include <vector>

namespace zynforge
{
    // A per-console-family profile for virtual-soundcheck control. Consoles
    // differ wildly in HOW you put them into virtual soundcheck:
    //
    //   * X32 / M32-class desks have NO dedicated VSC button, but expose a
    //     clean OSC routing model, so ZynForge repatches the inputs to the
    //     card returns over the wire (and can capture/restore head-amp gains).
    //     This is where the automation adds the most value.
    //
    //   * Large-format desks (DiGiCo, Yamaha, SSL, Allen & Heath) almost all
    //     have a NATIVE Virtual Soundcheck mode that flips every input between
    //     the stage preamps and the Dante/MADI record card in one console
    //     action -- and several use non-OSC control protocols (Yamaha SCP over
    //     TCP, A&H MIDI/TCP). For those, ZynForge records + plays the card
    //     returns and the engineer (or a recalled scene) flips VSC ON the
    //     console; per-input OSC repatch is neither needed nor exposed.
    //
    // A profile therefore declares its *capabilities* honestly, and supplies
    // the OSC address model only when `canRepatch` / `canCaptureGains` is true.
    struct ConsoleProfile
    {
        enum class Kind
        {
            BehringerX32,   // Behringer X32 / Midas M32  (OSC: full control)
            BehringerWing,  // Behringer WING             (OSC: read tier)
            DiGiCo,         // SD / Quantum               (native VSC)
            Yamaha,         // CL / QL / RIVAGE / DM       (native VSC, SCP/TCP)
            SSL,            // SSL Live                    (native VSC)
            AllenHeath,     // dLive / Avantis / SQ        (native VSC)
            GenericOsc      // any OSC desk / TouchOSC      (manual)
        };

        // How we physically talk to it. See ConsoleTransports.h.
        enum class Transport { OscUdp, TextLineTcp, MidiTcp };

        Kind         kind        { Kind::BehringerX32 };
        juce::String displayName { "Behringer X32 / Midas M32" };
        int          defaultPort { 10023 };
        Transport    transport   { Transport::OscUdp };

        // What ZynForge can do over the wire for THIS console.
        //
        // READ tier -- safe on any desk, never mutates console state, and where
        // essentially all the value is for a recorder: names label the session,
        // scene changes drop markers you can navigate by next morning.
        bool canReadNames    { false };
        bool canReadScenes   { false };
        // WRITE tier -- can change the desk. Gated behind BOTH this flag and a
        // successful handshake (see `verified`).
        bool canRepatch      { false };   // flip input sources
        bool canCaptureGains { false };   // poll/set head-amp gains
        bool hasNativeVsc    { false };   // the console has its own VSC mode

        // Do we TRUST this dialect's address model?
        //
        // This is a statement about the ADDRESS MODEL, not about the desk on
        // the other end. True = the protocol is documented and the encode/parse
        // is pinned by unit tests (the X32/M32 reference). False = written from
        // published conventions with nothing exercising it, which is every
        // dialect added without hardware to try it on.
        //
        // An untrusted dialect is still fully USABLE read-only -- names, scene
        // markers, transport. It just can't WRITE until the connect-time
        // handshake proves the console answers the way the dialect expects.
        // That asymmetry is the whole point: a wrong guess degrades to "no
        // console control", never to writing garbage into someone's show.
        //
        // NOTE this is deliberately NOT "hardware-verified". The X32's routing
        // enum indices and AES50 head-amp mapping still need a real desk before
        // anyone leans on them at a gig -- see tasks.md.
        bool dialectTrusted { false };

        ConsoleDialect dialect { passiveDialect() };

        // One-line guidance surfaced in the connect UI / status line.
        juce::String note;

        // ── OSC repatch model (used only when canRepatch) ───────────────
        // The console's input-routing is addressed as `numInBlocks` blocks;
        // enterSoundcheck() queries+stashes each block's current value then
        // sets it to (cardBlockFirst + blockIndex) = the record-card returns.
        int numInBlocks    { 0 };
        int cardBlockFirst { 0 };
        std::function<juce::String (int block)>   inBlockAddress;

        // ── Head-amp gain model (used only when canCaptureGains) ────────
        std::function<juce::String (int headamp)> gainAddress;
        std::function<float (float raw)>          gainToDb { [] (float v) { return v; } };
    };

    // The Behringer X32 / Midas M32 reference profile -- the one console
    // ZynForge drives end-to-end over OSC today.
    inline ConsoleProfile x32Profile()
    {
        ConsoleProfile p;
        p.kind            = ConsoleProfile::Kind::BehringerX32;
        p.displayName     = "Behringer X32 / Midas M32";
        p.defaultPort     = 10023;
        p.transport       = ConsoleProfile::Transport::OscUdp;
        p.canReadNames    = true;
        p.canReadScenes   = true;
        p.canRepatch      = true;
        p.canCaptureGains = true;
        p.hasNativeVsc    = false;
        p.dialectTrusted  = true;    // documented protocol, encode/parse pinned by ConsoleLinkTests
        p.dialect         = x32Dialect();
        p.note            = "Full OSC control: names, scene markers, repatch + head-amp gains.";
        p.numInBlocks     = 4;
        p.cardBlockFirst  = 16;   // CARD1-8..CARD25-32 = enum 16..19
        p.inBlockAddress  = [] (int b)
        {
            static const char* const a[] = {
                "/config/routing/IN/1-8",  "/config/routing/IN/9-16",
                "/config/routing/IN/17-24","/config/routing/IN/25-32" };
            return (b >= 0 && b < 4) ? juce::String (a[b]) : juce::String();
        };
        p.gainAddress = [] (int i)
        { return "/headamp/" + juce::String (i).paddedLeft ('0', 3) + "/gain"; };
        p.gainToDb    = [] (float v) { return -12.0f + 72.0f * v; };   // 0..1 -> -12..+60 dB
        return p;
    }

    // Behringer WING -- same OSC family as the X32, different address model.
    // READ tier only for now: names + scene markers are where the value is and
    // they can't hurt a desk. UNVERIFIED dialect (see ConsoleDialect.h).
    inline ConsoleProfile wingProfile()
    {
        ConsoleProfile p;
        p.kind            = ConsoleProfile::Kind::BehringerWing;
        p.displayName     = "Behringer WING";
        p.defaultPort     = 2223;
        p.transport       = ConsoleProfile::Transport::OscUdp;
        p.canReadNames    = true;
        p.canReadScenes   = true;
        p.dialectTrusted  = false;   // written from published docs; handshake-gated
        p.dialect         = wingDialect();
        p.note            = "Reads channel names + drops a marker on every scene recall.";
        return p;
    }

    // DiGiCo SD / Quantum -- OSC. Read tier only: the desk's own Virtual
    // Soundcheck does the repatch far better than we could over the wire.
    inline ConsoleProfile digicoProfile()
    {
        ConsoleProfile p;
        p.kind            = ConsoleProfile::Kind::DiGiCo;
        p.displayName     = "DiGiCo SD / Quantum";
        p.defaultPort     = 8000;
        p.transport       = ConsoleProfile::Transport::OscUdp;
        p.canReadNames    = true;
        p.canReadScenes   = true;
        p.hasNativeVsc    = true;
        p.dialectTrusted  = false;   // written from published docs; handshake-gated
        p.dialect         = digicoDialect();
        p.note            = "Reads names + snapshot markers; use the console's Virtual Soundcheck for the repatch.";
        return p;
    }

    // Yamaha CL / QL / RIVAGE / DM -- SCP, an ASCII line protocol over TCP.
    inline ConsoleProfile yamahaProfile()
    {
        ConsoleProfile p;
        p.kind            = ConsoleProfile::Kind::Yamaha;
        p.displayName     = "Yamaha CL / QL / RIVAGE / DM";
        p.defaultPort     = 49280;
        p.transport       = ConsoleProfile::Transport::TextLineTcp;
        p.canReadNames    = true;
        p.canReadScenes   = true;
        p.hasNativeVsc    = true;
        p.dialectTrusted  = false;   // written from published docs; handshake-gated
        p.dialect         = yamahaScpDialect();
        p.note            = "SCP over TCP: reads names + scene markers. Native Virtual Soundcheck on the console.";
        return p;
    }

    // Allen & Heath dLive / Avantis / SQ -- MIDI over TCP.
    inline ConsoleProfile allenHeathProfile()
    {
        ConsoleProfile p;
        p.kind            = ConsoleProfile::Kind::AllenHeath;
        p.displayName     = "Allen & Heath dLive / Avantis / SQ";
        p.defaultPort     = 51325;
        p.transport       = ConsoleProfile::Transport::MidiTcp;
        p.canReadScenes   = true;      // scene recall arrives as MIDI PC
        p.hasNativeVsc    = true;
        p.dialectTrusted  = false;   // written from published docs; handshake-gated
        p.dialect         = allenHeathMidiDialect();
        p.note            = "MIDI over TCP: drops a marker on every scene recall. Native Virtual Soundcheck on the console.";
        return p;
    }

    // Desks we can reach but deliberately don't drive, and the manual escape
    // hatch. These connect for transport/record use and nothing else.
    inline ConsoleProfile passiveProfile (ConsoleProfile::Kind k,
                                          const juce::String& name,
                                          const juce::String& note,
                                          int port)
    {
        ConsoleProfile p;
        p.kind         = k;
        p.displayName  = name;
        p.defaultPort  = port;
        p.hasNativeVsc = true;
        p.note         = note;
        p.dialect      = passiveDialect();
        return p;
    }

    // All known console profiles, X32 first (the default).
    inline std::vector<ConsoleProfile> consoleProfiles()
    {
        return {
            x32Profile(),
            wingProfile(),
            digicoProfile(),
            yamahaProfile(),
            allenHeathProfile(),
            passiveProfile (ConsoleProfile::Kind::SSL, "SSL Live",
                "Use the console's Virtual Soundcheck; ZynForge records + plays the card returns.", 8000),
            passiveProfile (ConsoleProfile::Kind::GenericOsc, "Generic OSC (manual)",
                "Transport + record/play only; flip soundcheck on the console.", 8000),
        };
    }

    inline ConsoleProfile consoleProfileFor (ConsoleProfile::Kind k)
    {
        for (auto& p : consoleProfiles())
            if (p.kind == k) return p;
        return x32Profile();
    }
}
