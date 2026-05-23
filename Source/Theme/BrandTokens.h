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

    // ── Typography scale ──────────────────────────────────────────────────
    //
    // Two pinned typeface families. Picked by name so we can swap to
    // bundled Inter / JetBrains Mono later by changing exactly these
    // two strings.
    //
    //   uiFamily   -- proportional UI text (labels, body, titles, buttons)
    //   monoFamily -- tabular numerals (BigClock, BPM, meter dB readouts,
    //                CPU%, MB/s -- anywhere a value changes and shouldn't
    //                cause horizontal layout jitter)
    //
    // These names match the bundled BinaryData faces directly. The
    // LookAndFeel still accepts "SF Pro" / "SF Mono" for any legacy
    // call, but new code should ask by the canonical name.
    inline constexpr const char* uiFamily   = "Inter";
    inline constexpr const char* monoFamily = "JetBrains Mono";

    namespace type
    {
        // Sized scale. Seven discrete heights. Anything outside this
        // list should be considered a bug -- the system should be tight.
        inline constexpr float h_label    = 10.0f;   // tiny captions / dB ruler ticks
        inline constexpr float h_caption  = 11.0f;   // small labels, chips
        inline constexpr float h_body     = 13.0f;   // standard body
        inline constexpr float h_title    = 14.0f;   // dialog + section titles
        inline constexpr float h_headline = 18.0f;   // big-ish accents
        inline constexpr float h_subhead  = 22.0f;   // cue countdown, next-up labels
        inline constexpr float h_display  = 28.0f;   // section hero numbers
        inline constexpr float h_hero     = 44.0f;   // BigClock timer

        inline juce::Font ui (float height, bool bold = false)
        {
            auto opts = juce::FontOptions().withName (uiFamily).withHeight (height);
            if (bold) opts = opts.withStyle ("Bold");
            return juce::Font (opts);
        }

        // Tabular / monospace digits. Anywhere a number is going to
        // change while the user is looking at it.
        inline juce::Font mono (float height, bool bold = false)
        {
            auto opts = juce::FontOptions().withName (monoFamily).withHeight (height);
            if (bold) opts = opts.withStyle ("Bold");
            return juce::Font (opts);
        }

        // ── Named roles ───────────────────────────────────────────
        inline juce::Font label()        { return ui   (h_label);          }
        inline juce::Font ledLabel()     { return mono (9.0f, true);       }
        inline juce::Font hint()         { return ui   (h_label);          }
        inline juce::Font statusBar()    { return ui   (h_caption);        }
        inline juce::Font uiLabel()      { return ui   (h_caption, true);  }
        inline juce::Font caption()      { return ui   (h_caption);        }
        inline juce::Font captionBold()  { return ui   (h_caption, true);  }
        inline juce::Font uiBody()       { return ui   (h_body);           }
        inline juce::Font channelName()  { return ui   (h_body, true);     }
        inline juce::Font sectionTitle() { return ui   (h_title, true);    }
        inline juce::Font headline()     { return ui   (h_headline, true); }
        // 22 pt UI bold -- for cue-countdown / "Next: ..." pills that
        // need to sit between body text and the BigClock numbers.
        inline juce::Font subhead()      { return ui   (h_subhead, true);  }
        // 28 pt mono bold -- for section hero numbers (cue index, etc.).
        inline juce::Font display()      { return mono (h_display, true);  }
        // 44 pt mono bold -- pinned for the BigClock timer.
        inline juce::Font hero()         { return mono (h_hero, true);     }
        // Numeric readouts -- every value-readout font goes through this.
        inline juce::Font readout (float height) { return mono (height, true); }
    }

    // ── Font shorthand (re-exports type:: in shorter form) ────────────
    // Site-code reads cleaner: brand::fonts::numeral16() vs
    // brand::type::mono (16.0f, true). Cuts verbosity ~40 %.
    namespace fonts
    {
        inline juce::Font small()       { return type::caption();        }
        inline juce::Font body()        { return type::uiBody();         }
        inline juce::Font bold()        { return type::channelName();    }
        inline juce::Font title()       { return type::sectionTitle();   }
        inline juce::Font headline()    { return type::headline();       }
        inline juce::Font hero()        { return type::hero();           }
        // Tabular numerals -- pick the closest size.
        inline juce::Font numeral10()   { return type::mono (10.0f, true); }
        inline juce::Font numeral11()   { return type::mono (11.0f, true); }
        inline juce::Font numeral13()   { return type::mono (13.0f, true); }
        inline juce::Font numeral16()   { return type::mono (16.0f, true); }
        inline juce::Font numeral22()   { return type::mono (22.0f, true); }
        inline juce::Font numeral28()   { return type::mono (28.0f, true); }
        inline juce::Font numeral44()   { return type::mono (44.0f, true); }
    }
}
