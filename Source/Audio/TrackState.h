#pragma once

#include <array>
#include <atomic>
#include <juce_core/juce_core.h>

namespace zynforge
{
    // Set by the app's UI timer: true while recording OR playing. Per-strip
    // LED meters + mini-spectra run at full refresh when this is true (or the
    // strip itself is armed/monitored); when the transport is stopped and a
    // strip isn't being watched, they throttle to ~8 Hz to save idle CPU --
    // the meters still move (signal-present check), just less often.
    inline std::atomic<bool> gTransportActive { false };

    struct TrackState
    {
        static constexpr int kFftSize = 1024;

        std::atomic<float> peak    { 0.0f };
        std::atomic<float> rms     { 0.0f };
        std::atomic<bool>  clipped { false };
        std::atomic<bool>  armed   { false };
        std::atomic<bool>  monitor { false };
        std::atomic<bool>  muted   { false };
        std::atomic<bool>  soloed  { false };

        // Playback / monitor only. Recording is always pre-fader.
        std::atomic<float> gainDb  { 0.0f };           // -60 .. +12
        std::atomic<float> pan     { 0.0f };           // -1 = L, 0 = C, +1 = R

        // Trim-follow (virtual-soundcheck gain compensation). The console's
        // live preamp/trim gain for this channel, pushed in via OSC, and the
        // value snapshotted at record-start. When trim-follow is on, playback
        // adds (live - capture) dB so adjusting the desk's input gain during
        // soundcheck moves the recorded track exactly as it would the live mic.
        std::atomic<float> liveInputGainDb    { 0.0f };
        std::atomic<float> captureInputGainDb { 0.0f };

        // Soft-takeover targets used during cue recall -- the audio thread
        // ramps gainDb / pan from their current value to these targets
        // over rampSamplesRemaining samples so a cue change doesn't
        // produce an audible step / click. When the ramp is inactive,
        // these mirror gainDb / pan.
        std::atomic<float>       rampTargetGainDb     { 0.0f };
        std::atomic<float>       rampTargetPan        { 0.0f };
        std::atomic<juce::int64> rampSamplesRemaining { 0 };
        // Output muting -- independent of `muted` (which gates BOTH the
        // monitor bus AND the per-channel output). outputMuted gates the
        // physical output only; muted continues to gate the monitor sum.
        std::atomic<bool> outputMuted { false };

        // VCA group assignment. -1 = unassigned; 0..7 = VCA bus index.
        // The audio thread sums the VCA bus's gain (post-fader) onto
        // this strip's gain when it routes the player output, so a
        // single VCA fader can ride a group of strips. The VCA's
        // muted flag also gates the strip's output.
        std::atomic<int> vcaGroup { -1 };

        // Edit Group -- Pro Tools-style "linked editing" affordance.
        // Strips sharing the same editGroup id receive broadcast
        // edits: when one strip's gain / pan / mute / solo / arm
        // changes via a user gesture, every other strip in the same
        // group gets the same change applied. -1 = unlinked. Edit
        // groups are independent of VCA assignment.
        std::atomic<int> editGroup { -1 };

        // Bus track flag -- when true this strip is an aux / mix bus.
        // Bus tracks have no input routing, no recording, no R / I
        // buttons. They receive audio from other strips via AuxSends
        // and then sum into the master / per-channel outputs like a
        // normal strip (with their own fader, pan, mute, solo).
        std::atomic<bool> isBus { false };

        // Aux sends -- 4 slots per non-bus strip. Each slot has a
        // target bus index (-1 = empty), a level (-60..+12 dB), and
        // a pre/post-fader switch. Audio thread reads these every
        // block and sums (strip_audio × send.gain × (postFader
        // ? strip.effectiveGain : 1)) into the target bus's scratch
        // buffer.
        static constexpr int kNumSends = 4;
        struct AuxSend
        {
            std::atomic<int>   targetBus { -1 };   // -1 = empty, 0..N-1 = bus track index
            std::atomic<float> levelDb   { 0.0f }; // -60..+12
            std::atomic<bool>  postFader { true };
        };
        std::array<AuxSend, kNumSends> sends;

        // Routing. -1 = unrouted (no input captured / no output played).
        // Default of -2 means "use identity routing" (resolved at init).
        std::atomic<int> inputRouting  { -2 };
        std::atomic<int> outputRouting { -2 };

        // Stream bus send (post-fader, post-mute/solo).
        std::atomic<bool> streamSend  { false };

        // True if this track is the LEFT half of a logical stereo strip.
        // The RIGHT half is at trackIndex + 1. The UI uses this flag to
        // render a single ChannelStrip controlling both underlying tracks
        // (linked mute / solo / gain / pan / colour / name).
        std::atomic<bool> isStereo    { false };

        std::atomic<int>          clipCount        { 0 };
        std::atomic<juce::int64>  lastClipSample   { -1 };

        // 0 means "use default personality colour for this index".
        std::atomic<juce::uint32> colourARGB       { 0 };

        // FFT FIFO -- audio thread fills `fftFifo`, snapshots into
        // `fftSnapshot` when full and sets `fftBlockReady`. UI thread
        // reads the snapshot, FFTs it, then clears the flag.
        std::array<float, kFftSize> fftFifo     {};
        std::array<float, kFftSize> fftSnapshot {};
        std::atomic<int>            fftIndex      { 0 };
        std::atomic<bool>           fftBlockReady { false };

        juce::String       name;
        // `name` is written on the message thread but ALSO copied by
        // captureStatus() from the companion-server's detached thread -- reading
        // a juce::String mid-reassignment is a data race (torn / freed buffer).
        // This SpinLock guards the cross-thread read + every write; pure
        // message-thread reads (the UI) can keep touching `name` directly since
        // they can't race the (also-message-thread) writers.
        mutable juce::SpinLock nameLock;
        juce::String getNameThreadSafe() const
        {
            const juce::SpinLock::ScopedLockType l (nameLock);
            return name;
        }
        void setNameThreadSafe (const juce::String& n)
        {
            const juce::SpinLock::ScopedLockType l (nameLock);
            name = n;
        }
        // Stable identity that survives a strip reorder. Generated
        // once on strip creation, persisted in appProps under
        // strip_uid_<n>, and referenced by cue snapshots so a cue
        // recalls the engineer's intent even after they shuffle the
        // mixer layout. Empty string means 'no ID yet' (legacy strip);
        // the engine generates one lazily and writes it back.
        juce::String       stripId;

        void reset() noexcept
        {
            peak.store    (0.0f, std::memory_order_relaxed);
            rms.store     (0.0f, std::memory_order_relaxed);
            clipped.store (false, std::memory_order_relaxed);
        }
    };
}
