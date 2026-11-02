#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "BwfStamp.h"
#include "ChannelAlarm.h"
#include "ClockSource.h"
#include "TrackState.h"
#include "VolumeProbe.h"

namespace zynforge
{
    enum class CaptureFormat
    {
        Wav16,        // 16-bit PCM WAV
        Wav24,        // 24-bit PCM WAV (default)
        Wav32Float,   // 32-bit IEEE float WAV -- clip-proof at file level
        Aiff16,       // 16-bit PCM AIFF
        Aiff24,       // 24-bit PCM AIFF
        Aiff32Float,  // 32-bit IEEE float AIFF
        Flac16,       // 16-bit FLAC
        Flac24        // 24-bit FLAC -- ~50% disk size vs WAV
    };

    // Lock-free per-channel ring buffer + background writers. The audio
    // thread only ever pushes; a distributor thread hands the samples to each
    // destination, and every destination writes on its own thread (CaptureCopy).
    //
    // Pre-roll: a separate per-channel ring of N seconds is always
    // pushed (when N > 0) so RECORD can dump audio that happened
    // BEFORE the button was pressed.
    class MultitrackRecorder final : private juce::TimeSliceClient
    {
    public:
        MultitrackRecorder();
        ~MultitrackRecorder() override;

        // Called by AudioEngine when the device opens / sample rate changes.
        void prepare (double sampleRateHz, int maxBlockSize, int numInputs);
        void release();

        // Real-time safe: pushes input samples and updates meters.
        // Called from the audio thread.
        // playheadAtBlock: the player position of inputs[.][0] while playback
        // rolls, else -1. A punch splices where its first sample was captured
        // (see punchCaptureOrigin).
        void processBlock (const float* const* inputs, int numChannels, int numSamples,
                           juce::int64 playheadAtBlock = -1) noexcept;

        bool startRecording (const juce::File& sessionDir);
        juce::String getStartError() const { return startError; }
        void stopRecording();
        bool isRecording() const noexcept { return recording.load (std::memory_order_acquire); }
        bool isFinalizing() const noexcept { return finalizing.load (std::memory_order_acquire); }
        bool beginFinalization();
        void finishFinalization();
        void endFinalization() noexcept { finalizing.store (false, std::memory_order_release); }

        // ── Punch-in (overdub a region of an existing take) ────────────────
        // Arm the NEXT startRecording as a punch-in at `punchInSample` on the
        // session's existing take. The real-time capture path is unchanged: we
        // still record a clean fresh Track_NN file from 0. The punch is an
        // OFFLINE splice on stop -- each armed track's existing primary/backup/
        // mirror copy is moved aside to a sidecar before the writer truncates,
        // then on stop spliced back as base[0,punchIn) + newTake + base[after],
        // via a temp file + atomic swap, so a failure can't corrupt the take.
        // Cleared automatically after each stopRecording(). Existing multi-part
        // takes are read as one base and flattened into the spliced result.
        void armPunchIn (juce::int64 punchInSample) noexcept
        {
            if (isFinalizing()) return;
            continueAsPart = false;   // one arm per start: punch and continue never combine
            punchInActive = true;
            punchInPos    = juce::jmax ((juce::int64) 0, punchInSample);
            // The fresh punch file records from sample 0, but on the TIMELINE
            // the take resumes at the punch point: anchor the clock / playhead /
            // live-capture waveform there so the new audio builds AT the punch
            // position (with the existing take under it), not from 0. Cleared in
            // stopRecording like the rest of the punch state.
            recordBaseSamples = punchInPos;
        }
        bool isPunchInArmed() const noexcept { return punchInActive; }
        void cancelPunchIn() noexcept
        {
            if (isFinalizing()) return;
            punchInActive = false;
            punchInPos = 0;
            recordBaseSamples = 0;
        }

        // ── Continue recording (append a new part) ─────────────────────────
        // Arm the NEXT startRecording to CONTINUE the existing take rather than
        // overwrite it: each armed track that already has a take records a fresh
        // Track_NN_partXX continuation file, leaving the existing file(s)
        // untouched -- the take is the sequence of parts (SessionPlayer stitches
        // them on playback). This is the safe "keep recording on my file" model:
        // nothing existing is modified, only a new finalised part is added.
        // Auto-cleared after each stopRecording(). An armed track with no take
        // yet just records Track_NN normally (part 1).
        // baseSamples is the existing take's length: the new part records into
        // its own file from 0, but the take's TIMELINE position continues from
        // baseSamples, so the clock / playhead / ruler show "carrying on from
        // where it stopped" instead of restarting at 0.
        void armContinue (juce::int64 baseSamples) noexcept
        {
            if (isFinalizing()) return;
            punchInActive     = false;   // see armPunchIn
            punchInPos        = 0;
            continueAsPart    = true;
            recordBaseSamples = juce::jmax ((juce::int64) 0, baseSamples);
        }
        bool isContinueArmed() const noexcept { return continueAsPart; }

        // Time of day for the NEXT take's BWF stamp when something better
        // than the wall clock is running (locked LTC / MTC). The recorder
        // stays ignorant of where it came from. Set before every start: each
        // start consumes it, and without one the take uses the wall clock.
        void setTimeReferenceHint (const bwf::TimeAnchor& anchor) { timeHint = anchor; hasTimeHint = true; }
        void clearTimeReferenceHint() noexcept { hasTimeHint = false; }

        // The device whose clock source goes into the take's report, by its
        // JUCE name. Set before start. Empty = no device to ask.
        void setClockDevice (const juce::String& deviceName) { clockDevice = deviceName; }
        // Asks that device now, waiting at most timeoutMs. Off the audio thread.
        clocksource::Reading readClockSource (const juce::String& deviceName, int timeoutMs)
        { return clockProbe.wait (deviceName, timeoutMs); }

        // Brings the dead-channel alarms up to date and returns the live ones.
        // Reads only atomics the audio thread stores, so any non-audio thread
        // may call it, as often as it likes: the 10 Hz status snapshot is the
        // caller that matters. Empty outside a take.
        std::vector<channelalarm::Alarm> evaluateChannelAlarms();

