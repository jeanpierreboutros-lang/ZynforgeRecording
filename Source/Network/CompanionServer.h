#pragma once

#include <juce_core/juce_core.h>
#include "../Audio/AudioEngine.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zynforge
{
    // Minimal HTTP server that powers the iPad / phone companion AND
    // the remote-audition WAV stream. One accept thread + one worker
    // per connection. Three endpoint families:
    //
    //   GET  /            → companion HTML / JS bundle (single page)
    //   GET  /state.json  → current channel + transport state
    //   POST /cmd         → action JSON: {action, channel?, value?}
    //   GET  /stream.wav  → infinite stereo 48k 16-bit PCM stream
    //
    // No WebSocket yet -- the companion polls /state.json every 500 ms.
    // Cheap, dead-simple, and works through every browser.
    class CompanionServer
    {
    public:
        explicit CompanionServer (AudioEngine& eng);
        ~CompanionServer();

        // Starts the accept thread on the given port. false if bind fails.
        // bindAddress defaults to loopback so an unauthenticated open-by-
        // accident never reaches the LAN; pass "0.0.0.0" explicitly when
        // the engineer needs phone-on-Wi-Fi access. A fresh access token
        // is generated on every start; every endpoint requires it via
        // ?t=<token> or `Authorization: Bearer <token>`.
        bool start (int port, const juce::String& bindAddress = "127.0.0.1");
        void stop();

        bool isRunning() const noexcept { return running.load(); }
        int  getPort()   const noexcept { return listenPort.load(); }

        // The currently-active access token (empty when the server is
        // stopped). Surface this to the engineer so they can paste the
        // full URL into a phone -- the token is the only thing keeping
        // someone else on the same Wi-Fi from arming tracks.
        juce::String getAccessToken() const;

        // Full URL the engineer should hand to their phone, including
        // the token. Returns empty when stopped.
        juce::String getAccessUrl() const;

        // True when the server bound to a non-loopback address (i.e.
        // is reachable from the LAN). Lets the host UI flag that the
        // companion is exposed beyond this machine.
        bool isExposedOnLan() const;

        // Audio thread feed: pushes PCM samples for /stream.wav. Non-
        // blocking; drops samples if no client is listening.
        void feedStreamSamples (const float* L, const float* R, int n) noexcept;

    private:
        AudioEngine&     engine;
        std::atomic<bool> running     { false };
        std::atomic<int>  listenPort  { -1 };
        juce::String      bind        { "127.0.0.1" };
        juce::String      accessToken;
        mutable std::mutex tokenLock;

        std::unique_ptr<juce::StreamingSocket> listener;
        std::thread acceptThread;

        // Ring buffer for the streaming WAV endpoint. Single producer
        // (audio thread), single consumer (per-stream worker thread).
        struct StreamRing
        {
            std::vector<float> dataL;
            std::vector<float> dataR;
            std::atomic<std::size_t> writeIdx { 0 };
            std::atomic<bool>        active   { false };

            void allocate (std::size_t cap) { dataL.assign (cap, 0.0f); dataR.assign (cap, 0.0f); }
            std::size_t capacity() const    { return dataL.size(); }
        };
        StreamRing streamRing;

        void acceptLoop();
        void handleClient (std::unique_ptr<juce::StreamingSocket>);
        void writeStreamWav (juce::StreamingSocket&);
        void writeStateJson (juce::StreamingSocket&);
        void handleCommand  (juce::StreamingSocket&, const juce::String& body);
        void writeHtmlPage  (juce::StreamingSocket&);
        void writeRaw (juce::StreamingSocket&, const juce::String& status,
                       const juce::String& contentType,
                       const juce::String& body);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompanionServer)
    };
}
