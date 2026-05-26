// Headless accessibility tests for ChannelStrip. Verifies the strip and its
// controls expose spoken names a screen reader can use -- the R/I/M/S
// toggles must announce "Record arm" / "Input monitor" / "Mute" / "Solo"
// rather than their single-letter glyphs, the strip is titled with the
// channel name, and that title tracks a rename.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <map>

#include "../UI/ChannelStrip.h"
#include "../Audio/TrackState.h"

namespace zynforge
{
    class ChannelStripAccessibilityTests final : public juce::UnitTest
    {
    public:
        ChannelStripAccessibilityTests() : UnitTest ("ChannelStrip accessibility", "zynforge") {}

        // Depth-first: run fn on every descendant component.
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
            beginTest ("Strip + R/I/M/S toggles expose spoken accessible names");
            {
                TrackState ts;
                ts.name = "Kick";
                ChannelStrip cs (0, ts);
                cs.setBounds (0, 0, 90, 420);

                // Strip itself is a labelled group named after the channel.
                expectEquals (cs.getTitle(), juce::String ("Kick"));

                const std::map<juce::String, juce::String> want {
                    { "R", "Record arm" }, { "I", "Input monitor" },
                    { "M", "Mute" },       { "S", "Solo" } };

                int matched = 0;
                walk (cs, [&] (juce::Component& c)
                {
                    if (auto* tb = dynamic_cast<juce::ToggleButton*> (&c))
                    {
                        const auto it = want.find (tb->getButtonText());
                        if (it != want.end())
                        {
                            expectEquals (tb->getTitle(), it->second,
                                          "toggle '" + tb->getButtonText() + "' accessible name");
                            ++matched;
                        }
                    }
                });
                expectEquals (matched, 4, "all four transport toggles found + named");

                // Rename propagates to the strip's accessible title.
                ts.name = "Snare Top";
                cs.refreshAppearance();
                expectEquals (cs.getTitle(), juce::String ("Snare Top"));

                // Paint smoke -- must not crash.
                juce::Image img (juce::Image::ARGB, 90, 420, true);
                juce::Graphics g (img);
                cs.paint (g);
                expect (true);
            }
        }
    };

    static ChannelStripAccessibilityTests channelStripAccessibilityTestsInstance;
}
