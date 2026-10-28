#pragma once

#include <vector>

// Logical-strip ordering shared by every reorder surface (MIXER selection
// move, EDIT swatch drag) and by the views' rebuild checks.
//
// A stereo pair is two adjacent physical tracks with isStereo set on the L
// track; it is one logical strip and always moves as one block. Callers build
// the COMPLETE physical order here and hand it to AudioEngine::reorderTracks()
// in ONE call. That function is a full on-disk transaction -- it stops the
// player, detaches the audio callback, renames every take file and rewrites
// the session documents -- so it must never run once per pointer step.
namespace zynforge::trackorder
{
    using Blocks = std::vector<std::vector<int>>;

    // stereoL[i] is the stereo flag stored on physical track i. A trailing L
    // flag with no partner is a mono strip, matching every view's collapse
    // rule (isStereo && i + 1 < n).
    inline Blocks logicalBlocks (const std::vector<bool>& stereoL)
    {
        Blocks blocks;
        const int n = (int) stereoL.size();
        for (int p = 0; p < n;)
        {
            const int width = stereoL[(size_t) p] && p + 1 < n ? 2 : 1;
            std::vector<int> block;
            for (int k = 0; k < width; ++k) block.push_back (p + k);
            blocks.push_back (std::move (block));
            p += width;
        }
        return blocks;
    }

    // Read the stereo flags from anything shaped like MultitrackRecorder
    // (getNumTracks() + getTrack(i).isStereo). Message-thread callers only.
    template <typename Recorder>
    std::vector<bool> stereoFlags (Recorder& recorder)
    {
        std::vector<bool> flags;
        const int n = recorder.getNumTracks();
        flags.reserve ((size_t) n);
        for (int i = 0; i < n; ++i) flags.push_back (recorder.getTrack (i).isStereo.load());
        return flags;
    }

    // Index of the block that STARTS at `physical`, or -1 when `physical` is
    // an R half or out of range (a stale layout from the caller's view).
    inline int blockStartingAt (const Blocks& blocks, int physical)
    {
        for (int b = 0; b < (int) blocks.size(); ++b)
            if (blocks[(size_t) b].front() == physical) return b;
        return -1;
    }

    inline std::vector<int> flatten (const Blocks& blocks)
    {
        std::vector<int> order;
        for (const auto& block : blocks) order.insert (order.end(), block.begin(), block.end());
        return order;
    }

    // Physical order after moving block `from` in front of block
    // `insertBefore` (blocks.size() = after the last). Empty when the request
    // is out of range or would leave the order unchanged.
    inline std::vector<int> moveBlock (Blocks blocks, int from, int insertBefore)
    {
        const int n = (int) blocks.size();
        if (from < 0 || from >= n || insertBefore < 0 || insertBefore > n) return {};
        if (insertBefore == from || insertBefore == from + 1) return {};
        auto moving = std::move (blocks[(size_t) from]);
        blocks.erase (blocks.begin() + from);
        const int target = insertBefore > from ? insertBefore - 1 : insertBefore;
        blocks.insert (blocks.begin() + target, std::move (moving));
        return flatten (blocks);
    }
}
