#pragma once

// Markers and cues are not on the undo stack. An undo or redo of a Crop or
// Ripple Delete puts back the markers and cues the step replaced, and whatever
// was done to them since must survive it, once. Both carry a runtime id
// (Marker::runtimeId, SetlistBar::Cue::runtimeId) that copies keep, so the
// three lists can be compared item by item:
//   target   -- what the step puts back (the state before it, on undo)
//   replaced -- what the step itself left (the state after it, on undo)
//   current  -- the live list now.

#include <algorithm>
#include <vector>

namespace zynforge::timelinecarry
{
    // The list an undo or redo of a timeline step leaves, in current order:
    //  - one added since stays as it is now (it is not the step's to move);
    //  - one deleted since stays deleted;
    //  - one moved or re-captured since (`changed`) keeps its own place and
    //    content -- its position was chosen on the timeline it was edited on;
    //  - one only renamed since moves with the step under its new name
    //    (`placed` takes the target's position into the current item);
    //  - one the step dropped (a Crop's out-of-range cue) comes back on undo,
    //    after the item it followed in `target`.
    template <class Item, class IdOf, class Changed, class Placed>
    std::vector<Item> carryForward (const std::vector<Item>& target, const std::vector<Item>& replaced,
                                    const std::vector<Item>& current,
                                    IdOf idOf, Changed changed, Placed placed)
    {
        const auto find = [&idOf] (const std::vector<Item>& list, const auto& id) -> const Item*
        {
            for (const auto& item : list)
                if (idOf (item) == id) return &item;
            return nullptr;
        };

        std::vector<Item> result;
        result.reserve (current.size() + target.size());
        for (const auto& now : current)
        {
            const auto* then = find (replaced, idOf (now));
            if (then == nullptr || changed (*then, now)) { result.push_back (now); continue; }
            if (const auto* back = find (target, idOf (now)))
                result.push_back (placed (now, *back));
            // else: the step removes it, and nothing was done to it since
        }

        // What the step dropped and puts back. Never deleted since: it was not
        // in `replaced`, so nothing since could have reached it.
        for (size_t i = 0; i < target.size(); ++i)
        {
            const auto& back = target[i];
            if (find (replaced, idOf (back)) != nullptr || find (current, idOf (back)) != nullptr) continue;
            auto at = result.begin();
            for (size_t j = i; j-- > 0;)
            {
                const auto previous = idOf (target[j]);
                const auto it = std::find_if (result.begin(), result.end(),
                                              [&] (const Item& r) { return idOf (r) == previous; });
                if (it != result.end()) { at = it + 1; break; }
            }
            result.insert (at, back);
        }
        return result;
    }
}
