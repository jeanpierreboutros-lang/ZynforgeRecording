// Headless accessibility test for MasterStrip. The "ST" mono/stereo toggle
// is an unreadable glyph; every master control must expose a spoken name
// and the strip must be a labelled group.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

#include "../UI/MasterStrip.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class MasterStripAccessibilityTests final : public juce::UnitTest
    {
    public:
        MasterStripAccessibilityTests() : UnitTest ("MasterStrip accessibility", "zynforge") {}

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
            beginTest ("Master controls expose spoken accessible names");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine eng;
                MasterStrip strip (eng);
                strip.setBounds (0, 0, 90, 420);

                expectEquals (strip.getTitle(), juce::String ("Master"));

                std::set<juce::String> titles;
                walk (strip, [&] (juce::Component& c)
                {
                    if (c.getTitle().isNotEmpty())
                        titles.insert (c.getTitle());
                });

                for (auto* expected : { "Master gain", "Master mute",
                                        "Mono / stereo", "Master output" })
                    expect (titles.count (expected) > 0,
                            juce::String (expected) + " control is missing an accessible title");

                juce::Image img (juce::Image::ARGB, 90, 420, true);
                juce::Graphics g (img);
                strip.paint (g);   // smoke
                expect (true);
            }
        }
    };

    static MasterStripAccessibilityTests masterStripAccessibilityTestsInstance;
}
