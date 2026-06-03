#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>
#include <functional>

namespace zynforge
{
    // Strip colour picker shown via juce::CallOutBox. A gradient palette
    // (hue sweep across the columns × a light→dark shade gradient down the
    // rows) of selectable swatches, with the strip's current / original
    // colour pinned as the first swatch ("1 default colour of the original").
    // Buttons: Default (revert to the per-index colour), Custom... (full
    // juce::ColourSelector with its own OK), and OK to confirm + close.
    // Clicking a swatch live-previews the colour on the strip; OK closes the
    // box. An empty / transparent colour means "revert to default".
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
        void buildGradient();
        juce::Rectangle<int> swatchBounds (int i) const;

        static constexpr int kCols   = 8;
        static constexpr int kRows   = 3;
        static constexpr int kSize   = 26;
        static constexpr int kGap    = 5;
        static constexpr int kMargin = 8;
        static constexpr int kBtnH   = 30;

        std::vector<juce::Colour> presets;   // [0] = original, rest = gradient
        juce::Colour     original;           // the strip's current colour
        juce::Colour     pending;            // selection awaiting OK / live preview
        Callback         callback;
        juce::TextButton okButton     { "OK" };
        juce::TextButton resetButton  { "Default" };
        juce::TextButton customButton { "Custom..." };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StripColourPicker)
    };
}
