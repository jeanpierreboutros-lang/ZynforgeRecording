#pragma once

#include <array>
#include <cmath>
#include <vector>

// Synthesized SMPTE 12M timecode, shared by the suites that need a decoder to
// reach LOCK without a timecode generator.
namespace zynforge::ltctest
{
    struct Tc { int h, m, s, f; };

    // One whole MTC quarter-frame run (types 0..7) labelling `t`: what makes
    // MTC chase live. A full frame alone is a cue and never does. rateCode
    // 0 = 24, 1 = 25, 2 = 29.97 drop, 3 = 30.
    inline std::array<unsigned char, 8> mtcQuarterFrames (Tc t, int rateCode)
    {
        const int nibbles[8] { t.f & 0x0F, (t.f >> 4) & 0x01, t.s & 0x0F, (t.s >> 4) & 0x03,
                               t.m & 0x0F, (t.m >> 4) & 0x03, t.h & 0x0F, ((t.h >> 4) & 0x01) | (rateCode << 1) };
        std::array<unsigned char, 8> run {};
        for (int type = 0; type < 8; ++type) run[(size_t) type] = (unsigned char) ((type << 4) | nibbles[type]);
        return run;
    }

    // Biphase-mark: a transition at every bit start, plus one mid-cell
    // for a '1'. `clock` is the running time in (fractional) samples.
    inline void appendBit (std::vector<float>& out, double& clock, float& level, double bitLen, int bit)
    {
        auto emitUntil = [&] (double t) { while ((double) out.size() < t) out.push_back (level); };
        level = -level;
        emitUntil (clock + bitLen * 0.5);
        if (bit != 0) level = -level;
        emitUntil (clock + bitLen);
        clock += bitLen;
    }

    // SMPTE 12M LTC: `count` consecutive frames from `start`, with the
    // drop-frame label skip when `df`.
    inline std::vector<float> ltcSignal (double sr, double fps, bool df, Tc start, int count)
    {
        std::vector<float> out;
        double clock = 0.0; float level = 0.5f;
        const int nominal = (int) std::lround (fps);
        const double bitLen = sr / (80.0 * fps);
        Tc t = start;
        for (int n = 0; n < count; ++n)
        {
            int bits[80] {};
            auto put = [&] (int at, int width, int value)
            { for (int i = 0; i < width; ++i) bits[at + i] = (value >> i) & 1; };
            put (0, 4, t.f % 10); put (8, 2, t.f / 10); bits[10] = df ? 1 : 0;
            put (16, 4, t.s % 10); put (24, 3, t.s / 10);
            put (32, 4, t.m % 10); put (40, 3, t.m / 10);
            put (48, 4, t.h % 10); put (56, 2, t.h / 10);
            const int sync[16] { 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
            for (int i = 0; i < 16; ++i) bits[64 + i] = sync[i];
            for (int b = 0; b < 80; ++b) appendBit (out, clock, level, bitLen, bits[b]);

            if (++t.f >= nominal)
            {
                t.f = 0;
                if (++t.s >= 60) { t.s = 0; if (++t.m >= 60) { t.m = 0; if (++t.h >= 24) t.h = 0; } }
                if (df && t.s == 0 && (t.m % 10) != 0) t.f = 2;
            }
        }
        // A real generator keeps going: the next frame's first edge is
        // what completes this frame's final bit cell.
        level = -level;
        while ((double) out.size() < clock + bitLen * 0.5) out.push_back (level);
        return out;
    }
}
