#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include "../Audio/AudioEngine.h"

#include <cstring>

// Mixer-undo snapshot of one strip, its restore, and a cheap fingerprint of
// the whole mixer -- all driven by ONE field table, so the change detector can
// never miss a field the snapshot records.
//
// Mixer edits (fader, pan, mute, solo, routing, rename, recolour...) have no
// per-gesture undo wiring; MainComponent::pollMixerUndo watches for changes on
// its 10 Hz timer and commits one undo step once a change settles. It used to
// build and stringify a JSON snapshot of every strip on EVERY tick (and on
// every tick of a take, just to re-baseline). The fingerprint below reads the
// same fields without allocating; the JSON is built only to commit a step.
namespace zynforge::mixersnapshot
{
    struct Field
    {
        const char* key;
        juce::var (*read) (const TrackState&);
    };

    // Names go through getNameThreadSafe(): network surfaces (console link,
    // OSC) rename strips from other threads, so a raw `name` read can tear.
    inline const std::array<Field, 12>& fields()
    {
        static const std::array<Field, 12> table {{
            { "name",    [] (const TrackState& t) { return juce::var (t.getNameThreadSafe()); } },
            { "colour",  [] (const TrackState& t) { return juce::var ((int) t.colourARGB.load (std::memory_order_relaxed)); } },
            { "gainDb",  [] (const TrackState& t) { return juce::var ((double) t.gainDb.load (std::memory_order_relaxed)); } },
            { "pan",     [] (const TrackState& t) { return juce::var ((double) t.pan.load (std::memory_order_relaxed)); } },
            { "in",      [] (const TrackState& t) { return juce::var (t.inputRouting.load (std::memory_order_relaxed)); } },
            { "out",     [] (const TrackState& t) { return juce::var (t.outputRouting.load (std::memory_order_relaxed)); } },
            { "outMute", [] (const TrackState& t) { return juce::var (t.outputMuted.load (std::memory_order_relaxed)); } },
            { "stream",  [] (const TrackState& t) { return juce::var (t.streamSend.load (std::memory_order_relaxed)); } },
            { "mute",    [] (const TrackState& t) { return juce::var (t.muted.load (std::memory_order_relaxed)); } },
            { "solo",    [] (const TrackState& t) { return juce::var (t.soloed.load (std::memory_order_relaxed)); } },
            { "mon",     [] (const TrackState& t) { return juce::var (t.monitor.load (std::memory_order_relaxed)); } },
            { "rec",     [] (const TrackState& t) { return juce::var (t.armed.load (std::memory_order_relaxed)); } },
        }};
        return table;
    }

    inline juce::var snapshotStrip (AudioEngine& eng, int physical)
    {
        if (physical < 0 || physical >= eng.getRecorder().getNumTracks()) return {};
        const auto& t = eng.getRecorder().getTrack (physical);
        auto* obj = new juce::DynamicObject();
        for (const auto& f : fields()) obj->setProperty (f.key, f.read (t));
        return juce::var (obj);
    }

    inline juce::var snapshotAll (AudioEngine& eng)
    {
        juce::Array<juce::var> arr;
        for (int i = 0; i < eng.getRecorder().getNumTracks(); ++i) arr.add (snapshotStrip (eng, i));
        return juce::var (arr);
    }

    // Restore goes through the engine setters (which mirror stereo pairs and
    // persist) wherever one exists.
    inline void restoreStrip (AudioEngine& eng, int physical, const juce::var& v)
    {
        if (physical < 0 || physical >= eng.getRecorder().getNumTracks()) return;
        auto* obj = v.getDynamicObject();
        if (obj == nullptr) return;
        auto& t = eng.getRecorder().getTrack (physical);
        eng.setTrackName  (physical, obj->getProperty ("name").toString());
        eng.setTrackColour(physical, juce::Colour ((juce::uint32) (int) obj->getProperty ("colour")));
        eng.setTrackGainDb(physical, (float) (double) obj->getProperty ("gainDb"));
        eng.setTrackPan   (physical, (float) (double) obj->getProperty ("pan"));
        eng.setTrackInputRouting  (physical, (int) obj->getProperty ("in"));
        eng.setTrackOutputRouting (physical, (int) obj->getProperty ("out"));
        t.outputMuted.store ((bool) obj->getProperty ("outMute"), std::memory_order_relaxed);
        t.streamSend .store ((bool) obj->getProperty ("stream"),  std::memory_order_relaxed);
        t.muted      .store ((bool) obj->getProperty ("mute"),    std::memory_order_relaxed);
        t.soloed     .store ((bool) obj->getProperty ("solo"),    std::memory_order_relaxed);
        t.monitor    .store ((bool) obj->getProperty ("mon"),     std::memory_order_relaxed);
        if (! eng.isRecording()) t.armed.store ((bool) obj->getProperty ("rec"), std::memory_order_relaxed);
    }

    // 64-bit FNV-1a over every field of every strip, plus the strip count.
    // Allocation-free apart from the name's reference-counted String copy.
    inline juce::uint64 fingerprint (AudioEngine& eng)
    {
        juce::uint64 h = 1469598103934665603ull;
        auto mix = [&h] (juce::uint64 v)
        {
            for (int b = 0; b < 8; ++b) { h ^= (v >> (b * 8)) & 0xffu; h *= 1099511628211ull; }
        };
        auto& rec = eng.getRecorder();
        const int n = rec.getNumTracks();
        mix ((juce::uint64) n);
        for (int i = 0; i < n; ++i)
        {
            const auto& t = rec.getTrack (i);
            for (const auto& f : fields())
            {
                const auto v = f.read (t);
                if (v.isString())      mix ((juce::uint64) v.toString().hashCode64());
                else if (v.isDouble()) { const double d = (double) v; juce::uint64 bits; std::memcpy (&bits, &d, sizeof bits); mix (bits); }
                else                   mix ((juce::uint64) (juce::int64) v);
            }
        }
        return h;
    }
}
