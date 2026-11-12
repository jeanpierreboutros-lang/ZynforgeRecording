#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>

namespace zynforge
{
    // ITU-R BS.1770-style loudness + true-peak meter for the stereo master.
    //
    // Fed by the audio thread from the master accumulator (RT-safe: only
    // fixed-size state, no alloc / lock / log on the hot path beyond the
    // K-weighting biquads and per-block energy). The UI thread reads the
    // momentary / short-term / integrated LUFS + true-peak dBTP.
    //
    // Method:
    //  - Two-stage BS.1770 K-weighting (high-shelf + high-pass) per channel.
    //  - 100 ms gating "chunks"; a 400 ms block = the last 4 chunks.
    //  - Momentary = the latest 400 ms block; short-term = last 3 s.
    //  - Integrated = gated mean of blocks (absolute -70 LUFS gate +
    //    relative -10 LU gate), computed on read from a fixed histogram.
    //  - True-peak via 4x cubic oversampling (estimate, labelled as such).
    class LoudnessMeter
    {
    public:
        void prepare (double sampleRate) noexcept
        {
            sr = sampleRate > 0.0 ? sampleRate : 48000.0;
            designKWeighting();
            chunkLen = juce::jmax (1, (int) std::lround (sr * 0.1));   // 100 ms
            reset();
        }

        void reset() noexcept
        {
            for (auto& s : stL) s = 0.0;
            for (auto& s : stR) s = 0.0;
            chunkAccum = 0.0; chunkCount = 0;
            for (auto& e : ring) e = 0.0;
            ringFill = 0; ringPos = 0;
            for (auto& e : ring3s) e = 0.0;
            ring3sFill = 0; ring3sPos = 0;
            for (auto& h : hist) h.store (0, std::memory_order_relaxed);
            momentary.store (-120.0f, std::memory_order_relaxed);
            shortTerm.store (-120.0f, std::memory_order_relaxed);
            truePeakDb.store (-120.0f, std::memory_order_relaxed);
            prevL = prevL2 = prevL3 = 0.0f;
            prevR = prevR2 = prevR3 = 0.0f;
        }

        // Integrated loudness and true peak start again from here: a take or a
        // session starts its own programme. Any thread may call it while the
        // audio thread feeds the meter: the readings clear at once, and the
        // audio thread drops what it still holds of the old programme (the
        // part-filled 100 ms chunk, the true-peak interpolation history) at
        // its next block. Momentary and short-term go on as they are.
        void resetProgramme() noexcept
        {
            clearProgramme();
            programmeReset.store (true, std::memory_order_release);
        }

        // Process one block of the (post-fader) master, read from the 64-bit
        // accumulator with master gain applied here.
        void processMasterDouble (const double* L, const double* R, double gain, int n) noexcept
        {
            if (programmeReset.exchange (false, std::memory_order_acq_rel))
            {
                chunkAccum = 0.0; chunkCount = 0;
                prevL = prevL2 = prevL3 = 0.0f;
                prevR = prevR2 = prevR3 = 0.0f;
                clearProgramme();
            }
            for (int i = 0; i < n; ++i)
            {
                const double l = L[i] * gain;
                const double r = R[i] * gain;
                accumTruePeak ((float) l, (float) r);

                // K-weight each channel, sum mean-square (with the BS.1770
                // channel weighting of 1.0 for L+R).
                const double fl = kweight (l, stL);
                const double fr = kweight (r, stR);
                chunkAccum += fl * fl + fr * fr;
                if (++chunkCount >= chunkLen)
                {
                    pushChunk (chunkAccum / (double) chunkCount);
                    chunkAccum = 0.0; chunkCount = 0;
                }
            }
        }

        float getMomentaryLufs()  const noexcept { return momentary.load (std::memory_order_relaxed); }
        float getShortTermLufs()  const noexcept { return shortTerm.load (std::memory_order_relaxed); }
        float getTruePeakDb()     const noexcept { return truePeakDb.load (std::memory_order_relaxed); }

