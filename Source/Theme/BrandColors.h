#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>

namespace zynforge::brand
{
    // Tokens taken verbatim from the shared ZynForge design system
    // (DESIGN.md). The Live and Recording apps must stay byte-aligned
    // here so the family reads as one product.

    // Backgrounds — values taken byte-for-byte from ZynForge Live's
    // ZynForgeColors.h so Recording paints the same dark surface.
    inline const auto bgDeep      = juce::Colour::fromRGB (0x0d, 0x0d, 0x12);
    inline const auto bgPanel     = juce::Colour::fromRGB (0x12, 0x13, 0x16);
    inline const auto bgStrip     = juce::Colour::fromRGB (0x1e, 0x1e, 0x1e);   // Live's bg-surface
    inline const auto bgElevated  = juce::Colour::fromRGB (0x30, 0x30, 0x30);
    inline const auto edge        = juce::Colour::fromRGB (0x26, 0x28, 0x2e);

    // Text (4-step) — Live values exactly.
    inline const auto textPrimary   = juce::Colour::fromRGB (0xff, 0xff, 0xff);
    inline const auto textSecondary = juce::Colour::fromRGB (0xb8, 0xc2, 0xcc);
    inline const auto textTertiary  = juce::Colour::fromRGB (0x7a, 0x8a, 0x9a);
    inline const auto textMuted     = juce::Colour::fromRGB (0x7a, 0x8a, 0x9a);

    // Semantic signal accents
    inline const auto accentRecord    = juce::Colour::fromRGB (0xff, 0x3b, 0x3b);  // record / danger
    inline const auto accentPlay      = juce::Colour::fromRGB (0x4a, 0xd8, 0x78);  // transport active
    inline const auto accentVS        = juce::Colour::fromRGB (0xff, 0xb4, 0x2a);  // virtual soundcheck / macros
    inline const auto accentStatus    = juce::Colour::fromRGB (0x5d, 0xd8, 0x7a);  // session-clock green
    inline const auto alertAmber      = juce::Colour::fromRGB (0xe8, 0x98, 0x27);
    inline const auto engagedAmber    = juce::Colour::fromRGB (0xff, 0x9e, 0x2c);  // BYPASS / LIVE / LOCK
    inline const auto featureEngaged  = juce::Colour::fromRGB (0x33, 0xc4, 0xd4);  // cool-teal toggle-on

    // Meter colours (deliberately duller than the play/record accents)
    inline const auto meterGreen   = juce::Colour::fromRGB (0x3c, 0xb8, 0x78);
    inline const auto meterAmber   = juce::Colour::fromRGB (0xf0, 0xc0, 0x60);
    inline const auto meterRed     = juce::Colour::fromRGB (0xdc, 0x38, 0x38);
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
