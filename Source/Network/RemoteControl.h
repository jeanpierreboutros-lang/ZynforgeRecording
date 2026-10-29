#pragma once

// Remote control of the engine from outside the app: the companion HTTP
// server, the console OSC listener (and its debug log + channel-name
// capture), and a MIDI control surface.
//
// AudioEngine owns ONE RemoteControl by value, declared where these members
// used to sit, so every remote's lifetime stays nested inside the engine's:
// each one holds AudioEngine& and calls back into it from its own thread.
// The audio thread touches exactly one thing here -- feedCompanionStream.
// Remote TRANSPORT still enters through AudioEngine::performRemoteTransport,
// the one host interception boundary.

#include <juce_core/juce_core.h>

#include <atomic>
#include <map>
#include <memory>

namespace zynforge
{
    class AudioEngine;
    class OscRemote;
    class MidiControlSurface;
    class CompanionServer;

    class RemoteControl final
    {
    public:
        explicit RemoteControl (AudioEngine& engine);
        ~RemoteControl();

        // ── Companion HTTP server (read-only iPad / remote audition) ────
        // Binds to loopback; the engineer opts into LAN exposure explicitly.
        bool         startCompanionServer (int port);
        bool         startCompanionServerOnLan (int port);
        juce::String getCompanionAccessUrl() const;
        bool         isCompanionExposedOnLan() const;
        void         stopCompanionServer();
        bool         isCompanionServerRunning() const noexcept;
        int          getCompanionServerPort() const noexcept;
        // Audio thread: hand the monitor pair to a running server's stream.
        void         feedCompanionStream (const float* L, const float* R, int numSamples) noexcept;

        // ── OSC remote ──────────────────────────────────────────────────
        // A depth-capped OscPacketReceiver bound to a UDP port, with a
        // dialect parser for DiGiCo / A&H / SSL / Yamaha consoles plus a
        // generic /zynforge/* schema for tablet apps.
        bool         startOsc (int udpPort, int dialectIndex);
        void         stopOsc();
        bool         isOscListening() const;
        int          getOscPort() const;
        juce::String getOscAccessToken() const;
        int          getOscDialect() const;

        // ── Outbound console control (DiGiCo bidirectional link) ────────
        // Ask the console (at host:port) to report all channel names. The
        // replies arrive on the normal OSC receive path. Returns false if the
        // sender couldn't connect.
        bool requestConsoleChannelNames (const juce::String& host, int port, int dialect = 1);

        // OSC debug log -- so an engineer can verify the request/reply traffic
        // against their actual console. When on, sent + received OSC is logged.
        void              setOscDebug (bool on) noexcept { oscDebug.store (on, std::memory_order_relaxed); }
        bool              isOscDebug() const noexcept    { return oscDebug.load (std::memory_order_relaxed); }
        void              logOsc (const juce::String& line);   // thread-safe; keeps the last ~200 lines
        juce::StringArray getOscLog() const;
        void              clearOscLog();

        // Channel-name capture: while on, every console channel name we receive
        // is collected (1-based index -> name) AND applied to a matching track.
        // Used by "Create session from console". Enabling clears the buffer.
        void setConsoleNameCapture (bool on);
        bool isConsoleNameCapture() const noexcept { return consoleCapture.load (std::memory_order_relaxed); }
        // Called by OscRemote for every incoming console channel name.
        void onConsoleChannelName (int oneBasedIndex, const juce::String& name);
        // Take + clear the captured names (message thread).
        std::map<int, juce::String> takeCapturedConsoleNames();

        // ── MIDI control surface (Mackie Control / FaderPort) ───────────
        // Open a control surface on the given MIDI in/out device names. `key`
        // identifies the active brand (one surface at a time). Returns success.
        bool         enableControlSurface (const juce::String& key, const juce::String& inName,
                                           const juce::String& outName);
        void         disableControlSurface();
        juce::String getActiveSurfaceKey() const { return activeSurfaceKey; }

    private:
        AudioEngine& engine;

        // Declaration order is teardown order, reversed: the remotes go
        // first (companion, then OSC, then the surface), while the log and
        // capture state they write to are still alive.
        std::atomic<bool>           consoleCapture { false };
        std::map<int, juce::String> capturedConsoleNames;   // message-thread only
        std::atomic<bool>           oscDebug { false };
        juce::CriticalSection       oscLogLock;
        juce::StringArray           oscLog;
        juce::String                activeSurfaceKey;

        std::unique_ptr<MidiControlSurface> surface;
        std::unique_ptr<OscRemote>          osc;
        std::unique_ptr<CompanionServer>    companion;

        JUCE_DECLARE_NON_COPYABLE (RemoteControl)
    };
}
