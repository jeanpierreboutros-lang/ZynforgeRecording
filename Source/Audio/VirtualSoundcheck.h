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

    // The output a strip has on the show patch, with or without VSC on. Anything
    // stored for later (a cue, a template, a handoff) wants this, not the live
    // output, or the soundcheck patch gets kept as the show's.
    inline int showOutput (const TrackState& track)
    {
        const int saved = track.outputBeforeVsc.load (std::memory_order_relaxed);
        return saved != TrackState::kNoSavedOutput ? saved : track.outputRouting.load (std::memory_order_relaxed);
    }

    // Where a strip's output sits on the soundcheck patch. An input of -2 is the
    // identity default, so the output lands on the strip's own channel.
    inline int soundcheckOutput (const TrackState& track, int index)
    {
        const int input = track.inputRouting.load (std::memory_order_relaxed);
        return input == -2 ? index : input;
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
            engine.setTrackOutputRouting (i, soundcheckOutput (track, i));
        }
        return strips;
    }

    // A stored mix (a cue) brings back a strip's show output. On the soundcheck
    // patch that is what Restore will put back: the strip stays on its input's
    // channel, or playback leaves the desk with VSC still lit and the master can
    // drop onto a pair the strips have just left. Set the input first, because
    // the strip follows it.
    inline void recallOutput (AudioEngine& engine, int index, int output)
    {
        auto& track = engine.getRecorder().getTrack (index);
        if (track.outputBeforeVsc.load (std::memory_order_relaxed) == TrackState::kNoSavedOutput)
        {
            engine.setTrackOutputRouting (index, output);
            return;
        }
        track.outputBeforeVsc.store (output, std::memory_order_relaxed);
        engine.setTrackOutputRouting (index, soundcheckOutput (track, index));
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
