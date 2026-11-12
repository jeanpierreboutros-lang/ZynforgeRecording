#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

#include <utility>
#include <vector>

namespace zynforge
{
    // Modal patch page with INPUT PATCH / OUTPUT PATCH tabs. Rows are the
    // device's hardware inputs (or outputs); columns are the channel
    // strips. Clicking a circle assigns that hardware channel to the
    // strip; clicking the same circle again clears it. Each strip can be
    // patched to one hardware channel at a time (column-radio behaviour).
    class PatchPage
    {
    public:
        static juce::DialogWindow* launch (AudioEngine& engine);

        // One hardware row of a matrix as it is drawn.
        struct RowInfo
        {
            int          channel { 0 };   // the device channel
            juce::String label;           // "IN 3" / "OUT 5"
            juce::String note;            // a second line under the label, empty for none
            bool         held { false };  // a strip sits on it
        };
        // The device's rows, each saying how many strips play into it when that
        // is more than one, and on the output side one more row, channel -1,
        // while any strip sits on an output this device does not have.
        static std::vector<RowInfo> rowsFor (AudioEngine& engine, bool inputSide);
        // The output past this device's last that a strip (L, or a pair's R)
        // sits on, or -1. Such a strip plays nowhere.
        static int offDeviceOutput (AudioEngine& engine, int firstTrack, bool stereo);
        // What a patch click says while a take records or saves: the engine
        // refuses it, and a click that does nothing looks broken mid-show.
        static juce::String lockedForTake() { return "Patch is locked while a take records -- change it after STOP"; }

        // Column and row of every strip a drag lays down, from where it began.
        // A pair takes two rows, so the strip after it starts two rows on and
        // no two strips share a channel. A pair that would run off the last
        // row ends the drag.
        static std::vector<std::pair<int, int>> dragTargets (const std::vector<bool>& stripIsStereo,
                                                             int startCol, int startRow,
                                                             int rowsDragged, int numRows);
    };
}
