#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace zynforge
{
    // Modal dialog: pick which session tracks to export. Shows one tick per
    // track (scrollable for big sessions) plus Select all / Clear, then
    // Export. Calls onResult with the chosen 0-based track indices on
    // Export, or std::nullopt on Cancel. The caller chains this into
    // ExportDialog (format) → destination chooser → exportTracksTo.
    class TrackSelectDialog
    {
    public:
        using ResultCallback = std::function<void (std::optional<std::vector<int>>)>;

        // names[i] is the display label for track index i (0-based).
        static void launch (const juce::String& title,
                            const std::vector<juce::String>& names,
                            ResultCallback onResult);
    };
}