        // Drop a punch or continue arm that no take consumed. Every refused
        // start calls this: a stale arm would turn the NEXT RECORD into a
        // punch, a continue, or both at once, which splices silence over the
        // original take. No-op while a take is live or finalizing, because
        // the stop path still reads the arm.
        void discardTakeArms() noexcept
        {
            if (recording.load() || isFinalizing()) return;
            punchInActive     = false;
            punchInPos        = 0;
            continueAsPart    = false;
            recordBaseSamples = 0;
            punchCaptureOrigin.store (-1, std::memory_order_relaxed);
        }

        // Recording position ON THE TIMELINE: the take base (0 for a normal
        // take, the existing length for a continue) + samples captured so far.
        // Use this for the playhead / clock; getSamplesSinceStart() is the new
        // FILE's length (integrity / write accounting).
        juce::int64 getRecordTimelineSamples() const noexcept
        { return recordBaseSamples + getSamplesSinceStart(); }
        juce::int64 getRecordBaseSamples() const noexcept { return recordBaseSamples; }
        // Pre-roll written ahead of the first live sample of this take.
        juce::int64 getPreRollHistorySamples() const noexcept { return preRollHistorySamples; }

        // True if the session's take for `trackIndex` was auto-split into
        // multiple parts (Track_NN_partXX) -- punch-in must refuse these.
        static bool takeIsMultiPart (const juce::File& sessionDir, int trackIndex);

        // Restore originals left in .punchbase sidecars by an interrupted
        // punch. Any partial new take is retained under Session File Backups.
        // Returns false without discarding either file when recovery fails.
        static bool recoverInterruptedPunches (const juce::File& audioDir,
                                               int* recovered = nullptr);

        // Next continuation file for a take: Track_NN.<ext> is part 1, then
        // Track_NN_part02, _part03 ... Returns the path to write next + its part
        // number (1 when no take exists yet -> a normal first recording).
        static std::pair<juce::File, int> nextContinuationPart (
            const juce::File& audioDir, const juce::String& trackName, const juce::String& ext);

        // Capture format & pre-roll are settings -- only changeable while not recording.
        void setCaptureFormat (CaptureFormat f) noexcept     { if (! isRecording() && ! isFinalizing()) captureFormat = f; }
        CaptureFormat getCaptureFormat() const noexcept       { return captureFormat; }

        // Optional second format for the backup writer. When set to a
        // different value than the primary, recording produces TWO files
        // per channel -- e.g., 24-bit WAV to the main drive AND 24-bit
        // FLAC to the backup drive. Default mirrors the primary.
        void setBackupCaptureFormat (CaptureFormat f) noexcept { if (! isRecording() && ! isFinalizing()) backupCaptureFormat = f; }
        CaptureFormat getBackupCaptureFormat() const noexcept  { return backupCaptureFormat; }

        void setPreRollSeconds (int seconds);
        int  getPreRollSeconds() const noexcept               { return preRollSeconds; }

        // Decouples strip count from device input count -- engineer picks
        // how many tracks they want, routing chooses which device input
        // each strip captures. No-op while recording.
        void addTrack();
        void removeLastTrack();
        void setTrackCount (int n);

        int          getNumTracks() const noexcept { return (int) tracks.size(); }

        // ── Cross-thread access to the track vector ────────────────────────
        // The vector SHRINKS on the message thread (removeLastTrack / setTrackCount),
        // which FREES TrackStates. Off-thread readers -- the OSC receiver,
        // the MCU surface --
        // range-check getNumTracks() and then call getTrack(), which is a
        // TOCTOU: the vector can shrink between the two, and the read lands in
        // freed memory. condemnAllStrips() closes this for the UI, but it detaches
        // COMPONENTS -- it does nothing for a network thread.
        //
        // So: every structural change takes this lock, and every non-audio-thread
        // reader takes it around "check the count, then use the track". It is a
        // juce::CriticalSection (recursive), so nesting from the message thread
        // is safe. The AUDIO thread does NOT take it and must not -- its
        // relationship to the vector is unchanged by this.
        const juce::CriticalSection& getStructureLock() const noexcept { return structureLock; }

        // Bumped whenever the track SET changes (added / removed / cleared
        // + recreated on a device restart). UI watches this to rebuild
        // strips even when the COUNT is unchanged but the underlying
        // TrackState objects were replaced -- otherwise a cached
        // TrackState& dangles and crashes on the next paint/timer.
        int          getTrackGeneration() const noexcept { return trackGen.load (std::memory_order_relaxed); }
        // Bump the generation without changing the track COUNT -- e.g. a stereo
        // link/unlink changes how every view collapses the pair, so the MIXER /
        // EDIT rebuild + the meterbridge (which both watch this counter) must
        // re-run even though the count is unchanged.
        void         bumpTrackGeneration() noexcept { trackGen.fetch_add (1, std::memory_order_relaxed); }
        // NOT bounds-checked in release -- this is on the audio thread's hot
        // path (once per strip per block), so every caller must range-check
        // against getNumTracks() first. The debug assert catches the callers
        // that don't: an out-of-range index here is an unchecked
        // `*tracks[i]` past the end of the vector, which is how a stale
        // player-derived track count crashed the stereo-mix bounce.
        TrackState&  getTrack (int i) noexcept
        {
            jassert (i >= 0 && i < (int) tracks.size());
            return *tracks[(std::size_t) i];
        }

        // Health + position counters -- all RT-safe to read.
        juce::int64 getSamplesSinceStart() const noexcept { return samplesSinceStart.load(std::memory_order_relaxed); }
        juce::int64 getMissedSamples()     const noexcept
        {
            return missedSamples.load (std::memory_order_relaxed)
                 + latches->primaryLostFrames.load (std::memory_order_relaxed);
        }
        bool hasCaptureDeviceLost() const noexcept { return captureDeviceLost.load(); }
        void setMeterUnarmedInputs (bool on) noexcept { meterUnarmedInputs.store (on); }
        void markCaptureDeviceLost() noexcept { captureDeviceLost.store (true, std::memory_order_relaxed); }
        int         getLastWriteMs()       const noexcept { return lastWriteMs      .load(std::memory_order_relaxed); }

