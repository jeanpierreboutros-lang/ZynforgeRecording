#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include "../UI/MainComponent.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Audits every menu the app builds for the id-collision class that has
    // bitten this project repeatedly: (a) one id used by two different items
    // (e.g. "Delete Template" + "Session Format" both = 250), and (b) a menu
    // id landing inside a dispatch range that maps elsewhere (e.g. Control
    // Surfaces = 130 falling in the per-track-export range 100..199). Both
    // would have been caught here. Builds MainComponent headlessly via the
    // s_testConstruct hook (no timers / dialogs / menu-bar registration).
    class MenuDispatchTests final : public juce::UnitTest
    {
    public:
        MenuDispatchTests() : UnitTest ("Menu dispatch", "zynforge") {}

        void collect (const juce::PopupMenu& menu, std::map<int, juce::String>& seen,
                      bool& dup, bool& rangeHit)
        {
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            {
                const auto& item = it.getItem();
                if (item.subMenu != nullptr) collect (*item.subMenu, seen, dup, rangeHit);

                const int id = item.itemID;
                if (id <= 0) continue;   // separators / disabled placeholders

                // The per-track-export handler (id-100) is dynamic-only: no
                // static menu item should sit in [100,199].
                if (id >= 100 && id < 200)
                { rangeHit = true; logMessage ("id " + juce::String (id) + " ('" + item.text
                                               + "') collides with the export range 100-199"); }

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
            beginTest ("No menu id is reused, and none collide with a dispatch range");

            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            {
                MainComponent mc;
                std::map<int, juce::String> seen;
                bool dup = false, rangeHit = false;
                for (int top = 0; top < 5; ++top)
                    collect (mc.getMenuForIndex (top, {}), seen, dup, rangeHit);

                expect (! dup,      "a menu id is claimed by two different items");
                expect (! rangeHit, "a menu id falls inside the per-track-export range 100-199");
                expect ((int) seen.size() > 25, "expected to walk a populated menu bar");

                // Dispatch-RANGE collision guard: a static single-purpose item
                // must not reuse an id that a menuItemSelected RANGE handler
                // claims, or its real handler becomes dead code. The companion
                // item was id 270, which sat inside the 261..289 template range
                // -> "Start companion server" never reached its handler. Assert
                // the marquee static items live outside every dispatch range.
                static const std::pair<int,int> reservedRanges[] = {
                    { 100, 199 },   // per-track export + OSC dialects
                    { 200, 249 },   // session templates
                    { 261, 289 },   // template-as-default
                    { 401, 430 },   // MIDI clock outputs
                    { 620, 699 },   // LTC / MTC sources
                };
                for (const auto& [mid, text] : seen)
                    if (text.containsIgnoreCase ("companion server")
                        || text.containsIgnoreCase ("Auto-Save"))
                        for (const auto& r : reservedRanges)
                            expect (mid < r.first || mid > r.second,
                                    "menu item '" + text + "' (id " + juce::String (mid)
                                    + ") falls inside dispatch range " + juce::String (r.first)
                                    + ".." + juce::String (r.second) + " -> its handler is dead code");
            }
            MainComponent::s_testConstruct = false;
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static MenuDispatchTests menuDispatchTests;
}
