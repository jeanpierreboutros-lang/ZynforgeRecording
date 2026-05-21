#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/ZynForgeLookAndFeel.h"
#include "BigClockPanel.h"
#include "ChannelStrip.h"
#include "PhaseMeter.h"
#include "TimelineStrip.h"

#include <memory>
#include <vector>

namespace zynforge { class ChannelStrip; }

class MainComponent final : public juce::Component,
                            public juce::KeyListener,
                            private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool keyPressed (const juce::KeyPress&, juce::Component*) override;

private:
    void rebuildStrips();
    void onRecordClicked();
    void onDeviceClicked();
    void onLoadSessionClicked();
    void onPlayClicked();
    void onStopClicked();
    void onFileMenuClicked();
    void onSaveSessionState();
    void onSaveSessionAs();
    void onExportAllTracks();
    void onExportIndividualTrack (int channelIndex);
    bool saveSessionStateTo (const juce::File& dir);
    int  exportTracksTo (const juce::File& dir, const std::vector<int>& channelIndices);
    void showStatus (const juce::String& msg);
    void onFormatClicked();
    void onPreRollClicked();
    void refreshFormatButton();
    void refreshPreRollButton();
    void updateTransportLabels();
    juce::File makeNewSessionDir() const;
    juce::File getSessionsRoot() const;
    void timerCallback() override;

    zynforge::ZynForgeLookAndFeel laf;
    zynforge::AudioEngine         engine;

    juce::Label  titleLabel        { {}, "ZYNFORGE  RECORDING" };
    juce::Label  statusLabel       { {}, "Idle" };
    juce::Label  sessionLabel      { {}, "No session loaded" };
    juce::Label  transportLabel    { {}, "00:00 / 00:00" };
    juce::TextButton recordButton  { "RECORD" };
    juce::TextButton deviceButton  { "AUDIO DEVICE" };
    juce::TextButton loadButton    { "FILE" };
    juce::TextButton playButton    { "PLAY" };
    juce::TextButton stopButton    { "STOP" };
    juce::TextButton formatButton  { "WAV 24" };
    juce::TextButton preRollButton { "PRE 0s" };

    std::unique_ptr<juce::FileChooser> chooser;

    zynforge::BigClockPanel bigClock;
    std::unique_ptr<zynforge::PhaseMeter>    phaseMeter;
    std::unique_ptr<zynforge::TimelineStrip> timeline;

    std::vector<std::unique_ptr<zynforge::ChannelStrip>> strips;
    int  lastTrackCount { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
