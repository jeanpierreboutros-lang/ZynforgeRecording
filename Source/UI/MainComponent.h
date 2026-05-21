#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/ZynForgeLookAndFeel.h"
#include "ChannelStrip.h"

#include <memory>
#include <vector>

namespace zynforge { class ChannelStrip; }

class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void rebuildStrips();
    void onRecordClicked();
    void onDeviceClicked();
    juce::File makeNewSessionDir() const;
    void timerCallback() override;

    zynforge::ZynForgeLookAndFeel laf;
    zynforge::AudioEngine         engine;

    juce::Label  titleLabel        { {}, "ZYNFORGE  RECORDING" };
    juce::Label  statusLabel       { {}, "Idle" };
    juce::TextButton recordButton  { "RECORD" };
    juce::TextButton deviceButton  { "AUDIO DEVICE" };

    std::vector<std::unique_ptr<zynforge::ChannelStrip>> strips;
    int  lastTrackCount { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
