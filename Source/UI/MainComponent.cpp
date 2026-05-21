#include "MainComponent.h"
#include "../Theme/BrandColors.h"

using namespace zynforge;

MainComponent::MainComponent()
{
    setLookAndFeel (&laf);

    titleLabel.setFont (juce::Font (juce::FontOptions().withHeight (16.0f).withStyle ("Bold")));
    titleLabel.setColour (juce::Label::textColourId, brand::textPrimary);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    statusLabel.setFont (juce::Font (juce::FontOptions().withHeight (13.0f)));
    statusLabel.setColour (juce::Label::textColourId, brand::textMuted);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    recordButton.setColour (juce::TextButton::buttonColourId, brand::accentRecord.withAlpha (0.18f));
    recordButton.setColour (juce::TextButton::textColourOffId, brand::accentRecord);
    recordButton.onClick = [this] { onRecordClicked(); };
    addAndMakeVisible (recordButton);

    deviceButton.onClick = [this] { onDeviceClicked(); };
    addAndMakeVisible (deviceButton);

    startTimerHz (10);  // poll for input-channel count changes
    rebuildStrips();
}

MainComponent::~MainComponent()
{
    setLookAndFeel (nullptr);
}

void MainComponent::rebuildStrips()
{
    auto& recorder = engine.getRecorder();
    const int n = recorder.getNumTracks();

    strips.clear();
    strips.reserve ((std::size_t) n);
    for (int i = 0; i < n; ++i)
    {
        auto s = std::make_unique<ChannelStrip> (i, recorder.getTrack (i));
        addAndMakeVisible (*s);
        strips.push_back (std::move (s));
    }
    lastTrackCount = n;
    resized();
}

void MainComponent::timerCallback()
{
    const int n = engine.getRecorder().getNumTracks();
    if (n != lastTrackCount)
        rebuildStrips();
}

void MainComponent::onRecordClicked()
{
    if (engine.isRecording())
    {
        engine.stopRecording();
        statusLabel.setText ("Idle", juce::dontSendNotification);
        recordButton.setButtonText ("RECORD");
    }
    else
    {
        const auto dir = makeNewSessionDir();
        if (engine.startRecording (dir))
        {
            statusLabel.setText ("Recording → " + dir.getFileName(), juce::dontSendNotification);
            recordButton.setButtonText ("STOP");
        }
        else
        {
            statusLabel.setText ("Failed to start recording", juce::dontSendNotification);
        }
    }
}

void MainComponent::onDeviceClicked()
{
    auto* panel = new juce::AudioDeviceSelectorComponent (engine.getDeviceManager(),
                                                          0, 64, 0, 64,
                                                          false, false, true, false);
    panel->setSize (560, 480);

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle                  = "Audio Device";
    opts.content.setOwned (panel);
    opts.componentToCentreAround      = this;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar            = true;
    opts.resizable                    = true;
    opts.launchAsync();
}

juce::File MainComponent::makeNewSessionDir() const
{
    auto root = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                    .getChildFile ("Zynforge Sessions");
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    return root.getChildFile ("Session_" + stamp);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (brand::bgDeep);

    auto header = getLocalBounds().removeFromTop (48).toFloat();
    g.setColour (brand::bgPanel);
    g.fillRect (header);
    g.setColour (brand::edge);
    g.drawHorizontalLine ((int) header.getBottom() - 1,
                          header.getX(), header.getRight());
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (48).reduced (12, 8);

    titleLabel  .setBounds (header.removeFromLeft (260));
    recordButton.setBounds (header.removeFromRight (110).reduced (0, 2));
    header.removeFromRight (8);
    deviceButton.setBounds (header.removeFromRight (130).reduced (0, 2));
    statusLabel .setBounds (header);

    if (strips.empty()) return;

    const int gap = 6;
    const int margin = 12;
    auto strip = r.reduced (margin);
    const int total = (int) strips.size();
    const int w = juce::jmax (60, (strip.getWidth() - (total - 1) * gap) / total);

    for (int i = 0; i < total; ++i)
    {
        strips[(std::size_t) i]->setBounds (strip.removeFromLeft (w));
        if (i < total - 1) strip.removeFromLeft (gap);
    }
}
