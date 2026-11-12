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

    // Strips whose input channel has no output on this device, or that have no
    // input at all: the patch holds, and nothing reaches the desk from them.
    // Counted from the patch itself, so the question can say it before the press.
    inline int stripsWithNoOutput (AudioEngine& engine)
    {
        auto& recorder = engine.getRecorder();
        const int outputs = engine.getCurrentDeviceOutputCount();
        int silent = 0;
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            const int output = soundcheckOutput (recorder.getTrack (i), i);
            if (output < 0 || output >= outputs) ++silent;
        }
        return silent;
    }

    // Device outputs more than one track plays into, on the soundcheck patch
    // (where two strips on one input both land on its channel) or on the patch
    // in use now. The desk hears them summed. Zero-based, lowest first.
    inline std::vector<int> sharedOutputs (AudioEngine& engine, bool soundcheckPatch)
    {
        auto& recorder = engine.getRecorder();
        const int outputs = engine.getCurrentDeviceOutputCount();
        std::vector<int> tracksOn ((size_t) juce::jmax (0, outputs), 0);
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            const auto& track = recorder.getTrack (i);
            const int output = soundcheckPatch ? soundcheckOutput (track, i)
                                               : track.outputRouting.load (std::memory_order_relaxed);
            if (output >= 0 && output < outputs) ++tracksOn[(size_t) output];
        }
        std::vector<int> shared;
        for (int output = 0; output < outputs; ++output)
            if (tracksOn[(size_t) output] > 1) shared.push_back (output);
        return shared;
    }

    // "Out 1, Out 5" for a sentence; the first few and a count of the rest.
    inline juce::String outputList (const std::vector<int>& outputs, int limit = 4)
    {
        juce::StringArray names;
        for (int i = 0; i < (int) outputs.size() && i < limit; ++i) names.add ("Out " + juce::String (outputs[(size_t) i] + 1));
        auto text = names.joinIntoString (", ");
        if ((int) outputs.size() > limit) text << " and " << ((int) outputs.size() - limit) << " more";
        return text;
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
