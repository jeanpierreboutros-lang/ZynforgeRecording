#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
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
        // `sessionRate`: see ExportAudioDialog::launch.
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

    // File > Export > Export Audio...: one dialog for the three kinds of audio a
    // session hands over. Each goes to the render path it always had.
    //   Raw takes      the recorded files, without clip edits, in the chosen format
    //   Edited tracks  each ticked strip's arrangement with clip edits, 24-bit WAV
    //   Stereo mix     the arrangement through the mix, one 24-bit WAV
    //
    // Per-format constraints of the controls:
    //   - WAV  : 16 / 24 / 32 (32 = IEEE float)
    //   - AIFF : 16 / 24 / 32 (32 = IEEE float)
    //   - FLAC : 16 / 24       (FLAC does not support 32-bit)
    class ExportAudioContent final : public juce::Component
    {
    public:
        enum class Source { rawTakes, editedTracks, stereoMix };

        struct Result
        {
            Source           source { Source::rawTakes };
            std::vector<int> rows;      // ticked rows of the names given; empty for a stereo mix
            ExportOptions    options;   // raw takes only
        };

        using ResultCallback = std::function<void (std::optional<Result>)>;

        // `sessionRate`: see ExportAudioDialog::launch. names[i] labels row i.
        ExportAudioContent (double sessionRate, const std::vector<juce::String>& names, ResultCallback onResult);

        void   chooseSource (Source);
        Source getSource() const;
        Result getResult() const;
        bool   canExport() const noexcept          { return exportButton.isEnabled(); }
        bool   formatApplies() const noexcept      { return format.isEnabled(); }
        bool   tracksApply() const noexcept        { return viewport.isEnabled(); }
        juce::String getNote() const               { return note.getText(); }
        void   setAllTicked (bool on);

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        static constexpr int kWidth = 420, kRowH = 26, kVisibleRows = 8;

        void refresh();
        void finish (bool accepted);

        ResultCallback callback;
        juce::ToggleButton rawButton { "Raw takes" }, editedButton { "Edited tracks" }, mixButton { "Stereo mix" };
        juce::Label     note;
        juce::Viewport  viewport;
        juce::Component listHolder;
        std::vector<std::unique_ptr<juce::ToggleButton>> ticks;
        juce::TextButton selectAllButton, clearButton, exportButton, cancelButton;
        ExportFormatControls format;
    };

    class ExportAudioDialog
    {
    public:
        // `sessionSampleRate` is the rate the session's audio is actually AT.
        // It preselects the matching entry (adding it to the list when it isn't
        // one of the four standard rates) so the default export is a straight
        // copy. The dialog used to hardcode 48 kHz regardless, which silently
        // resampled every 44.1 / 88.2 / 96 kHz session -- TrackExporter has a
        // deliberate bit-exact path for matched rates, and the default sent you
        // off it. Pass 0 when unknown to fall back to 48 kHz.
        static void launch (double sessionSampleRate,
                            const std::vector<juce::String>& trackNames,
                            ExportAudioContent::ResultCallback onResult);
    };
}
