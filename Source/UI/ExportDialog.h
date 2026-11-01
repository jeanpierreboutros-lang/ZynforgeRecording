#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

#include "../Audio/TrackExporter.h"

namespace zynforge
{
    // The format rows every export dialog shares: format, sample rate and bit
    // depth, with the per-format constraints below.
    class ExportFormatControls final : public juce::Component
    {
    public:
        // `sessionRate`: see ExportDialog::launch.
        explicit ExportFormatControls (double sessionRate);

        ExportOptions getOptions() const;

        static constexpr int kPreferredHeight = 126;

        void resized() override;

    private:
        void refreshAvailability();

        std::vector<double> rateHz;
        double              sessionRateHz { 48000.0 };
        juce::Label         srNote;
        juce::Label         formatLabel, rateLabel, bitsLabel;
        juce::ComboBox      formatBox,   rateBox,   bitsBox;
    };

    // Modal dialog: pick format, sample rate, bit depth.
    // Per-format constraints:
    //   - WAV  : 16 / 24 / 32 (32 = IEEE float)
    //   - AIFF : 16 / 24 / 32 (32 = IEEE float)
    //   - FLAC : 16 / 24       (FLAC does not support 32-bit)
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
