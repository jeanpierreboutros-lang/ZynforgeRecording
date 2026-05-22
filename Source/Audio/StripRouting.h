#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace zynforge
{
    // Persists per-channel input/output routing overrides via a
    // juce::PropertiesFile. Keys "strip_in_<i>" / "strip_out_<i>" → int
    // device channel index, or -1 for unrouted. Absent = identity
    // routing (strip N ↔ device N).
    class StripRouting
    {
    public:
        StripRouting();

        bool hasInput  (int channelIndex) const;
        bool hasOutput (int channelIndex) const;
        int  getInput  (int channelIndex) const;
        int  getOutput (int channelIndex) const;
        void setInput  (int channelIndex, int deviceChannel);
        void setOutput (int channelIndex, int deviceChannel);
        void clearInput  (int channelIndex);
        void clearOutput (int channelIndex);

    private:
        static juce::String inKey  (int i);
        static juce::String outKey (int i);
        std::unique_ptr<juce::PropertiesFile> props;
    };
}
