// Headless accessibility test for TransportBar. The six transport controls
// are icon-only (no text glyph a screen reader can read), so each must
// expose an explicit spoken name, and the bar must be a labelled group.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

#include "../UI/TransportBar.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class TransportBarAccessibilityTests final : public juce::UnitTest
    {
    public:
        TransportBarAccessibilityTests() : UnitTest ("TransportBar accessibility", "zynforge") {}

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
            beginTest ("Transport buttons expose spoken accessible names");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine eng;
                TransportBar bar (eng);
                bar.setBounds (0, 0, 320, 48);

                expectEquals (bar.getTitle(), juce::String ("Transport"));

                std::set<juce::String> titles;
                walk (bar, [&] (juce::Component& c)
                {
                    if (auto* b = dynamic_cast<juce::Button*> (&c))
                        titles.insert (b->getTitle());
                });

                for (auto* expected : { "Go to start", "Go to end", "Play",
                                        "Stop", "Record", "Loop region" })
                    expect (titles.count (expected) > 0,
                            juce::String (expected) + " button is missing an accessible title");

                juce::Image img (juce::Image::ARGB, 320, 48, true);
                juce::Graphics g (img);
                bar.paint (g);   // smoke
                expect (true);
            }
        }
    };

    static TransportBarAccessibilityTests transportBarAccessibilityTestsInstance;
}
