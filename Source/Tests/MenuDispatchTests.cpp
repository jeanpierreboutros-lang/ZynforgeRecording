#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include "../UI/MainComponent.h"
#include "../UI/MenuCommandTable.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Audits every menu the app builds against the dispatch table for the
    // id-collision class that has bitten this project repeatedly:
    //   (a) one id used by two different items ("Delete Template" + "Session
    //       Format" were both 250);
    //   (b) an exact id lying inside a dispatch RANGE (the companion-server
    //       item sat at 270 inside a template range and never ran);
    //   (c) a built menu item with no handler at all.
    // The table reports (b) itself via conflicts(), so this no longer depends
    // on a hand-kept list of ranges -- the old list had already gone stale.
    // Builds MainComponent headlessly via the s_testConstruct hook (no timers /
    // dialogs / menu-bar registration).
    class MenuDispatchTests final : public juce::UnitTest
    {
    public:
        MenuDispatchTests() : UnitTest ("Menu dispatch", "zynforge") {}

        void collect (const juce::PopupMenu& menu, std::map<int, juce::String>& seen, bool& dup)
        {
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            {
                const auto& item = it.getItem();
                if (item.subMenu != nullptr) collect (*item.subMenu, seen, dup);

                const int id = item.itemID;
                if (id <= 0) continue;   // separators / disabled placeholders

                const auto existing = seen.find (id);
                if (existing != seen.end() && existing->second != item.text)
                { dup = true; logMessage ("id " + juce::String (id) + " used by '"
                                          + existing->second + "' AND '" + item.text + "'"); }
                else
                    seen[id] = item.text;
            }
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            {
                MainComponent mc;
                std::map<int, juce::String> seen;
                bool dup = false;
                for (int top = 0; top < 5; ++top)
                    collect (mc.getMenuForIndex (top, {}), seen, dup);
                const auto& table = mc.getMenuCommands();

                beginTest ("No menu id is reused");
                expect (! dup, "a menu id is claimed by two different items");
                expect ((int) seen.size() > 25, "expected to walk a populated menu bar");

                beginTest ("No exact menu id lies inside a dispatch range, and no ranges overlap");
                const auto conflicts = table.conflicts();
                for (const auto& c : conflicts) logMessage (c);
                expect (conflicts.isEmpty(), "dispatch table conflicts: " + conflicts.joinIntoString ("; "));

                beginTest ("Every item the menu bar builds has a handler");
                for (const auto& [id, text] : seen)
                    expect (table.handles (id), "menu item '" + text + "' (id " + juce::String (id)
                                                + ") has no handler -- selecting it does nothing");
            }
            MainComponent::s_testConstruct = false;
            AudioEngine::setTestModeSkipAudioInit (false);

            beginTest ("The table prefers exact ids and reports conflicts");
            {
                MenuCommandTable t;
                int hit = 0, rangeOffset = -1;
                t.add (5, [&hit] { hit = 5; });
                t.addRange (100, 199, [&rangeOffset] (int offset) { rangeOffset = offset; });
                expect (t.dispatch (5) && hit == 5);
                expect (t.dispatch (142) && rangeOffset == 42);
                expect (! t.dispatch (300) && ! t.handles (300));
                expect (t.conflicts().isEmpty());
                t.add (150, [&hit] { hit = 150; });           // exact inside the range
                t.addRange (190, 210, [] (int) {});           // overlapping range
                expect (t.dispatch (150) && hit == 150, "exact ids win over ranges");
                expectEquals (t.conflicts().size(), 2);
            }
        }
    };

    static MenuDispatchTests menuDispatchTests;
}
