#pragma once

#include <juce_core/juce_core.h>

namespace zynforge::aiffrepair
{
    enum class Result { untouched, repaired, failed };

    // A crash leaves an AIFF whose header is older than its audio: the bytes
    // are in the file, but COMM and SSND still describe the last header write,
    // which for integer files from older builds means 0 frames. This rewrites
    // those sizes from the real file size. The signature is precise: bytes past
    // the end of the FORM, with SSND as the FORM's last chunk. Any other file,
    // including every finished one, is left exactly as it is.
    inline Result repairStaleHeader (const juce::File& file)
    {
        juce::int64 commAt = -1, ssndAt = -1, offset = 0, formEnd = 0, fileSize = 0;
        int channels = 0, bits = 0;
        juce::uint32 frames = 0, ssndSize = 0;
        {
            juce::FileInputStream in (file);
            if (in.failedToOpen()) return Result::untouched;
            fileSize = in.getTotalLength();
            if (fileSize < 12) return Result::untouched;

            const auto id = [&in]
            {
                char text[4] {};
                return in.read (text, 4) == 4 ? juce::String (text, 4) : juce::String();
            };
            if (id() != "FORM") return Result::untouched;
            formEnd = 8 + (juce::int64) (juce::uint32) in.readIntBigEndian();
            const auto kind = id();
            if (kind != "AIFF" && kind != "AIFC") return Result::untouched;
            if (fileSize <= formEnd) return Result::untouched;

            for (juce::int64 at = 12; at + 8 <= formEnd;)
            {
                if (! in.setPosition (at)) return Result::untouched;
                const auto chunk = id();
                const auto length = (juce::uint32) in.readIntBigEndian();
                if (chunk == "COMM")
                {
                    commAt   = at;
                    channels = in.readShortBigEndian();
                    frames   = (juce::uint32) in.readIntBigEndian();
                    bits     = in.readShortBigEndian();
                    if (kind == "AIFC")
                    {
                        in.skipNextBytes (10);   // the sample rate
                        const auto compression = id();
                        if (compression != "fl32" && compression != "FL32"
                            && compression != "NONE" && compression != "twos")
                            return Result::untouched;
                    }
                }
                else if (chunk == "SSND")
                {
                    ssndAt   = at;
                    ssndSize = length;
                    offset   = (juce::uint32) in.readIntBigEndian();
                    break;
                }
                at += 8 + (juce::int64) length + (length & 1);
            }
        }
        if (commAt < 0 || ssndAt < 0 || ssndSize < 8) return Result::untouched;
        if (ssndAt + 8 + (juce::int64) ssndSize + (ssndSize & 1) < formEnd)
            return Result::untouched;   // other chunks follow the audio: not this shape
        if ((bits != 16 && bits != 24 && bits != 32) || channels < 1 || channels > 256)
            return Result::untouched;

        const auto frameBytes = (juce::int64) channels * (bits / 8);
        const auto dataStart  = ssndAt + 16 + offset;
        if (dataStart >= fileSize) return Result::untouched;
        const auto realFrames = (fileSize - dataStart) / frameBytes;
        if (realFrames <= (juce::int64) frames) return Result::untouched;
        const auto audioBytes = realFrames * frameBytes;
        const auto newSsnd    = audioBytes + (audioBytes & 1) + 8 + offset;
        const auto newForm    = ssndAt + newSsnd;
        if (newForm > 0x7fffffffLL) return Result::failed;

        juce::FileOutputStream out (file);
        if (! out.openedOk()) return Result::failed;
        // FORM last: a repair cut short still shows the stale signature, so the
        // next open finishes it.
        const bool ok = out.setPosition (commAt + 10) && out.writeIntBigEndian ((int) realFrames)
                     && out.setPosition (ssndAt + 4)  && out.writeIntBigEndian ((int) newSsnd)
                     && out.setPosition (4)           && out.writeIntBigEndian ((int) newForm);
        out.flush();
        return ok && out.getStatus().wasOk() ? Result::repaired : Result::failed;
    }

    // Every take file in one copy's Audio Files, sidecars included: a sidecar
    // is restored as the take, and a partial replacement is kept as audio.
    inline void repairTakeFolder (const juce::File& audioDir, int* repaired = nullptr, int* failed = nullptr)
    {
        for (const auto& file : audioDir.findChildFiles (juce::File::findFiles, false,
                                                         "Track_*.aif;Track_*.aiff"))
        {
            const auto result = repairStaleHeader (file);
            if (result == Result::repaired && repaired != nullptr) ++*repaired;
            if (result == Result::failed && failed != nullptr) ++*failed;
        }
    }
}
