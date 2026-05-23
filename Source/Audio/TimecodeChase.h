#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cmath>

namespace zynforge
{
    // Linear Timecode (LTC) + MIDI Time Code (MTC) chase.
    //
    // LTC is biphase-mark-encoded: a transition at the start of every
    // bit cell. A '1' has an additional transition in the middle of the
    // cell. The frame is 80 bits at one of 24 / 25 / 29.97 / 30 fps and
    // ends with the sync word 0011111111111101 (LSB-first on the wire =
    // 0xBFFC; MSB-first when assembled = 0x3FFD).
    //
    // The decoder is a zero-crossing → interval → bit-classifier state
    // machine. It learns the half-bit cell length from a running mean
    // of short intervals, then classifies each new interval as either
    // a half-cell or a full cell. Two halves in a row = '1', one full
    // = '0'. Bits shift into a 16-bit window; when the window matches
    // the sync word, the previous 64 bits are parsed as a SMPTE frame.
    //
    // MTC quarter-frame: 8 F1-status messages, each carrying one nibble
    // of hr/mn/sc/fr plus the frame-rate code. After all 8 arrive a
    // full timecode is assembled. Out-of-order nibbles reset the
    // accumulator so a partial transmit doesn't corrupt the readout.
    class TimecodeChase
    {
    public:
        void reset() noexcept
        {
            running       .store (false, std::memory_order_relaxed);
            crossingsPerS .store (0.0f,  std::memory_order_relaxed);
            ltcHours      .store (0, std::memory_order_relaxed);
            ltcMinutes    .store (0, std::memory_order_relaxed);
            ltcSeconds    .store (0, std::memory_order_relaxed);
            ltcFrames     .store (0, std::memory_order_relaxed);
            ltcFps        .store (0.0f, std::memory_order_relaxed);
            ltcDropFrame  .store (false, std::memory_order_relaxed);
            crossings   = 0;
            samplesAcc  = 0;
            prevSample  = 0.0f;
            samplesSinceLastCrossing = 0;
            halfBitMean = 0.0f;
            haveHalfBit = false;
            pendingHalf = false;
            bitWindow   = 0;
            framePos    = 0;
            for (auto& b : frameBits) b = 0;
            mtcAcc = MtcAcc{};
        }

        // Audio-thread feed. Counts zero crossings AND tracks the
        // interval (in samples) between consecutive crossings. Long
        // intervals = full bit cell ('0'); two consecutive short
        // intervals = one '1' bit. Bits accumulate into bitWindow;
        // when the sync word is hit, the prior 64 bits are decoded.
        void feedLtc (const float* samples, int n, double sampleRate) noexcept
        {
            if (samples == nullptr || n <= 0 || sampleRate <= 0.0) return;
            for (int i = 0; i < n; ++i)
            {
                const float s = samples[i];
                ++samplesSinceLastCrossing;

                const bool zc = (s >= 0.0f && prevSample < 0.0f)
                             || (s <  0.0f && prevSample >= 0.0f);
                prevSample = s;

                if (zc)
                {
                    ++crossings;
                    onCrossing (samplesSinceLastCrossing);
                    samplesSinceLastCrossing = 0;
                }
            }

            samplesAcc += n;
            if ((double) samplesAcc >= sampleRate)
            {
                const float rate = (float) crossings * (float) (sampleRate / (double) samplesAcc);
                crossingsPerS.store (rate, std::memory_order_relaxed);
                // LTC bit clock × 2 = zero-crossing rate. fps in
                // 24..30 → 80 bits/frame × 2 → 3840..4800 crossings/s.
                running.store (rate > 1500.0f && rate < 5500.0f,
                               std::memory_order_relaxed);
                crossings  = 0;
                samplesAcc = 0;
            }
        }

