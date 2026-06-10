#pragma once

#include <juce_core/juce_core.h>

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
            BehringerX32,   // Behringer X32 / Midas M32  (OSC repatch + gains)
            DiGiCo,         // SD / Quantum               (native VSC)
            Yamaha,         // CL / QL / RIVAGE / DM       (native VSC, SCP/TCP)
            SSL,            // SSL Live                    (native VSC)
            AllenHeath,     // dLive / Avantis / SQ        (native VSC)
            GenericOsc      // any OSC desk / TouchOSC      (manual)
        };

        Kind         kind        { Kind::BehringerX32 };
        juce::String displayName { "Behringer X32 / Midas M32" };
        int          defaultPort { 10023 };

        // What ZynForge can do over the wire for THIS console:
        bool canRepatch      { false };   // flip input sources via OSC
        bool canCaptureGains { false };   // poll/set head-amp gains via OSC
        bool hasNativeVsc    { false };   // the console has its own VSC mode

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
        p.canRepatch      = true;
        p.canCaptureGains = true;
        p.hasNativeVsc    = false;
        p.note            = "Full OSC control: repatch + head-amp gain capture.";
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

    // Honest placeholders for the large-format desks: ZynForge records + plays
    // their record card; the console's native Virtual Soundcheck does the
    // repatch. canRepatch/canCaptureGains stay false until their control
    // protocols are wired + verified on real hardware (see decisions.md).
    inline ConsoleProfile nativeVscProfile (ConsoleProfile::Kind k,
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
        return p;
    }

    // All known console profiles, X32 first (the default).
    inline std::vector<ConsoleProfile> consoleProfiles()
    {
        return {
            x32Profile(),
            nativeVscProfile (ConsoleProfile::Kind::DiGiCo, "DiGiCo SD / Quantum",
                "Use the console's Virtual Soundcheck / a snapshot; ZynForge records + plays the card returns.", 8000),
            nativeVscProfile (ConsoleProfile::Kind::Yamaha, "Yamaha CL / QL / RIVAGE",
                "Native Virtual Soundcheck on the console (control is SCP/TCP, not OSC).", 49280),
            nativeVscProfile (ConsoleProfile::Kind::SSL, "SSL Live",
                "Use the console's Virtual Soundcheck; ZynForge records + plays the card returns.", 8000),
            nativeVscProfile (ConsoleProfile::Kind::AllenHeath, "Allen & Heath dLive / Avantis / SQ",
                "Native Virtual Soundcheck on the console (control is MIDI/TCP).", 51325),
            nativeVscProfile (ConsoleProfile::Kind::GenericOsc, "Generic OSC (manual)",
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
