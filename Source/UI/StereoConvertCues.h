#pragma once

#include "../Audio/AudioEngine.h"
#include "SetlistBar.h"

#include <vector>

// Convert to Stereo moves channels to make room (AudioEngine::convertToStereo).
// A cue saved before it holds each strip's old patch by strip id, so recalling
// it put the moved strips back on the channels the right half now uses, and
// gave the left half its old mono pan. The cues move the same way the strips
// did (audit 2026-11-22, H5).
namespace zynforge::stereoconvert
{
    inline int carried (int channel, int from, int to) noexcept
    {
        return from >= 0 && channel >= from && channel <= to ? channel + 1 : channel;
    }

    inline void carryCues (std::vector<SetlistBar::Cue>& cues, const AudioEngine::StereoConversion& m)
    {
        // A cue holds the show output: the saved one during soundcheck.
        const int outFrom = m.showOutputsSaved ? m.savedFrom : m.outputsFrom;
        const int outTo   = m.showOutputsSaved ? m.savedTo   : m.outputsTo;
        for (auto& cue : cues)
        {
            int left = -1, right = -1;
            for (int i = 0; i < (int) cue.strips.size(); ++i)
            {
                auto& s = cue.strips[(size_t) i];
                s.inputRouting  = carried (s.inputRouting,  m.inputsFrom, m.inputsTo);
                s.outputRouting = carried (s.outputRouting, outFrom, outTo);
                if (m.leftId.isNotEmpty() && s.stripId == m.leftId)   left  = i;
                if (m.rightId.isNotEmpty() && s.stripId == m.rightId) right = i;
            }
            if (left < 0) continue;
            // The pair's image: L hard left, and R (new to every older cue)
            // hard right, as linking leaves them. Recall gives R only its pan.
            cue.strips[(size_t) left].pan = -1.0f;
            if (right < 0 && m.rightId.isNotEmpty())
            {
                auto r = cue.strips[(size_t) left];
                r.stripId = m.rightId;
                r.pan = 1.0f;
                if (r.inputRouting  >= 0) r.inputRouting  += 1;
                if (r.outputRouting >= 0) r.outputRouting += 1;
                cue.strips.insert (cue.strips.begin() + left + 1, r);
            }
        }
    }
}