        // MTC quarter-frame: F1 nn where nn = (type << 4) | nibble.
        // 8 messages cycle through:
        //   0: frame  LS nibble
        //   1: frame  MS nibble + bit 0 of fps code
        //   2: second LS nibble
        //   3: second MS nibble
        //   4: minute LS nibble
        //   5: minute MS nibble
        //   6: hour   LS nibble
        //   7: hour   MS nibble + bits 1-2 of fps code
        // We accept the message regardless of arrival order, but only
        // publish hr:mn:sc:fr when all 8 nibbles have been seen since
        // the last reset.
        void feedMtcQuarterFrame (juce::uint8 data) noexcept
        {
            const juce::uint8 type   = (data >> 4) & 0x07;
            const juce::uint8 nibble = data & 0x0F;
            mtcAcc.nibbles[type] = nibble;
            mtcAcc.seen |= juce::uint8 (1 << type);

            if (mtcAcc.seen == 0xFF)   // all 8 received
            {
                const juce::uint8 fr =  mtcAcc.nibbles[0]
                                     | ((mtcAcc.nibbles[1] & 0x01) << 4);
                const juce::uint8 sc =  mtcAcc.nibbles[2]
                                     |  (mtcAcc.nibbles[3] << 4);
                const juce::uint8 mn =  mtcAcc.nibbles[4]
                                     |  (mtcAcc.nibbles[5] << 4);
                const juce::uint8 hr =  mtcAcc.nibbles[6]
                                     | ((mtcAcc.nibbles[7] & 0x01) << 4);

                ltcFrames .store (fr, std::memory_order_relaxed);
                ltcSeconds.store (sc, std::memory_order_relaxed);
                ltcMinutes.store (mn, std::memory_order_relaxed);
                ltcHours  .store (hr, std::memory_order_relaxed);
                running   .store (true, std::memory_order_relaxed);
                mtcAcc.seen = 0;
            }
            lastMtcTickMs = juce::Time::getMillisecondCounter();
        }

        // Full-frame MTC message (F0 7F cc 01 01 hr mn sc fr F7). The
        // MIDI driver pre-extracts the 4 bytes and hands them in.
        void feedMtcFullFrame (juce::uint8 hr, juce::uint8 mn,
                               juce::uint8 sc, juce::uint8 fr) noexcept
        {
            ltcHours  .store (hr, std::memory_order_relaxed);
            ltcMinutes.store (mn, std::memory_order_relaxed);
            ltcSeconds.store (sc, std::memory_order_relaxed);
            ltcFrames .store (fr, std::memory_order_relaxed);
            running   .store (true, std::memory_order_relaxed);
            mtcAcc = MtcAcc{};   // restart the QF accumulator
            lastMtcTickMs = juce::Time::getMillisecondCounter();
        }

        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }
        juce::uint8 getHours()   const noexcept { return ltcHours  .load (std::memory_order_relaxed); }
        juce::uint8 getMinutes() const noexcept { return ltcMinutes.load (std::memory_order_relaxed); }
        juce::uint8 getSeconds() const noexcept { return ltcSeconds.load (std::memory_order_relaxed); }
        juce::uint8 getFrames()  const noexcept { return ltcFrames .load (std::memory_order_relaxed); }
        float getCrossingsPerSecond() const noexcept
        {
            return crossingsPerS.load (std::memory_order_relaxed);
        }
        // Detected frame rate (24, 25, 29.97, 30) -- 0 until a sync word lands.
        float getFps() const noexcept { return ltcFps.load (std::memory_order_relaxed); }
        bool  isDropFrame() const noexcept { return ltcDropFrame.load (std::memory_order_relaxed); }

    private:
        struct MtcAcc
        {
            juce::uint8 nibbles[8] {};
            juce::uint8 seen { 0 };   // bitmask of which types have arrived
        };

        // Called for every zero crossing on the LTC line. Maintains the
        // running half-bit-cell estimate and classifies each interval.
        void onCrossing (int intervalSamples) noexcept
        {
            if (intervalSamples <= 0) return;

            if (! haveHalfBit)
            {
                // Bootstrap -- first 32 intervals seed the estimate; we
                // take whichever turns out to be the shorter group.
                if (halfBitMean <= 0.0f) halfBitMean = (float) intervalSamples;
                else                     halfBitMean = halfBitMean * 0.9f + (float) intervalSamples * 0.1f;

                if (++bootstrapCount > 32)
                {
                    haveHalfBit  = true;
                    bootstrapCount = 0;
                }
                return;
            }

            // Classify: short ≈ halfBitMean, long ≈ 2 × halfBitMean. The
            // boundary sits at 1.5 × halfBitMean.
            const float boundary = halfBitMean * 1.5f;
            const bool isShort = (float) intervalSamples < boundary;

            // Update the half-bit estimate with confirmed short intervals
            // (slow EMA, only on shorts so longs don't bias it).
            if (isShort)
                halfBitMean = halfBitMean * 0.97f + (float) intervalSamples * 0.03f;

            // Biphase-mark decode: two consecutive shorts = '1', one long = '0'.
            int bit = -1;
            if (isShort)
            {
                if (pendingHalf) { bit = 1; pendingHalf = false; }
                else             { pendingHalf = true; }
            }
            else
            {
                bit = 0;
                pendingHalf = false;
            }

            if (bit < 0) return;

            // Shift into the rolling window + the 80-bit frame buffer.
            bitWindow = (bitWindow << 1) | (juce::uint16) bit;
            frameBits[framePos] = (juce::uint8) bit;
            framePos = (framePos + 1) % 80;

            // Sync word -- last 16 bits of every LTC frame.
            // 0011111111111101 = 0x3FFD when read in bit order.
            if (bitWindow == 0x3FFD)
                decodeFrame();
        }

