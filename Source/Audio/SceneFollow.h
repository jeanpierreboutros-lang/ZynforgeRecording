#pragma once

// What a console scene / snapshot recall does. Both ways a recall reaches the
// app (ConsoleLink and the OSC listener) arrive at RemoteControl::onConsoleScene,
// which gathers the facts and applies this rule, so they cannot drift apart.
//
// A recall during a take marks the take. Anywhere else it must not leave a
// marker behind: rehearsing against a recorded show fires the same snapshots
// again, and the session would fill with strays at wherever the playhead was.
// Follow, off by default, instead moves the playhead to the song the snapshot
// belongs to. A jump the engineer didn't ask for in the middle of rehearsing
// moves is worse than no jump, which is why it is opt-in and refuses so readily.

#include "Markers.h"

#include <vector>

namespace zynforge::scenefollow
{
    enum class Action : int
    {
        DropMarker,   // a take is rolling
        Locate,       // Follow on, and a marker carries the scene's name
        NoMatch,      // Follow on, and no marker does
        Refused,      // Follow on, but the playhead may not move right now
        Nothing       // no take, Follow off
    };

    struct Facts
    {
        bool capturing    { false };   // AudioEngine::isRecording(): local or daemon
        bool followOn     { false };
        bool locked       { false };
        bool seekLocked   { false };   // a punch holds the playhead
        bool sessionBusy  { false };   // a session job or capture finalization
        bool playerLoaded { false };   // nothing to move otherwise
    };

    struct Decision
    {
        Action       action { Action::Nothing };
        int          markerIndex { -1 };   // Locate only
        juce::String refusal;              // Refused only
    };

    // The name a recall goes by, as a marker and as what Follow looks for. Some
    // desks send only a number, and a marker dropped during the take took this
    // same label, so a number still finds its marker.
    inline juce::String sceneLabel (int sceneNumber, const juce::String& sceneName)
    {
        const auto name = sceneName.trim();
        return name.isNotEmpty() ? name : "Scene " + juce::String (sceneNumber);
    }

    // `markers` is ascending (MarkersManager keeps it so). A scene fired more
    // than once in a show has several markers: take the next one ahead of the
    // playhead so the rehearsal walks forward through the set, else the first.
    inline int findMarker (const std::vector<Marker>& markers, const juce::String& label, juce::int64 playhead)
    {
        const auto wanted = label.trim();
        int first = -1;
        for (int i = 0; i < (int) markers.size(); ++i)
        {
            if (! markers[(size_t) i].name.trim().equalsIgnoreCase (wanted)) continue;
            if (markers[(size_t) i].sampleOffset > playhead) return i;
            if (first < 0) first = i;
        }
        return first;
    }

    // Empty when the playhead may move. The desk macro's marker commands use
    // this too, so a locate has one list of refusals.
    inline juce::String locateRefusal (const Facts& f)
    {
        if (f.locked)        return "session is locked";
        if (f.seekLocked)    return "a punch holds the playhead";
        if (f.sessionBusy)   return "a session operation is running";
        if (! f.playerLoaded) return "no audio is loaded";
        return {};
    }

    inline Decision decide (const Facts& f, const std::vector<Marker>& markers,
                            const juce::String& label, juce::int64 playhead)
    {
        if (f.capturing)  return { Action::DropMarker };
        if (! f.followOn) return { Action::Nothing };
        if (const auto why = locateRefusal (f); why.isNotEmpty())
            return { Action::Refused, -1, why };
        const int index = findMarker (markers, label, playhead);
        if (index < 0) return { Action::NoMatch };
        return { Action::Locate, index };
    }

    // Marker navigation for a desk macro.
    inline int nextMarker (const std::vector<Marker>& markers, juce::int64 playhead)
    {
        for (int i = 0; i < (int) markers.size(); ++i)
            if (markers[(size_t) i].sampleOffset > playhead) return i;
        return -1;
    }

    // `grace`: a rolling playhead has always just left the marker it was sent
    // to, so without it a second PREV would land on the same marker and the
    // desk could never step back past one while playing.
    inline int previousMarker (const std::vector<Marker>& markers, juce::int64 playhead, juce::int64 grace)
    {
        for (int i = (int) markers.size() - 1; i >= 0; --i)
            if (markers[(size_t) i].sampleOffset < playhead - grace) return i;
        return -1;
    }
}
