#include <juce_core/juce_core.h>
#include "../Audio/ChannelCsv.h"

namespace zynforge
{
    class ChannelCsvTests final : public juce::UnitTest
    {
    public:
        ChannelCsvTests() : UnitTest ("Channel CSV", "zynforge") {}

        void runTest() override
        {
            using namespace channelcsv;

            beginTest ("One name per line");
            {
                const auto n = parseNames ("Kick\nSnare\nHat\n");
                expectEquals (n.size(), 3);
                expectEquals (n[0], juce::String ("Kick"));
                expectEquals (n[2], juce::String ("Hat"));
            }

            beginTest ("Channel-number + name rows, header dropped");
            {
                const auto n = parseNames ("Channel,Name\n1,Kick\n2,Snare\n3,Vocal\n");
                expectEquals (n.size(), 3);
                expectEquals (n[0], juce::String ("Kick"));
                expectEquals (n[1], juce::String ("Snare"));
            }

            beginTest ("Quoted fields with commas are preserved");
            {
                const auto n = parseNames ("1,\"Snare, top\"\n2,\"Lead \"\"Vox\"\"\"\n");
                expectEquals (n.size(), 2);
                expectEquals (n[0], juce::String ("Snare, top"));
                expectEquals (n[1], juce::String ("Lead \"Vox\""));
            }

            beginTest ("Blank lines skipped; semicolons + tabs as separators");
            {
                const auto n = parseNames ("1;Kick\n\n2\tSnare\n");
                expectEquals (n.size(), 2);
                expectEquals (n[0], juce::String ("Kick"));
                expectEquals (n[1], juce::String ("Snare"));
            }

            beginTest ("Plain name list with a 'Name' header line");
            {
                const auto n = parseNames ("Name\nKick\nSnare\n");
                expectEquals (n.size(), 2);
                expectEquals (n[0], juce::String ("Kick"));
            }

            beginTest ("A numbered row with no name keeps later names on their channels");
            {
                const auto n = parseNames ("Ch,Name\n1,Kick\n2,Snare\n3,\n4,Tom\n");
                expectEquals (n.size(), 4);
                expectEquals (n[0], juce::String ("Kick"));
                expectEquals (n[2], juce::String(), "channel 3 has no name");
                expectEquals (n[3], juce::String ("Tom"), "Tom shifted onto channel 3");
            }

            beginTest ("Gaps in the channel numbers leave those channels unnamed");
            {
                const auto n = parseNames ("1,Kick\n2,Snare\n5,Vocal\n");
                expectEquals (n.size(), 5);
                expectEquals (n[1], juce::String ("Snare"));
                expectEquals (n[2], juce::String());
                expectEquals (n[4], juce::String ("Vocal"), "Vocal landed on channel 3");
            }

            beginTest ("'Ch 1' labels are channel numbers, not names");
            {
                const auto n = parseNames ("Ch 1,Kick\nCH02,Snare\nInput 3,Hat\n");
                expectEquals (n.size(), 3);
                expectEquals (n[0], juce::String ("Kick"));
                expectEquals (n[1], juce::String ("Snare"));
                expectEquals (n[2], juce::String ("Hat"));
            }

            beginTest ("A list with no names at all is empty");
            {
                expect (parseNames ("1,\n2,\n").isEmpty());
            }
        }
    };

    static ChannelCsvTests channelCsvTests;
}
