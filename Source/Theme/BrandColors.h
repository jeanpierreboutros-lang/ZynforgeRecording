#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>

namespace zynforge::brand
{
    // Shared with ZynForge Live: near-black canvas, muted per-strip
    // personality washes, bright cyan/green/amber accents reserved for
    // status and important toggles.
    inline const auto bgDeep      = juce::Colour::fromRGB (0x0a, 0x0a, 0x0c);
    inline const auto bgPanel     = juce::Colour::fromRGB (0x12, 0x13, 0x16);
    inline const auto bgStrip     = juce::Colour::fromRGB (0x18, 0x19, 0x1d);
    inline const auto edge        = juce::Colour::fromRGB (0x26, 0x28, 0x2e);
    inline const auto textPrimary = juce::Colour::fromRGB (0xe8, 0xe8, 0xee);
    inline const auto textMuted   = juce::Colour::fromRGB (0x88, 0x8a, 0x94);

    inline const auto accentRecord = juce::Colour::fromRGB (0xff, 0x3b, 0x3b);
    inline const auto accentPlay   = juce::Colour::fromRGB (0x4a, 0xd8, 0x78);  // brighter — matches Live's status green
    inline const auto accentVS     = juce::Colour::fromRGB (0xff, 0xb4, 0x2a);
    inline const auto accentStatus = juce::Colour::fromRGB (0x5d, 0xd8, 0x7a);  // session-clock green

    inline const auto meterGreen   = juce::Colour::fromRGB (0x4a, 0xd8, 0x78);
    inline const auto meterAmber   = juce::Colour::fromRGB (0xff, 0xb4, 0x2a);
    inline const auto meterRed     = juce::Colour::fromRGB (0xff, 0x3b, 0x3b);
    inline const auto meterIdle    = juce::Colour::fromRGB (0x1c, 0x1f, 0x26);

    // Muted, desaturated washes — sampled from ZynForge Live's INS 1–8.
    // Each is dark enough that white text reads cleanly on top.
    inline const std::array<juce::Colour, 8> personality {
        juce::Colour::fromRGB (0x3a, 0x50, 0x63),  // INS 1 — dusty blue
        juce::Colour::fromRGB (0x39, 0x56, 0x46),  // INS 2 — moss
        juce::Colour::fromRGB (0x51, 0x4a, 0x35),  // INS 3 — olive
        juce::Colour::fromRGB (0x4a, 0x3d, 0x57),  // INS 4 — violet
        juce::Colour::fromRGB (0x58, 0x2e, 0x3a),  // INS 5 — wine
        juce::Colour::fromRGB (0x2d, 0x50, 0x51),  // INS 6 — teal
        juce::Colour::fromRGB (0x5b, 0x45, 0x28),  // INS 7 — amber
        juce::Colour::fromRGB (0x5a, 0x4d, 0x2c),  // INS 8 — mustard
    };

    inline juce::Colour stripColour (int index) noexcept
    {
        return personality[(std::size_t) (index % (int) personality.size())];
    }
}
