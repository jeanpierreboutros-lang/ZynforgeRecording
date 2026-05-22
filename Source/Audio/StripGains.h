#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace zynforge
{
    // Persists per-channel playback gain (dB) + pan (-1..+1) via a
    // juce::PropertiesFile in the user's application support folder.
    // Keys "strip_gain_<channelIndex>" and "strip_pan_<channelIndex>".
    class StripGains
    {
    public:
        StripGains();

        bool  hasGain (int channelIndex) const;
        float getGainDb (int channelIndex) const;
        void  setGainDb (int channelIndex, float dB);
        void  clearGain (int channelIndex);

        bool  hasPan (int channelIndex) const;
        float getPan (int channelIndex) const;
        void  setPan (int channelIndex, float pan);
        void  clearPan (int channelIndex);

    private:
        static juce::String gainKey (int channelIndex);
        static juce::String panKey  (int channelIndex);

        std::unique_ptr<juce::PropertiesFile> props;
    };
}
