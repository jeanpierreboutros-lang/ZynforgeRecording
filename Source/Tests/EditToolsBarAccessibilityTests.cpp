// Headless accessibility test for EditToolsBar. The six tool buttons are
// raw icon-painted Components (not juce::Button), so they get an explicit
// accessible name + a press action + keyboard focus. Verify the names are
// exposed and the bar is a labelled group.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

#include "../UI/EditToolsBar.h"

namespace zynforge
{
    class EditToolsBarAccessibilityTests final : public juce::UnitTest
    {
    public:
        EditToolsBarAccessibilityTests() : UnitTest ("EditToolsBar accessibility", "zynforge") {}

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
            beginTest ("Edit tools expose spoken accessible names");
            {
                EditToolsBar bar;
                bar.setBounds (0, 0, 340, 32);

                expectEquals (bar.getTitle(), juce::String ("Edit tools"));

                std::set<juce::String> titles;
                walk (bar, [&] (juce::Component& c)
                {
                    if (c.getTitle().isNotEmpty())
                        titles.insert (c.getTitle());
                });

                for (auto* expected : { "Smart", "Selector", "Trim",
                                        "Grabber", "Fade", "Scrubber" })
                    expect (titles.count (expected) > 0,
                            juce::String (expected) + " tool is missing an accessible title");

                juce::Image img (juce::Image::ARGB, 340, 32, true);
                juce::Graphics g (img);
                bar.paint (g);   // smoke
                expect (true);
            }
        }
    };

    static EditToolsBarAccessibilityTests editToolsBarAccessibilityTestsInstance;
}
