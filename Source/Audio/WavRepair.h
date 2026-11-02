#pragma once

#include <juce_core/juce_core.h>

namespace zynforge::wavrepair
{
    enum class Result { untouched, repaired, failed };

    // A crash leaves a WAV whose header is older than its audio: the periodic
    // flush keeps it within seconds, but those seconds are hidden on reopen. This
    // rewrites the sizes from the real file size. The signature is precise: PCM
    // or float audio in a data chunk that is the file's last, with whole frames
    // past the end the header gives. Any other file, including every finished
    // one, is left exactly as it is.
    //
    // A take that crossed 4 GiB after its last flush still has a RIFF header. The
    // writer reserves room for that at the front, so it becomes RF64 here just
    // as it would have at the next flush.
    inline Result repairStaleHeader (const juce::File& file)
    {
        juce::int64 fileSize = 0, sizesAt = -1, dataAt = -1, declared = 0, outerEnd = 0;
        juce::uint32 slotLength = 0;
        bool rf64 = false;
        int blockAlign = 0;
        {
            juce::FileInputStream in (file);
            if (in.failedToOpen()) return Result::untouched;
            fileSize = in.getTotalLength();
            if (fileSize < 44) return Result::untouched;

            const auto id = [&in]
            {
                char text[4] {};
                return in.read (text, 4) == 4 ? juce::String (text, 4) : juce::String();
            };
            const auto first = id();
            if (first != "RIFF" && first != "RF64") return Result::untouched;
            rf64 = first == "RF64";
            outerEnd = 8 + (juce::int64) (juce::uint32) in.readInt();
            if (id() != "WAVE") return Result::untouched;

            for (juce::int64 at = 12; at + 8 <= fileSize;)
            {
                if (! in.setPosition (at)) return Result::untouched;
                const auto chunk = id();
                const auto length = (juce::uint32) in.readInt();
                if (at == 12 && (chunk == "JUNK" || chunk == "ds64"))
                {
                    sizesAt = at;
                    slotLength = length;
                    if (rf64)
                    {
                        if (chunk != "ds64" || length < 28) return Result::untouched;
                        outerEnd = 8 + in.readInt64();
                        declared = in.readInt64();
                    }
                }
                else if (chunk == "fmt ")
                {
                    const auto tag = (juce::uint16) in.readShort();
                    const int channels = in.readShort();
                    in.skipNextBytes (8);
                    blockAlign = in.readShort();
                    const int bits = in.readShort();
                    if ((tag != 1 && tag != 3 && tag != 0xfffe) || channels < 1 || channels > 256
                        || (bits != 16 && bits != 24 && bits != 32) || blockAlign != channels * (bits / 8))
                        return Result::untouched;
                }
                else if (chunk == "data")
                {
                    dataAt = at;
                    if (! rf64) declared = length;
                    break;
                }
                at += 8 + (juce::int64) length + (length & 1);
            }
        }
        if (dataAt < 0 || blockAlign <= 0) return Result::untouched;

        const auto dataStart = dataAt + 8;
        const auto declaredEnd = dataStart + declared + (declared & 1);
        if (outerEnd > declaredEnd) return Result::untouched;   // other chunks follow the audio: not this shape
        const auto audioBytes = (fileSize - dataStart) / blockAlign * blockAlign;
        const auto realEnd = dataStart + audioBytes + (audioBytes & 1);
        if (audioBytes <= declared && outerEnd >= realEnd) return Result::untouched;

        const bool promote = ! rf64 && realEnd - 8 >= 0x100000000LL;
        if (promote && (sizesAt < 0 || (slotLength != 28 && slotLength < 36))) return Result::failed;

        juce::FileOutputStream out (file);
        if (! out.openedOk()) return Result::failed;
        // The data size first and the outer size last, so a repair cut short still
        // shows the stale signature and the next open finishes it.
        bool ok = true;
        if (rf64 || promote)
        {
            ok = out.setPosition (sizesAt);
            if (promote)
                ok = ok && out.write ("ds64", 4) && out.writeInt (28);
            ok = ok && out.setPosition (sizesAt + 16) && out.writeInt64 (audioBytes);
            ok = ok && out.setPosition (sizesAt + 8) && out.writeInt64 (realEnd - 8);
            if (promote)
            {
                ok = ok && out.setPosition (sizesAt + 24) && out.writeRepeatedByte (0, 12);
                if (slotLength > 28)
                    ok = ok && out.write ("JUNK", 4) && out.writeInt ((int) slotLength - 28 - 8);
                ok = ok && out.setPosition (0) && out.write ("RF64", 4) && out.writeInt (-1)
                        && out.setPosition (dataAt + 4) && out.writeInt (-1);
            }
        }
        else
        {
            ok = out.setPosition (dataAt + 4) && out.writeInt ((int) (juce::uint32) audioBytes)
              && out.setPosition (4)          && out.writeInt ((int) (juce::uint32) (realEnd - 8));
        }
        out.flush();
        return ok && out.getStatus().wasOk() ? Result::repaired : Result::failed;
    }

    // Every take file in one copy's Audio Files, sidecars included: a sidecar
    // is restored as the take, and a partial replacement is kept as audio.
    inline void repairTakeFolder (const juce::File& audioDir, int* repaired = nullptr, int* failed = nullptr)
    {
        for (const auto& file : audioDir.findChildFiles (juce::File::findFiles, false, "Track_*.wav"))
        {
            const auto result = repairStaleHeader (file);
            if (result == Result::repaired && repaired != nullptr) ++*repaired;
            if (result == Result::failed && failed != nullptr) ++*failed;
        }
    }
}
