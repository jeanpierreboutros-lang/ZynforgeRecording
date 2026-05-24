#pragma once

#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <unordered_map>

namespace zynforge::brand
{
    // ── Motion / animation timings (milliseconds, Hz) ────────────────────
    //
    // Every component that animates picks its own delay / duration / rate
    // ad-hoc otherwise -- audit found Toast at 200ms fade-in, BigClock
    // pulsing at 1 Hz, PEAK tally holding 1000 ms, the cue-ramp easing
    // computing internally. Names below carry the design intent so all
    // animated components share one rhythm.
    namespace motion
    {
        // Fade-in for brief feedback (toast surface, status pills).
        inline constexpr int  quickFadeMs   = 200;
        // Fade-out for the same: longer than the fade-in so the
        // engineer registers what just appeared before it leaves.
        inline constexpr int  fadeOutMs     = 320;
        // Hold duration for a non-modal toast before it begins fading.
        inline constexpr int  toastHoldMs   = 2800;
        // Hold for a one-shot clip indicator (PEAK tally bar, etc.)
        // after the last clip event.
        inline constexpr int  clipLatchMs   = 1000;
        // Rate at which the BigClock pulses while recording / waiting.
        inline constexpr float pulseHz      = 1.0f;
        // Cross-strip soft-takeover ramp on cue recall. Seconds, not
        // milliseconds, because the audio thread integrates per-sample.
        inline constexpr float cueRampSec   = 0.50f;
        // Standard delay before showing a launch-flow dialog so the
        // window has finished sizing first.
        inline constexpr int  launchDelayMs = 250;
    }

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

        // Small (height, bold) -> Font cache. JUCE 8 routes
        // juce::Font(FontOptions) through a CoreText typeface lookup
        // that's not free; if a paint method asks for the same font
        // every frame the cost compounds (profile showed ~10 % runtime
        // in _hb_coretext_shaper_font_data_create before this).
        // unordered_map keyed on (rounded-height, bold-flag, family-id)
        // -- the hash hit is microseconds, the Font construction is
        // ~10x slower.
        namespace detail
        {
            inline juce::Font& cachedFont (const char* family, float height, bool bold)
            {
                // Key encodes height (rounded to nearest 0.5) + bold + family
                // pointer (the two family strings are constexpr in this header,
                // so pointer identity is stable).
                const int    h2  = (int) std::round (height * 2.0f);
                const size_t key = ((size_t) h2 << 2)
                                 | (bold ? 1u : 0u)
                                 | ((size_t) family << 16);
                static std::unordered_map<size_t, juce::Font> cache;
                auto it = cache.find (key);
                if (it != cache.end()) return it->second;
                auto opts = juce::FontOptions().withName (family).withHeight (height);
                if (bold) opts = opts.withStyle ("Bold");
                return cache.emplace (key, juce::Font (opts)).first->second;
            }
        }

        inline juce::Font ui (float height, bool bold = false)
        {
            return detail::cachedFont (uiFamily, height, bold);
        }

        // Tabular / monospace digits. Anywhere a number is going to
        // change while the user is looking at it.
        inline juce::Font mono (float height, bool bold = false)
        {
            return detail::cachedFont (monoFamily, height, bold);
        }

        // ── Named roles ───────────────────────────────────────────
        // Each named role caches a single static Font instance built
        // once on first use. JUCE 8 routes juce::Font(FontOptions)
        // construction through a CoreText typeface lookup that's
        // measurable on every call (CPU profile showed ~10 % runtime
        // burned in _hb_coretext_shaper_font_data_create when these
        // accessors were called fresh every paint). The cache turns
        // each accessor into a near-free copy of an already-resolved
        // Font reference.
        inline juce::Font label()        { static const juce::Font f = ui   (h_label);          return f; }
        inline juce::Font ledLabel()     { static const juce::Font f = mono (9.0f, true);       return f; }
        inline juce::Font hint()         { static const juce::Font f = ui   (h_label);          return f; }
        inline juce::Font statusBar()    { static const juce::Font f = ui   (h_caption);        return f; }
        inline juce::Font uiLabel()      { static const juce::Font f = ui   (h_caption, true);  return f; }
        inline juce::Font caption()      { static const juce::Font f = ui   (h_caption);        return f; }
        inline juce::Font captionBold()  { static const juce::Font f = ui   (h_caption, true);  return f; }
        inline juce::Font uiBody()       { static const juce::Font f = ui   (h_body);           return f; }
        inline juce::Font channelName()  { static const juce::Font f = ui   (h_body, true);     return f; }
        inline juce::Font sectionTitle() { static const juce::Font f = ui   (h_title, true);    return f; }
        inline juce::Font headline()     { static const juce::Font f = ui   (h_headline, true); return f; }
        // 22 pt UI bold -- for cue-countdown / "Next: ..." pills.
        inline juce::Font subhead()      { static const juce::Font f = ui   (h_subhead, true);  return f; }
        // 28 pt mono bold -- for section hero numbers.
        inline juce::Font display()      { static const juce::Font f = mono (h_display, true);  return f; }
        // 44 pt mono bold -- pinned for the BigClock timer.
        inline juce::Font hero()         { static const juce::Font f = mono (h_hero, true);     return f; }
        // Numeric readouts -- every value-readout font goes through this.
        inline juce::Font readout (float height) { return mono (height, true); }
    }

    // ── DEPRECATED: brand::fonts:: ────────────────────────────────────
    // Was a verbosity shim over brand::type::. Migrated to canonical
    // brand::type::* on 2026-05-24 -- the two were duplicating intent
    // and engineers had two ways to ask for the same font. New code
    // uses brand::type::* exclusively.
    //
    // If you reach for a numeral helper, use brand::type::mono(size,
    // bold) directly; the old brand::fonts::numeralN aliases were
    // size-pinned to fixed values that didn't compose.
}
