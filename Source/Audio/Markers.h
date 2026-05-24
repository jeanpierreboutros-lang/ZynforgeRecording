#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace zynforge
{
    struct Marker
    {
        juce::int64  sampleOffset { 0 };
        juce::String name;
        juce::String type { "user" };   // "user", "scene", "song", ...

        // Pro Tools-style "Memory Location" recall fields. When set,
        // a Cmd+N jump to this marker also restores the EDIT-view
        // zoom level and the logical-strip visibility mask. Absent
        // (zoom <= 0) = jump position only, leave layout alone.
        // visibleTracks is a list of logical strip indices that
        // should be shown; empty + zoom > 0 means "show all".
        float                     zoom         { -1.0f };
        std::vector<int>          visibleTracks;
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

        const std::vector<Marker>& getAll() const noexcept { return markers; }
        Marker getMarker (int i) const;
        void   removeMarker (int i);
        void   renameMarker (int i, const juce::String& newName);
        void   setMarkerSample (int i, juce::int64 sample);

        bool save() const;
        bool load();

    private:
        juce::File   sessionDir;
        double       sampleRate { 48000.0 };
        std::vector<Marker> markers;
    };
}
