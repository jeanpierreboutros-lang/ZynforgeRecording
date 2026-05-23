#pragma once

// SIMD helpers for the 64-bit-accumulator audio path. The hot loops
// are float-source → double-accumulator FMA (per-channel + stream-bus
// sums) and double → float downcast (final master output). Clang at
// -O3 already auto-vectorises the scalar loops, but the hand-rolled
// NEON path on Apple Silicon shaves a measurable percentage at 256+
// tracks and guarantees the same code shape across LLVM revisions.
//
// Fallbacks: scalar loop for non-ARM64 and any odd-sized tail.

#if defined(__aarch64__) || defined(_M_ARM64)
 #include <arm_neon.h>
 #define ZYNFORGE_HAVE_NEON 1
#else
 #define ZYNFORGE_HAVE_NEON 0
#endif

namespace zynforge { namespace fastaccum
{
    // dst[i] += (double) src[i] * gain
    inline void addFloatScaledIntoDouble (double* dst,
                                           const float* src,
                                           double gain,
                                           int numSamples) noexcept
    {
        int s = 0;
       #if ZYNFORGE_HAVE_NEON
        // Process 4 floats / 4 doubles per iteration. Apple Silicon
        // P-cores can sustain 1 vfmaq_f64 + 1 vld + 1 vst per cycle
        // on the same vector unit; this works out to ~4 samples /
        // 3 cycles in steady state.
        const float64x2_t g = vdupq_n_f64 (gain);
        for (; s + 4 <= numSamples; s += 4)
        {
            const float32x4_t f      = vld1q_f32 (src + s);
            const float64x2_t lo     = vcvt_f64_f32 (vget_low_f32 (f));
            const float64x2_t hi     = vcvt_high_f64_f32 (f);
            float64x2_t       accLo  = vld1q_f64 (dst + s);
            float64x2_t       accHi  = vld1q_f64 (dst + s + 2);
            accLo = vfmaq_f64 (accLo, lo, g);
            accHi = vfmaq_f64 (accHi, hi, g);
            vst1q_f64 (dst + s,     accLo);
            vst1q_f64 (dst + s + 2, accHi);
        }
       #endif
        for (; s < numSamples; ++s)
            dst[s] += (double) src[s] * gain;
    }

    // dst[i] += (double) src[i] * gain — same as above but adds
    // the FMA into an EXISTING double accumulator. Identical body
    // — kept as a distinct name for call-site clarity.
    inline void mulAddFloatToDouble (double* dst,
                                      const float* src,
                                      double gain,
                                      int numSamples) noexcept
    {
        addFloatScaledIntoDouble (dst, src, gain, numSamples);
    }

    // dst[i] = (float) src[i]  — the final master downcast at the end
    // of the audio callback. Handles N samples; pairs of float64x2
    // narrow into one float32x4 via vcvt_high_f32_f64.
    inline void downcastDoubleToFloat (float* dst,
                                        const double* src,
                                        int numSamples) noexcept
    {
        int s = 0;
       #if ZYNFORGE_HAVE_NEON
        for (; s + 4 <= numSamples; s += 4)
        {
            const float64x2_t lo  = vld1q_f64 (src + s);
            const float64x2_t hi  = vld1q_f64 (src + s + 2);
            const float32x2_t fLo = vcvt_f32_f64 (lo);
            const float32x4_t f   = vcvt_high_f32_f64 (fLo, hi);
            vst1q_f32 (dst + s, f);
        }
       #endif
        for (; s < numSamples; ++s)
            dst[s] = (float) src[s];
    }
}}   // namespace zynforge::fastaccum

#undef ZYNFORGE_HAVE_NEON
