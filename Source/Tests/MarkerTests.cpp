// Marker store + persistence tests. Exercises the in-memory list AND
// the markers.json round-trip via a temp session directory.

#include <juce_core/juce_core.h>
#include "../Audio/Markers.h"

namespace zynforge
{
    class MarkerStoreTests final : public juce::UnitTest
    {
    public:
        MarkerStoreTests() : UnitTest ("Marker store", "zynforge") {}

        void runTest() override
        {
            beginTest ("drop / getCount / getAll");
            {
                MarkersManager m;
                m.setContext (juce::File(), 48000.0);     // no session, in-memory only
                m.drop (48000,        "one");
                m.drop (96000,        "two");
                m.drop (48000 * 100);                     // auto-named
                expectEquals (m.getCount(), 3);
                expectEquals (m.getAll()[0].name, juce::String ("one"));
                expectEquals (m.getAll()[1].sampleOffset, (juce::int64) 96000);
                expect (m.getAll()[2].name.isNotEmpty());  // auto-name is non-empty
            }

            beginTest ("rename + removeMarker");
            {
                MarkersManager m;
                m.drop (1000, "a");
                m.drop (2000, "b");
                m.drop (3000, "c");
                m.renameMarker (1, "BB");
                expectEquals (m.getAll()[1].name, juce::String ("BB"));
                m.removeMarker (0);
                expectEquals (m.getCount(), 2);
                expectEquals (m.getAll()[0].name, juce::String ("BB"));
                expectEquals (m.getAll()[1].name, juce::String ("c"));
            }

            beginTest ("setMarkerSample updates position");
            {
                MarkersManager m;
                m.drop (1000, "a");
                m.setMarkerSample (0, 50000);
                expectEquals (m.getAll()[0].sampleOffset, (juce::int64) 50000);
            }

            beginTest ("out-of-range remove / rename are no-ops");
            {
                MarkersManager m;
                m.drop (1000, "a");
                m.removeMarker (99);          // shouldn't crash
                m.renameMarker (-1, "x");     // shouldn't crash
                m.setMarkerSample (50, 100);  // shouldn't crash
                expectEquals (m.getCount(), 1);
                expectEquals (m.getAll()[0].name, juce::String ("a"));
            }

            beginTest ("save / load round-trips through markers.json");
            {
                auto temp = juce::File::createTempFile ("");
                temp.deleteRecursively();
                temp.createDirectory();
                {
                    MarkersManager m;
                    m.setContext (temp, 48000.0);
                    m.drop (10000, "intro");
                    m.drop (20000, "verse");
                    m.drop (30000, "bridge");
                    expect (m.save());
                }
                MarkersManager m2;
                m2.setContext (temp, 48000.0);
                expectEquals (m2.getCount(), 3);
                expectEquals (m2.getAll()[1].name, juce::String ("verse"));
                expectEquals (m2.getAll()[1].sampleOffset, (juce::int64) 20000);
                temp.deleteRecursively();
            }

            beginTest ("clear empties the list");
            {
                MarkersManager m;
                m.drop (1000, "a");
                m.drop (2000, "b");
                m.clear();
                expectEquals (m.getCount(), 0);
                expect (m.getAll().empty());
            }
        }
    };

    static MarkerStoreTests markerStoreTestsInstance;
}
