#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

#include "../Audio/TrackExporter.h"

namespace zynforge
{
    // Modal dialog: pick format, sample rate, bit depth (or MP3 bitrate).
    // Per-format constraints:
    //   - WAV  : 16 / 24 / 32 (32 = IEEE float)
    //   - AIFF : 16 / 24 / 32 (32 = IEEE float)
    //   - FLAC : 16 / 24       (FLAC does not support 32-bit)
    //   - MP3  : bit-depth N/A, uses MP3 bitrate (128 / 256 / 320 kbps)
    //
    // Calls onResult with ExportOptions on OK, std::nullopt on Cancel.
    class ExportDialog
    {
    public:
        using ResultCallback = std::function<void (std::optional<ExportOptions>)>;

        static void launch (const juce::String& title, ResultCallback onResult);
    };
}
