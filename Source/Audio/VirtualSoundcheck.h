#pragma once

#include "AudioEngine.h"

// Virtual Soundcheck: each strip's output moves onto its input's channel, so
// playback reaches the desk on the channels the live inputs use. The output
// patch it replaces is kept on the strips and saved with the session, because
// the way back has to outlive the undo history: a soundcheck in the afternoon
// and a show at night can be a quit and a reopen apart.
namespace zynforge::vsc
{
    inline bool hasSavedPatch (AudioEngine& engine)
    {
        auto& recorder = engine.getRecorder();
        for (int i = 0; i < recorder.getNumTracks(); ++i)
            if (recorder.getTrack (i).outputBeforeVsc.load (std::memory_order_relaxed) != TrackState::kNoSavedOutput)
                return true;
        return false;
    }

    // Returns the number of strips repatched. Pressed again it does nothing,
    // or the second press would save the soundcheck patch as the show patch.
    inline int repatch (AudioEngine& engine)
    {
        if (hasSavedPatch (engine)) return 0;
        auto& recorder = engine.getRecorder();
        const int strips = recorder.getNumTracks();
        for (int i = 0; i < strips; ++i)
        {
            auto& track = recorder.getTrack (i);
            track.outputBeforeVsc.store (track.outputRouting.load (std::memory_order_relaxed),
                                         std::memory_order_relaxed);
            // -2 means the identity default, so the output lands on the strip's
            // own channel.
            const int input = track.inputRouting.load (std::memory_order_relaxed);
            engine.setTrackOutputRouting (i, input == -2 ? i : input);
        }
        return strips;
    }

    // Returns the number of strips put back. A strip added after the soundcheck
    // has nothing saved and keeps what it has.
    inline int restore (AudioEngine& engine)
    {
        auto& recorder = engine.getRecorder();
        int restored = 0;
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& track = recorder.getTrack (i);
            const int saved = track.outputBeforeVsc.exchange (TrackState::kNoSavedOutput, std::memory_order_relaxed);
            if (saved == TrackState::kNoSavedOutput) continue;
            engine.setTrackOutputRouting (i, saved);
            ++restored;
        }
        return restored;
    }
}
