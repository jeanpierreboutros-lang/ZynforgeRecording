// Direct tests against EditPage::TrackRow.
//
// This file only exists because TrackRow was made a PUBLIC nested declaration.
// It used to be private, with its ~4,000-line definition in EditTrackRow.h
// (included solely by EditPage.cpp) — so the three mouse handlers, every
// hit-test, the routing combos and the meter lifetime were structurally
// unreachable from the suite. Two consecutive audits found real defects in
// there that could only ever be smoke-tested. These are the regression guards
// for the ones that mattered.
//
// TrackRow is a juce::Component; constructing it headlessly is fine (its
// timers never fire without a running message loop) — same approach as
// EditPageAccessibilityTests.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"

namespace zynforge
{
    class EditTrackRowTests final : public juce::UnitTest
    {
    public:
        EditTrackRowTests() : juce::UnitTest ("EditPage TrackRow", "zynforge") {}

        // Everything a TrackRow needs to exist, in one place.
        struct Host
        {
            AudioEngine                engine;
            juce::AudioFormatManager   formats;
            juce::AudioThumbnailCache  cache { 8 };

            explicit Host (int strips)
            {
                formats.registerBasicFormats();
                // TEST ISOLATION. Every test-mode AudioEngine shares ONE
                // throwaway zynforge-test.settings, so per-strip writes
                // (setTrackStereo -> strip_stereo_N, routing -> strip_in_N)
                // persist into the NEXT suite that constructs an engine and
                // calls applyPersistedStripState(). This suite genuinely broke
                // "Player maps files by Track_NN index" that way. Wipe before
                // AND after so neither direction leaks.
                engine.clearAllStripOverrides();
                engine.prepareForTests (48000.0, 512);
                engine.setStripCount (strips);
            }

            ~Host() { engine.clearAllStripOverrides(); }
        };

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            // ── The meter-condemn UAF guard ──────────────────────────────────
            // Each TrackRow owns a LedMeter holding a TrackState& on its own
            // timer, and the rows only rebuild on EditPage's next 24 Hz tick --
            // so every recorder-vector shrink left EDIT reading freed memory
            // until then. condemnAllStrips() now calls TrackRow::invalidate().
            // Observable contract: once condemned, the row STOPS reading its
            // TrackState. (The same hazard was fixed three times on the MIXER
            // while this sat open, which is why it gets a real test.)
            beginTest ("invalidate() stops the row reading its TrackState");
            {
                Host h (4);
                EditPage::TrackRow row (0, false, h.engine, h.formats, h.cache);

                h.engine.setTrackName (0, "KICK");
                row.updatePollState();
                expectEquals (row.getNameTextForTests(), juce::String ("KICK"),
                              "a live row must track its TrackState");
                expect (! row.isCondemnedForTests());

                row.invalidate();
                expect (row.isCondemnedForTests());

                // After condemning, a change to the (about-to-be-freed) state
                // must NOT be picked up -- that read is the use-after-free.
                h.engine.setTrackName (0, "SNARE");
                row.updatePollState();
                expectEquals (row.getNameTextForTests(), juce::String ("KICK"),
                              "a condemned row must not dereference its TrackState");
            }

            // ── The stale-index guard ────────────────────────────────────────
            // Even without an explicit condemn, a row whose index is past the
            // end of a shrunken recorder must not index the vector. paint() has
            // always guarded this; updatePollState() did not.
            beginTest ("A row past the end of a shrunken recorder polls nothing");
            {
                Host h (4);
                EditPage::TrackRow row (3, false, h.engine, h.formats, h.cache);
                h.engine.setTrackName (3, "TOM");
                row.updatePollState();
                expectEquals (row.getNameTextForTests(), juce::String ("TOM"));

                h.engine.setStripCount (2);          // row 3 no longer exists
                row.updatePollState();               // must not index out of range
                expectEquals (row.getNameTextForTests(), juce::String ("TOM"),
                              "the poll must no-op rather than read past the end");
            }

            // ── Odd-index stereo routing must still be displayed ─────────────
            // A stereo row lists only EVEN start channels (ids 2, 4, 6 ...) but
            // the selection is routing+2, so a stereo L at an odd physical index
            // selected an id that wasn't in the list and the combo went BLANK --
            // the engineer had no way to see what the pair was patched to.
            beginTest ("A stereo pair at an odd index shows its routing, not a blank");
            {
                Host h (4);
                // Mono strip 0, stereo pair at 1-2 => L sits at an ODD index.
                h.engine.setTrackStereo (1, true);
                h.engine.setTrackInputRouting  (1, 1);   // odd
                h.engine.setTrackOutputRouting (1, 1);

                EditPage::TrackRow row (1, true, h.engine, h.formats, h.cache);
                row.refreshRoutingSelection();

                const auto in  = row.getInputRoutingTextForTests();
                const auto out = row.getOutputRoutingTextForTests();
                expect (in .isNotEmpty(), "input combo blanked on an odd stereo routing");
                expect (out.isNotEmpty(), "output combo blanked on an odd stereo routing");
                // Channels are 1-based on screen: device index 1 => "In 2-3".
                // startsWith, not equals: entries for channels the (absent, in
                // test mode) device doesn't have are suffixed "(off)". The
                // contract under test is that the routing is SHOWN at all.
                expect (in .startsWith ("In 2-3"),  "expected In 2-3..., got: "  + in);
                expect (out.startsWith ("Out 2-3"), "expected Out 2-3..., got: " + out);
            }

            beginTest ("An even-index stereo pair is unchanged");
            {
                Host h (4);
                h.engine.setTrackStereo (0, true);
                h.engine.setTrackInputRouting (0, 0);
                EditPage::TrackRow row (0, true, h.engine, h.formats, h.cache);
                row.refreshRoutingSelection();
                const auto txt = row.getInputRoutingTextForTests();
                expect (txt.startsWith ("In 1-2"),
                        "the normal even case must not regress; got: " + txt);
            }

            // ── Unrouted stays unrouted ──────────────────────────────────────
            beginTest ("An unrouted strip reads (unrouted)");
            {
                Host h (2);
                h.engine.setTrackInputRouting (0, -1);
                EditPage::TrackRow row (0, false, h.engine, h.formats, h.cache);
                row.refreshRoutingSelection();
                expectEquals (row.getInputRoutingTextForTests(), juce::String ("(unrouted)"));
            }
        }
    };

    static EditTrackRowTests editTrackRowTests;
}