        // Apple-Silicon scheduling: when the engine learns about a new
        // audio device, it forwards the device's AudioWorkgroup here so
        // every writer worker can join it -- the macOS scheduler then
        // co-schedules them with the CoreAudio IO thread (no priority
        // inversion, no jitter under load). Pass an empty/disengaged
        // workgroup to revert to the default scheduler.
        void setAudioWorkgroup (juce::AudioWorkgroup);

        // Live telemetry for the CPU/disk dashboard:
        //   - bytesPerSec: rolling window of bytes written to disk, every copy.
        //   - ringFillPct: 0..100, worst of the audio FIFOs and the copies' rings.
        // Both are published by the distributor, which never blocks on a disk,
        // so a stuck drive shows here instead of freezing the reading.
        float getDiskBytesPerSec() const noexcept;
        float getRingFillPct()     const noexcept;
        juce::File  getActiveSessionDir()  const          { return activeSessionDir; }

        // Free-space pre-flight + live "minutes remaining" estimate.
        //
        // estimateBytesPerSecondForArmedTracks computes the worst-case
        // per-second disk-write rate for the current arm + format
        // configuration, summed across primary + (when active) backup.
        // Engineers see this before they hit record and can decide
        // whether to add storage or change format.
        //
        // estimateMinutesRemaining queries the free space on the
        // active session's volume (and the backup root's volume when
        // backup is active), divides by the projected rate, and
        // returns whichever drive runs out first. Returns 0 if no
        // session is active or no tracks are armed; returns INT_MAX
        // when free space is effectively unbounded. It stats every
        // destination, so it is for between takes only: once a take is
        // on disk the numbers come from readDiskEstimate.
        juce::int64 estimateBytesPerSecondForArmedTracks() const noexcept;
        int         estimateMinutesRemaining (const juce::File& primaryVolume,
                                              const juce::File& backupVolume) const noexcept;

        // What the take's volume probes last published. Reads atomics only, so
        // the window and the daemon can call it however dead a mount is. A copy
        // that stops answering is left out of `minutes` and flagged; it never
        // changes the primary's numbers. A primary that stops answering leaves
        // no number at all.
        struct DiskEstimate
        {
            bool        watching { false };       // a take is on disk
            int         minutes { 0 };            // 0 = not known
            juce::int64 primaryFreeBytes { -1 };  // -1 = not known
            bool        primaryStale { false };
            bool        copyStale { false };
        };
        DiskEstimate readDiskEstimate() const;

        // Disk-keep-up health, as a proxy for the SMART data we don't
        // (currently) read. Returns true if actual bytes/sec has been
        // < ~85 % of expected (estimateBytesPerSecondForArmedTracks)
        // for sustained windows -- means the disk is failing to drain
        // the ring as fast as audio arrives, missed samples are
        // imminent. UI surfaces this as "⚠ DISK STRUGGLING".
        bool isDiskStruggling() const noexcept
        {
            return diskStrugglingFlag.load (std::memory_order_relaxed);
        }
        // Update the disk-struggling flag from the latest sampled
        // throughput. Called from the UI's timer; cheap (one atomic
        // compare + EMA). Pass the current expected bytes/sec so the
        // recorder doesn't have to re-derive the arm + format setup.
        void updateDiskHealth (juce::int64 expectedBytesPerSec) noexcept;

        // SMART status query. macOS-only at present; shells out to
        // `diskutil info <mountPoint>` and parses the SMART Status
        // line. Slow (~50-200 ms blocking subprocess call) -- the
        // host calls this from a low-frequency timer (every 30 s),
        // not the audio path. Returns:
        //   Verified  -- drive's SMART controller reports healthy
        //   Failing   -- imminent hardware failure predicted
        //   Unknown   -- internal disk that doesn't report SMART
        //                (some NVMe + APFS containers), network mount,
        //                or query failed
        enum class SmartStatus { Verified, Failing, Unknown };
        static SmartStatus querySmartStatus (const juce::File& mountPoint);

        // Optional second copy of every track to a backup folder.
        // Pass an empty File to disable. Only effective for the next session.
        bool setBackupDirectory (const juce::File& dir);   // false, unchanged, while a take owns it
        juce::File getBackupDirectory() const               { return backupDir; }
        bool       isBackupActive() const noexcept
        {
            return backupActive.load (std::memory_order_relaxed)
                && ! latches->backupDropped.load (std::memory_order_relaxed);
        }

        // ── N-way mirror destinations ──────────────────────────────
        // Beyond primary + backup, the engineer can register any
        // number of extra mirror destinations -- each gets a full
        // parallel copy of every armed track, in its own configurable
        // format. Useful for broadcast / archival workflows that want
        // primary (fast SSD) + backup (different drive) + off-board
        // mirror (NAS / second machine) running concurrently.
        struct MirrorConfig
        {
            juce::File    root;
            CaptureFormat format { CaptureFormat::Wav24 };
        };
        // Returns false (and changes nothing) when a take is rolling -- the
        // caller MUST NOT persist a config the recorder just refused, or the
        // settings file and the live recorder disagree for the rest of the show.
        bool setMirrors (const std::vector<MirrorConfig>& configs);
        std::vector<MirrorConfig> getMirrors() const               { return mirrorConfigs; }

        // Why a mirror root is unusable, or an empty string when it's safe.
        //
        // A mirror writes to `root/<sessionName>/Audio Files/Track_NN.<ext>`,
        // which is the SAME shape the primary and backup use. So a root that
        // happens to be the session's parent resolves to the primary take's own
        // files -- two writers, one path, for the length of the show. The mirror
        // picker opens at ~/Music and the sessions root is ~/Music/Zynforge
        // Sessions, so "mirror into my sessions folder" is one click away and
        // destroys the take it was meant to protect. Same for two rows on one
        // drive, and for a root pointed at the backup destination.
        //
        // Checked at record start (colliding mirrors are skipped, never opened)
        // AND in the mirror picker, so the engineer hears about it at the desk
        // rather than at load-out.
        static juce::String mirrorRootRejection (const juce::File& root,
                                                 const juce::File& sessionDir,
                                                 const juce::File& backupRoot,
                                                 const std::vector<juce::File>& alreadyAccepted);

