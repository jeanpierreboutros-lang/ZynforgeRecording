#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace zynforge
{
    struct Marker
    {
        juce::int64  sampleOffset { 0 };
        juce::String name;
        juce::String type { "user" };   // "user", "scene", "song", …
    };

    // Per-session marker list. Persisted as markers.json in the session dir.
    // Mutated only from the message thread; read by UI thread.
    class MarkersManager
    {
    public:
        // Set the active session. Tries to load markers.json from the
        // directory; clears the in-memory list if absent.
        void setContext (const juce::File& sessionDir, double sampleRate);
        void clearContext();

        bool hasContext() const noexcept       { return sessionDir.isDirectory(); }
        juce::File getSessionDir() const       { return sessionDir; }
        double     getSampleRate() const       { return sampleRate; }

        void  drop (juce::int64 sampleOffset, const juce::String& name = {});
        void  clear();
        int   getCount() const                 { return (int) markers.size(); }
        Marker getLast() const;

        bool save() const;
        bool load();

    private:
        juce::File   sessionDir;
        double       sampleRate { 48000.0 };
        std::vector<Marker> markers;
    };
}
