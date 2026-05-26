#include "ClickEngine.h"
#include <cmath>

namespace zynforge
{
    ClickEngine::VoicePreset ClickEngine::getVoicePreset (Voice v) noexcept
    {
        switch (v)
        {
            case Voice::Sine:      return { 880.0,   30.0 };
            case Voice::Beep:      return { 1500.0,  80.0 };
            case Voice::Cowbell:   return { 540.0,   18.0 };
            case Voice::WoodBlock: return { 1200.0, 100.0 };
            case Voice::Click:     return { 2200.0, 140.0 };
        }
        return { 1000.0, 60.0 };
    }

    double ClickEngine::subFactor (Subdivision s) noexcept
    {
        switch (s)
        {
            case Subdivision::Whole:     return 1.0 / 4.0;   // 1 beat per 4 quarter-notes
            case Subdivision::Half:      return 1.0 / 2.0;   // 1 beat per 2 quarter-notes
            case Subdivision::Quarter:   return 1.0;          // 1 beat per quarter-note
            case Subdivision::Eighth:    return 2.0;          // 2 per quarter
            case Subdivision::Sixteenth: return 4.0;          // 4 per quarter
            case Subdivision::Triplet:   return 3.0;          // 3 per quarter
            case Subdivision::Silent:    return 0.0;          // no clicks
        }
        return 1.0;
    }

    void ClickEngine::triggerBurst (Burst& b, Voice v, double gainLin) noexcept
    {
        b.tSec    = 0.0;
        b.phase   = 0.0;
        b.active  = true;
        b.gainLin = gainLin;
        // Per-voice tonal flavour. All damped sines -- fast, real-time
        // safe, sound distinct enough at metronome volume.
        switch (v)
        {
            case Voice::Sine:      b.freq = 880.0;  b.decay = 30.0;  break;
            case Voice::Beep:      b.freq = 1500.0; b.decay = 80.0;  break;
            case Voice::Cowbell:   b.freq = 540.0;  b.decay = 18.0;  break;
            case Voice::WoodBlock: b.freq = 1200.0; b.decay = 100.0; break;
            case Voice::Click:     b.freq = 2200.0; b.decay = 140.0; break;
        }
    }

    float ClickEngine::renderBurst (Burst& b, double sampleRate) noexcept
    {
        if (! b.active) return 0.0f;
        const double dt   = 1.0 / sampleRate;
        const double env  = std::exp (-b.tSec * b.decay);
        const double samp = std::sin (b.phase) * env * b.gainLin;
        b.phase += juce::MathConstants<double>::twoPi * b.freq * dt;
        b.tSec  += dt;
        if (env < 0.001) b.active = false;
        return (float) samp;
    }

    void ClickEngine::prepare (double sr)
    {
        sampleRate = sr;
        samplesUntilNextBeat1 = 0.0;   // fire immediately on first block
        samplesUntilNextBeat2 = 0.0;
        beat1Counter = 0;
        beat2Counter = 0;
        voice1Burst  = {};
        voice2Burst  = {};
    }

    void ClickEngine::processBlock (float* outL, float* outR, int numSamples)
    {
        if (! enabled.load (std::memory_order_relaxed)) return;
        if (sampleRate <= 0.0) return;

        const float bpm = tempoBpm.load (std::memory_order_relaxed);
        const double samplesPerQuarter = 60.0 * sampleRate / juce::jmax (20.0f, bpm);

        const auto sub1Now = (Subdivision) sub1.load (std::memory_order_relaxed);
        const auto sub2Now = (Subdivision) sub2.load (std::memory_order_relaxed);
        const double f1 = subFactor (sub1Now);
        const double f2 = subFactor (sub2Now);
        const double samplesPerClick1 = (f1 > 0.0) ? (samplesPerQuarter / f1) : 0.0;
        const double samplesPerClick2 = (f2 > 0.0) ? (samplesPerQuarter / f2) : 0.0;

        const int bpb = juce::jmax (1, beatsPerBar.load (std::memory_order_relaxed));
        const auto v1 = (Voice) voice1.load (std::memory_order_relaxed);
        const auto v2 = (Voice) voice2.load (std::memory_order_relaxed);
        const float lin1 = juce::Decibels::decibelsToGain (vol1Db.load (std::memory_order_relaxed));
        const float lin2 = juce::Decibels::decibelsToGain (vol2Db.load (std::memory_order_relaxed));

        for (int i = 0; i < numSamples; ++i)
        {
            // CLICK 1 -- accent, only on every 4th beat of the underlying
            // quarter-note grid. Silent if Subdivision::Silent.
            if (samplesPerClick1 > 0.0)
            {
                samplesUntilNextBeat1 -= 1.0;
                if (samplesUntilNextBeat1 <= 0.0)
                {
                    samplesUntilNextBeat1 += samplesPerClick1;
                    if ((beat1Counter % bpb) == 0)
                        triggerBurst (voice1Burst, v1, lin1);
                    ++beat1Counter;
                }
            }
            // CLICK 2 -- on the other beats. We don't want a stack-up
            // with CLICK 1 on the downbeat, so skip when beat2Counter
            // % 4 == 0 (only when subs are aligned, i.e. quarter-note).
            if (samplesPerClick2 > 0.0)
            {
                samplesUntilNextBeat2 -= 1.0;
                if (samplesUntilNextBeat2 <= 0.0)
                {
                    samplesUntilNextBeat2 += samplesPerClick2;
                    const bool isDownbeatAligned =
                        (sub2Now == Subdivision::Quarter) && (beat2Counter % bpb) == 0;
                    if (! isDownbeatAligned)
                        triggerBurst (voice2Burst, v2, lin2);
                    ++beat2Counter;
                }
            }

            const float s = renderBurst (voice1Burst, sampleRate)
                          + renderBurst (voice2Burst, sampleRate);
            if (outL != nullptr) outL[i] += s;
            if (outR != nullptr) outR[i] += s;
        }
    }
}
