#pragma once

#include <functional>

#include <juce_core/juce_core.h>

#include <vector>

namespace zynforge
{
    struct Marker
    {
        juce::int64  sampleOffset { 0 };
        juce::String name;
        // "user" and "song" divide a show into songs. The rest mark a place:
        // "split" (where a clip was cut), "range" (range in/out), "scene".
        juce::String type { "user" };

        // Pro Tools-style "Memory Location" recall fields. When set,
        // a Cmd+N jump to this marker also restores the EDIT-view
        // zoom level and the logical-strip visibility mask. Absent
        // (zoom <= 0) = jump position only, leave layout alone.
        // visibleTracks is a list of logical strip indices that
        // should be shown; empty + zoom > 0 means "show all".
        float                     zoom         { -1.0f };
        std::vector<int>          visibleTracks;
        juce::String runtimeId { juce::Uuid().toString() };
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

        void  drop (juce::int64 sampleOffset, const juce::String& name = {},
                    const juce::String& type = "user");
        void  clear();
        int   getCount() const                 { return (int) markers.size(); }
        Marker getLast() const;
        juce::String lastDroppedId;

        const std::vector<Marker>& getAll() const noexcept { return markers; }
        Marker getMarker (int i) const;
        // Where the marker with this runtimeId sits now, or -1. A list index
        // goes stale across any await: markers stay sorted by position, so one
        // dropped ahead of it shifts every later index.
        int    indexOfId (const juce::String& runtimeId) const;
        void   removeMarker (int i);
        void   renameMarker (int i, const juce::String& newName);
        void   setMarkerSample (int i, juce::int64 sample);
        void   replaceAll (std::vector<Marker> newMarkers);
        void   insert (Marker marker);
        void   removeById (const juce::String& runtimeId);

        bool save() const;
        bool load();
        // A save moved a damaged markers.json aside rather than replace it.
        std::function<void (const juce::File& kept)> onDamagedFileKept;

    private:
        // Keep `markers` ascending by sampleOffset (stable, so equal-position
        // markers preserve insertion order). Consumers — next/previous nav,
        // timeline draw order — assume ascending.
        void sortByPosition();

        juce::File   sessionDir;
        double       sampleRate { 48000.0 };
        std::vector<Marker> markers;
        // Monotonic within a session; drives the default "Marker N" name so a
        // removeMarker() can't make the next auto-name collide with an existing
        // one (which the old markers.size()+1 scheme did).
        int          markerCounter { 0 };
    };
}
