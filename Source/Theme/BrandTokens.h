#pragma once

#include <juce_graphics/juce_graphics.h>

namespace zynforge::brand
{
    // ── Corner-radius scale (matches ZynForge Live's rounded.{sm,md,lg,xl}) ──
    namespace radius
    {
        inline constexpr float sm = 2.5f;   // chips, fader caps, micro pills
        inline constexpr float md = 4.0f;   // patch dots, picker rows, slots
        inline constexpr float lg = 5.0f;   // knobs, fader thumbs, toolbar buttons
        inline constexpr float xl = 8.0f;   // dialogs, callouts
    }

    // ── Spacing scale (matches ZynForge Live's spacing.xs..xl) ───────────────
    namespace space
    {
        inline constexpr int xs = 4;   // tight icon-to-text padding
        inline constexpr int sm = 6;   // sibling controls on the same row
        inline constexpr int md = 8;   // section inner padding
        inline constexpr int lg = 10;  // section outer margin
        inline constexpr int xl = 16;  // large section gaps

        inline constexpr int ctrlH = 22;   // standard control height
        inline constexpr int ioH   = 20;   // I/O selector rows
        inline constexpr int rowH  = 26;   // standard row pitch
        inline constexpr int btnH  = 24;   // action button height
    }

    // ── Typography scale (matches ZynForge Live's typography.*) ─────────────
    namespace type
    {
        inline juce::Font label()
        {
            return juce::Font (juce::FontOptions().withHeight (9.0f));
        }
        inline juce::Font ledLabel()
        {
            return juce::Font (juce::FontOptions().withHeight (9.0f).withStyle ("Bold"));
        }
        inline juce::Font hint()
        {
            return juce::Font (juce::FontOptions().withHeight (10.0f));
        }
        inline juce::Font statusBar()
        {
            return juce::Font (juce::FontOptions().withHeight (10.5f));
        }
        inline juce::Font uiLabel()
        {
            return juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
        }
        inline juce::Font caption()
        {
            return juce::Font (juce::FontOptions().withHeight (12.0f));
        }
        inline juce::Font captionBold()
        {
            return juce::Font (juce::FontOptions().withHeight (12.0f).withStyle ("Bold"));
        }
        inline juce::Font uiBody()
        {
            return juce::Font (juce::FontOptions().withHeight (13.0f));
        }
        inline juce::Font channelName()
        {
            return juce::Font (juce::FontOptions().withHeight (13.0f).withStyle ("Bold"));
        }
        inline juce::Font sectionTitle()
        {
            return juce::Font (juce::FontOptions().withHeight (14.0f).withStyle ("Bold"));
        }
        inline juce::Font display()
        {
            return juce::Font (juce::FontOptions().withHeight (22.0f).withStyle ("Bold"));
        }
        inline juce::Font hero()
        {
            return juce::Font (juce::FontOptions().withHeight (28.0f).withStyle ("Bold"));
        }
    }
}
