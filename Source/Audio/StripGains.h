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

        // Test isolation -- see StripNames::setTestMode. Flipped on by the
        // --run-tests path so unit tests never write to the engineer's
        // real per-channel gains/pans.
        static void setTestMode (bool on) noexcept { testModeFlag() = on; }

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
        static bool& testModeFlag() noexcept { static bool t = false; return t; }

        std::unique_ptr<juce::PropertiesFile> props;
    };
}
