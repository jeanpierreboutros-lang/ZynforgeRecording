#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace zynforge
{
    // Small popup shown via juce::CallOutBox: 10 fixed colour swatches
    // (2 rows × 5) plus a "Custom…" button that opens a juce::ColourSelector.
    // Reports the chosen colour through `onColourPicked`. Passing an
    // empty / transparent colour means "revert to default".
    class StripColourPicker final : public juce::Component
    {
    public:
        using Callback = std::function<void (juce::Colour)>;

        StripColourPicker (juce::Colour currentColour, Callback onColourPicked);

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void resized() override;

    private:
        void openCustomSelector();

        static constexpr int kCols  = 5;
        static constexpr int kRows  = 2;
        static constexpr int kSize  = 30;
        static constexpr int kGap   = 6;
        static constexpr int kMargin = 8;

        std::array<juce::Colour, kCols * kRows> presets;
        juce::Colour     current;
        Callback         callback;
        juce::TextButton customButton { "Custom…" };
        juce::TextButton resetButton  { "Default" };

        std::unique_ptr<juce::Component> colourSelectorContent;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StripColourPicker)
    };
}
