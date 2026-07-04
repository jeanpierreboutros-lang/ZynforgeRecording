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
    std::atomic<bool> shouldQuit  { false };
    std::atomic<int>  signalCount { 0 };
    void onSignal (int) { shouldQuit.store (true); signalCount.fetch_add (1); }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // message manager + basics, no window

    int  port     = 17890;
    int  inputs   = 32;
    bool noDevice = false;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--port"   && i + 1 < argc) port   = juce::String (argv[++i]).getIntValue();
        if (a == "--inputs" && i + 1 < argc) inputs = juce::String (argv[++i]).getIntValue();
        if (a == "--no-device") noDevice = true;   // tests / CI: no audio hardware
    }

    // A dropped GUI socket must fail the write, not kill the take.
    std::signal (SIGPIPE, SIG_IGN);
    std::signal (SIGTERM, onSignal);
    std::signal (SIGINT,  onSignal);

    zynforge::capture::CaptureDaemon daemon;
    daemon.onQuitRequest = [] { shouldQuit.store (true); };   // wire Quit command
    if (noDevice) daemon.setTestModeNoDevice (true);
    if (! daemon.start (port, juce::jlimit (1, 256, inputs)))
    {
        juce::Logger::writeToLog ("[capture] failed to start (port " + juce::String (port) + ")");
        return 1;
    }
    if (noDevice)
        daemon.prepareForTests (48000.0, 256, juce::jlimit (1, 256, inputs));
    juce::Logger::writeToLog ("[capture] listening on 127.0.0.1:" + juce::String (daemon.getPort())
                              + ", " + juce::String (inputs) + " inputs");

    while (! shouldQuit.load())
        juce::Thread::sleep (100);

    // Take-safety: a SIGTERM/SIGINT must not truncate a rolling take, matching
    // the daemon's "Quit refused mid-take" rule. Wait for the take to finish
    // (a StopRecording from the GUI) before tearing down. A Quit command only
    // ever sets the flag while idle, so it exits this wait immediately. A
    // SECOND signal (operator insists) forces the stop -- which still finalises
    // the take cleanly (files + manifest), so it is never data loss.
    while (daemon.isRecording() && signalCount.load() < 2)
        juce::Thread::sleep (100);

    // Clean stop: closes the take (files + manifest) before comms/device.
    daemon.stop();
    juce::Logger::writeToLog ("[capture] stopped cleanly");
    return 0;
}
