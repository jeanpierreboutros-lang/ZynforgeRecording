#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

#include <memory>

namespace zynforge
{
    // Persists per-channel strip colour overrides via a juce::PropertiesFile
    // in the user's application support folder. Keys are
    // "strip_color_<channelIndex>", values are ARGB ints. Absent key =
    // "use default personality colour for that index".
    class StripColours
    {
    public:
        StripColours();

        bool         hasColour (int channelIndex) const;
        juce::Colour getColour (int channelIndex) const;
        void         setColour (int channelIndex, juce::Colour);
        void         clearColour (int channelIndex);

    private:
        static juce::String keyFor (int channelIndex);
        std::unique_ptr<juce::PropertiesFile> props;
    };
}
