#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Phase correlation meter: −1 (inverted) … 0 (uncorrelated) … +1 (in phase).
    // Two arrow buttons let the engineer pick which input pair to read.
    class PhaseMeter final : public juce::Component,
                             private juce::Timer
    {
    public:
        explicit PhaseMeter (AudioEngine& eng);
        ~PhaseMeter() override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void cyclePair (int delta);
        void refreshLabel();

        AudioEngine& engine;

        juce::Label      title       { {}, "PHASE" };
        juce::Label      pairLabel   { {}, "1 / 2" };
        juce::TextButton prevButton  { "<" };
        juce::TextButton nextButton  { ">" };

        float displayValue { 0.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhaseMeter)
    };
}
