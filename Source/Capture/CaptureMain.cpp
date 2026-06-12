// zynforge-capture -- the headless capture daemon (Phase 1c of the
// capture-process split; see decisions.md). Owns the audio device + the
// MultitrackRecorder; the GUI is a CaptureClient over the local-socket
// CaptureProtocol. If the GUI dies, this process records on and writes the
// integrity manifest.
//
//   zynforge-capture [--port N] [--inputs N]
//
// Stops cleanly on SIGTERM / SIGINT (finishes the take first).

#include <juce_events/juce_events.h>

#include "CaptureDaemon.h"

#include <atomic>
#include <csignal>

namespace
{
    std::atomic<bool> shouldQuit { false };
    void onSignal (int) { shouldQuit.store (true); }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // message manager + basics, no window

    int port   = 17890;
    int inputs = 32;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--port"   && i + 1 < argc) port   = juce::String (argv[++i]).getIntValue();
        if (a == "--inputs" && i + 1 < argc) inputs = juce::String (argv[++i]).getIntValue();
    }

    // A dropped GUI socket must fail the write, not kill the take.
    std::signal (SIGPIPE, SIG_IGN);
    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT,  onSignal);

    zynforge::capture::CaptureDaemon daemon;
    if (! daemon.start (port, juce::jlimit (1, 256, inputs)))
    {
        juce::Logger::writeToLog ("[capture] failed to start (port " + juce::String (port) + ")");
        return 1;
    }
    juce::Logger::writeToLog ("[capture] listening on 127.0.0.1:" + juce::String (daemon.getPort())
                              + ", " + juce::String (inputs) + " inputs");

    while (! shouldQuit.load())
        juce::Thread::sleep (100);

    // Clean stop: closes the take (files + manifest) before comms/device.
    daemon.stop();
    juce::Logger::writeToLog ("[capture] stopped cleanly");
    return 0;
}