        // After a sync word lands, the 64 bits preceding it in framePos
        // are the data field of the just-completed frame. The biphase
        // ordering walks back 80 from framePos (the sync 16 + data 64).
        // We pick the 64 data bits and unpack into SMPTE digits.
        void decodeFrame() noexcept
        {
            juce::uint8 d[80] {};
            for (int i = 0; i < 80; ++i)
                d[i] = frameBits[(framePos + 80 - 80 + i) % 80];

            // Standard LTC frame layout (offsets within d[0..79]):
            //   0-3 : frame units      (BCD)
            //   8-9 : frame tens       (BCD, 2 bits)
            //  10   : drop-frame flag
            //  16-19: second units
            //  24-26: second tens     (3 bits)
            //  32-35: minute units
            //  40-42: minute tens     (3 bits)
            //  48-51: hour units
            //  56-57: hour tens       (2 bits)
            //  64-79: sync word
            auto bits = [&] (int start, int count) -> juce::uint8
            {
                juce::uint8 v = 0;
                for (int i = 0; i < count; ++i)
                    v |= (juce::uint8) (d[start + i] & 1) << i;
                return v;
            };

            const juce::uint8 fu  = bits (0,  4);
            const juce::uint8 ft  = bits (8,  2);
            const bool        df  = (bits (10, 1) != 0);
            const juce::uint8 su  = bits (16, 4);
            const juce::uint8 st  = bits (24, 3);
            const juce::uint8 mu  = bits (32, 4);
            const juce::uint8 mt  = bits (40, 3);
            const juce::uint8 hu  = bits (48, 4);
            const juce::uint8 ht  = bits (56, 2);

            // Reject obviously corrupt frames so a noise burst can't
            // jolt the readout to bogus values.
            if (fu > 9 || su > 9 || mu > 9 || hu > 9
                || ft > 3 || st > 5 || mt > 5 || ht > 2) return;

            const juce::uint8 fr = (juce::uint8) (ft * 10 + fu);
            const juce::uint8 sc = (juce::uint8) (st * 10 + su);
            const juce::uint8 mn = (juce::uint8) (mt * 10 + mu);
            const juce::uint8 hr = (juce::uint8) (ht * 10 + hu);

            ltcHours    .store (hr, std::memory_order_relaxed);
            ltcMinutes  .store (mn, std::memory_order_relaxed);
            ltcSeconds  .store (sc, std::memory_order_relaxed);
            ltcFrames   .store (fr, std::memory_order_relaxed);
            ltcDropFrame.store (df, std::memory_order_relaxed);
            running     .store (true, std::memory_order_relaxed);

            // Infer fps from the crossings/sec rate (80 bits/frame × 2
            // crossings/bit = 160 × fps).
            const float xs = crossingsPerS.load (std::memory_order_relaxed);
            if (xs > 0.0f)
            {
                const float fps = xs / 160.0f;
                if      (fps > 23.5f && fps < 24.5f) ltcFps.store (24.0f, std::memory_order_relaxed);
                else if (fps > 24.5f && fps < 25.5f) ltcFps.store (25.0f, std::memory_order_relaxed);
                else if (fps > 29.0f && fps < 30.5f) ltcFps.store (df ? 29.97f : 30.0f,
                                                                  std::memory_order_relaxed);
            }
        }

        // LTC state (audio thread).
        int   crossings  { 0 };
        int   samplesAcc { 0 };
        float prevSample { 0.0f };
        int   samplesSinceLastCrossing { 0 };
        float halfBitMean { 0.0f };
        int   bootstrapCount { 0 };
        bool  haveHalfBit { false };
        bool  pendingHalf { false };
        juce::uint16 bitWindow { 0 };
        juce::uint8  frameBits[80] {};
        int          framePos { 0 };

        // Cross-thread atomics.
        std::atomic<bool>  running       { false };
        std::atomic<float> crossingsPerS { 0.0f };
        std::atomic<juce::uint8> ltcHours   { 0 };
        std::atomic<juce::uint8> ltcMinutes { 0 };
        std::atomic<juce::uint8> ltcSeconds { 0 };
        std::atomic<juce::uint8> ltcFrames  { 0 };
        std::atomic<float>       ltcFps     { 0.0f };
        std::atomic<bool>        ltcDropFrame { false };
        juce::uint32 lastMtcTickMs { 0 };

        // MTC quarter-frame accumulator.
        MtcAcc mtcAcc;
    };
}
