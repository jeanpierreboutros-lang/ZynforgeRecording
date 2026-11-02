#include <juce_core/juce_core.h>

#include "../Audio/DisplayText.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    class DisplayTextTests final : public juce::UnitTest
    {
    public:
        DisplayTextTests() : juce::UnitTest ("Display text", "zynforge") {}

        void runTest() override
        {
            beginTest ("Counts read as words");
            {
                expectEquals (counted (1, "track"), juce::String ("1 track"));
                expectEquals (counted (9, "armed track"), juce::String ("9 armed tracks"));
                expectEquals (counted (0, "strip"), juce::String ("0 strips"));
            }

            beginTest ("A message drawn on screen has a real dash and keeps its source text");
            {
                const juce::String source ("! PRIMARY DRIVE NOT ANSWERING -- check the session drive");
                expect (typeset (source).contains (juce::String::fromUTF8 ("ANSWERING \xe2\x80\x94 check")));
                expect (! typeset (source).contains ("--"));
                expectEquals (typeset ("no dash here"), juce::String ("no dash here"));
                expectEquals (typeset ("--"), juce::String ("--"), "an empty readout is not a sentence");
            }

            beginTest ("A drive reads the same on every screen, in Finder's units");
            {
                expectEquals (juce::String (bytesToGB (25000000000LL), 1), juce::String ("25.0"));
                expectEquals (bytesToMB (9400000.0), 9.4);
            }

            beginTest ("Record time reads one way");
            {
                expectEquals (recordTimeText (19200.0), juce::String ("5h20m"));
                expectEquals (recordTimeText (0.0), juce::String ("--"));
                expectEquals (recordTimeText (400000.0), juce::String (">99h"));
            }

            beginTest ("An R button stays readable and stays below the alarm's red");
            {
                const auto contrast = [] (juce::Colour fill)
                { return 1.05f / (brand::relativeLuminance (fill) + 0.05f); };
                expect (contrast (brand::armFill) >= 4.5f, "the armed R is hard to read");
                expect (contrast (brand::armRollFill) >= 4.5f, "the rolling R is hard to read");
                expect (brand::armFill != brand::accentRecord, "armed is the alarm colour");
                expect (brand::relativeLuminance (brand::armRollFill) < brand::relativeLuminance (brand::accentRecord),
                        "a rolling R outshines the alarm");
            }
        }
    };

    static DisplayTextTests displayTextTests;
}
