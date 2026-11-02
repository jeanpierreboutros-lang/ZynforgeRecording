#pragma once

// The mixing RULES shared by the real-time callback (AudioEngine.cpp) and the
// offline stereo bounce (AudioEngineClips.cpp).
//
// Both paths used to carry their own copy of the solo / mute / VCA gate, the
// pan law and the aux-send gain. A fix in one (a VCA reaching the monitor) then
// had to be found and repeated by hand in the other, and a bounce could
// silently stop matching what the engineer heard. Each rule now
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
    // otherwise the strip's mute decides.
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

    // The stream bus and StereoMix.wav are a record of the mix, and solo is a
    // monitoring tool: it must not strip the show out of the file because the
    // engineer checked a mic. Only a mute removes a strip here.
    constexpr bool streamAudible (bool muted, VcaGate vca) noexcept
    {
        return audible (muted, false, vca, false, false);
    }

    // A strip's fader and its VCA meet in dB, and either one
    // at its -60 dB floor is OFF, as on a console. Summing first left a strip
    // pulled to the floor at -48 dB under a VCA at +12, and a VCA at the floor
    // still passed strips set above 0 dB. `vcaDb` is 0 for an unassigned
    // strip. kOffDb stays far below the floor even after a trim is added.
    inline constexpr float kFaderFloorDb = -60.0f;
    inline constexpr float kOffDb        = -200.0f;
    constexpr float stripDb (float faderDb, float vcaDb) noexcept
    {
        return (faderDb <= kFaderFloorDb || vcaDb <= kFaderFloorDb) ? kOffDb : faderDb + vcaDb;
    }

    // A POST-fader send follows the channel mute like a real console: the
    // strip's own mute and its VCA's mute both silence it.
    constexpr bool postFaderMuted (bool stripMute, bool vcaMuted) noexcept
    {
        return stripMute || vcaMuted;
    }

    // Aux-send gain. A POST-fader send scales by the strip's post-fader gain
    // (0 when muted); a PRE-fader send is independent of the fader.
    inline float sendGain (float sendDb, bool postFader, float postFaderGain) noexcept
    {
        return juce::Decibels::decibelsToGain (sendDb, -60.0f) * (postFader ? postFaderGain : 1.0f);
    }

    // A stereo strip is one send. On a mono bus its two halves land together,
    // so each brings half, and a centred source arrives at the level the send
    // shows rather than 6 dB over it.
    constexpr float sendFoldGain (bool sourceIsStereoHalf, bool busIsStereoHalf) noexcept
    {
        return sourceIsStereoHalf && ! busIsStereoHalf ? 0.5f : 1.0f;
    }

    // Trim-Follow: the desk's preamp move since the take was captured, added
    // to recorded playback. With no capture reference there is no "move" to
    // follow, only the preamp's absolute value, so the offset is 0 dB. The
    // limits are a PA backstop against a wrong reference or a spoofed reading:
    // louder is the dangerous side, so it gets the tighter limit.
    inline constexpr float kTrimFollowMaxUpDb   =  12.0f;
    inline constexpr float kTrimFollowMaxDownDb = -30.0f;
    constexpr float trimFollowDb (bool referenceKnown, float liveDb, float captureDb) noexcept
    {
        if (! referenceKnown) return 0.0f;
        const float move = liveDb - captureDb;
        return move > kTrimFollowMaxUpDb ? kTrimFollowMaxUpDb
             : move < kTrimFollowMaxDownDb ? kTrimFollowMaxDownDb : move;
    }

    struct PanGains { double left { 0.0 }, right { 0.0 }; };

    // Constant-power pan law (-3 dB at centre). Pan is clamped to [-1, 1].
    inline PanGains constantPowerPan (double gain, float pan) noexcept
    {
        const double p = ((double) juce::jlimit (-1.0f, 1.0f, pan) + 1.0) * 0.5;
        return { gain * std::cos (p * juce::MathConstants<double>::halfPi),
                 gain * std::sin (p * juce::MathConstants<double>::halfPi) };
    }

    struct OutputPair { int left { -1 }, right { -1 }; };

    // The device outputs the master bus uses. A negative left means no master
    // output was chosen. A device smaller than the saved pair gets its last
    // pair, two channels still: clamping each side on its own put both on the
    // last output, and the right half of the mix was dropped.
    constexpr OutputPair masterOutputs (int left, int right, int numOutputs) noexcept
    {
        if (left < 0 || numOutputs <= 0) return {};
        if (left < numOutputs && right >= 0 && right < numOutputs) return { left, right };
        return numOutputs >= 2 ? OutputPair { numOutputs - 2, numOutputs - 1 } : OutputPair { 0, 0 };
    }

    // A mix bus stays off an output a strip is patched to. 1:1 strips on a rig
    // whose outputs are all console returns would otherwise put the whole mix
    // on the console channels of the first pair, on top of their own tracks.
    // A two-output device has nowhere else, and there it is the monitor pair.
    constexpr bool busHeldOffOutputs (bool sharesStripOutput, int numOutputs, bool sharingAllowed) noexcept
    {
        return sharesStripOutput && numOutputs > 2 && ! sharingAllowed;
    }

    // Master fader + mute on the summed bus.
    inline double masterGain (bool muted, float gainDb) noexcept
    {
        return muted ? 0.0 : juce::Decibels::decibelsToGain ((double) gainDb, -60.0);
    }
}
