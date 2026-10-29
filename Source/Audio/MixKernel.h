#pragma once

// The mixing RULES shared by the real-time callback (AudioEngine.cpp) and the
// offline stereo bounce (AudioEngineClips.cpp).
//
// Both paths used to carry their own copy of the solo / mute / VCA gate, the
// stereo-pair automation rule, the mute-lane semantics, the pan law and the
// aux-send gain. A fix in one (a mute lane reaching the sends, a VCA reaching
// the monitor) then had to be found and repeated by hand in the other, and a
// bounce could silently stop matching what the engineer heard. Each rule now
// has exactly one definition, here. AudioCallbackTests renders one session
// both ways and compares them sample for sample, and the invariants gate
// ("mix rules live only in MixKernel.h") refuses a second copy.
//
// Header-only, noexcept and allocation-free: the audio callback calls these
// per strip per block. Nothing here reads engine state -- callers pass the
// values in, so the rules can be tested on their own.

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace zynforge::mix
{
    // Automation lanes a strip's mix reads. Values match
    // AudioEngine::AutomationParam (static_assert'ed there).
    enum class Lane : int { Volume = 0, Pan = 1, Mute = 2 };

    // Stereo pairs store volume + mute automation on the LEFT track, so the
    // RIGHT partner reads its left neighbour's lanes. Pan stays per channel
    // so the image isn't collapsed.
    constexpr int automationChannel (int track, bool previousTrackIsStereoLeft) noexcept
    {
        return (track > 0 && previousTrackIsStereoLeft) ? track - 1 : track;
    }

    // The VCA a strip belongs to, as the solo / mute gate sees it.
    struct VcaGate
    {
        bool assigned { false };
        bool muted    { false };
        bool soloed   { false };
    };

    // Console solo / mute gate: a muted VCA silences its strips; a VCA solo
    // drops every other VCA's strips AND the ungrouped ones; then a strip
    // solo passes only soloed strips (solo beats the strip's own mute);
    // otherwise the strip's mute decides. The Mute LANE is separate -- see
    // StripLanes -- because the paths apply it at different points.
    constexpr bool audible (bool muted, bool soloed, VcaGate vca,
                            bool anySolo, bool anyVcaSolo) noexcept
    {
        if (vca.assigned)
        {
            if (vca.muted) return false;
            if (anyVcaSolo && ! vca.soloed) return false;
        }
        else if (anyVcaSolo)
        {
            return false;
        }
        return anySolo ? soloed : ! muted;
    }

    // One strip's lane-driven mix inputs at one timeline position.
    struct StripLanes
    {
        float volDb        { 0.0f };   // fader, or the Volume lane (+ trim)
        float pan          { 0.0f };   // static pan, or the Pan lane (+ trim)
        bool  muteLane     { false };  // the Mute lane is on (false without a lane)
        bool  monitorMuted { false };  // monitor rule: the lane if present, else the static mute
    };

    // A Mute lane holds 0..1, so a negative fallback means "no lane here".
    inline constexpr float kNoMuteLane = -1.0f;

    // Resolve one strip's lanes. `readLane (channel, Lane, fallback)` returns
    // the lane value or the fallback; the caller already holds the automation
    // lock. `readLanes` is false when the transport is stopped, the lock was
    // lost or automation is suspended -- then the static values stand.
    // `rolling` gates the static monitor mute: while stopped, the monitor
    // follows the solo / mute gate alone (unchanged live behaviour).
    template <typename LaneReader>
    StripLanes resolveLanes (int track, int autoCh, float faderDb, float staticPan,
                             bool staticMute, bool rolling, bool readLanes,
                             LaneReader&& readLane) noexcept
    {
        StripLanes m;
        m.volDb        = faderDb;
        m.pan          = staticPan;
        m.monitorMuted = rolling && staticMute;
        if (readLanes)
        {
            m.volDb = readLane (autoCh, Lane::Volume, m.volDb);
            m.pan   = readLane (track,  Lane::Pan,    m.pan);
            const float lane = readLane (autoCh, Lane::Mute, kNoMuteLane);
            if (lane >= 0.0f) { m.muteLane = lane > 0.5f; m.monitorMuted = m.muteLane; }
        }
        return m;
    }

    // A POST-fader send follows the channel mute like a real console: the
    // strip's own mute, its VCA's mute and the drawn Mute lane all silence it.
    constexpr bool postFaderMuted (bool staticMute, bool vcaMuted, bool muteLane) noexcept
    {
        return staticMute || vcaMuted || muteLane;
    }

    // Aux-send gain. A POST-fader send scales by the strip's post-fader gain
    // (0 when muted); a PRE-fader send is independent of the fader.
    inline float sendGain (float sendDb, bool postFader, float postFaderGain) noexcept
    {
        return juce::Decibels::decibelsToGain (sendDb, -60.0f) * (postFader ? postFaderGain : 1.0f);
    }

    struct PanGains { double left { 0.0 }, right { 0.0 }; };

    // Constant-power pan law (-3 dB at centre). Pan is clamped to [-1, 1].
    inline PanGains constantPowerPan (double gain, float pan) noexcept
    {
        const double p = ((double) juce::jlimit (-1.0f, 1.0f, pan) + 1.0) * 0.5;
        return { gain * std::cos (p * juce::MathConstants<double>::halfPi),
                 gain * std::sin (p * juce::MathConstants<double>::halfPi) };
    }

    // Master fader + mute on the summed bus.
    inline double masterGain (bool muted, float gainDb) noexcept
    {
        return muted ? 0.0 : juce::Decibels::decibelsToGain ((double) gainDb, -60.0);
    }
}
