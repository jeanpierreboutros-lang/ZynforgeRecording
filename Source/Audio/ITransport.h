#pragma once

#include <juce_core/juce_core.h>

namespace zynforge
{
    // Narrow transport contract carved out of the AudioEngine hub.
    //
    // AudioEngine exposes ~255 methods; a consumer that only needs to
    // start/stop playback or recording (a transport bar, a remote /cmd
    // handler, an OSC dialect) should be able to depend on this six-method
    // contract instead of the whole engine. AudioEngine implements it, so
    // an `ITransport&` is a drop-in for those call sites and the seam can
    // be mocked in tests.
    //
    // This is the first extracted facet of the engine's interface-
    // segregation pass; further facets (clip editing, routing, automation)
    // follow the same shape. Consumers are migrated incrementally -- this
    // header introduces the contract without changing any existing caller.
    struct ITransport
    {
        virtual ~ITransport() = default;

        virtual void startPlayback() = 0;
        virtual void stopPlayback()  = 0;
        virtual bool startRecording (const juce::File& sessionDir) = 0;
        virtual void stopRecording() = 0;
        virtual bool isPlaying()   const = 0;
        virtual bool isRecording() const = 0;
    };
}
