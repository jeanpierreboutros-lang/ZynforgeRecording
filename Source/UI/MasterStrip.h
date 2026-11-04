#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "FineFader.h"
#include "LedMeter.h"

#include <memory>
#include <utility>

namespace zynforge
{
    // Fixed master strip -- sits to the right of the channel-strip viewport
    // and never scrolls. Controls the master gain / mute / output routing
    // and meters the master bus.
    class MasterStrip final : public juce::Component, private juce::Timer
    {
    public:
        explicit MasterStrip (AudioEngine&);
        ~MasterStrip() override;

        void paint    (juce::Graphics&) override;
        void resized  () override;

        // Re-populate the output combos when device topology changes.
        void refreshOutputs();

        // The outputs a mode flip leaves the master on. Mono is one output,
        // (L, L); back in stereo it is the pair that output belongs to, as the
        // stereo list offers them. Off stays off.
        static std::pair<int, int> outputsForMode (bool stereo, int left, int right) noexcept;

    private:
        void timerCallback() override;

        AudioEngine&     engine;
        juce::Label      title { {}, "MASTER" };
        juce::Label      gainLabel;
        juce::ComboBox   outputCombo;
        juce::Label      heldLabel;       // shown while the mix is held off its pair
        juce::ToggleButton modeButton { "ST" };   // toggles between MONO and STEREO
        juce::ToggleButton muteButton { "MUTE" };
        FineFader        fader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
        LedMeter         meter;
        int lastNumOutputs { -1 };
        bool             cachedStereo { true };
        bool             cachedHeldOff { false };
        // Stereo mode the output combo was last BUILT for -- refreshOutputs()
        // skips its 10 Hz rebuild while (device count, mode, saved pair) are
        // unchanged.
        bool             lastComboStereo { true };
        int              lastComboLeft { -2 }, lastComboRight { -2 };
        juce::String     outputMovedTooltip;   // set while the saved pair is not on this device

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
    };
}
