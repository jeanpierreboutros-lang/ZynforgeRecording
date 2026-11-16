#pragma once

#include <juce_core/juce_core.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"

namespace zynforge
{
    // Where the timeline is now. The player stands still through a plain take,
    // so a take reads its own clock, and only a punch's rolling playhead counts.
    inline juce::int64 currentPlayheadSamples (AudioEngine& engine)
    {
        auto& player = engine.getPlayer();
        if (engine.isRecording())
            return engine.isCaptureWindowActive() && player.isPlaying() ? player.getPositionSamples()
                                                                        : engine.getRecordTimelineSamples();
        return player.isLoaded() ? player.getPositionSamples() : 0;
    }

    // A folder on a drive that is not plugged in. Free-space reads walk up to
    // the first folder that exists, so for one under /Volumes they report the
    // boot disk, and the engine's sessions folder falls back to ~/Music. A
    // folder not made yet on a drive that is there is not missing.
    inline bool folderDriveMissing (const juce::File& folder)
    {
        if (folder == juce::File() || ! folder.isAChildOf (juce::File ("/Volumes"))) return false;
        auto f = folder;
        while (! f.exists() && f.getParentDirectory() != f) f = f.getParentDirectory();
        return f == juce::File ("/Volumes");
    }

    // Where a new session is made: the sessions folder the engineer chose, even
    // with its drive unplugged or when it cannot be made (the create then
    // refuses and says why). The engine's getSessionsRoot puts ~/Music in place
    // of such a folder, which is how a show landed on the boot disk and the
    // choice was then saved over; ~/Music only when nothing was chosen.
    inline juce::File newSessionsRoot (const AudioEngine& engine)
    {
        if (auto* props = engine.getAppProps())
            if (const auto chosen = props->getValue ("sessionsRoot"); chosen.isNotEmpty() && juce::File::isAbsolutePath (chosen))
                return juce::File (chosen);
        return engine.getSessionsRoot();
    }

    // Every New Session path says this, and makes nothing, for a folder whose
    // drive is not plugged in. Empty when the drive is there.
    inline juce::String newSessionDriveRefusal (const juce::File& folder)
    {
        return folderDriveMissing (folder) ? "The drive for " + folder.getFullPathName()
                                                 + " is not connected -- connect it or choose another folder"
                                           : juce::String();
    }

    // FLAC counts at its uncompressed size, which is conservative.
    inline int captureBytesPerSample (CaptureFormat format) noexcept
    {
        return (format == CaptureFormat::Wav16 || format == CaptureFormat::Aiff16
                || format == CaptureFormat::Flac16)                                         ? 2
             : (format == CaptureFormat::Wav32Float || format == CaptureFormat::Aiff32Float) ? 4
             : 3;
    }

    // What a take needs free before RECORD starts: 30 minutes at this rate, arm
    // count and format. Pre-flight shares this so the two can never disagree.
    inline juce::int64 recordHeadroomBytes (double sampleRate, int armedTracks, CaptureFormat format) noexcept
    {
        return (juce::int64) (sampleRate * armedTracks * captureBytesPerSample (format)) * 60 * 30;
    }

    // Pre-flight's green tick. With nothing armed there is no rate to measure,
    // so it falls back to a plain floor.
    inline bool preflightDiskOk (double freeGB, juce::int64 wantBytes, int armedTracks) noexcept
    {
        return armedTracks == 0 ? freeGB > 5.0 : freeGB * 1.0e9 >= (double) wantBytes;
    }

    // A rolling manual punch starts where playback is NOW. The edit cursor is
    // only an insertion point when transport is stopped; otherwise an old
    // cursor (often at zero) turns a punch into an unrelated continuation.
    inline juce::int64 manualRecordPosition (bool playbackRolling,
                                             juce::int64 playhead,
                                             juce::int64 editCursor) noexcept
    {
        return (! playbackRolling && editCursor >= 0) ? editCursor : playhead;
    }

    // Only a position the engineer chose can punch: a rolling playhead or a
    // placed cursor. A playhead merely parked inside the take (pause, marker
    // jump, remote locate, the end of an earlier punch) appends, which never
    // touches the take. A cursor at zero is a deliberate punch from the start;
    // a deliberate position may also sit in a reference bed past the capture.
    inline bool manualRecordPunches (bool playbackRolling, juce::int64 editCursor, juce::int64 position,
                                     juce::int64 captureLength, juce::int64 timelineLength) noexcept
    {
        return (playbackRolling || editCursor >= 0)
            && position < juce::jmax (captureLength, timelineLength);
    }

    // The installed builds' Loop button stored a whole-timeline region the
    // engineer never selected, so a saved one that spans everything means
    // nothing: it is dropped on open rather than restored.
    inline bool savedRegionSpansTimeline (juce::int64 start, juce::int64 end, juce::int64 timelineLength) noexcept
    {
        return start <= 0 && end >= timelineLength;
    }
}
