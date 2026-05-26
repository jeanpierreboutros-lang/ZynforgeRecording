// Headless accessibility test for AutomationToolbar. Its text buttons read
// from their labels; the two combos need explicit titles and "+ Point"
// gets a clearer spoken name. Verify the names resolve and the toolbar is
// a labelled group.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

#include "../UI/AutomationToolbar.h"

namespace zynforge
{
    class AutomationToolbarAccessibilityTests final : public juce::UnitTest
    {
    public:
        AutomationToolbarAccessibilityTests() : UnitTest ("AutomationToolbar accessibility", "zynforge") {}

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
            beginTest ("Automation controls expose spoken accessible names");
            {
                AutomationToolbar bar;
                bar.setBounds (0, 0, 600, 30);

                expectEquals (bar.getTitle(), juce::String ("Automation"));

                std::set<juce::String> names;
                walk (bar, [&] (juce::Component& c)
                {
                    // A button's spoken name is its title (if set) else its text.
                    if (auto* b = dynamic_cast<juce::Button*> (&c))
                        names.insert (b->getTitle().isNotEmpty() ? b->getTitle() : b->getButtonText());
                    else if (c.getTitle().isNotEmpty())
                        names.insert (c.getTitle());
                });

                for (auto* expected : { "Select", "Add point", "Delete",
                                        "Automation lane", "Write mode" })
                    expect (names.count (expected) > 0,
                            juce::String (expected) + " has no spoken accessible name");

                juce::Image img (juce::Image::ARGB, 600, 30, true);
                juce::Graphics g (img);
                bar.paint (g);   // smoke
                expect (true);
            }
        }
    };

    static AutomationToolbarAccessibilityTests automationToolbarAccessibilityTestsInstance;
}
