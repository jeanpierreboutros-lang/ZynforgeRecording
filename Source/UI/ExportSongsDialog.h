#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/SongExport.h"
#include "ExportDialog.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace zynforge
{
    // Pick the songs to export: one row per marker range with a tick box and an
    // editable name, then what to write (mix, stems or both) and the format.
    class ExportSongsContent final : public juce::Component
    {
    public:
        using ResultCallback = std::function<void (std::optional<songexport::Choice>)>;

        ExportSongsContent (const std::vector<songexport::Range>& songs, double sessionRate,
                            ResultCallback onResult);

        void resized() override;
        void paint (juce::Graphics&) override;

        // The dialog's answer as it stands; empty songs when nothing is ticked.
        songexport::Choice currentChoice() const;

    private:
        friend class SongExportTests;

        struct Row : juce::Component
        {
            juce::ToggleButton tick;
            juce::TextEditor   name;
            juce::Label        span;
            songexport::Range  range;
            void resized() override;
        };

        void refreshExportEnabled();
        void setAll (bool on);
        void finish (bool accepted);

        ResultCallback callback;

        juce::Viewport  viewport;
        juce::Component listHolder;
        std::vector<std::unique_ptr<Row>> rows;

        juce::TextButton   selectAllButton, clearButton, exportButton, cancelButton;
        juce::Label        contentLabel;
        juce::ComboBox     contentBox;
        juce::ToggleButton stereoOneFile { "Stereo pair = one file" };
        ExportFormatControls format;
    };

    class ExportSongsDialog
    {
    public:
        static void launch (const std::vector<songexport::Range>& songs, double sessionRate,
                            ExportSongsContent::ResultCallback onResult);
    };
}
