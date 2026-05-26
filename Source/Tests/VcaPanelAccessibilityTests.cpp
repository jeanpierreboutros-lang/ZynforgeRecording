// Headless accessibility test for VcaPanel. The 8 VCA mini-strips have
// M / S glyph toggles; every per-bus control must expose a spoken name
// (tagged with the bus number) and the panel must be a labelled group.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

#include "../UI/VcaPanel.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class VcaPanelAccessibilityTests final : public juce::UnitTest
    {
    public:
        VcaPanelAccessibilityTests() : UnitTest ("VcaPanel accessibility", "zynforge") {}

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
            beginTest ("VCA bus controls expose per-bus spoken names");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine eng;
                VcaPanel panel (eng);
                panel.setBounds (0, 0, 640, 300);

                expectEquals (panel.getTitle(), juce::String ("VCA groups"));

                std::set<juce::String> titles;
                walk (panel, [&] (juce::Component& c)
                {
                    if (c.getTitle().isNotEmpty())
                        titles.insert (c.getTitle());
                });

                // First and last bus, each control kind.
                for (auto* expected : { "VCA 1 gain", "VCA 1 mute", "VCA 1 solo", "VCA 1 name",
                                        "VCA 8 gain", "VCA 8 mute", "VCA 8 solo" })
                    expect (titles.count (expected) > 0,
                            juce::String (expected) + " is missing an accessible title");

                juce::Image img (juce::Image::ARGB, 640, 300, true);
                juce::Graphics g (img);
                panel.paint (g);   // smoke
                expect (true);
            }
        }
    };

    static VcaPanelAccessibilityTests vcaPanelAccessibilityTestsInstance;
}
