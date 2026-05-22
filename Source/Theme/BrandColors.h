#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>

namespace zynforge::brand
{
    // ── Tokens taken byte-for-byte from the shared ZynForge design
    // system (DESIGN.md authored 2026-05-21). Live and Recording must
    // stay aligned here so the family reads as one product.

    // ── Brand mark ────────────────────────────────────────────────────────
    inline const auto brandOrange = juce::Colour::fromRGB (0xff, 0x77, 0x33);  // forge accent
    inline const auto brandDeep   = juce::Colour::fromRGB (0x0a, 0x0a, 0x0f);  // navy ground

    // ── Background levels (3-step) ────────────────────────────────────────
    inline const auto bgDeep      = juce::Colour::fromRGB (0x0d, 0x0d, 0x12);  // deepest plane
    inline const auto bgPanel     = juce::Colour::fromRGB (0x12, 0x13, 0x16);  // panel surface
    inline const auto bgStrip     = juce::Colour::fromRGB (0x1e, 0x1e, 0x1e);  // main app surface
    inline const auto bgElevated  = juce::Colour::fromRGB (0x30, 0x30, 0x30);  // popups, dialogs

    // ── Control chrome ────────────────────────────────────────────────────
    inline const auto edge          = juce::Colour::fromRGB (0x26, 0x28, 0x2e);
    inline const auto borderSubtle  = juce::Colour::fromRGB (0x3a, 0x3a, 0x3a);
    inline const auto borderBright  = juce::Colour::fromRGB (0x5a, 0x5a, 0x5a);

    // ── Text (4-step, all WCAG-AA against bg-surface) ─────────────────────
    inline const auto textPrimary   = juce::Colour::fromRGB (0xff, 0xff, 0xff);
    inline const auto textSecondary = juce::Colour::fromRGB (0xb8, 0xc2, 0xcc);
    inline const auto textTertiary  = juce::Colour::fromRGB (0x7a, 0x8a, 0x9a);
    inline const auto textMuted     = juce::Colour::fromRGB (0x7a, 0x8a, 0x9a);  // alias of textTertiary

    // ── Semantic signal accents ───────────────────────────────────────────
    inline const auto accentRecord    = juce::Colour::fromRGB (0xff, 0x3b, 0x3b);  // record / danger
    inline const auto accentPlay      = juce::Colour::fromRGB (0x4a, 0xd8, 0x78);  // transport active
    inline const auto accentVS        = juce::Colour::fromRGB (0xff, 0xb4, 0x2a);  // virtual soundcheck
    inline const auto accentStatus    = juce::Colour::fromRGB (0x5d, 0xd8, 0x7a);  // session-clock green
    inline const auto accentSolo      = juce::Colour::fromRGB (0xff, 0xd6, 0x4d);  // solo yellow
    inline const auto alertAmber      = juce::Colour::fromRGB (0xe8, 0x98, 0x27);
    inline const auto engagedAmber    = juce::Colour::fromRGB (0xff, 0x9e, 0x2c);  // BYPASS / LIVE / LOCK
    inline const auto featureEngaged  = juce::Colour::fromRGB (0x33, 0xc4, 0xd4);  // cool-teal toggle-on

    // ── Meter colours (deliberately duller than accents) ──────────────────
    inline const auto meterGreen   = juce::Colour::fromRGB (0x3c, 0xb8, 0x78);
    inline const auto meterAmber   = juce::Colour::fromRGB (0xf0, 0xc0, 0x60);
    inline const auto meterRed     = juce::Colour::fromRGB (0xdc, 0x38, 0x38);
    inline const auto meterIdle    = juce::Colour::fromRGB (0x2c, 0x30, 0x3a);   // visible against bgDeep

    // ── Transport / control-button chrome (3 hover states) ────────────────
    inline const auto controlBg          = juce::Colour::fromRGB (0x2a, 0x2c, 0x30);
    inline const auto controlBgHover     = juce::Colour::fromRGB (0x36, 0x38, 0x3e);
    inline const auto controlBgDown      = juce::Colour::fromRGB (0x3e, 0x40, 0x46);
    inline const auto controlBorder      = juce::Colour::fromRGB (0x14, 0x15, 0x18);

    // ── Fader thumb body greys ────────────────────────────────────────────
    inline const auto faderThumbHi    = juce::Colour::fromRGB (0x33, 0x35, 0x3c);
    inline const auto faderThumbLo    = juce::Colour::fromRGB (0x1c, 0x1e, 0x23);
    inline const auto faderThumbEdge  = juce::Colour::fromRGB (0x55, 0x57, 0x60);
    inline const auto faderThumbGrip  = juce::Colour::fromRGB (0xa0, 0xa3, 0xad);

    // ── Strip-colour picker neutral presets ───────────────────────────────
    inline const auto swatchSlate    = juce::Colour::fromRGB (0x40, 0x45, 0x50);
    inline const auto swatchGraphite = juce::Colour::fromRGB (0x55, 0x55, 0x5a);

    // ── Personality wash colours — INS 1-8 ────────────────────────────────
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

    // ── Gradient helpers — every painted surface should use these ─────────
    // Top-to-bottom vertical gradient for buttons / pills / strip backs.
    inline juce::ColourGradient verticalGradient (juce::Colour base,
                                                  juce::Rectangle<float> r,
                                                  float lift   = 0.10f,
                                                  float shadow = 0.20f) noexcept
    {
        return juce::ColourGradient (base.brighter (lift),   r.getX(), r.getY(),
                                     base.darker  (shadow),  r.getX(), r.getBottom(),
                                     false);
    }
}