        // Mirrors dropped at the last record start (collision or unreachable
        // drive). A configured mirror that never opens produces no Mirror entry,
        // so anyMirrorFailed() structurally cannot see it -- this is how the
        // banner reports "you have fewer copies than you configured".
        int getMirrorsSkippedAtStart() const noexcept
        {
            return mirrorsSkippedAtStart.load (std::memory_order_relaxed)
                 + latches->mirrorsDropped.load (std::memory_order_relaxed);
        }
        // Any mirror failed during the active take? Cleared at start.
        bool anyMirrorFailed() const noexcept;
        bool       hasBackupFailed() const noexcept         { return backupFailed.load (std::memory_order_relaxed); }
        bool hasRecoveryMarkerFailed() const noexcept
        {
            return recoveryMarkerFailed.load (std::memory_order_relaxed);
        }
        bool hasReportWriteFailed() const noexcept
        {
            return reportWriteFailed.load (std::memory_order_relaxed)
                || hasAsyncReportFailure();
        }
        bool hasAsyncReportFailure() const noexcept
        { return reportAsyncState->failed.load (std::memory_order_relaxed); }
        // One line per copy that lost audio or stopped responding, naming it,
        // for the banner. Empty for a clean take.
        juce::StringArray getCopyProblems() const;
        bool hasPunchSpliceFailed() const noexcept
        {
            return punchSpliceFailed.load (std::memory_order_relaxed);
        }

    private:
        // Inherited but no longer used directly: the distributor is a
        // ShardClient. Kept as a stub returning a long delay so the base class
        // doesn't compile-error.
        int useTimeSlice() override;

        // The distributor: moves samples from the audio FIFOs of channels
        // [firstChannel, lastChannel) to every copy's ring on its own
        // juce::TimeSliceThread. It never touches a disk.
        struct ShardClient final : public juce::TimeSliceClient
        {
            MultitrackRecorder* owner          { nullptr };
            int                 firstChannel   { 0 };
            int                 lastChannel    { 0 };

            // Per-shard throughput accumulator + per-shard publish atomic
            // so the global getDiskBytesPerSec() can sum without ever
            // taking a lock.
            std::atomic<float>  shardBytesPerSec      { 0.0f };
            juce::int64         throughputBaseBytes   { 0 };
            double              throughputWindowMs    { 0.0 };
            std::atomic<int>    shardRingFillPct      { 0 };

            // Serialises drainShard() for THIS shard. The FIFO is single-
            // reader, but the shard's own TimeSliceThread and a fan-out
            // drainOnce() (stopRecording's final flush, or the test harness's
            // drainPendingForTests) can both try to drain it -- two readers
            // corrupt the FIFO + the part-file/byte bookkeeping. Per-shard
            // (not global) so different shards still drain in parallel.
            // Never taken on the audio thread (drainShard is consumer-side).
            juce::CriticalSection drainLock;

            // Reused planar staging buffers: the L + R FIFOs each hand back a
            // possibly-wrapped two-region read, and the copies take contiguous
            // arrays. Owned by the shard so a steady take never reallocates.
            std::vector<float> stageL, stageR;

            int useTimeSlice() override;
        };

        struct ChannelFifo
        {
            juce::AbstractFifo fifo { 1 };
            std::vector<float> data;
            // Audio and gaps occupy the SAME absolute capture timeline. All
            // cursor/descriptor access uses captureBoundary; sample copies and
            // disk writes never hold that lock on the consumer. Fixed metadata
            // costs 4 KiB per channel, independent of take duration.
            struct Gap { juce::int64 start {}, length {}; };
            static constexpr size_t gapCapacity = 256;
            std::array<Gap, gapCapacity> gaps {};
            size_t gapHead {}, gapCount {};
            juce::int64 producedFrames {}, consumedFrames {};

            struct DrainPlan
            {
                struct Span { int offset {}, length {}; bool silence {}; };
                std::array<Span, 2 * gapCapacity + 1> spans {};
                size_t count {};
                juce::AbstractFifo::ScopedRead samples;
            };

            bool canAcceptAudio() const noexcept { return gapCount < gapCapacity; }

            void accountCapture (int written, int requested) noexcept
            {
                const int lost = requested - written;
                if (lost > 0)
                {
                    const auto start = producedFrames + written;
                    const auto tail = (gapHead + gapCount + gapCapacity - 1) % gapCapacity;
                    if (gapCount > 0 && gaps[tail].start + gaps[tail].length == start)
                        gaps[tail].length += lost;
                    else
                    {
                        // A saturated descriptor ring refuses further real
                        // samples until a gap is consumed. They extend its last
                        // contiguous gap and are counted in missedSamples.
                        jassert (gapCount < gapCapacity);
                        gaps[(gapHead + gapCount++) % gapCapacity] = { start, lost };
                    }
                }
                producedFrames += requested;
            }

            void reserveTimeline (DrainPlan& plan, int frames) noexcept
            {
                int offset = 0, realFrames = 0;
                while (offset < frames)
                {
                    const bool silence = gapCount > 0 && gaps[gapHead].start == consumedFrames;
                    const auto run = silence ? gaps[gapHead].length
                        : (gapCount > 0 ? gaps[gapHead].start - consumedFrames : frames - offset);
                    const int count = (int) juce::jmin ((juce::int64) (frames - offset), run);
                    jassert (count > 0 && plan.count < plan.spans.size());
                    plan.spans[plan.count++] = { offset, count, silence };
                    if (silence)
                    {
                        auto& gap = gaps[gapHead];
                        gap.start += count; gap.length -= count;
                        if (gap.length == 0) { gapHead = (gapHead + 1) % gapCapacity; --gapCount; }
                    }
                    else realFrames += count;
                    offset += count; consumedFrames += count;
                }
                plan.samples = fifo.read (realFrames);
                jassert (plan.samples.blockSize1 + plan.samples.blockSize2 == realFrames);
            }

            void resetCaptureTimeline() noexcept
            { gapHead = gapCount = 0; producedFrames = consumedFrames = 0; }

