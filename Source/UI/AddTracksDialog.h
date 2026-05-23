#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace zynforge
{
    // Pro Tools-style 'New Tracks' picker. Engineer can stack multiple
    // rows in one dialog; each row carries a count + mono/stereo +
    // base name. The '+' button on the right of any row appends another
    // row below; Create commits all rows in one batch.
    class AddTracksDialog
    {
    public:
        struct Entry
        {
            int  count    { 1 };
            bool stereo   { false };
            // Pro Tools-style: tracks can be audio (input/output)
            // or bus (no input; sums sends + routes to outputs).
            bool isBus    { false };
            juce::String baseName;
        };

        using ResultCallback = std::function<void (const std::vector<Entry>&)>;

        // Opens the dialog modal-async. Callback fires with a non-empty
        // vector on Create, or an empty vector on Cancel.
        static void launch (ResultCallback);
    };
}
