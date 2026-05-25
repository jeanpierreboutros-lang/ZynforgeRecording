// Headless tests for the reusable PlaceholderView. We can't assert pixels,
// but we can drive the full state machine, verify the action button shows /
// hides correctly, and render every state (including empty strings and
// degenerate bounds) to an off-screen image to prove paint() never crashes.

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UI/PlaceholderView.h"

namespace zynforge
{
    class PlaceholderViewTests final : public juce::UnitTest
    {
    public:
        PlaceholderViewTests() : UnitTest ("PlaceholderView", "zynforge") {}

        static void render (PlaceholderView& pv)
        {
            juce::Image img (juce::Image::ARGB,
                             juce::jmax (1, pv.getWidth()),
                             juce::jmax (1, pv.getHeight()), true);
            juce::Graphics g (img);
            pv.paint (g);   // must not throw / crash for any state
        }

        static bool actionButtonVisible (PlaceholderView& pv, const juce::String& label)
        {
            for (int i = 0; i < pv.getNumChildComponents(); ++i)
                if (auto* b = dynamic_cast<juce::TextButton*> (pv.getChildComponent (i)))
                    return b->isVisible() && (label.isEmpty() || b->getButtonText() == label);
            return false;
        }

        void runTest() override
        {
            beginTest ("State machine + action-button visibility");
            {
                PlaceholderView pv;
                pv.setBounds (0, 0, 400, 300);
                expect (pv.getState() == PlaceholderView::State::Hidden);
                expect (! pv.isVisible());

                pv.showLoading ("Scanning waveforms");
                expect (pv.getState() == PlaceholderView::State::Loading);
                expect (pv.isVisible());
                expect (! actionButtonVisible (pv, {}), "loading has no action button");
                render (pv);

                int fired = 0;
                pv.showEmpty ("No session loaded", "Load or record to begin",
                              "Load session", [&] { ++fired; });
                expect (pv.getState() == PlaceholderView::State::Empty);
                expect (actionButtonVisible (pv, "Load session"), "empty action should show");
                render (pv);

                pv.showEmpty ("Nothing here");   // no action label / callback
                expect (! actionButtonVisible (pv, {}), "no-action empty hides the button");
                render (pv);

                pv.showError ("Couldn't open session", "The drive was ejected.",
                              "Retry", [&] { ++fired; });
                expect (pv.getState() == PlaceholderView::State::Error);
                expect (actionButtonVisible (pv, "Retry"), "error retry should show");
                render (pv);

                pv.clear();
                expect (pv.getState() == PlaceholderView::State::Hidden);
                expect (! pv.isVisible());
            }

            beginTest ("Edge cases: empty strings + tiny / degenerate bounds");
            {
                PlaceholderView pv;
                pv.setBounds (0, 0, 24, 14);     // too short for the glyph
                pv.showEmpty ("");               // empty title
                render (pv);
                pv.showError ("", "", "", {});   // everything empty, no action
                render (pv);
                pv.setBounds (0, 0, 1, 1);       // degenerate
                render (pv);
                pv.showLoading ("");             // empty caption while animating
                render (pv);
                expect (true, "reached here without a crash");
            }
        }
    };

    static PlaceholderViewTests placeholderViewTestsInstance;
}