            void resize (int size)
            {
                data.assign ((std::size_t) size, 0.0f);
                fifo.setTotalSize (size);
                fifo.reset();
                resetCaptureTimeline();
            }
        };

        // What one track contributes to a take, whichever copies it lands on.
        // The files themselves belong to each CaptureCopy.
        struct WriterChannel
        {
            // Strip names for each file's bext Description. Read once at record
            // start: a roll opens a new part on a copy's thread, which must not
            // touch TrackState::name.
            juce::String bextNameLeft, bextNameRight;
            // Every WAV this pass opened, and what it was stamped with at open:
            // the finalize patch works from this.
            struct StampedPart
            {
                juce::File   file;
                juce::int64  framesBefore { 0 };     // of this pass, in earlier parts
                juce::uint64 stampedAtOpen { 0 };
            };

            // Interleaved-stereo capture: a stereo pair (isStereo on the L
            // track) records ONE 2-channel file named for the L slot --
            // Track_NN.wav with two channels -- instead of two mono files.
            // The R partner's WriterChannel is left empty; its FIFO is drained
            // here, by the L channel's step, which reads both FIFOs. Every copy
            // carries the same channel count, so one field governs them all.
            // 1 = mono (the default, every non-stereo track), 2 = interleaved.
            int          numChannels { 1 };

            // Does this channel PARTICIPATE in the take? The drain keys on this
            // rather than on any one copy's file, so a primary that failed or
            // never opened does not stop its backup and mirrors. Empty R slots
            // and unarmed channels stay false and are skipped.
            bool         active { false };
        };

        // Per-channel rolling history used for pre-roll. The audio thread is
        // the only writer (push, continuously -- NOT gated on recording); each
        // copy's thread reads the detached history once per take.
        // No lock: push publishes its data writes with a release store on
        // totalWritten that readHistory pairs with an acquire load, and the
        // ring is over-allocated by kPreRollSafetySec seconds BEYOND the
        // requested pre-roll so an in-flight push during the (sub-millisecond)
        // read can never wrap around and overwrite the window being copied.
        struct PreRollBuffer
        {
            std::vector<float>       data;
            std::atomic<juce::int64> totalWritten { 0 };

            void allocate (int samples) noexcept
            {
                data.assign ((std::size_t) samples, 0.0f);
                totalWritten.store (0, std::memory_order_release);
            }

            void push (const float* src, int n) noexcept;

            // Reads up to samplesWanted of the most recent history into dest.
            // Returns how many were actually available.
            int readHistory (float* dest, int samplesWanted) const noexcept;
        };

        void drainOnce();
        void drainShard (ShardClient&);
        void rebuildShards();
        void closeWriters();
        void allocatePreRollBuffers();
        // Pre-roll: goLive sizes it, then each copy's thread writes the history
        // before its first live samples (see goLive).
        void preparePreRollDump();
        int  preRollHistorySamples { 0 };

        // One destination of a take, with its own thread and ring (CaptureCopy.h).
        struct CaptureCopy;
        // Failure flags outlive the recorder: see CaptureCopy.
        struct CaptureLatches
        {
            std::atomic<bool> primaryFailed { false }, backupFailed { false }, mirrorFailed { false };
            std::atomic<juce::int64> primaryLostFrames { 0 };
            // Copies that could not join after go-live. A copy's thread can
            // outlive the recorder, so the news it leaves lives here.
            std::mutex        droppedLock;
            juce::StringArray dropped;
            std::atomic<int>  mirrorsDropped { 0 };
            std::atomic<bool> backupDropped { false };
        };
        std::shared_ptr<CaptureLatches> latches { std::make_shared<CaptureLatches>() };
        // Primary, then the backup if it is in play, then each active mirror.
        // Replaced only at record start, under structureLock.
        std::vector<std::shared_ptr<CaptureCopy>> copies;
        // Left behind at STOP and still stuck: their drive is not touched again
        // until the thread comes back, so the next RECORD cannot hang on it.
        std::vector<std::shared_ptr<CaptureCopy>> leftBehind;
        bool isStillLeftBehind (const juce::File& root) const;
        juce::StringArray droppedCopies;   // left out of this take, naming each

        // Every destination of the take on disk, each with its own probe.
        // Built once before go-live and only read until finalization drops it.
        struct WatchedVolume
        {
            std::shared_ptr<VolumeProbe> probe;
            std::shared_ptr<const CaptureCopy> copy;
            int bytesPerSample { 0 };
        };
        struct DiskWatch
        {
            std::vector<WatchedVolume> volumes;
            double sampleRate { 0.0 };
            double startedMs { 0.0 };
            double staleAfterMs { 0.0 };
        };
        void startDiskWatch();
        std::shared_ptr<const DiskWatch> diskWatch;   // std::atomic_load / store only
        const CaptureCopy* copyAt (int copyIndex) const noexcept;
        bool isCopyOutOfTake (int copyIndex) const noexcept;
        int  copyRingFrames (int numCopies, int channels) const noexcept;
        juce::StringArray describeCopyProblems() const;
        void finishCopies();

