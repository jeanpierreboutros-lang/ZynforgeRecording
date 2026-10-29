#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <memory>
#include "SettingsFile.h"

namespace zynforge
{
    // Persists per-channel playback gain (dB) + pan (-1..+1) via a
    // juce::PropertiesFile in the user's application support folder.
    // Keys "strip_gain_<channelIndex>" and "strip_pan_<channelIndex>".
    //
    // Gain / pan writes are COALESCED: the first after a quiet spell goes to
    // disk at once, the rest of a burst are held in `pending` (readable at
    // once) and written together at most every kMinWriteIntervalMs. A cue
    // fade sets gain + pan on every strip at 10 Hz, and each call used to
    // reload and rewrite the whole shared settings file -- 1,120 rewrites a
    // second on 56 strips, which stalled the message thread for the fade.
    class StripGains : private juce::Timer
    {
    public:
        StripGains();
        ~StripGains() override;

        // Test isolation -- see StripNames::setTestMode. Flipped on by the
        // test-runner app (Tests/TestMain.cpp) so unit tests never write to the engineer's
        // real per-channel gains/pans.
        static void setTestMode (bool on) noexcept { testModeFlag() = on; if (on) isolateSettings(); }

        bool  hasGain (int channelIndex) const;
        float getGainDb (int channelIndex) const;
        void  setGainDb (int channelIndex, float dB);
        void  clearGain (int channelIndex);

        bool  hasPan (int channelIndex) const;
        float getPan (int channelIndex) const;
        void  setPan (int channelIndex, float pan);
        void  clearPan (int channelIndex);

        // Clear this module's overrides in [firstIndex, lastIndexExclusive)
        // with one shared-file reload/save, keeping its live cache consistent.
        void clearRange (int firstIndex, int lastIndexExclusive);
        void resetRange (int firstIndex, int lastIndexExclusive);

        // Write any held gain / pan changes now. Also runs on a timer and on
        // destruction.
        void flush();

    private:
        static juce::String gainKey (int channelIndex);
        static juce::String panKey  (int channelIndex);
        static bool& testModeFlag() noexcept { static bool t = false; return t; }

        void timerCallback() override;
        void write (const juce::String& key, double value);
        // Reload-to-replace, then re-apply held writes so they stay newer
        // than whatever sibling writers saved meanwhile. Caller holds `lock`.
        void reloadKeepingPending();
        const juce::var* pendingValue (const juce::String& key) const;

        std::unique_ptr<juce::PropertiesFile> props;
        mutable juce::CriticalSection lock;
        juce::NamedValueSet pending;
        juce::uint32 lastWriteMs { 0 };
        bool         everWritten { false };
        static constexpr juce::uint32 kMinWriteIntervalMs = 250;
    };
}
