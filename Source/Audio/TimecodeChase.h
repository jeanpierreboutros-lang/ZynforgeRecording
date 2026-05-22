#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cmath>

namespace zynforge
{
    // Linear Timecode (LTC) + MIDI Time Code (MTC) chase.
    //
    // Phase 1 (current): LTC PRESENCE detection via zero-crossing rate.
    // The audio thread feeds blocks of samples through feedLtc(); the
    // chase counts the per-second zero-crossing count and flags
    // `running` when the rate falls inside the LTC bit-clock band
    // (24 / 25 / 29.97 / 30 fps × 80 bits ≈ 1920..2400 crossings/s).
    //
    // Full bit-perfect SMPTE decoding (recovering hr:mn:sc:fr) is Phase
    // 2 — the bit clock + 80-bit frame + sync-word lookup is real work.
    // What we have now is enough for the engineer to see "LTC detected"
    // in the UI and confirm the desk's timecode line is wired correctly.
    //
    // MTC quarter-frame and full-frame messages are routed through
    // feedMtcQuarterFrame() / feedMtcFullFrame() from the MIDI thread.
    class TimecodeChase
    {
    public:
        void reset() noexcept
        {
            running       .store (false, std::memory_order_relaxed);
            crossingsPerS .store (0.0f,  std::memory_order_relaxed);
            crossings   = 0;
            samplesAcc  = 0;
            prevSample  = 0.0f;
        }

        // Audio-thread feed. Counts zero crossings of the input block;
        // recomputes the per-second rate once samplesAcc has covered
        // ~1 second so the running flag updates ~1 Hz without floating-
        // point divides every block.
        void feedLtc (const float* samples, int n, double sampleRate) noexcept
        {
            if (samples == nullptr || n <= 0 || sampleRate <= 0.0) return;
            for (int i = 0; i < n; ++i)
            {
                const float s = samples[i];
                if ((s >= 0.0f && prevSample < 0.0f) || (s < 0.0f && prevSample >= 0.0f))
                    ++crossings;
                prevSample = s;
            }
            samplesAcc += n;
            if ((double) samplesAcc >= sampleRate)
            {
                const float rate = (float) crossings * (float) (sampleRate / (double) samplesAcc);
                crossingsPerS.store (rate, std::memory_order_relaxed);
                // LTC bit clock is 80 × fps; fps is 24, 25, 29.97 or 30.
                // Zero crossings per second sit at 2 × bit clock in the
                // worst case, so the realistic band is ~1500..5000.
                running.store (rate > 1500.0f && rate < 5000.0f,
                               std::memory_order_relaxed);
                crossings  = 0;
                samplesAcc = 0;
            }
        }

        void feedMtcQuarterFrame (juce::uint8 /*data*/) noexcept
        {
            // Phase 2: assemble 8 quarter-frame nibbles into hr:mn:sc:fr.
            running.store (true, std::memory_order_relaxed);
            lastMtcTickMs = juce::Time::getMillisecondCounter();
        }

        void feedMtcFullFrame (juce::uint8 hr, juce::uint8 mn,
                               juce::uint8 sc, juce::uint8 fr) noexcept
        {
            hours  .store (hr, std::memory_order_relaxed);
            minutes.store (mn, std::memory_order_relaxed);
            seconds.store (sc, std::memory_order_relaxed);
            frames .store (fr, std::memory_order_relaxed);
            running.store (true, std::memory_order_relaxed);
            lastMtcTickMs = juce::Time::getMillisecondCounter();
        }

        // True when an LTC or MTC stream has been seen recently. The
        // LTC side updates ~1 Hz; the MTC side flips when a quarter or
        // full frame arrives. UI polls this at ~5 Hz.
        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        // Last decoded MTC timecode (zeros if MTC has never been seen).
        juce::uint8 getHours()   const noexcept { return hours  .load (std::memory_order_relaxed); }
        juce::uint8 getMinutes() const noexcept { return minutes.load (std::memory_order_relaxed); }
        juce::uint8 getSeconds() const noexcept { return seconds.load (std::memory_order_relaxed); }
        juce::uint8 getFrames()  const noexcept { return frames .load (std::memory_order_relaxed); }

        // Live LTC bit-clock estimate — useful for UI debugging.
        float getCrossingsPerSecond() const noexcept
        {
            return crossingsPerS.load (std::memory_order_relaxed);
        }

    private:
        // LTC state (audio thread).
        int   crossings  { 0 };
        int   samplesAcc { 0 };
        float prevSample { 0.0f };

        // Cross-thread atomics.
        std::atomic<bool>  running       { false };
        std::atomic<float> crossingsPerS { 0.0f };
        std::atomic<juce::uint8> hours   { 0 };
        std::atomic<juce::uint8> minutes { 0 };
        std::atomic<juce::uint8> seconds { 0 };
        std::atomic<juce::uint8> frames  { 0 };
        juce::uint32 lastMtcTickMs       { 0 };
    };
}