        // ── startRecording stages ───────────────────────────────────────
        // startRecording (MultitrackRecorder.cpp) runs these in order under
        // structureLock. TakeStart is the working state of ONE call and is
        // defined in the .cpp. A stage that returns false refuses the take;
        // from stashPunchBases on, a refusing stage undoes its own media moves.
        struct TakeStart;
        struct TakeFormat { int container { 0 }; int bitDepth { 24 }; const char* ext { ".wav" }; };
        // What a copy of a track must match to prove it holds the take.
        struct TakeBase { bool exists { false }, readable { false }; juce::int64 length { 0 }; int channels { 0 }; double rate { 0.0 }; };
        struct FormatCheck { int track { 0 }; int channels { 1 }; bool rightOfPair { false }; };
        struct LeadInPlan
        {
            bool editPass { false }, continuing { false }, punching { false };
            juce::int64 timelineStart { 0 };
            double sampleRate { 48000.0 };
        };
        // Roots the copies of a take have settled on, compared by canonical path
        // outside any lock: a path on a dead drive can block for good.
        struct CopyRoots
        {
            std::mutex lock;
            std::vector<juce::File> roots;
            bool claim (const juce::File& canonical)
            {
                const std::lock_guard<std::mutex> guard (lock);
                if (std::find (roots.begin(), roots.end(), canonical) != roots.end()) return false;
                roots.push_back (canonical);
                return true;
            }
        };
        std::shared_ptr<CopyRoots> openingRoots;
        struct CopyOpen;
        static TakeFormat takeFormatFor (CaptureFormat) noexcept;
        bool isArmedCapture (std::size_t track) const noexcept;
        std::vector<FormatCheck> formatChecks() const;
        bool opensOwnCaptureFile (std::size_t track) const noexcept;
        bool validateTakeStart (const TakeStart&);
        bool createTakeFolders (TakeStart&);
        void resolveTakeMirrors (const TakeStart&);
        enum class CopyClaim { claimed, conflict, unwritable };
        bool claimTakeIdentity (TakeStart&);
        bool recoverPrimaryPunches (const TakeStart&);
        static bool adoptExistingTakeFormat (const juce::File& copyAudioDir, TakeFormat& selected,
                                             const std::vector<FormatCheck>&, double sampleRate,
                                             juce::String& problem);
        bool resolveTakeFormats (TakeStart&);
        void prepareBackupCopy (TakeStart&);
        static bool orphanedContinuation (const juce::File& dir, const juce::String& stem,
                                          const juce::String& ext);
        bool continuationBasesIntact (const TakeStart&);
        void noteExistingPrimaryBases (TakeStart&);
        void invalidateReportsBeforeMediaMutation (TakeStart&);
        bool stashForPunch (TakeStart&, const juce::File& target);
        static bool stashPunchParts (const juce::File& target,
                                     std::vector<std::pair<juce::File, juce::File>>& stashes);
        void restorePunchStashes (TakeStart&);
        bool stashPunchBases (TakeStart&);
        bool measureContinuationBase (TakeStart&);
        void openTakeWriters (TakeStart&);
        bool writeLeadInMarkers (TakeStart&);
        void abandonTakeStart (TakeStart&);
        void goLive (const TakeStart&);
        void beginChannelAlarms();

        // ── finishFinalization stages ───────────────────────────────────
        // TakeStop: totals + each writer's files, captured before the
        // writers close. SessionReport: everything session.report.json says,
        // by value, so the background hashing thread can rebuild it.
        struct TakeStop;
        struct CopySplice;
        struct CopyFinish;
        void waitForCopies (double totalLimitMs);
        void finishCopyFiles (TakeStop&);
        void noteCopyProblem (const juce::String&);
        void completeLeadIns (const TakeStop&);
        struct SessionReport;
        void quiesceWritersForStop();
        TakeStop snapshotTakeAtStop();
        void reattachWriterShards();
        void splicePunchedTracks (TakeStop&);
        void stampTakeFiles (TakeStop&);
        void manifestSessionMedia (TakeStop&);
        SessionReport buildSessionReport (TakeStop&);
        void publishSessionReport (SessionReport);

        // Samples since midnight of this pass's first written frame, not yet
        // wrapped into a day. An estimate until the first live block lands.
        juce::int64 passStartSamples() const noexcept;

    public:
        // Safe byte-count ceiling per container. WAV / FLAC: 3.9 GiB.
        // AIFF: 1.9 GiB. (The actual format limits are 4 GiB unsigned
        // for RIFF and 2 GiB signed for AIFF; we cut some margin so
        // header rewrites + tail flushes don't push over the line.)
        // containerCode: 0 = WAV, 1 = AIFF, 2 = FLAC.
        static juce::int64 maxBytesForContainer (int containerCode) noexcept;

        // Test hook: when > 0, overrides maxBytesForContainer for all
        // formats so a unit test can force the auto-split path without
        // writing actual gigabytes. 0 = use real limits (production).
        static void setAutoSplitThresholdBytesForTests (juce::int64 bytes) noexcept;

        // Test hook: force a drain and wait for every copy to write it.
        // Production code lets the threads schedule this; tests feed the IO
        // callback synchronously and need a way to flush the ring between
        // blocks so the auto-split path doesn't roll one gigantic write.
        void drainPendingForTests();
        // Test hook: wait until every copy has joined the take or left it.
        void waitForCopiesOpenForTests();

        // Test hook: rewrite every open writer's header right now (the same
        // thing the periodic in-take flush does), so a test can verify that a
        // crashed-but-flushed file is already readable before stopRecording.
        void flushOpenWritersForTests();

        // Report-write generation, keyed by SESSION.
        //
        // stopRecording writes session.report.json synchronously, then a
        // BACKGROUND thread hashes the audio (GBs) and rewrites it with the
        // SHA-256s. Two takes stopped close together put two hash threads on
        // the SAME file, and the slower one wins -- so the integrity manifest
        // could end up describing the PREVIOUS take. Worse, a punch splice
        // REPLACES the take while an earlier take's hash thread may still be
        // reading it, so that thread's hashes are of a file being overwritten.
        //
        // Each stop claims a generation for its session; a background write is
        // dropped once a newer stop has claimed one. Keyed by session PATH and
        // process-wide, because the racing stops are frequently different
        // recorder instances writing one session -- a per-recorder counter (the
        // first version of this fix) doesn't serialise them at all.
        //
        // Returns a shared token so the background thread can capture it BY
        // VALUE: that thread deliberately never captures `this`, so the token
        // has to be able to outlive the recorder.
        static std::shared_ptr<std::atomic<juce::int64>> claimReportGeneration (
            const juce::File& sessionDir, juce::int64& outGeneration);

        // Blocks a worker until this session's report hashing has finished, so
        // a move or copy never races it. False if `cancel` was raised first.
        // watchReportFile: the hashing may be in the capture daemon's process,
        // so also wait for the report to stop saying it is pending, for at most
        // half an hour: a report left pending by a crash would never change.
        static bool waitForReportHashes (const juce::File& sessionDir, const std::atomic<bool>& cancel,
                                         bool watchReportFile = false);

        // Cancel obsolete report jobs and exclude their publication while a
        // caller removes an idle session. Caller must first guard active capture.
        static bool deleteSessionAfterCancellingReports (const juce::File& sessionDir);
        // Runs `remove` once no report for this session can still be published.
        static bool removeSessionAfterCancellingReports (const juce::File& sessionDir,
                                                         const std::function<bool()>& remove);

