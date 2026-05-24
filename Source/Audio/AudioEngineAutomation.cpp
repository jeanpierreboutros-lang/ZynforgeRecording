// Automation lane subsystem -- all the per-track point storage,
// curve interpolation, JSON round-trip, range ops, Safe lock,
// trim, and WRITE-mode thinning. Extracted from AudioEngine.cpp
// as part of the 2026-05-24 god-class split.
//
// Real-time read path (automationValueAt) lives here too. It uses
// ScopedTryLock so the audio thread can fall back to the live
// fader value when the message thread is mid-edit.

#include "AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace zynforge
{

    // Caller holds automationLock. Encapsulates the kSnap + Mute-snap
    // + sort semantics so both addAutomationPoint and the WRITE-mode
    // thinned path share one implementation.
    static void addPointLocked (std::vector<AudioEngine::AutomationPoint>& lane,
                                AudioEngine::AutomationParam p,
                                juce::int64 samplePos, float value)
    {
        using C = AudioEngine::AutomationCurve;
        C newCurve = C::Linear;
        if (p == AudioEngine::AutomationParam::Mute)
        {
            value    = value >= 0.5f ? 1.0f : 0.0f;
            newCurve = C::Hold;
        }

        constexpr juce::int64 kSnap = 4096;
        for (auto& pt : lane)
        {
            if (std::abs (pt.samplePos - samplePos) < kSnap)
            {
                pt.value = value;
                if (p == AudioEngine::AutomationParam::Mute) pt.curve = C::Hold;
                return;
            }
        }
        lane.push_back ({ samplePos, value, newCurve, 0.0f });
        std::sort (lane.begin(), lane.end(),
                   [] (const auto& a, const auto& b) { return a.samplePos < b.samplePos; });
    }

    std::vector<AudioEngine::AutomationPoint>
    AudioEngine::copyAutomationRange (int track, AutomationParam p,
                                      juce::int64 startSample, juce::int64 endSample) const
    {
        if (startSample > endSample) std::swap (startSample, endSample);
        std::vector<AutomationPoint> out;
        const juce::ScopedLock sl (automationLock);
        const auto* lane = findLane (track, p);
        if (lane == nullptr) return out;
        for (const auto& pt : *lane)
        {
            if (pt.samplePos < startSample) continue;
            if (pt.samplePos > endSample)   break;
            out.push_back ({ pt.samplePos - startSample, pt.value, pt.curve });
        }
        return out;
    }

    void AudioEngine::clearAutomationRange (int track, AutomationParam p,
                                            juce::int64 startSample, juce::int64 endSample)
    {
        if (startSample > endSample) std::swap (startSample, endSample);
        if (track < 0) return;
        const juce::ScopedLock sl (automationLock);
        if (track >= (int) automationData.size()) return;
        if (automationData[(size_t) track].safe.load (std::memory_order_acquire)) return;
        auto* lane = findLane (track, p);
        if (lane == nullptr || lane->empty()) return;
        auto pred = [startSample, endSample] (const AutomationPoint& pt)
        {
            return pt.samplePos >= startSample && pt.samplePos <= endSample;
        };
        lane->erase (std::remove_if (lane->begin(), lane->end(), pred), lane->end());
    }

    void AudioEngine::pasteAutomationRange (int track, AutomationParam p,
                                            juce::int64 anchorSample,
                                            const std::vector<AutomationPoint>& points)
    {
        if (points.empty()) return;
        if (track < 0) return;
        const juce::ScopedLock sl (automationLock);
        if (track >= (int) automationData.size())
            automationData.resize ((size_t) track + 1);
        if (automationData[(size_t) track].safe.load (std::memory_order_acquire)) return;
        auto* lane = findLane (track, p);
        if (lane == nullptr) return;
        // Pre-clear the paste span so the new points don't visually
        // overlap held-over old points; the engineer asked for a
        // paste, not a merge.
        const juce::int64 spanStart = anchorSample;
        const juce::int64 spanEnd   = anchorSample + points.back().samplePos;
        for (auto it = lane->begin(); it != lane->end();)
        {
            if (it->samplePos >= spanStart && it->samplePos <= spanEnd)
                it = lane->erase (it);
            else
                ++it;
        }
        // Drop the new ones at anchor + offset.
        for (const auto& src : points)
        {
            AutomationPoint pt = src;
            pt.samplePos = anchorSample + src.samplePos;
            // Mute paste forces Hold curves (matches addAutomationPoint).
            if (p == AutomationParam::Mute) pt.curve = AutomationCurve::Hold;
            lane->push_back (pt);
        }
        std::sort (lane->begin(), lane->end(),
                   [] (const auto& a, const auto& b) { return a.samplePos < b.samplePos; });
    }

    void AudioEngine::setTrackAutomationSafe (int track, bool safe)
    {
        const juce::ScopedLock sl (automationLock);
        if (track < 0) return;
        if (track >= (int) automationData.size())
            automationData.resize ((size_t) track + 1);
        automationData[(size_t) track].safe.store (safe, std::memory_order_release);
    }

    bool AudioEngine::isTrackAutomationSafe (int track) const
    {
        const juce::ScopedLock sl (automationLock);
        if (track < 0 || track >= (int) automationData.size()) return false;
        return automationData[(size_t) track].safe.load (std::memory_order_acquire);
    }

    // Forward decl -- the body lives next to addAutomationPoint
    // below since both call sites share it.
    static void addPointLocked (std::vector<AudioEngine::AutomationPoint>& lane,
                                AudioEngine::AutomationParam p,
                                juce::int64 samplePos, float value);

    void AudioEngine::writeAutomationPointThinned (int track, AutomationParam p,
                                                   juce::int64 samplePos, float value,
                                                   juce::int64 minSamplesBetween)
    {
        if (track < 0) return;
        // Punch-gate first -- skip cheap before grabbing the lock.
        if (isAutomationPunchEnabled() && ! isInsideAutomationPunchRange (samplePos))
            return;

        const juce::ScopedLock sl (automationLock);
        if (track >= (int) automationData.size())
            automationData.resize ((size_t) track + 1);
        auto& a = automationData[(size_t) track];
        if (a.safe.load (std::memory_order_acquire)) return;

        const int idx = (p == AutomationParam::Volume) ? 0
                      : (p == AutomationParam::Pan)    ? 1 : 2;
        const auto last = a.lastWriteSample[idx].load (std::memory_order_acquire);
        if (last >= 0 && std::llabs ((long long) (samplePos - last)) < (long long) minSamplesBetween)
            return;     // too soon -- skip this drop, keep ramp resolution sensible

        a.lastWriteSample[idx].store (samplePos, std::memory_order_release);
        auto* lane = findLane (track, p);
        if (lane == nullptr) return;
        addPointLocked (*lane, p, samplePos, value);
    }

    void AudioEngine::setAutomationTrim (int track, AutomationParam p, float trim)
    {
        if (p == AutomationParam::Mute) return;
        const juce::ScopedLock sl (automationLock);
        if (track < 0) return;
        if (track >= (int) automationData.size())
            automationData.resize ((size_t) track + 1);
        auto& a = automationData[(size_t) track];
        if (p == AutomationParam::Volume) a.volumeTrim.store (trim, std::memory_order_release);
        else                              a.panTrim   .store (trim, std::memory_order_release);
    }

    float AudioEngine::getAutomationTrim (int track, AutomationParam p) const
    {
        if (p == AutomationParam::Mute) return 0.0f;
        const juce::ScopedLock sl (automationLock);
        if (track < 0 || track >= (int) automationData.size()) return 0.0f;
        const auto& a = automationData[(size_t) track];
        return (p == AutomationParam::Volume ? a.volumeTrim.load (std::memory_order_acquire)
                                              : a.panTrim   .load (std::memory_order_acquire));
    }

    void AudioEngine::clearAllAutomationTrims()
    {
        const juce::ScopedLock sl (automationLock);
        for (auto& a : automationData)
        {
            a.volumeTrim.store (0.0f, std::memory_order_release);
            a.panTrim   .store (0.0f, std::memory_order_release);
        }
    }

    juce::var AudioEngine::automationToJson() const
    {
        // Snapshot every track's three lanes (volume / pan / mute)
        // as a JSON array. Empty tracks emit an entry with empty
        // lanes so the load path can match by index without gaps.
        const juce::ScopedLock sl (automationLock);
        juce::Array<juce::var> tracks;

        auto laneToVar = [] (const std::vector<AutomationPoint>& lane)
        {
            juce::Array<juce::var> arr;
            for (const auto& pt : lane)
            {
                juce::DynamicObject::Ptr o (new juce::DynamicObject());
                o->setProperty ("s", (juce::int64) pt.samplePos);
                o->setProperty ("v", (double) pt.value);
                o->setProperty ("c", (int) pt.curve);
                if (std::abs (pt.tension) > 1.0e-4f)
                    o->setProperty ("t", (double) pt.tension);
                arr.add (juce::var (o.get()));
            }
            return juce::var (arr);
        };

        for (size_t t = 0; t < automationData.size(); ++t)
        {
            const auto& a = automationData[t];
            juce::DynamicObject::Ptr trk (new juce::DynamicObject());
            trk->setProperty ("track",  (int) t);
            trk->setProperty ("volume", laneToVar (a.volume));
            trk->setProperty ("pan",    laneToVar (a.pan));
            trk->setProperty ("mute",   laneToVar (a.mute));
            trk->setProperty ("safe",   a.safe.load (std::memory_order_acquire));
            trk->setProperty ("vTrim",  (double) a.volumeTrim.load (std::memory_order_acquire));
            trk->setProperty ("pTrim",  (double) a.panTrim   .load (std::memory_order_acquire));
            tracks.add (juce::var (trk.get()));
        }
        return juce::var (tracks);
    }

    void AudioEngine::loadAutomationFromJson (const juce::var& v)
    {
        auto* arr = v.getArray();
        if (arr == nullptr) return;

        const juce::ScopedLock sl (automationLock);

        auto varToLane = [] (const juce::var& laneVar,
                             std::vector<AutomationPoint>& out)
        {
            out.clear();
            auto* pa = laneVar.getArray();
            if (pa == nullptr) return;
            for (const auto& ptVar : *pa)
            {
                auto* obj = ptVar.getDynamicObject();
                if (obj == nullptr) continue;
                AutomationPoint pt;
                pt.samplePos = (juce::int64) obj->getProperty ("s");
                pt.value     = (float) (double) obj->getProperty ("v");
                pt.curve     = (AutomationCurve) (int) obj->getProperty ("c");
                if (obj->hasProperty ("t"))
                    pt.tension = (float) (double) obj->getProperty ("t");
                // Legacy preset migration -- old .zfproj files only
                // had the discrete shape enum. Map the two power-curve
                // presets onto an equivalent tension on a Linear
                // segment so the drag handle finds them in the right
                // place and the new interpolator produces the same
                // shape the old switch produced.
                if (pt.curve == AutomationCurve::ExpUp)
                {
                    pt.curve   = AutomationCurve::Linear;
                    pt.tension = -0.5f;             // ease-in (t^2-ish)
                }
                else if (pt.curve == AutomationCurve::ExpDown)
                {
                    pt.curve   = AutomationCurve::Linear;
                    pt.tension = +0.5f;             // ease-out
                }
                out.push_back (pt);
            }
        };

        for (const auto& item : *arr)
        {
            auto* trk = item.getDynamicObject();
            if (trk == nullptr) continue;
            const int t = (int) trk->getProperty ("track");
            if (t < 0) continue;
            if (t >= (int) automationData.size())
                automationData.resize ((size_t) t + 1);
            auto& a = automationData[(size_t) t];
            varToLane (trk->getProperty ("volume"), a.volume);
            varToLane (trk->getProperty ("pan"),    a.pan);
            varToLane (trk->getProperty ("mute"),   a.mute);
            if (trk->hasProperty ("safe"))
                a.safe.store ((bool) trk->getProperty ("safe"), std::memory_order_release);
            if (trk->hasProperty ("vTrim"))
                a.volumeTrim.store ((float) (double) trk->getProperty ("vTrim"), std::memory_order_release);
            if (trk->hasProperty ("pTrim"))
                a.panTrim.store ((float) (double) trk->getProperty ("pTrim"), std::memory_order_release);
        }
    }

    std::vector<AudioEngine::AutomationPoint>* AudioEngine::findLane (int track, AutomationParam p)
    {
        if (track < 0) return nullptr;
        if (track >= (int) automationData.size())
            automationData.resize ((size_t) track + 1);
        auto& a = automationData[(size_t) track];
        switch (p)
        {
            case AutomationParam::Volume: return &a.volume;
            case AutomationParam::Pan:    return &a.pan;
            case AutomationParam::Mute:   return &a.mute;
        }
        return nullptr;
    }

    const std::vector<AudioEngine::AutomationPoint>* AudioEngine::findLane (int track, AutomationParam p) const
    {
        if (track < 0 || track >= (int) automationData.size()) return nullptr;
        auto& a = automationData[(size_t) track];
        switch (p)
        {
            case AutomationParam::Volume: return &a.volume;
            case AutomationParam::Pan:    return &a.pan;
            case AutomationParam::Mute:   return &a.mute;
        }
        return nullptr;
    }

    const std::vector<AudioEngine::AutomationPoint>&
    AudioEngine::getAutomation (int track, AutomationParam p) const
    {
        if (auto* lane = findLane (track, p)) return *lane;
        return emptyAutomation;
    }


    void AudioEngine::addAutomationPoint (int track, AutomationParam p,
                                          juce::int64 samplePos, float value)
    {
        if (track < 0) return;
        const juce::ScopedLock sl (automationLock);
        // Grow automationData lazily so writes on a freshly-created
        // strip (setStripCount adds to the recorder, but the engine
        // doesn't pre-populate per-strip lane storage) actually
        // land instead of silently no-op'ing. Order matters: the
        // resize HAS to happen before the safe-check so the safe
        // atomic exists to read.
        if (track >= (int) automationData.size())
            automationData.resize ((size_t) track + 1);
        if (automationData[(size_t) track].safe.load (std::memory_order_acquire)) return;
        auto* lane = findLane (track, p);
        if (lane == nullptr) return;
        addPointLocked (*lane, p, samplePos, value);
    }

    void AudioEngine::setAutomationCurveAt (int track, AutomationParam p,
                                            juce::int64 samplePos, juce::int64 tolerance,
                                            AutomationCurve curve)
    {
        // Mute lanes always Hold -- ignore curve change requests.
        if (p == AutomationParam::Mute) return;

        const juce::ScopedLock sl (automationLock);
        auto* lane = findLane (track, p);
        if (lane == nullptr) return;
        for (auto& pt : *lane)
        {
            if (std::abs (pt.samplePos - samplePos) <= tolerance)
            {
                pt.curve = curve;
                // Picking a preset shape resets the per-segment
                // tension so the curve-picker menu always produces
                // exactly what its label says (a "Linear" pick should
                // be straight, not still bent from a previous drag).
                pt.tension = 0.0f;
                return;
            }
        }
    }

    void AudioEngine::setAutomationTensionAt (int track, AutomationParam p,
                                              juce::int64 samplePos, juce::int64 tolerance,
                                              float tension)
    {
        // Mute lanes always Hold -- tension is meaningless there.
        if (p == AutomationParam::Mute) return;

        const juce::ScopedLock sl (automationLock);
        auto* lane = findLane (track, p);
        if (lane == nullptr) return;
        for (auto& pt : *lane)
        {
            if (std::abs (pt.samplePos - samplePos) <= tolerance)
            {
                // Hold and SCurve don't honour tension; promote to
                // Linear so the drag has visible effect. (Engineer
                // dragged the handle, the handle is a Linear
                // affordance -- presets are reachable via the
                // curve-picker right-click menu.)
                if (pt.curve != AutomationCurve::Linear)
                    pt.curve = AutomationCurve::Linear;
                pt.tension = juce::jlimit (-1.0f, 1.0f, tension);
                return;
            }
        }
    }

    float AudioEngine::automationValueAt (int track, AutomationParam p,
                                          juce::int64 samplePos,
                                          float fallback) const noexcept
    {
        // SUSPEND short-circuits every lane to the live fader value
        // so the engineer can audition raw moves without the engine
        // pulling them back to stored automation.
        if (automationReadSuspended.load (std::memory_order_acquire)) return fallback;
        const juce::ScopedTryLock stl (automationLock);
        if (! stl.isLocked()) return fallback;     // UI mid-edit -- use the slider value
        if (track < 0 || track >= (int) automationData.size()) return fallback;
        const auto& a = automationData[(size_t) track];
        const std::vector<AutomationPoint>* lane = nullptr;
        switch (p)
        {
            case AutomationParam::Volume: lane = &a.volume; break;
            case AutomationParam::Pan:    lane = &a.pan;    break;
            case AutomationParam::Mute:   lane = &a.mute;   break;
        }
        // Trim offset applies to volume + pan regardless of whether
        // any automation points exist. Read it before the early-out
        // so a 'no points + non-zero trim' still nudges the value.
        float trim = 0.0f;
        if (p == AutomationParam::Volume) trim = a.volumeTrim.load (std::memory_order_acquire);
        if (p == AutomationParam::Pan)    trim = a.panTrim   .load (std::memory_order_acquire);

        if (lane == nullptr || lane->empty())
            return p == AutomationParam::Pan
                       ? juce::jlimit (-1.0f, 1.0f, fallback + trim)
                       : fallback + trim;

        // Find the two surrounding points (prev, next). If samplePos
        // is before the first point or after the last point, hold
        // the boundary value (no extrapolation).
        // Helper that applies trim + (for pan) clamps. Used at every
        // return site below so trim layers atop whatever value the
        // curve produces.
        auto withTrim = [p, trim] (float baseValue) -> float
        {
            const float v = baseValue + trim;
            return (p == AutomationParam::Pan) ? juce::jlimit (-1.0f, 1.0f, v) : v;
        };

        const auto& v0 = (*lane)[0];
        if (samplePos <= v0.samplePos) return withTrim (v0.value);
        const auto& vN = lane->back();
        if (samplePos >= vN.samplePos) return withTrim (vN.value);

        // Walk to the segment that contains samplePos. The points
        // are kept sorted by samplePos in addAutomationPoint, so a
        // single linear scan is correct.
        const AutomationPoint* prev = &v0;
        const AutomationPoint* next = nullptr;
        for (size_t i = 1; i < lane->size(); ++i)
        {
            const auto& pt = (*lane)[i];
            if (samplePos < pt.samplePos)
            {
                next = &pt;
                prev = &(*lane)[i - 1];
                break;
            }
        }
        if (next == nullptr) return withTrim (vN.value);

        // Normalised time within this segment.
        const double span = (double) (next->samplePos - prev->samplePos);
        if (span <= 0.0) return withTrim (prev->value);
        const double t = (double) (samplePos - prev->samplePos) / span;

        // Apply the curve type pinned on the PREV point. That field
        // controls how value evolves FROM prev TO next. For Linear,
        // the per-point `tension` warps the ramp into ease-in (<0)
        // or ease-out (>0) via t^exp where exp = 2^(-tension*4).
        double shaped = t;
        switch (prev->curve)
        {
            case AutomationCurve::Hold:
                return withTrim (prev->value);          // step
            case AutomationCurve::Linear:
            {
                const float tn = juce::jlimit (-1.0f, 1.0f, prev->tension);
                if (std::abs (tn) < 1.0e-4f) { shaped = t; break; }
                const double exp = std::pow (2.0, (double) (-tn) * 4.0);
                shaped = std::pow (t, exp);
                break;
            }
            case AutomationCurve::SCurve:
                shaped = t * t * (3.0 - 2.0 * t);       // smoothstep
                break;
            case AutomationCurve::ExpUp:
                shaped = t * t;                          // legacy ease-in preset
                break;
            case AutomationCurve::ExpDown:
                shaped = 1.0 - (1.0 - t) * (1.0 - t);    // legacy ease-out preset
                break;
        }
        return withTrim ((float) (prev->value + shaped * (next->value - prev->value)));
    }

    void AudioEngine::removeAutomationPointNear (int track, AutomationParam p,
                                                 juce::int64 samplePos, juce::int64 tolerance)
    {
        if (track < 0) return;
        const juce::ScopedLock sl (automationLock);
        if (track >= (int) automationData.size()) return;   // nothing stored = nothing to remove
        if (automationData[(size_t) track].safe.load (std::memory_order_acquire)) return;
        auto* lane = findLane (track, p);
        if (lane == nullptr || lane->empty()) return;

        // Pick the single closest point inside the tolerance window.
        auto best = lane->end();
        juce::int64 bestDist = tolerance + 1;
        for (auto it = lane->begin(); it != lane->end(); ++it)
        {
            const auto d = std::abs (it->samplePos - samplePos);
            if (d < bestDist) { bestDist = d; best = it; }
        }
        if (best != lane->end()) lane->erase (best);
    }

    void AudioEngine::clearAutomation (AutomationParam p)
    {
        const juce::ScopedLock sl (automationLock);
        for (auto& a : automationData)
        {
            switch (p)
            {
                case AutomationParam::Volume: a.volume.clear(); break;
                case AutomationParam::Pan:    a.pan   .clear(); break;
                case AutomationParam::Mute:   a.mute  .clear(); break;
            }
        }
    }

    void AudioEngine::clearAutomationForTrack (int track, AutomationParam p)
    {
        if (auto* lane = findLane (track, p)) lane->clear();
    }
}
