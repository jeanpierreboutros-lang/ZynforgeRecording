#include "SessionMirror.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/EngineStatus.h"
#include <juce_core/juce_core.h>

namespace zynforge
{
    SessionMirror::SessionMirror (AudioEngine& e) : engine (e) {}
    SessionMirror::~SessionMirror() { stop(); }

    void SessionMirror::start (const juce::String& primaryHost, int p, int periodMs)
    {
        host = primaryHost;
        port = p;
        if (host.isEmpty()) { stop(); return; }
        startTimer (periodMs);
    }

    void SessionMirror::stop()
    {
        stopTimer();
        // Signal the read loop to bail, then wait for the background thread so
        // nothing outlives this object (the thread + the message thread are the
        // only writers, so this join is race-free).
        abortFetch.store (true);
        if (fetchThread.joinable()) fetchThread.join();
        fetchActive.store (false);
        resultReady.store (false);
    }

    void SessionMirror::timerCallback()
    {
        if (host.isEmpty()) return;

        // 1. Apply any completed fetch. Parsing + applyState (which mutates the
        //    engine) stay on the message thread -- the background thread only
        //    hands us a string. Never blocks the UI.
        if (resultReady.exchange (false))
        {
            juce::String payload, err;
            {
                std::lock_guard<std::mutex> g (resultLock);
                payload = resultPayload;
                err     = resultError;
            }
            if (err.isNotEmpty())
            {
                lastError = err;
            }
            else
            {
                const auto v = juce::JSON::parse (payload);
                if (! v.isObject()) { lastError = "bad JSON payload"; }
                else
                {
                    applyState (v);
                    lastSyncMs = juce::Time::currentTimeMillis();
                    lastError.clear();
                }
            }
        }

        // 2. Kick a new background fetch if none is in flight. Reap the prior
        //    (finished) thread first. The 500 ms poll interval is preserved --
        //    a fetch that overruns simply skips the tick it would collide with.
        if (! fetchActive.load())
        {
            if (fetchThread.joinable()) fetchThread.join();
            const auto h = host;
            const int  p = port;
            abortFetch.store (false);
            fetchActive.store (true);
            fetchThread = std::thread ([this, h, p]
            {
                fetchOnce (h, p);
                fetchActive.store (false);
            });
        }
    }

    void SessionMirror::fetchOnce (juce::String h, int p)
    {
        const auto url = juce::URL ("http://" + h + ":" + juce::String (p) + "/state.json");

        // Connection timeout is 300 ms; the read loop below adds a hard byte
        // ceiling + a total-time budget so a slow/rogue host streaming forever
        // can neither freeze us nor exhaust memory (finding #5).
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                          .withConnectionTimeoutMs (300);
        std::unique_ptr<juce::InputStream> stream (url.createInputStream (options));

        juce::String payload, err;
        if (stream == nullptr)
        {
            err = "no response from " + h + ":" + juce::String (p);
        }
        else
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) kFetchBudgetMs;
            juce::MemoryBlock mb;
            char buf[4096];
            while (! stream->isExhausted()
                   && (int) mb.getSize() < kMaxPayloadBytes
                   && juce::Time::getMillisecondCounter() < deadline
                   && ! abortFetch.load())
            {
                const int n = stream->read (buf, (int) sizeof (buf));
                if (n <= 0) break;
                mb.append (buf, (std::size_t) n);
            }
            payload = juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize());
        }

        std::lock_guard<std::mutex> g (resultLock);
        resultPayload = payload;
        resultError   = err;
        resultReady.store (true);
    }

    void SessionMirror::applyState (const juce::var& v)
    {
        if (v.getDynamicObject() == nullptr) return;

        // Parse the EXACT schema the primary's CompanionServer emits
        // (EngineStatus::toJson): a "tracks" array whose colours are "#RRGGBB"
        // strings. The old code read a "channels" array + an int "colour" + a
        // "bpm" field that state.json never emits, so getProperty returned void
        // and the mirror applied NOTHING end to end. Reuse the canonical parser.
        const auto st = EngineStatus::fromJson (v);
        auto& rec = engine.getRecorder();
        const int n = juce::jmin ((int) st.tracks.size(), rec.getNumTracks());
        for (int i = 0; i < n; ++i)
        {
            const auto& t   = st.tracks[(std::size_t) i];
            auto&       cur = rec.getTrack (i);
            // Change-detect BEFORE writing. setTrackName/setTrackColour each do
            // a synchronous shared-.settings reload+parse on the message thread;
            // applying every track every 500 ms poll thrashed that file (~2N
            // reads/sec) for the whole show even when nothing changed.
            if (t.name.isNotEmpty() && cur.getNameThreadSafe() != t.name)
                engine.setTrackName (i, t.name);
            if ((t.colourARGB & 0x00ffffffu) != 0
                && cur.colourARGB.load (std::memory_order_relaxed) != t.colourARGB)
                engine.setTrackColour (i, juce::Colour (t.colourARGB));
        }
    }
}