        // Integrated loudness with the BS.1770 two-stage gate, computed
        // from the histogram on the UI thread.
        float getIntegratedLufs() const noexcept
        {
            // Pass 1: absolute gate (-70 LUFS), find the ungated mean.
            double sumE = 0.0; juce::int64 cnt = 0;
            for (int b = 0; b < kBins; ++b)
            {
                const auto c = hist[(size_t) b].load (std::memory_order_relaxed);
                if (c == 0) continue;
                const double lufs = binToLufs (b);
                if (lufs < -70.0) continue;
                sumE += (double) c * energyFromLufs (lufs);
                cnt  += c;
            }
            if (cnt == 0) return -120.0f;
            const double ungatedMean = sumE / (double) cnt;
            const double relGate = -0.691 + 10.0 * std::log10 (ungatedMean) - 10.0;
            // Pass 2: relative gate.
            sumE = 0.0; cnt = 0;
            for (int b = 0; b < kBins; ++b)
            {
                const auto c = hist[(size_t) b].load (std::memory_order_relaxed);
                if (c == 0) continue;
                const double lufs = binToLufs (b);
                if (lufs < -70.0 || lufs < relGate) continue;
                sumE += (double) c * energyFromLufs (lufs);
                cnt  += c;
            }
            if (cnt == 0) return -120.0f;
            return (float) (-0.691 + 10.0 * std::log10 (sumE / (double) cnt));
        }

    private:
        void clearProgramme() noexcept
        {
            for (auto& h : hist) h.store (0, std::memory_order_relaxed);
            truePeakDb.store (-120.0f, std::memory_order_relaxed);
        }

        std::atomic<bool> programmeReset { false };

        // ── K-weighting ────────────────────────────────────────────────
        struct Biquad { double b0, b1, b2, a1, a2; };
        Biquad shelf {}, hpf {};

        // The BS.1770 K-weighting. The standard publishes its two biquads at
        // 48 kHz only (shelf b 1.53512485958697, -2.69169618940638,
        // 1.19839281085285, a -1.69065929318241, 0.73248077421585; high-pass
        // b 1, -2, 1, a -1.99004745483398, 0.99007225036621). Below are the
        // analogue prototypes those numbers come from, through the bilinear
        // transform as libebur128 and pyloudnorm take them: the published
        // filter exactly at 48 kHz, the same curve at other rates. An RBJ shelf
        // and high-pass built from the same f0 and Q are a different filter
        // (a 1 kHz tone read 0.26 LU low).
        void designKWeighting() noexcept
        {
            shelf = kWeightingShelf (sr);
            hpf   = kWeightingHighPass (sr);
        }

        static Biquad kWeightingShelf (double rate) noexcept
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K  = std::tan (juce::MathConstants<double>::pi * f0 / rate);
            const double Vh = std::pow (10.0, G / 20.0);
            const double Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            return { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                     2.0 * (K * K - 1.0) / a0,       (1.0 - K / Q + K * K) / a0 };
        }

        // The standard keeps the high-pass numerator at 1, -2, 1, unscaled.
        static Biquad kWeightingHighPass (double rate) noexcept
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K  = std::tan (juce::MathConstants<double>::pi * f0 / rate);
            const double a0 = 1.0 + K / Q + K * K;
            return { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }

        // Per-channel cascade state: [shelf x1,x2,y1,y2][hpf x1,x2,y1,y2].
        std::array<double, 8> stL {}, stR {};

        double kweight (double x, std::array<double, 8>& s) const noexcept
        {
            // Stage 1 (shelf).
            double y = shelf.b0 * x + shelf.b1 * s[0] + shelf.b2 * s[1]
                                    - shelf.a1 * s[2] - shelf.a2 * s[3];
            s[1] = s[0]; s[0] = x; s[3] = s[2]; s[2] = y;
            // Stage 2 (hpf).
            const double x2 = y;
            double y2 = hpf.b0 * x2 + hpf.b1 * s[4] + hpf.b2 * s[5]
                                    - hpf.a1 * s[6] - hpf.a2 * s[7];
            s[5] = s[4]; s[4] = x2; s[7] = s[6]; s[6] = y2;
            return y2;
        }

        // ── Gating blocks ──────────────────────────────────────────────
        int chunkLen { 4800 };
        double chunkAccum { 0.0 };
        int    chunkCount { 0 };
        std::array<double, 4>  ring {};     // last 4 x 100 ms mean-square
        int ringFill { 0 }, ringPos { 0 };
        std::array<double, 30> ring3s {};   // last 30 x 100 ms = 3 s
        int ring3sFill { 0 }, ring3sPos { 0 };

