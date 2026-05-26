// Headless accessibility test for SetlistBar. The prev/next cue arrows are
// path-drawn (no readable glyph); every control must expose a spoken name
// and the bar must be a labelled group.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

#include "../UI/SetlistBar.h"

namespace zynforge
{
    class SetlistBarAccessibilityTests final : public juce::UnitTest
    {
    public:
        SetlistBarAccessibilityTests() : UnitTest ("SetlistBar accessibility", "zynforge") {}

        static void walk (juce::Component& c, const std::function<void (juce::Component&)>& fn)
        {
            for (int i = 0; i < c.getNumChildComponents(); ++i)
            {
                auto* ch = c.getChildComponent (i);
                fn (*ch);
                walk (*ch, fn);
            }
        }

        void runTest() override
        {
            beginTest ("Cue controls expose spoken accessible names");
            {
                SetlistBar bar;
                bar.setBounds (0, 0, 420, 40);

                expectEquals (bar.getTitle(), juce::String ("Setlist"));

                std::set<juce::String> titles;
                walk (bar, [&] (juce::Component& c)
                {
                    if (c.getTitle().isNotEmpty())
                        titles.insert (c.getTitle());
                });

                for (auto* expected : { "Previous cue", "Next cue", "Setlist cue",
                                        "Add cue", "Update cue" })
                    expect (titles.count (expected) > 0,
                            juce::String (expected) + " control is missing an accessible title");

                juce::Image img (juce::Image::ARGB, 420, 40, true);
                juce::Graphics g (img);
                bar.paint (g);   // smoke
                expect (true);
            }
        }
    };

    static SetlistBarAccessibilityTests setlistBarAccessibilityTestsInstance;
}
