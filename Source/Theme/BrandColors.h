#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>

namespace zynforge::brand
{
    inline const auto bgDeep      = juce::Colour::fromRGB (0x07, 0x08, 0x0a);
    inline const auto bgPanel     = juce::Colour::fromRGB (0x0f, 0x10, 0x14);
    inline const auto bgStrip     = juce::Colour::fromRGB (0x14, 0x16, 0x1c);
    inline const auto edge        = juce::Colour::fromRGB (0x22, 0x24, 0x2c);
    inline const auto textPrimary = juce::Colour::fromRGB (0xe8, 0xe8, 0xee);
    inline const auto textMuted   = juce::Colour::fromRGB (0x7a, 0x7c, 0x86);

    inline const auto accentRecord = juce::Colour::fromRGB (0xff, 0x3b, 0x3b);
    inline const auto accentPlay   = juce::Colour::fromRGB (0x35, 0xc9, 0x6e);
    inline const auto accentVS     = juce::Colour::fromRGB (0xff, 0xb4, 0x2a);

    inline const auto meterGreen   = juce::Colour::fromRGB (0x2a, 0xd4, 0x7a);
    inline const auto meterAmber   = juce::Colour::fromRGB (0xff, 0xb4, 0x2a);
    inline const auto meterRed     = juce::Colour::fromRGB (0xff, 0x3b, 0x3b);
    inline const auto meterIdle    = juce::Colour::fromRGB (0x1c, 0x1f, 0x26);

    inline const std::array<juce::Colour, 8> personality {
        juce::Colour::fromRGB (0x4d, 0xb6, 0xff),  // azure
        juce::Colour::fromRGB (0xff, 0x6b, 0x6b),  // coral
        juce::Colour::fromRGB (0xb3, 0x88, 0xff),  // violet
        juce::Colour::fromRGB (0x4a, 0xde, 0x80),  // mint
        juce::Colour::fromRGB (0xff, 0xb4, 0x2a),  // amber
        juce::Colour::fromRGB (0xff, 0x7a, 0xc6),  // pink
        juce::Colour::fromRGB (0x5e, 0xe0, 0xd4),  // teal
        juce::Colour::fromRGB (0xff, 0xd6, 0x4d),  // lemon
    };

    inline juce::Colour stripColour (int index) noexcept
    {
        return personality[(std::size_t) (index % (int) personality.size())];
    }
}
