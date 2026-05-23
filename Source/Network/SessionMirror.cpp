#include "SessionMirror.h"
#include "../Audio/AudioEngine.h"
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

    void SessionMirror::stop() { stopTimer(); }

    void SessionMirror::timerCallback()
    {
        if (host.isEmpty()) return;

        const auto url = juce::URL ("http://" + host + ":" + juce::String (port) + "/state.json");

        // Quick non-blocking GET. CompanionServer responds tiny -- <2 KB.
        auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                          .withConnectionTimeoutMs (300);
        std::unique_ptr<juce::InputStream> stream (url.createInputStream (options));
        if (stream == nullptr)
        {
            lastError = "no response from " + host + ":" + juce::String (port);
            return;
        }
        const auto json = stream->readEntireStreamAsString();
        const auto v    = juce::JSON::parse (json);
        if (! v.isObject()) { lastError = "bad JSON payload"; return; }

        applyState (v);
        lastSyncMs = juce::Time::currentTimeMillis();
        lastError.clear();
    }

    void SessionMirror::applyState (const juce::var& v)
    {
        auto* obj = v.getDynamicObject();
        if (obj == nullptr) return;

        // Tempo + transport indicators (read-only -- the mirror UI shows
        // what the primary is doing but doesn't drive its own audio).
        if (obj->hasProperty ("bpm"))
            engine.setSessionTempoBpm ((float) (double) obj->getProperty ("bpm"));

        // Per-channel name / colour mirror. The primary's CompanionServer
        // already includes a 'channels' array in its state.json.
        const auto chArr = obj->getProperty ("channels");
        if (auto* arr = chArr.getArray())
        {
            for (int i = 0; i < arr->size() && i < engine.getRecorder().getNumTracks(); ++i)
            {
                auto* ch = (*arr)[i].getDynamicObject();
                if (ch == nullptr) continue;
                const auto name   = ch->getProperty ("name").toString();
                const auto colour = (int) ch->getProperty ("colour");
                if (name.isNotEmpty()) engine.setTrackName (i, name);
                if (colour != 0)       engine.setTrackColour (i, juce::Colour ((juce::uint32) colour));
            }
        }
    }
}
