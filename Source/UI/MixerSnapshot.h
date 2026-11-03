#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/VirtualSoundcheck.h"

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
    inline const std::array<Field, 14>& fields()
    {
        static const std::array<Field, 14> table {{
            { "name",    [] (const TrackState& t) { return juce::var (t.getNameThreadSafe()); } },
            { "colour",  [] (const TrackState& t) { return juce::var ((int) t.colourARGB.load (std::memory_order_relaxed)); } },
            { "gainDb",  [] (const TrackState& t) { return juce::var ((double) t.gainDb.load (std::memory_order_relaxed)); } },
            { "pan",     [] (const TrackState& t) { return juce::var ((double) t.pan.load (std::memory_order_relaxed)); } },
            { "in",      [] (const TrackState& t) { return juce::var (t.inputRouting.load (std::memory_order_relaxed)); } },
            { "out",     [] (const TrackState& t) { return juce::var (t.outputRouting.load (std::memory_order_relaxed)); } },
            { "vscOut",  [] (const TrackState& t) { return juce::var (t.outputBeforeVsc.load (std::memory_order_relaxed)); } },
            { "outMute", [] (const TrackState& t) { return juce::var (t.outputMuted.load (std::memory_order_relaxed)); } },
            { "stream",  [] (const TrackState& t) { return juce::var (t.streamSend.load (std::memory_order_relaxed)); } },
            { "mute",    [] (const TrackState& t) { return juce::var (t.muted.load (std::memory_order_relaxed)); } },
            { "solo",    [] (const TrackState& t) { return juce::var (t.soloed.load (std::memory_order_relaxed)); } },
            { "mon",     [] (const TrackState& t) { return juce::var (t.monitor.load (std::memory_order_relaxed)); } },
            { "rec",     [] (const TrackState& t) { return juce::var (t.armed.load (std::memory_order_relaxed)); } },
            { "stereo",  [] (const TrackState& t) { return juce::var (t.isStereo.load (std::memory_order_relaxed)); } },
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

    // Undo's restore, of ONE physical half: undo replays every strip, so this
    // writes exactly what it is given, the soundcheck's way back included. A
    // paste of one logical strip goes through pasteStrip instead.
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
        // -2 is the default patch, which the setters would clamp to -1: an undo
        // of anything at all would unpatch the strip.
        const int in  = (int) obj->getProperty ("in");
        const int out = (int) obj->getProperty ("out");
        eng.setTrackInputRouting  (physical, in  == -2 ? physical : in);
        eng.setTrackOutputRouting (physical, out == -2 ? -1 : out);
        t.outputBeforeVsc.store (obj->hasProperty ("vscOut") ? (int) obj->getProperty ("vscOut")
                                                             : TrackState::kNoSavedOutput,
                                 std::memory_order_relaxed);
        t.outputMuted.store ((bool) obj->getProperty ("outMute"), std::memory_order_relaxed);
        t.streamSend .store ((bool) obj->getProperty ("stream"),  std::memory_order_relaxed);
        t.muted      .store ((bool) obj->getProperty ("mute"),    std::memory_order_relaxed);
        t.soloed     .store ((bool) obj->getProperty ("solo"),    std::memory_order_relaxed);
        t.monitor    .store ((bool) obj->getProperty ("mon"),     std::memory_order_relaxed);
        if (! eng.isRecording()) t.armed.store ((bool) obj->getProperty ("rec"), std::memory_order_relaxed);
    }

    // A link is layout, not a strip's setting, so restoreStrip leaves it alone
    // and a paste can't link anything. Undo and redo put the layout back first:
    // unlinks before links, or a link could find its neighbour still taken.
    inline void restoreLinks (AudioEngine& eng, const juce::var& strips)
    {
        auto* all = strips.getArray();
        if (all == nullptr) return;
        for (const bool linked : { false, true })
            for (int i = 0; i < all->size() && i < eng.getRecorder().getNumTracks(); ++i)
                if (auto* obj = (*all)[i].getDynamicObject())
                    if ((bool) obj->getProperty ("stereo") == linked
                        && eng.getRecorder().getTrack (i).isStereo.load (std::memory_order_relaxed) != linked)
                        eng.setTrackStereo (i, linked);
    }

    // What Copy Strip Settings carries. A stereo strip is one strip here, so it
    // takes both halves. It carries the show's output and no soundcheck state:
    // the way back belongs to the strip it was saved on.
    inline juce::var copyStrip (AudioEngine& eng, int physical)
    {
        auto& rec = eng.getRecorder();
        const auto half = [&] (int index)
        {
            auto copied = snapshotStrip (eng, index);
            if (auto* obj = copied.getDynamicObject())
            {
                obj->setProperty ("out", vsc::showOutput (rec.getTrack (index)));
                obj->removeProperty ("vscOut");
            }
            return copied;
        };
        auto copied = half (physical);
        if (auto* obj = copied.getDynamicObject())
            if (physical + 1 < rec.getNumTracks() && rec.getTrack (physical).isStereo.load (std::memory_order_relaxed))
                obj->setProperty ("right", half (physical + 1));
        return copied;
    }

    // Paste onto one logical strip. Both halves of a stereo target change: the
    // strip shows L, so a paste that left R behind would play and record
    // something the screen does not show. Stereo onto stereo pastes half for
    // half. Mono onto stereo brings R onto L as linking does, and leaves the
    // pans, because they are the pair's stereo image.
    inline void pasteStrip (AudioEngine& eng, int physical, const juce::var& v)
    {
        auto& rec = eng.getRecorder();
        if (physical < 0 || physical >= rec.getNumTracks()) return;
        auto* obj = v.getDynamicObject();
        if (obj == nullptr) return;

        const auto patchInput = [&eng] (int strip, int in)
        {
            if (in != -2) eng.setTrackInputRouting (strip, in);
        };
        // The pasted output is a show output. On a strip in soundcheck it is
        // where that strip goes back to, and the live output follows the input.
        const auto patchOutput = [&] (int strip, int out)
        {
            auto& t = rec.getTrack (strip);
            if (t.outputBeforeVsc.load (std::memory_order_relaxed) == TrackState::kNoSavedOutput)
            {
                if (out != -2) eng.setTrackOutputRouting (strip, out);
                return;
            }
            if (out != -2) t.outputBeforeVsc.store (out, std::memory_order_relaxed);
            const int in = t.inputRouting.load (std::memory_order_relaxed);
            eng.setTrackOutputRouting (strip, in == -2 ? strip : in);
        };
        const auto flags = [&] (int strip, const juce::DynamicObject& from)
        {
            auto& t = rec.getTrack (strip);
            eng.setTrackGainDb (strip, (float) (double) from.getProperty ("gainDb"));
            t.outputMuted.store ((bool) from.getProperty ("outMute"), std::memory_order_relaxed);
            t.streamSend .store ((bool) from.getProperty ("stream"),  std::memory_order_relaxed);
            t.muted      .store ((bool) from.getProperty ("mute"),    std::memory_order_relaxed);
            t.soloed     .store ((bool) from.getProperty ("solo"),    std::memory_order_relaxed);
            t.monitor    .store ((bool) from.getProperty ("mon"),     std::memory_order_relaxed);
            if (! eng.isRecording()) t.armed.store ((bool) from.getProperty ("rec"), std::memory_order_relaxed);
        };

        const bool targetStereo = physical + 1 < rec.getNumTracks()
                               && rec.getTrack (physical).isStereo.load (std::memory_order_relaxed);
        const auto* right = obj->getProperty ("right").getDynamicObject();
        const juce::Colour colour ((juce::uint32) (int) obj->getProperty ("colour"));

        eng.setTrackName   (physical, obj->getProperty ("name").toString());
        eng.setTrackColour (physical, colour);
        flags (physical, *obj);
        const int in = (int) obj->getProperty ("in"), out = (int) obj->getProperty ("out");
        patchInput  (physical, in);
        patchOutput (physical, out);
        if (targetStereo == (right != nullptr))
            eng.setTrackPan (physical, (float) (double) obj->getProperty ("pan"));
        if (! targetStereo) return;

        const int r = physical + 1;
        eng.setTrackColour (r, colour);
        if (right != nullptr)
        {
            eng.setTrackName (r, right->getProperty ("name").toString());
            eng.setTrackPan  (r, (float) (double) right->getProperty ("pan"));
            flags (r, *right);
            patchInput  (r, (int) right->getProperty ("in"));
            patchOutput (r, (int) right->getProperty ("out"));
            return;
        }
        flags (r, *obj);
        patchInput  (r, in  >= 0 ? in  + 1 : in);
        patchOutput (r, out >= 0 ? out + 1 : out);
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
