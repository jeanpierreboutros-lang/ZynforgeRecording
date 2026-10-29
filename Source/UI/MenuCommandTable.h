#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <map>
#include <vector>

namespace zynforge
{
    // The menu bar's dispatch table: exact item ids, plus id RANGES for
    // generated items (session templates 200+i, MIDI clock outputs 401+i...).
    //
    // MainComponent::menuItemSelected used to be one ~500-line if/else chain in
    // which a range branch swallowed every later exact id inside it -- a static
    // item placed there silently became dead code (the companion-server item
    // did, at id 270). The chain also kept branches for ids no menu built any
    // more. Here exact ids always win, and conflicts() names every exact id
    // that sits inside a range and every pair of overlapping ranges, so a test
    // fails instead of an engineer finding a menu item that does nothing.
    class MenuCommandTable
    {
    public:
        void add (int id, std::function<void()> fn)
        {
            jassert (exact.count (id) == 0);   // one handler per id
            exact[id] = std::move (fn);
        }

        // Items first..last inclusive; the handler receives (id - first).
        void addRange (int first, int last, std::function<void (int offset)> fn)
        {
            jassert (first <= last);
            ranges.push_back ({ first, last, std::move (fn) });
        }

        bool dispatch (int id) const
        {
            if (const auto it = exact.find (id); it != exact.end()) { it->second(); return true; }
            for (const auto& r : ranges)
                if (id >= r.first && id <= r.last) { r.fn (id - r.first); return true; }
            return false;
        }

        bool handles (int id) const
        {
            if (exact.count (id) != 0) return true;
            for (const auto& r : ranges)
                if (id >= r.first && id <= r.last) return true;
            return false;
        }

        bool isEmpty() const noexcept { return exact.empty() && ranges.empty(); }

        juce::StringArray conflicts() const
        {
            juce::StringArray out;
            for (const auto& entry : exact)
                for (const auto& r : ranges)
                    if (entry.first >= r.first && entry.first <= r.last)
                        out.add ("id " + juce::String (entry.first) + " lies inside range "
                                 + juce::String (r.first) + ".." + juce::String (r.last));
            for (size_t i = 0; i < ranges.size(); ++i)
                for (size_t j = i + 1; j < ranges.size(); ++j)
                    if (ranges[i].first <= ranges[j].last && ranges[j].first <= ranges[i].last)
                        out.add ("ranges " + juce::String (ranges[i].first) + ".." + juce::String (ranges[i].last)
                                 + " and " + juce::String (ranges[j].first) + ".." + juce::String (ranges[j].last)
                                 + " overlap");
            return out;
        }

    private:
        struct Range { int first, last; std::function<void (int)> fn; };
        std::map<int, std::function<void()>> exact;
        std::vector<Range> ranges;
    };
}