        // -73.0 .. +30.0 LUFS in 0.1 LU bins. The top must sit well above any
        // real momentary level: a value louder than the highest bin clamps INTO
        // it (lufsToBin's jlimit), and getIntegratedLufs then counts that block
        // at the bin's capped energy — biasing hot masters low. +30 LUFS is
        // unreachable in practice, so nothing under-counts.
        static constexpr int kBins = 1031;  // (-73.0 + 0.1*1030) = +30.0 LUFS top
        std::array<std::atomic<std::uint32_t>, kBins> hist {};

        static double binToLufs (int b) noexcept { return -73.0 + 0.1 * (double) b; }
        static int    lufsToBin (double l) noexcept
        { return juce::jlimit (0, kBins - 1, (int) std::lround ((l + 73.0) * 10.0)); }
        static double energyFromLufs (double lufs) noexcept
        { return std::pow (10.0, (lufs + 0.691) / 10.0); }

        void pushChunk (double meanSq100) noexcept
        {
            // 400 ms momentary block = mean of the last 4 chunks.
            ring[(size_t) ringPos] = meanSq100;
            ringPos = (ringPos + 1) % 4;
            if (ringFill < 4) ++ringFill;

            // 3 s short-term ring.
            ring3s[(size_t) ring3sPos] = meanSq100;
            ring3sPos = (ring3sPos + 1) % 30;
            if (ring3sFill < 30) ++ring3sFill;

            if (ringFill >= 4)
            {
                double ms = 0.0; for (double e : ring) ms += e; ms *= 0.25;
                if (ms > 0.0)
                {
                    const double lufs = -0.691 + 10.0 * std::log10 (ms);
                    momentary.store ((float) lufs, std::memory_order_relaxed);
                    if (lufs >= -70.0)
                        hist[(size_t) lufsToBin (lufs)].fetch_add (1, std::memory_order_relaxed);
                }
            }
            if (ring3sFill >= 1)
            {
                double ms = 0.0; for (int i = 0; i < ring3sFill; ++i) ms += ring3s[(size_t) i];
                ms /= (double) ring3sFill;
                if (ms > 0.0)
                    shortTerm.store ((float) (-0.691 + 10.0 * std::log10 (ms)),
                                     std::memory_order_relaxed);
            }
        }

        // ── True peak (4x cubic oversample estimate) ───────────────────
        float prevL { 0 }, prevL2 { 0 }, prevL3 { 0 };
        float prevR { 0 }, prevR2 { 0 }, prevR3 { 0 };

        void accumTruePeak (float l, float r) noexcept
        {
            const float pkL = osPeak (prevL3, prevL2, prevL, l);
            const float pkR = osPeak (prevR3, prevR2, prevR, r);
            prevL3 = prevL2; prevL2 = prevL; prevL = l;
            prevR3 = prevR2; prevR2 = prevR; prevR = r;
            const float pk = juce::jmax (pkL, pkR);
            if (pk > 1.0e-9f)
            {
                const float db = 20.0f * std::log10 (pk);
                if (db > truePeakDb.load (std::memory_order_relaxed))
                    truePeakDb.store (db, std::memory_order_relaxed);
            }
        }

        // Catmull-Rom interpolate p1..p2 at 1/4, 1/2, 3/4 and take the max
        // magnitude of those + the sample itself.
        static float osPeak (float p0, float p1, float p2, float p3) noexcept
        {
            float pk = std::abs (p2);
            for (float t : { 0.25f, 0.5f, 0.75f })
            {
                const float a = p1;
                const float b = 0.5f * (p2 - p0);
                const float c = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
                const float d = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
                const float v = ((d * t + c) * t + b) * t + a;
                pk = juce::jmax (pk, std::abs (v));
            }
            return pk;
        }

        double sr { 48000.0 };
        std::atomic<float> momentary  { -120.0f };
        std::atomic<float> shortTerm  { -120.0f };
        std::atomic<float> truePeakDb { -120.0f };
    };
}
