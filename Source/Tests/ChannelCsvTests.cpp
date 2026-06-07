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
        }
    };

    static ChannelCsvTests channelCsvTests;
}
