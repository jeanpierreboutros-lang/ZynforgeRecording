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
    // machine. One FIXED split serves every standard rate: at 24..30 fps a
    // half cell is always shorter than sampleRate / 3000 and a full cell
    // always longer, so there is no learned estimate to drift onto the wrong
    // side (the old running mean could settle between the two and then read
    // every cell as "short"). Two halves in a row = '1', one full = '0'; an
    // interval outside both ranges is a bit error. Bits shift into a 16-bit
    // window; a sync word only yields a frame when EXACTLY 80 clean bits have
    // passed since the previous sync word.
    //
    // LOCK (isLtcLocked) needs three such frames in a row whose labels count
    // up one frame at a time. It then holds through a relocate as long as
    // valid frames keep arriving, and drops a quarter second after they stop:
    // counted in samples while the strip is still fed, and in wall-clock time
    // when the feed itself stops (strip unrouted, device change). The frame
    // rate is the measured sync-to-sync period, floored by the highest frame
    // number seen -- never the zero-crossing rate, which depends on how many
    // '1' bits a frame carries (about 100-110 crossings per frame, not 160).
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
            lastMtcTickMs .store (0, std::memory_order_relaxed);
            ltcLocked     .store (false, std::memory_order_relaxed);
            lastLtcFrameMs.store (0, std::memory_order_relaxed);
            crossings   = 0;
            samplesAcc  = 0;
            ltcHigh     = false;
            ltcEnvelope = 0.0f;
            samplesSinceLastCrossing = 0;
            pendingHalf = false;
            frameError  = true;
            bitsSinceSync = 0;
            bitWindow   = 0;
            framePos    = 0;
            for (auto& b : frameBits) b = 0;
            sampleClock     = 0;
            lastSyncSample  = -1;
            lastValidSample = -1;
            chain = 0;
            prevFrameCount = -1;
            maxFrameSeen = 0;
            locked = false;
            mtcAcc = MtcAcc{};
        }

        // Race-free subset of reset() that is safe to call from the message
        // thread WHILE the audio (feedLtc) / MIDI (feedMtc*) threads keep
        // feeding. It clears only the atomics that gate isRunning()/freewheel;
        // the non-atomic decoder scratch (mtcAcc, crossings, frameBits, ...) is
        // self-healing and must NOT be written from another thread. Use this,
        // not reset(), for a live mode switch.
        void clearFreewheel() noexcept
        {
            running       .store (false, std::memory_order_relaxed);
            lastMtcTickMs .store (0,     std::memory_order_relaxed);
            ltcLocked     .store (false, std::memory_order_relaxed);
            lastLtcFrameMs.store (0,     std::memory_order_relaxed);
        }

        // Audio-thread feed: no locks, no allocation. A Schmitt trigger at 15%
        // of a slow peak follower finds the transitions (so a noise floor
        // can't chatter around zero), each interval between two of them is
        // classified as a half or full bit cell, and complete frames are
        // decoded on their sync word.
        void feedLtc (const float* samples, int n, double sampleRate) noexcept
        {
            if (samples == nullptr || n <= 0 || sampleRate <= 0.0) return;

            float blockPeak = 0.0f;
            for (int i = 0; i < n; ++i) blockPeak = juce::jmax (blockPeak, std::abs (samples[i]));
            ltcEnvelope = juce::jmax (blockPeak, ltcEnvelope * 0.8f);
            const float hysteresis = juce::jmax (1.0e-4f, ltcEnvelope * 0.15f);
            const Cells cells { sampleRate };

            for (int i = 0; i < n; ++i)
            {
                if (samplesSinceLastCrossing < (1 << 30)) ++samplesSinceLastCrossing;
                const float s = samples[i];
                if (ltcHigh ? (s < -hysteresis) : (s > hysteresis))
                {
                    ltcHigh = ! ltcHigh;
                    ++crossings;
                    onInterval (samplesSinceLastCrossing, sampleClock + i, cells);
                    samplesSinceLastCrossing = 0;
                }
            }
            sampleClock += n;

            samplesAcc += n;
            if ((double) samplesAcc >= sampleRate)
            {
                // Diagnostic only. It no longer decides anything: a tone or
                // program audio crosses zero at LTC-like rates too.
                crossingsPerS.store ((float) crossings * (float) (sampleRate / (double) samplesAcc),
                                     std::memory_order_relaxed);
                crossings  = 0;
                samplesAcc = 0;
            }

            // Valid frames stopped arriving on a strip that is still fed.
            if (locked && sampleClock - lastValidSample > (juce::int64) (sampleRate * kLtcDropSeconds))
            {
                locked = false;
                chain = 0;
                maxFrameSeen = 0;
                ltcLocked.store (false, std::memory_order_release);
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
                const auto fr = static_cast<juce::uint8> (mtcAcc.nibbles[0]
                              | ((mtcAcc.nibbles[1] & 0x01) << 4));
                const auto sc = static_cast<juce::uint8> (mtcAcc.nibbles[2]
                              |  (mtcAcc.nibbles[3] << 4));
                const auto mn = static_cast<juce::uint8> (mtcAcc.nibbles[4]
                              |  (mtcAcc.nibbles[5] << 4));
                const auto hr = static_cast<juce::uint8> (mtcAcc.nibbles[6]
                              | ((mtcAcc.nibbles[7] & 0x01) << 4));

                ltcFrames .store (fr, std::memory_order_relaxed);
                ltcSeconds.store (sc, std::memory_order_relaxed);
                ltcMinutes.store (mn, std::memory_order_relaxed);
                ltcHours  .store (hr, std::memory_order_relaxed);
                running   .store (true, std::memory_order_relaxed);

                // Frame-rate lives in the hour-MS nibble (type 7): bits 1-2.
                // 0=24, 1=25, 2=29.97-drop, 3=30. Publish it so the chaser
                // maps frames→samples correctly (LTC derives fps differently).
                const int rateCode = (mtcAcc.nibbles[7] >> 1) & 0x3;
                ltcFps.store (rateCode == 0 ? 24.0f : rateCode == 1 ? 25.0f
                            : rateCode == 2 ? 29.97f : 30.0f, std::memory_order_relaxed);
                ltcDropFrame.store (rateCode == 2, std::memory_order_relaxed);
                mtcAcc.seen = 0;
            }
            lastMtcTickMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
        }

        // Full-frame MTC message (F0 7F cc 01 01 hr mn sc fr F7). The
        // MIDI driver pre-extracts the 4 bytes and hands them in.
        void feedMtcFullFrame (juce::uint8 hr, juce::uint8 mn,
                               juce::uint8 sc, juce::uint8 fr,
                               float fps = 0.0f, bool dropFrame = false) noexcept
        {
            // Honour the full-frame's SMPTE rate (from the message's timecode
            // type) so a 24/25 fps locate seeks to the right sample immediately,
            // not the 30fps default until quarter-frames start. fps<=0 => keep
            // whatever the last frame rate was.
            if (fps > 0.0f)
            {
                ltcFps.store (fps, std::memory_order_relaxed);
                ltcDropFrame.store (dropFrame, std::memory_order_relaxed);
            }
            ltcHours  .store (hr, std::memory_order_relaxed);
            ltcMinutes.store (mn, std::memory_order_relaxed);
            ltcSeconds.store (sc, std::memory_order_relaxed);
            ltcFrames .store (fr, std::memory_order_relaxed);
            running   .store (true, std::memory_order_relaxed);
            mtcAcc = MtcAcc{};   // restart the QF accumulator
            lastMtcTickMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
        }

        // MTC is live: a full time has been assembled and quarter-frames are
        // still arriving. An MTC master streams quarter-frames continuously
        // (~every 8-10 ms); `running` is set true on each but never cleared
        // by the MIDI thread when the master stops, so honour the last-tick
        // time: no MTC for kMtcTimeoutMs => stopped.
        bool isMtcRunning() const noexcept
        {
            if (! running.load (std::memory_order_relaxed)) return false;
            const juce::uint32 last = lastMtcTickMs.load (std::memory_order_relaxed);
            if (last != 0
                && (juce::uint32) (juce::Time::getMillisecondCounter() - last) > kMtcTimeoutMs)
                return false;
            return true;
        }
        // LTC LOCK: decoded, consistent frames, the last one recent. The
        // wall-clock check covers a feed that simply stopped being called.
        bool isLtcLocked() const noexcept
        {
            if (! ltcLocked.load (std::memory_order_acquire)) return false;
            const juce::uint32 last = lastLtcFrameMs.load (std::memory_order_relaxed);
            return (juce::uint32) (juce::Time::getMillisecondCounter() - last) <= kLtcTimeoutMs;
        }
        bool isRunning() const noexcept { return isMtcRunning() || isLtcLocked(); }
        juce::uint8 getHours()   const noexcept { return ltcHours  .load (std::memory_order_relaxed); }
        juce::uint8 getMinutes() const noexcept { return ltcMinutes.load (std::memory_order_relaxed); }
        juce::uint8 getSeconds() const noexcept { return ltcSeconds.load (std::memory_order_relaxed); }
        juce::uint8 getFrames()  const noexcept { return ltcFrames .load (std::memory_order_relaxed); }
        float getCrossingsPerSecond() const noexcept
        {
            return crossingsPerS.load (std::memory_order_relaxed);
        }
        // Detected frame rate (24, 25, 29.97, 30) -- 0 until a frame decodes.
        float getFps() const noexcept { return ltcFps.load (std::memory_order_relaxed); }
        bool  isDropFrame() const noexcept { return ltcDropFrame.load (std::memory_order_relaxed); }

    private:
        struct MtcAcc
        {
            juce::uint8 nibbles[8] {};
            juce::uint8 seen { 0 };   // bitmask of which types have arrived
        };

        // Bit-cell limits for one sample rate. A half cell runs sr/4800 (30
        // fps) .. sr/3840 (24 fps), a full cell sr/2400 .. sr/1920; `split`
        // sits between the two groups with ~20% margin either side.
        struct Cells
        {
            explicit Cells (double sr) noexcept
                : shortest (sr / 9600.0), split (sr / 3000.0), longest (sr / 1280.0), rate (sr) {}
            double shortest, split, longest, rate;
        };

        void onInterval (int intervalSamples, juce::int64 at, const Cells& cells) noexcept
        {
            if (intervalSamples < cells.shortest || intervalSamples > cells.longest)
            {
                // Not a bit cell at all (noise, silence ending, a dropout):
                // whatever frame is in flight is damaged.
                pendingHalf = false;
                frameError  = true;
                bitWindow   = 0;
                return;
            }

            int bit = 0;
            if (intervalSamples < cells.split)
            {
                if (! pendingHalf) { pendingHalf = true; return; }
                pendingHalf = false;
                bit = 1;
            }
            else if (pendingHalf)
            {
                // A lone half cell: we were out of step. The full cell that
                // follows starts on a real bit boundary again.
                pendingHalf = false;
                frameError  = true;
            }

            bitWindow = static_cast<juce::uint16> ((bitWindow << 1) | static_cast<juce::uint16> (bit));
            frameBits[framePos] = (juce::uint8) bit;
            framePos = (framePos + 1) % 80;
            if (bitsSinceSync < 1000) ++bitsSinceSync;

            // Sync word -- last 16 bits of every LTC frame.
            // 0011111111111101 = 0x3FFD when read in bit order.
            if (bitWindow == 0x3FFD)
                onSync (at, cells.rate);
        }

        void onSync (juce::int64 at, double sr) noexcept
        {
            // A frame is the 80 bits since the previous sync word, decoded
            // without a single bit error; its period is sync-to-sync.
            const bool whole = bitsSinceSync == 80 && ! frameError && lastSyncSample >= 0;
            const juce::int64 period = at - lastSyncSample;
            lastSyncSample = at;
            bitsSinceSync  = 0;
            frameError     = false;
            if (! whole || period < (juce::int64) (sr / 31.5) || period > (juce::int64) (sr / 22.5))
            {
                chain = 0;
                return;
            }
            decodeFrame (sr / (double) period, at);
        }

        // framePos now points at the oldest of the last 80 bits, which is
        // bit 0 of the frame whose sync word just completed.
        void decodeFrame (double measuredFps, juce::int64 at) noexcept
        {
            juce::uint8 d[80] {};
            for (int i = 0; i < 80; ++i)
                d[i] = frameBits[(framePos + i) % 80];

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
            const juce::uint8 fr = (juce::uint8) (ft * 10 + fu);
            const juce::uint8 sc = (juce::uint8) (st * 10 + su);
            const juce::uint8 mn = (juce::uint8) (mt * 10 + mu);
            const juce::uint8 hr = (juce::uint8) (ht * 10 + hu);
            // ht is 2 bits + hu 0-9 can encode hours 24-29 (e.g. ht=2,hu=9);
            // a real SMPTE hour is 0-23, and no rate has a frame 30.
            if (fu > 9 || su > 9 || mu > 9 || hu > 9
                || st > 5 || mt > 5 || hr > 23 || fr > 29)
            {
                chain = 0;
                return;
            }

            // Frame rate from the period, snapped to the standard set; 29.97
            // vs 30 by the drop-frame flag. A frame number the rate cannot
            // hold (frame 24 at "24 fps") means the guess is too low.
            if (! locked && chain == 0) maxFrameSeen = 0;   // a fresh acquisition
            maxFrameSeen = juce::jmax (maxFrameSeen, (int) fr);
            float fps = measuredFps < 24.5 ? 24.0f : measuredFps < 27.5 ? 25.0f : 30.0f;
            if (maxFrameSeen >= (int) fps) fps = maxFrameSeen < 25 ? 25.0f : 30.0f;
            const int  nominal = (int) fps;
            const bool drop    = nominal == 30 && df;
            if (drop) fps = 29.97f;

            // Labels count up one frame at a time (drop-frame skips two
            // labels at most minute starts; midnight wraps).
            const int day   = 24 * 3600 * nominal;
            const int count = ((hr * 60 + mn) * 60 + sc) * nominal + fr;
            const bool countsUp = prevFrameCount >= 0
                && (count == (prevFrameCount + 1) % day || (drop && count == (prevFrameCount + 3) % day));
            chain = countsUp ? chain + 1 : 1;
            prevFrameCount  = count;
            lastValidSample = at;

            ltcHours    .store (hr, std::memory_order_relaxed);
            ltcMinutes  .store (mn, std::memory_order_relaxed);
            ltcSeconds  .store (sc, std::memory_order_relaxed);
            ltcFrames   .store (fr, std::memory_order_relaxed);
            ltcDropFrame.store (drop, std::memory_order_relaxed);
            ltcFps      .store (fps, std::memory_order_relaxed);

            if (! locked && chain >= kFramesToLock) locked = true;
            if (locked)
            {
                lastLtcFrameMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
                ltcLocked.store (true, std::memory_order_release);
            }
        }

        // LTC state (audio thread).
        int   crossings  { 0 };
        int   samplesAcc { 0 };
        bool  ltcHigh    { false };    // Schmitt-trigger output
        float ltcEnvelope { 0.0f };    // slow peak follower for the hysteresis
        int   samplesSinceLastCrossing { 0 };
        bool  pendingHalf { false };
        bool  frameError  { true };    // a bit error since the last sync word
        int   bitsSinceSync { 0 };
        juce::uint16 bitWindow { 0 };
        juce::uint8  frameBits[80] {};
        int          framePos { 0 };
        juce::int64  sampleClock     { 0 };    // samples fed so far
        juce::int64  lastSyncSample  { -1 };
        juce::int64  lastValidSample { -1 };   // last decoded frame
        int          chain { 0 };              // consecutive count-up frames
        int          prevFrameCount { -1 };
        int          maxFrameSeen { 0 };
        bool         locked { false };         // audio-thread view of ltcLocked
        static constexpr int    kFramesToLock   = 3;
        static constexpr double kLtcDropSeconds = 0.25;

        // Cross-thread atomics.
        std::atomic<bool>  running       { false };   // MTC only
        std::atomic<bool>  ltcLocked     { false };
        std::atomic<float> crossingsPerS { 0.0f };
        std::atomic<juce::uint8> ltcHours   { 0 };
        std::atomic<juce::uint8> ltcMinutes { 0 };
        std::atomic<juce::uint8> ltcSeconds { 0 };
        std::atomic<juce::uint8> ltcFrames  { 0 };
        std::atomic<float>       ltcFps     { 0.0f };
        std::atomic<bool>        ltcDropFrame { false };
        // Wall-clock ms of the last MTC tick (0 = never). Read on the
        // message thread (isRunning), written on the MIDI thread -> atomic.
        std::atomic<juce::uint32> lastMtcTickMs { 0 };
        // Wall-clock ms of the last decoded LTC frame while locked.
        std::atomic<juce::uint32> lastLtcFrameMs { 0 };
        static constexpr juce::uint32 kLtcTimeoutMs = 300;
        // No MTC for this long => the master stopped (quarter-frames arrive
        // ~every 8-10 ms; 250 ms is ~7-8 frames, well clear of jitter).
        static constexpr juce::uint32 kMtcTimeoutMs = 250;

        // MTC quarter-frame accumulator.
        MtcAcc mtcAcc;
    };
}
