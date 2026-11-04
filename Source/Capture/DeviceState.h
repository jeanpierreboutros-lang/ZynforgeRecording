#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

namespace zynforge::capture
{
    // The device this process is running now, in the form
    // AudioDeviceManager::initialise reads. createStateXml cannot stand in: it is
    // null until a setup was applied as a chosen device, and after a device
    // drops it goes on naming the last choice while the manager runs whatever it
    // fell back to.
    inline std::unique_ptr<juce::XmlElement> liveDeviceState (const juce::AudioDeviceManager& manager,
                                                              juce::AudioIODevice& device)
    {
        const auto setup = manager.getAudioDeviceSetup();
        auto state = std::make_unique<juce::XmlElement> ("DEVICESETUP");
        state->setAttribute ("deviceType", manager.getCurrentAudioDeviceType());
        state->setAttribute ("audioOutputDeviceName", setup.outputDeviceName);
        state->setAttribute ("audioInputDeviceName", setup.inputDeviceName);
        state->setAttribute ("audioDeviceRate", device.getCurrentSampleRate());
        state->setAttribute ("audioDeviceBufferSize", device.getCurrentBufferSizeSamples());
        if (! setup.useDefaultInputChannels)
            state->setAttribute ("audioDeviceInChans", setup.inputChannels.toString (2));
        if (! setup.useDefaultOutputChannels)
            state->setAttribute ("audioDeviceOutChans", setup.outputChannels.toString (2));
        return state;
    }

    // The same setup with its outputs taken out. The helper records and never
    // plays, so the window's monitor output is nothing to it. Named, it would
    // be the device the helper holds (CoreAudio names an input/output pair
    // after the output), and that output going away (headphones pulled, a
    // display asleep) would close the helper's device and end its take.
    inline void keepInputsOnly (juce::XmlElement& state)
    {
        // A single name means both directions: keep it as the input alone.
        if (state.hasAttribute ("audioDeviceName"))
        {
            if (! state.hasAttribute ("audioInputDeviceName"))
                state.setAttribute ("audioInputDeviceName", state.getStringAttribute ("audioDeviceName"));
            state.removeAttribute ("audioDeviceName");
        }
        state.removeAttribute ("audioOutputDeviceName");
        state.removeAttribute ("audioDeviceOutChans");
    }
}