        // Edit the session.report.json on disk under the lock every report
        // writer holds. False when there is no readable report to edit. Keys
        // other than `twin` are replaced by the next report this session writes.
        static bool editSessionReport (const juce::File& sessionDir,
                                       const std::function<void (juce::DynamicObject&)>& edit);
        // The lock file shared with the other process that writes this report.
        static constexpr const char* kReportLockName = "session-report.lock";

    private:

        // Audit-only scheduling seam. Tests install this before stop, and the
        // worker owns a snapshot; normal capture never installs callbacks.
        friend class EditViewAuditTests;           // EDIT post-STOP rescan fixture
        friend struct ConcurrencyAuditAccess;
        friend class MainTransportRegressionTests; // deterministic STOP failure-latch fixtures
        friend class AsyncStopAuditTests;
        friend class ReviewCaptureRepairTests;
        friend class CaptureCopyTests;
        friend class CaptureFixTests;
        friend class CaptureDaemonLinkTests;
        friend class PunchSessionGuardTests;   // splice-failure fixtures
        friend class TwinLinkTests;            // report publication ordering
        friend class CaptureMetaTests;
        friend class ChannelAlarmTests;        // what the daemon's loop latched
        std::function<void()> afterFifoReservationForTests;
        std::function<void()> beforeStampForTests;
        std::function<void (CaptureCopy&)> beforeCopyWriteForTests;
        int copyRingFramesForTests { 0 };
        // How long STOP waits on a copy that has stopped making progress, and on
        // a copy's drain altogether: a buffered write counts as progress, so a
        // dying drive could otherwise feed the first limit for hours.
        double copyStallLimitMs { 30000.0 };
        double copyDrainLimitMs { 60000.0 };
        std::function<void (CaptureCopy&)> beforeCopyFinishForTests;
        std::function<void (CaptureCopy&)> beforeCopyOpenForTests;
        VolumeProbe::Reader volumeReaderForTests;
        int    volumeProbePeriodMs { 2000 };
        double volumeStaleAfterMs { 15000.0 };
        struct ReportTestHooks
        {
            std::function<void()> beforeScan, beforeCopyScan, beforePublish, afterPublish, finished;
        };
        std::shared_ptr<ReportTestHooks> reportTestHooks;
        std::function<void()> beforeFinalizationForTests;
        std::atomic<bool> finalizing { false };

        double sampleRate { 48000.0 };
        int    blockSize  { 512 };

        std::vector<std::shared_ptr<TrackState>>     tracks;   // shared: UI surfaces hold keep-alives (TrackState.h)
        // Guards STRUCTURAL changes to `tracks` against off-thread readers.
        // See getStructureLock(). Not taken by the audio thread.
        mutable juce::CriticalSection                structureLock;
        std::atomic<int>                             trackGen { 0 };   // bumped on any track-set change
        std::vector<std::unique_ptr<ChannelFifo>>    fifos;
        std::vector<WriterChannel>                   writers;
        std::vector<std::unique_ptr<PreRollBuffer>>  preRoll;
        std::vector<std::unique_ptr<PreRollBuffer>>  preRollSnapshot;
        // Control side holds this only for pointer swaps/transport publication.
        juce::SpinLock captureBoundary;
        std::atomic<bool> meterUnarmedInputs { false };

        juce::String startError;
        juce::AudioFormatManager formatManager;

        // The distributor's thread (see chooseShardCount). rebuildShards()
        // reshapes the assignment when the track count changes.
        std::vector<std::unique_ptr<juce::TimeSliceThread>> writerThreads;
        std::vector<std::unique_ptr<ShardClient>>           shards;

        // Current audio device workgroup + a generation counter so the
        // shard threads know when to (re-)join.
        juce::CriticalSection workgroupLock;
        juce::AudioWorkgroup  currentWorkgroup;
        std::atomic<int>      workgroupGeneration { 0 };
        void joinAudioWorkgroupOnCurrentThread (juce::WorkgroupToken&);

        std::atomic<bool>        recording          { false };
        // Immutable between recording's release/acquire publication and stop.
        std::vector<bool>        captureArms;
        std::atomic<bool>        writersReady       { false };
        std::atomic<juce::int64> samplesSinceStart  { 0 };
        std::atomic<juce::int64> missedSamples      { 0 };

        // BWF time reference for this pass. The anchor and hint are set before
        // the writers open and read-only after. The audio thread stores the
        // first live block's clock reading and size (ticks last, so a reader
        // that sees the ticks sees the size), so nothing there converts time.
        bwf::TimeAnchor          timeHint, timeAnchor;
        bool                     hasTimeHint { false };
        std::atomic<juce::int64> firstLiveTicks     { 0 };   // 0 = none yet
        std::atomic<int>         firstLiveBlock     { 0 };

        // Dead-channel alarms. alarmFrames counts the frames the callback has
        // delivered this take (samplesSinceStart cannot serve: pre-roll lands
        // in it after capture goes live). alarmLock covers the monitor; the
        // audio thread never takes it.
        std::atomic<juce::int64> alarmFrames        { 0 };
        juce::CriticalSection                    alarmLock;
        channelalarm::Monitor                    alarmMonitor;
        std::vector<std::shared_ptr<TrackState>> alarmTracks;
        std::vector<juce::int64>                 alarmScratch;
        bool                                     alarmsRunning { false };
        void updateChannelAlarmsLocked();
        juce::int64 alarmOrigin() const noexcept;

        juce::String       clockDevice;
        clocksource::Probe clockProbe;
        std::atomic<bool>        captureDeviceLost  { false };
        std::atomic<int>         lastWriteMs        { 0 };
        CaptureFormat captureFormat        { CaptureFormat::Wav24 };
        CaptureFormat backupCaptureFormat  { CaptureFormat::Wav24 };
        int           preRollSeconds { 0 };  // 0 = disabled

