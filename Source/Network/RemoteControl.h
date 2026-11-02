#pragma once

// Remote control of the engine from outside the app: the console OSC
// listener (and its debug log + channel-name capture), the desk macro
// listener, and a MIDI control surface.
//
// AudioEngine owns ONE RemoteControl by value, declared where these members
// used to sit, so every remote's lifetime stays nested inside the engine's:
// each one holds AudioEngine& and calls back into it from its own thread.
// The audio thread touches nothing here. Remote TRANSPORT still enters through AudioEngine::performRemoteTransport,
// the one host interception boundary.

#include <juce_core/juce_core.h>

#include "../Audio/SceneFollow.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>

namespace zynforge
{
    class AudioEngine;
    class OscRemote;
    class DeskMacroListener;
    class MidiControlSurface;

    class RemoteControl final
    {
    public:
        explicit RemoteControl (AudioEngine& engine);
        ~RemoteControl();

        // What only the host knows or can show. All message thread; all
        // optional, so a headless engine still runs every rule below.
        struct Host
        {
            std::function<void (const juce::String&)> showStatus;
            std::function<bool()>                     isSessionBusy;     // sessionIoBusy
            std::function<void (juce::int64)>         revealPosition;    // scroll EDIT to a locate
        };
        void setHost (Host h) { host = std::move (h); }
        void showHostStatus (const juce::String& message) { if (host.showStatus) host.showStatus (message); }
        // A wheel sends thirty of these a second; one notice a second says it.
        void noteJogHeld();
        bool isSessionBusy() const;

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
        // against their actual console. When on, every console path logs here,
        // in and out. Off, nothing is stored; what was captured stays until
        // Clear, so it can still be saved.
        static constexpr int kOscLogMaxLines = 50000;
        static constexpr int kOscLogMaxChars = 400;
        void              setOscDebug (bool on) noexcept { oscDebug.store (on, std::memory_order_relaxed); }
        bool              isOscDebug() const noexcept    { return oscDebug.load (std::memory_order_relaxed); }
        void              logOsc (const juce::String& line);   // message thread
        juce::StringArray getOscLog() const;
        juce::StringArray getOscLogTail (int maxLines) const;
        // Bumped by every change, so a view can skip an unchanged log.
        juce::int64       getOscLogRevision() const;
        // Oldest lines the cap has discarded since the last clear.
        juce::int64       getOscLogEvicted() const;
        void              clearOscLog();
        // The whole log with a header saying what was listening, ready for a
        // file. `consoleLinkSummary` is the host's line about the console link.
        juce::String      formatConsoleLog (const juce::String& consoleLinkSummary) const;

        // Channel-name capture: while on, every console channel name we receive
        // is collected (1-based index -> name) AND applied to a matching track.
        // Used by "Create session from console". Enabling clears the buffer.
        void setConsoleNameCapture (bool on);
        // Called by OscRemote for every incoming console channel name.
        void onConsoleChannelName (int oneBasedIndex, const juce::String& name);
        // Take + clear the captured names (message thread).
        std::map<int, juce::String> takeCapturedConsoleNames();
        // A desk's stereo link state, kept only during that same capture: a
        // link changing on the desk later must never restructure a session.
        void onConsoleStereoLink (int leftCh1, bool linked);
        std::map<int, bool> takeCapturedStereoLinks();

        // A marker asked for over the network, held to scenefollow::MarkerBudget.
        // What dropMarkerAtCurrentPosition returns, or -1 when refused.
        int dropRemoteMarker (const juce::String& name = {}, const juce::String& type = "user");

        // Both console paths report a scene / snapshot recall here. The rule
        // is scenefollow::decide; this applies it and tells the engineer.
        scenefollow::Action onConsoleScene (int sceneNumber, const juce::String& sceneName,
                                            scenefollow::Path path);
        // Move the playhead without touching the transport state.
        void locate (juce::int64 samplePosition);

        // ── Desk macro listener (DeskMacro.h) ───────────────────────────
        // Off until the engineer starts it, on every launch. Refused under
        // LOCK, and without one usable desk address.
        bool         startDeskMacro (int udpPort, const juce::String& deskIp);
        void         stopDeskMacro();
        bool         isDeskMacroListening() const;
        int          getDeskMacroPort() const;
        juce::String getDeskMacroSource() const;
        juce::int64  getDeskMacroDropped() const;
        juce::String getDeskMacroLastDropped() const;
        juce::String deskLocateRefusal() const;
        // The same question once the target is known.
        juce::String deskLocateRefusal (juce::int64 target) const;

        // ── MIDI control surface (Mackie Control / FaderPort) ───────────
        // Open a control surface on the given MIDI in/out device names. `key`
        // identifies the active brand (one surface at a time). Returns success.
        bool         enableControlSurface (const juce::String& key, const juce::String& inName,
                                           const juce::String& outName);
        void         disableControlSurface();
        juce::String getActiveSurfaceKey() const { return activeSurfaceKey; }

    private:
        scenefollow::RecallEchoFilter recallEchoes;
        scenefollow::MarkerBudget     markerBudget;
        juce::int64                   markersRefused { 0 };
        juce::uint32                  lastMarkerNoticeMs { 0 };
        juce::uint32                  lastJogNoticeMs { 0 };
        scenefollow::Facts sceneFacts() const;

        AudioEngine& engine;
        Host         host;

        // Declaration order is teardown order, reversed: the remotes go
        // first (OSC, then the macro listener and the surface),
        // while the log and capture state they write to are still alive.
        std::atomic<bool>           consoleCapture { false };
        std::map<int, juce::String> capturedConsoleNames;   // message-thread only
        std::map<int, bool>         capturedStereoLinks;    // message-thread only
        std::atomic<bool>           oscDebug { false };
        juce::CriticalSection       oscLogLock;
        // A deque: at the cap every new line evicts one, which a StringArray
        // would pay for by shifting every line on the message thread.
        std::deque<juce::String>    oscLog;
        juce::int64                 oscLogRevision { 0 };
        juce::int64                 oscLogEvicted { 0 };
        juce::String                activeSurfaceKey;

        std::unique_ptr<MidiControlSurface> surface;
        std::unique_ptr<DeskMacroListener>  deskMacro;
        std::unique_ptr<OscRemote>          osc;

        JUCE_DECLARE_NON_COPYABLE (RemoteControl)
    };
}
