#pragma once

#include <juce_graphics/juce_graphics.h>

#include "ForgeTokens.h"

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
        inline constexpr int  quickFadeMs   = forge::motion_quickFadeMs;
        // Fade-out for the same: longer than the fade-in so the
        // engineer registers what just appeared before it leaves.
        inline constexpr int  fadeOutMs     = forge::motion_fadeOutMs;
        // Hold duration for a non-modal toast before it begins fading.
        inline constexpr int  toastHoldMs   = forge::motion_toastHoldMs;
        // Hold for a one-shot clip indicator (PEAK tally bar, etc.)
        // after the last clip event.
        inline constexpr int  clipLatchMs   = forge::motion_clipLatchMs;
        // Rate at which the BigClock pulses while recording / waiting.
        inline constexpr float pulseHz      = forge::motion_pulseHz;
        // Cross-strip soft-takeover ramp on cue recall. Seconds, not
        // milliseconds, because the audio thread integrates per-sample.
        inline constexpr float cueRampSec   = forge::motion_cueRampSec;
        // Standard delay before showing a launch-flow dialog so the
        // window has finished sizing first.
        inline constexpr int  launchDelayMs = forge::motion_launchDelayMs;
    }

    // ── Corner-radius scale (matches ZynForge Live's rounded.{sm,md,lg,xl}) ──
    namespace radius
    {
        inline constexpr float sm = forge::radius_sm;   // chips, fader caps, micro pills
        inline constexpr float md = forge::radius_md;   // patch dots, picker rows, slots
        inline constexpr float lg = forge::radius_lg;   // knobs, fader thumbs, toolbar buttons
        inline constexpr float xl = forge::radius_xl;   // dialogs, callouts
    }

    // ── Spacing scale (matches ZynForge Live's spacing.xs..xl) ───────────────
    namespace space
    {
        inline constexpr int xs = forge::space_xs;   // tight icon-to-text padding
        inline constexpr int sm = forge::space_sm;   // sibling controls on the same row
        inline constexpr int md = forge::space_md;   // section inner padding
        inline constexpr int lg = forge::space_lg;  // section outer margin
        inline constexpr int xl = forge::space_xl;  // large section gaps

        inline constexpr int ctrlH = forge::space_ctrlH;   // standard control height
        inline constexpr int ioH   = forge::space_ioH;   // I/O selector rows
        inline constexpr int rowH  = forge::space_rowH;   // standard row pitch
        inline constexpr int btnH  = forge::space_btnH;   // action button height

        // ── Shared EDIT-view layout constants ──────────────────────
        // Previously duplicated as `headerW`/`kHeaderW` literals across
        // TrackRow, the EditPage timer, and the auto-scroll logic with
        // "keep in sync" comments -- a duplicated-constant class that
        // already shipped one bug (the 4 px wave-inset playhead drift).
        inline constexpr int editHeaderW   = 380;  // pinned row-header column
        inline constexpr int editWaveInset = xs;   // lane clip-content inset (each side)
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
    inline constexpr const char* uiFamily   = forge::uiFamily;
    inline constexpr const char* monoFamily = forge::monoFamily;

    namespace type
    {
        // Sized scale. Seven discrete heights. Anything outside this
        // list should be considered a bug -- the system should be tight.
        inline constexpr float h_micro    = forge::type_micro;   // sub-label EDIT-lane density (clip badges, gain pips)
        inline constexpr float h_label    = forge::type_label;   // tiny captions / dB ruler ticks
        inline constexpr float h_caption  = forge::type_caption;   // small labels, chips
        inline constexpr float h_body     = forge::type_body;   // standard body
        inline constexpr float h_title    = forge::type_title;   // dialog + section titles
        inline constexpr float h_headline = forge::type_headline;   // big-ish accents
        inline constexpr float h_subhead  = forge::type_subhead;   // cue countdown, next-up labels
        inline constexpr float h_display  = forge::type_display;   // section hero numbers
        inline constexpr float h_hero     = forge::type_hero;   // BigClock timer

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
            // The cache lives behind an accessor (not a raw function-local
            // static inside cachedFont) so clearFontCache() below can empty
            // it at shutdown -- see the note there for why that matters.
            inline std::unordered_map<size_t, juce::Font>& fontCache()
            {
                static std::unordered_map<size_t, juce::Font> cache;
                return cache;
            }

            inline juce::Font& cachedFont (const char* family, float height, bool bold)
            {
                // Key encodes height (rounded to nearest 0.5) + bold + family
                // pointer (the two family strings are constexpr in this header,
                // so pointer identity is stable).
                const int    h2  = (int) std::round (height * 2.0f);
                const size_t key = ((size_t) h2 << 2)
                                 | (bold ? 1u : 0u)
                                 | ((size_t) family << 16);
                auto& cache = fontCache();
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
        // Each role is a thin wrapper over ui()/mono(), which resolve
        // through detail::fontCache() -- so the expensive JUCE 8 CoreText
        // typeface lookup (CPU profile once showed ~10 % runtime burned
        // in _hb_coretext_shaper_font_data_create when fonts were built
        // fresh every paint) happens once per (size, weight) and every
        // later call is a hash hit + Font copy. These deliberately do NOT
        // hold their own `static juce::Font`: a function-local static
        // Font destructs at __cxa_finalize, after JUCE tears down
        // CoreText, and its typeface destructor then crashes the process
        // on quit. Routing through the clearable cache (emptied in
        // clearFontCache()) keeps the perf win without the teardown bomb.
        inline juce::Font micro()        { return ui   (h_micro);          }
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
        // RESERVED scale rungs. The type ramp (h_label..h_hero) is a
        // deliberately CONTINUOUS scale -- every rung exists so a future
        // layout can step up/down without inventing an off-scale size.
        // These two have no live consumer today (intended: subhead for the
        // cue-countdown / "Next: ..." pills, display for section hero
        // numbers); they are kept to keep the ramp unbroken, NOT dead code.
        // If a rung is still unused at the next type pass, fold it out then.
        inline juce::Font subhead()      { return ui   (h_subhead, true);  }  // 22 pt UI bold (reserved)
        inline juce::Font display()      { return mono (h_display, true);  }  // 28 pt mono bold (reserved)
        // 44 pt mono bold -- pinned for the BigClock timer.
        inline juce::Font hero()         { return mono (h_hero, true);     }
        // Numeric readouts -- every value-readout font goes through this.
        inline juce::Font readout (float height) { return mono (height, true); }

        // Release every cached juce::Font. MUST be called from the app's
        // shutdown() while CoreText is still alive. detail::fontCache() is
        // a function-local static that otherwise destructs at
        // __cxa_finalize -- AFTER JUCE tears down CoreText -- so the
        // cached fonts' CoreText typeface destructors fire into a dead
        // framework and the process terminate()s on quit. Emptying the
        // map here destroys them in time; the now-empty map destructing
        // later is harmless.
        inline void clearFontCache() { detail::fontCache().clear(); }
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
