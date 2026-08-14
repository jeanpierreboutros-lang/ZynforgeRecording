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

        // `sessionSampleRate` is the rate the session's audio is actually AT.
        // It preselects the matching entry (adding it to the list when it isn't
        // one of the four standard rates) so the default export is a straight
        // copy. The dialog used to hardcode 48 kHz regardless, which silently
        // resampled every 44.1 / 88.2 / 96 kHz session -- TrackExporter has a
        // deliberate bit-exact path for matched rates, and the default sent you
        // off it. Pass 0 when unknown to fall back to 48 kHz.
        static void launch (const juce::String& title,
                            double sessionSampleRate,
                            ResultCallback onResult);
    };
}