        juce::File activeSessionDir;
        juce::File backupDir;
        std::atomic<bool> backupActive  { false };
        std::atomic<bool>& backupFailed  { latches->backupFailed };
        // Latched for the whole take: a copy's objects are gone after STOP, but
        // status and report consumers still need to know.
        std::atomic<bool>& mirrorFailed  { latches->mirrorFailed };
        // Metadata failures do not stop audio capture, but must be visible:
        // without the marker crash recovery is unavailable; without the report
        // the operator loses the take's integrity manifest.
        std::atomic<bool> recoveryMarkerFailed { false };
        std::atomic<bool> reportWriteFailed { false };
        // The final hash/write runs after stopRecording returns and cannot
        // capture `this`: the recorder may be destroyed before it finishes.
        // Each new take invalidates older completion notifications.
        struct AsyncReportState
        {
            std::atomic<juce::uint64> generation { 0 };
            std::atomic<bool> failed { false };
        };
        std::shared_ptr<AsyncReportState> reportAsyncState { std::make_shared<AsyncReportState>() };
        std::atomic<bool> punchSpliceFailed { false };

        // Primary-writer failure: a write failed (disk full, path gone) or the
        // primary fell a whole ring behind. The audio thread keeps capturing;
        // the engineer loses the primary's audio from there. UI surfaces this
        // so the operator can act -- audio continues to land on backup + any
        // mirrors.
        std::atomic<bool>& primaryFailed { latches->primaryFailed };

        // Disk-struggling flag + 2-second EMA of the keep-up ratio.
        // Updated by updateDiskHealth() from the UI timer; the audio
        // thread + writer threads don't read these.
        std::atomic<bool>  diskStrugglingFlag { false };
        float              diskKeepUpEma      { 1.0f };  // 0..1, 1.0 = caught up
        int                diskStrugglingStreak { 0 };   // consecutive low samples
    public:
        bool hasPrimaryFailed() const noexcept
        {
            return primaryFailed.load (std::memory_order_relaxed);
        }
    private:

        // Extra mirror destinations beyond primary + backup. Editable
        // only while not recording; the active take captures whatever
        // was configured at startRecording.
        std::vector<MirrorConfig> mirrorConfigs;
        // The subset of mirrorConfigs that actually opened for the live take:
        // collisions and unreachable drives are filtered out at record start.
        // WriterChannel::mirrors is built from THIS list, so anything indexing
        // a mirror by position (the session report) must index this one -- not
        // the configured list, which can be longer.
        std::vector<MirrorConfig> activeMirrors;
        std::atomic<int>          mirrorsSkippedAtStart { 0 };

        std::vector<float> scratch;

        // Punch-in state for the active take (set by armPunchIn, cleared in
        // stopRecording). punchInPos is the timeline sample the freshly-recorded
        // audio is spliced in at.
        std::atomic<bool> punchInActive { false };
        juce::int64 punchInPos    { 0 };
        // Where the first captured sample of a punch sat on the timeline, set
        // once by the audio thread (-1 = not yet, or playback not rolling).
        // Start work runs while playback rolls, so capture begins after the
        // armed point; the splice uses this so the new audio lands where it
        // was played, not hundreds of ms early.
        std::atomic<juce::int64> punchCaptureOrigin { -1 };

    public:
        // ── Deferred lead-ins ───────────────────────────────────────────
        // A copy that has less audio than the take's timeline position (a
        // newly armed track on a continue or punch, or a backup/mirror added
        // mid-session) needs silence before this pass's audio. Writing it at
        // record start held RECORD for as long as the write took (GBs on a
        // long take), so capture starts at once instead: the new audio goes
        // into a later part slot, and the reserved slots are filled with
        // silence after STOP (finalization), using the real timeline start.
        // A marker in "<copy>/Audio Files/Pending Lead-ins/" lets session open
        // (recoverInterruptedPunches) finish the job after a crash.
        struct PendingLeadIn
        {
            juce::File   audioDir;               // the copy's Audio Files folder
            juce::String trackName, ext;         // "Track_05", ".wav"
            int          firstPart { 1 }, parts { 1 };   // reserved part slots
            int          container { 0 }, bitDepth { 24 }, channels { 1 };
            double       sampleRate { 48000.0 };
            juce::int64  priorLength   { 0 };    // samples this copy had before the pass
            juce::int64  timelineStart { 0 };    // where the pass begins (start-time estimate)
            int          copyIndex { 0 };        // 0 primary, 1 backup, 2+ mirror (not persisted)
        };
        static juce::File partFileFor (const juce::File& audioDir, const juce::String& trackName,
                                       const juce::String& ext, int part);
        // Fills the reserved slots with (timelineStart - priorLength) samples
        // of silence. Skips (true) when this pass wrote no audio to the copy.
        static bool writeLeadIn (const PendingLeadIn&, juce::int64 timelineStart);
        static bool finishLeadIn (const PendingLeadIn&, juce::int64 timelineStart);
        static bool writeLeadInMarker (const PendingLeadIn&);
        static bool openTrackFile (CaptureCopy&, std::size_t track, const juce::File& dir, const TakeFormat&,
                                   const juce::String& trackName, int channels, const LeadInPlan&,
                                   bool primaryHadBase, juce::int64 prior, std::vector<PendingLeadIn>&);
        // Completes every marker left in audioDir by an interrupted take.
        static bool recoverPendingLeadIns (const juce::File& audioDir);

    private:
        std::vector<PendingLeadIn> pendingLeadIns;   // this take's reservations
        static juce::File leadInMarkerFor (const PendingLeadIn&);
        static bool writeSilenceFile (const juce::File&, int container, int bits, int chans,
                                      double sampleRate, juce::int64 frames);
        std::atomic<bool> continueAsPart { false };   // append a new part instead of overwriting
        std::atomic<juce::int64> recordBaseSamples { 0 };       // timeline offset for a continue take

        // Sidecar path that holds an existing take while a punch records over
        // it: "Audio Files/Track_01.wav" -> "Audio Files/Track_01.punchbase.wav".
        static juce::File punchSidecar (const juce::File& take)
        {
            return take.getParentDirectory().getChildFile (
                take.getFileNameWithoutExtension() + ".punchbase" + take.getFileExtension());
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackRecorder)
    };
}
