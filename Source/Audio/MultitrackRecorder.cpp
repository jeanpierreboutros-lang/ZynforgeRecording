#include "MultitrackRecorder.h"
#include "AiffRepair.h"
#include "CaptureCopy.h"
#include "FloatAiffWriter.h"
#include "AtomicFile.h"
#include "FastHash.h"
#include "FlacRepair.h"
#include "PunchSplice.h"
#include "MultiPartReader.h"
#include "PreflightProbes.h"
#include "TrackOrderLog.h"
#include "WavRepair.h"
#include "../Network/CaptureIdentity.h"

#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>

#if JUCE_MAC
 #include <pthread/qos.h>
#endif

namespace zynforge
{
    namespace
    {
        struct SessionReportState
        {
            std::mutex publication;
            std::atomic<juce::int64> generation { 0 };
            std::atomic<int> runningScans { 0 };   // hash threads alive for this session
        };

        // `publication` only spans one process, and with a capture daemon the
        // report has two: the daemon publishes hashes while the GUI merges the
        // twin block, each reading the file and writing it back. flock works
        // across them, and the kernel lets go if the holder dies. The wait is
        // bounded because every hold is one small read and write: a holder that
        // is stuck must not cost the report.
        class ReportFileLock
        {
        public:
            ReportFileLock() : file (capture::identity::Endpoint::openSharedFile (MultitrackRecorder::kReportLockName))
            {
                const auto giveUp = juce::Time::getMillisecondCounterHiRes() + kWaitMs;
                while (file >= 0 && ::flock (file, LOCK_EX | LOCK_NB) != 0
                       && juce::Time::getMillisecondCounterHiRes() < giveUp)
                    juce::Thread::sleep (2);
            }
            ~ReportFileLock() { if (file >= 0) ::close (file); }   // closing releases it

        private:
            static constexpr double kWaitMs = 3000.0;
            int file;
            JUCE_DECLARE_NON_COPYABLE (ReportFileLock)
        };

        std::shared_ptr<SessionReportState> sessionReportState (const juce::File& directory)
        {
            static std::mutex registryLock;
            static std::map<juce::String, std::weak_ptr<SessionReportState>> registry;
            const std::lock_guard<std::mutex> lock (registryLock);
            for (auto it = registry.begin(); it != registry.end();)
                if (it->second.expired()) it = registry.erase (it); else ++it;
            auto& weak = registry[directory.getFullPathName()];
            auto state = weak.lock();
            if (! state) { state = std::make_shared<SessionReportState>(); weak = state; }
            return state;
        }
    }

    static constexpr int kFifoSeconds     = 4;   // per-channel FIFO length
    static constexpr int kPreRollSafetySec = 2;  // extra buffer beyond user setting

    void MultitrackRecorder::PreRollBuffer::push (const float* src, int n) noexcept
    {
        const int size = (int) data.size();
        if (size <= 0 || src == nullptr) return;
        const auto startPos = totalWritten.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            data[(std::size_t) ((startPos + i) % size)] = src[i];
        totalWritten.store (startPos + n, std::memory_order_release);
    }

    int MultitrackRecorder::PreRollBuffer::readHistory (float* dest, int samplesWanted) const noexcept
    {
        const int size = (int) data.size();
        if (size <= 0 || samplesWanted <= 0) return 0;
        const auto pos      = totalWritten.load (std::memory_order_acquire);
        const auto cap      = (juce::int64) size;
        const auto wanted   = juce::jmin ((juce::int64) samplesWanted,
                                          juce::jmin (pos, cap));
        const auto startPos = pos - wanted;
        for (juce::int64 i = 0; i < wanted; ++i)
            dest[(std::size_t) i] = data[(std::size_t) ((startPos + i) % size)];
        return (int) wanted;
    }

    static int chooseShardCount()
    {
        // One distributor. It only moves samples from the audio FIFOs to each
        // copy's ring, so one thread keeps up with any channel count, and the
        // disk work is parallel per destination (CaptureCopy). A writer must
        // only ever be touched by one thread: the corruption that once forced
        // this to 1 was a flush of every writer from whichever shard crossed
        // the five-second mark, which seeked other threads' files mid-write.
        return 1;
    }

    MultitrackRecorder::MultitrackRecorder()
    {
        formatManager.registerBasicFormats();

        const int n = chooseShardCount();
        for (int i = 0; i < n; ++i)
        {
            auto t = std::make_unique<juce::TimeSliceThread> (
                "ZF Recorder Writer " + juce::String (i + 1));
            t->startThread();
            writerThreads.push_back (std::move (t));
        }
    }

    MultitrackRecorder::~MultitrackRecorder()
    {
        stopRecording();
        // Unhook clients before joining so a thread isn't iterating a
        // stale client list while the destructor unwinds.
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());
        for (auto& th : writerThreads)
            th->stopThread (-1);   // any other timeout kills it mid-write
    }

    void MultitrackRecorder::prepare (double sr, int maxBlock, int numInputs)
    {
        const juce::ScopedLock structureGuard (structureLock);
        if (isFinalizing()) return;
        stopRecording();
        // Detach existing shards from their threads -- they'll be rebuilt.
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());
        shards.clear();

        sampleRate = sr;
        blockSize  = maxBlock;

        const int fifoSize = juce::nextPowerOfTwo ((int) (sr * kFifoSeconds));
        scratch.assign ((std::size_t) maxBlock, 0.0f);

        // Preserve existing TrackState objects when the count hasn't
        // changed -- destroying them here would leave every ChannelStrip
        // / EditPage row holding a dangling reference (those views grab
        // a TrackState& at construction). Heap corruption results when
        // those references are later dereferenced.
        // Only reshape the storage; resize the FIFO + pre-roll to match
        // the new sample rate; keep the TrackState atomics intact.
        if ((int) tracks.size() != numInputs)
        {
            tracks.clear();
            fifos.clear();
            preRoll.clear();
            for (int i = 0; i < numInputs; ++i)
            {
                auto t = std::make_shared<TrackState>();
                t->setNameThreadSafe (juce::String (i + 1));
                tracks.push_back (std::move (t));

                auto f = std::make_unique<ChannelFifo>();
                f->resize (fifoSize);
                fifos.push_back (std::move (f));

                preRoll.push_back (std::make_unique<PreRollBuffer>());
            }
            trackGen.fetch_add (1, std::memory_order_relaxed);   // TrackState set replaced
        }
        else
        {
            // Same track count -- just resize the existing FIFOs to the
            // new sample rate. TrackState objects are reused so the UI's
            // references stay valid.
            for (auto& f : fifos)
                f->resize (fifoSize);
        }
        allocatePreRollBuffers();

        rebuildShards();
    }

    void MultitrackRecorder::rebuildShards()
    {
        // Detach + recreate. We never resize the thread pool itself --
        // only the per-shard channel ranges change, so the engineer
        // doesn't pay a thread-creation cost on every track count change.
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());
        shards.clear();

        const int total = (int) fifos.size();
        const int nThreads = (int) writerThreads.size();
        if (total <= 0 || nThreads <= 0) return;

        const int perShard = juce::jmax (1, (total + nThreads - 1) / nThreads);
        int first = 0;
        int shardIndex = 0;
        while (first < total)
        {
            const int last = juce::jmin (total, first + perShard);
            auto sc = std::make_unique<ShardClient>();
            sc->owner        = this;
            sc->firstChannel = first;
            sc->lastChannel  = last;
            // Round-robin shards onto threads so each thread gets at
            // most one shard but the pool itself is reusable across
            // restarts without thread churn.
            writerThreads[shardIndex % nThreads]->addTimeSliceClient (sc.get());
            shards.push_back (std::move (sc));
            first = last;
            ++shardIndex;
        }
    }

    bool MultitrackRecorder::setBackupDirectory (const juce::File& dir)
    {
        if (isRecording() || isFinalizing()) return false;
        backupDir = dir;
        return true;
    }

    void MultitrackRecorder::addTrack()
    {
        if (isRecording() || isFinalizing()) return;

        const int fifoSize = juce::nextPowerOfTwo ((int) (sampleRate * kFifoSeconds));

        const juce::ScopedLock sl (structureLock);
        auto t = std::make_shared<TrackState>();
        t->setNameThreadSafe (juce::String ((int) tracks.size() + 1));
        tracks.push_back (std::move (t));

        auto f = std::make_unique<ChannelFifo>();
        f->resize (fifoSize);
        fifos.push_back (std::move (f));

        preRoll.push_back (std::make_unique<PreRollBuffer>());
        allocatePreRollBuffers();
        trackGen.fetch_add (1, std::memory_order_relaxed);
        rebuildShards();
    }

    void MultitrackRecorder::removeLastTrack()
    {
        if (isRecording() || isFinalizing() || tracks.empty()) return;
        const juce::ScopedLock sl (structureLock);   // off-thread readers are mid-getTrack
        tracks.pop_back();
        fifos.pop_back();
        preRoll.pop_back();
        trackGen.fetch_add (1, std::memory_order_relaxed);
        rebuildShards();
    }

    void MultitrackRecorder::setTrackCount (int n)
    {
        if (isRecording() || isFinalizing()) return;
        // One lock for the whole resize: a reader must never observe a
        // half-applied count. CriticalSection is recursive, so the
        // add/removeLastTrack calls below re-entering it is fine.
        const juce::ScopedLock sl (structureLock);
        n = juce::jmax (0, n);
        while ((int) tracks.size() > n) removeLastTrack();
        while ((int) tracks.size() < n) addTrack();
    }

    void MultitrackRecorder::setPreRollSeconds (int seconds)
    {
        seconds = juce::jlimit (0, 30, seconds);
        if (seconds == preRollSeconds) return;
        if (isRecording() || isFinalizing()) return;
        preRollSeconds = seconds;
        allocatePreRollBuffers();
    }

    void MultitrackRecorder::allocatePreRollBuffers()
    {
        const int samples = preRollSeconds > 0
                             ? (int) (sampleRate * (preRollSeconds + kPreRollSafetySec))
                             : 0;
        std::vector<std::unique_ptr<PreRollBuffer>> replacement;
        for (std::size_t i = 0; i < preRoll.size(); ++i)
        {
            auto buffer = std::make_unique<PreRollBuffer>();
            buffer->allocate (samples);
            replacement.push_back (std::move (buffer));
        }
        { const juce::SpinLock::ScopedLockType guard (captureBoundary);
          preRoll.swap (replacement); }
        // Release old storage outside the audio boundary lock.
    }

    void MultitrackRecorder::preparePreRollDump()
    {
        // Pre-record history belongs only to a fresh take. A punch inserts
        // its new file at punchInPos, and a continuation starts at the old
        // take's end; prepending history to either shifts the edit boundary.
        preRollHistorySamples = 0;
        preRollCutFrames.store (0, std::memory_order_relaxed);
        if (preRollSeconds <= 0 || punchInActive || continueAsPart)
        {
            preRollSnapshot.clear();
            return;
        }

        const int wanted = (int) (sampleRate * preRollSeconds);
        int available = 0;
        for (std::size_t i = 0; i < writers.size() && i < preRollSnapshot.size(); ++i)
            if (writers[i].active)
                available = juce::jmax (available, (int) juce::jmin (
                    (juce::int64) wanted, preRollSnapshot[i]->totalWritten.load()));
        // The newest history is the part kept: it runs straight into the take.
        preRollHistorySamples = juce::jmin (available, preRollLimit);
        preRollCutFrames.store (available - preRollHistorySamples, std::memory_order_release);
        if (preRollHistorySamples <= 0)
        {
            preRollSnapshot.clear();
            return;
        }
        samplesSinceStart.fetch_add (preRollHistorySamples, std::memory_order_relaxed);
        auto history = std::make_shared<std::vector<std::unique_ptr<PreRollBuffer>>> (std::move (preRollSnapshot));
        preRollSnapshot.clear();
        for (auto& copy : copies)
        {
            copy->history = history;
            copy->historySamples = preRollHistorySamples;
        }
    }

    void MultitrackRecorder::release()
    {
        const juce::ScopedLock structureGuard (structureLock);
        if (isFinalizing()) return;
        // Called from AudioEngine::audioDeviceStopped on every device
        // stop / restart (device swap, sample-rate or buffer-size change).
        // Detach the device- and thread-bound writer shards, but PRESERVE
        // the per-track state (tracks / fifos / preRoll). Every
        // ChannelStrip / LedMeter / EditPage row caches a
        // TrackState& grabbed at construction, and the CoreAudio IO
        // callback reads getTrack(i) every block -- destroying TrackState
        // here left all of them dangling and crashed the audio thread
        // (use-after-free) on every device change. prepare() rebuilds the
        // device-rate-dependent buffers on the following
        // audioDeviceAboutToStart, so the strip set survives the device
        // change unchanged -- which is what "preserve count across device
        // restarts" in AudioEngine::audioDeviceAboutToStart relies on.
        stopRecording();
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());
        shards.clear();
    }

    void MultitrackRecorder::processBlock (const float* const* inputs,
                                           int numChannels,
                                           int numSamples,
                                           juce::int64 playheadAtBlock) noexcept
    {
        const juce::SpinLock::ScopedLockType boundaryGuard (captureBoundary);
        const int n = juce::jmin (numChannels, (int) fifos.size());
        const bool rec = recording.load (std::memory_order_acquire);

        // Where this block ends in the take. The first one also pins the
        // pass's real start for the BWF stamp, read off the audio thread.
        juce::int64 takeFrames = 0;
        if (rec)
        {
            takeFrames = alarmFrames.load (std::memory_order_relaxed) + numSamples;
            if (firstLiveTicks.load (std::memory_order_relaxed) == 0)
            {
                firstLiveBlock.store (numSamples, std::memory_order_relaxed);
                firstLiveTicks.store (juce::Time::getHighResolutionTicks(), std::memory_order_release);
            }
        }

        if (rec && playheadAtBlock >= 0 && punchInActive.load (std::memory_order_relaxed))
        {
            juce::int64 unset = -1;
            if (punchCaptureOrigin.compare_exchange_strong (unset, playheadAtBlock,
                                                            std::memory_order_acq_rel))
                recordBaseSamples.store (playheadAtBlock, std::memory_order_relaxed);
        }

        for (int ch = 0; ch < n; ++ch)
        {
            auto& t = *tracks[(std::size_t) ch];

            float inputPeak = 0.0f;
            if (inputs[ch] != nullptr)
                for (int i = 0; i < numSamples; ++i)
                    inputPeak = juce::jmax (inputPeak, std::abs (inputs[ch][i]));
            t.inputPeak.store (inputPeak, std::memory_order_relaxed);

            // Arm-or-monitor gate. Pro Tools convention:
            //   ARM     = will record when you hit RECORD
            //   MONITOR = listen to the input through the monitor bus
            // Either of those modes should drive the strip's meter so
            // the engineer can gain-stage WITHOUT having to arm the
            // strip first. Pre-roll history + ring buffer push are
            // still arm-gated below.
            const bool armed   = rec ? captureArms[(size_t) ch]
                                     : t.armed.load (std::memory_order_relaxed);
            const bool monitor = t.monitor.load (std::memory_order_relaxed);

            // RIGHT half of a stereo pair whose LEFT is armed: this channel
            // MUST be captured together with its partner even if its own arm
            // flag is off. The pair records ONE interleaved 2-ch file and the
            // drain writes min(L,R) frames; if only L is fed, R's FIFO stays
            // empty, avail is always 0, the 2-ch write never fires, L overflows
            // and the whole stereo take is lost. Driving R from the pair's (L)
            // arm state keeps both FIFOs fed in lockstep.
            bool pairArmed = false;
            if (ch > 0)
            {
                const auto& lft = *tracks[(std::size_t) (ch - 1)];
                pairArmed = lft.isStereo.load (std::memory_order_relaxed)
                         && (rec ? captureArms[(size_t) ch - 1]
                                 : lft.armed.load (std::memory_order_relaxed));
            }

            // Exact zero is the fault signature, so any non-zero peak counts.
            // Pre-fader like the recording itself: a muted strip still counts.
            if (rec && inputPeak > 0.0f && (armed || pairArmed))
                t.lastInputSignal.store (takeFrames, std::memory_order_relaxed);

            // Histories share a block boundary, including unarmed and unrouted
            // inputs. Changing arm state must not compress a track's history.
            if (ch < (int) preRoll.size() && ! preRoll[(size_t) ch]->data.empty())
            {
                if (inputs[ch] != nullptr) preRoll[(size_t) ch]->push (inputs[ch], numSamples);
                else
                {
                    for (int offset = 0; offset < numSamples;)
                    {
                        const int count = juce::jmin (numSamples - offset, (int) scratch.size());
                        if (count == 0) break;
                        preRoll[(size_t) ch]->push (scratch.data(), count);
                        offset += count;
                    }
                }
            }
            if (! armed && ! monitor && ! pairArmed && ! meterUnarmedInputs.load())
            {
                const float prevPeak = t.peak.load (std::memory_order_relaxed);
                const float prevRms  = t.rms .load (std::memory_order_relaxed);
                t.peak.store (prevPeak * 0.92f, std::memory_order_relaxed);
                t.rms .store (prevRms  * 0.85f, std::memory_order_relaxed);
                continue;
            }

            const float* src = inputs[ch];
            if (src == nullptr)
            {
                // Every captured channel advances even when its input is absent.
                // Stereo drain consumes min(L,R): either missing side must feed
                // zeros or it starves the valid side and loses the entire pair.
                if (! (rec && (armed || pairArmed))) continue;
                const int  n0  = juce::jmin (numSamples, (int) scratch.size());
                if (n0 <= 0) continue;
                auto& cfz = *fifos[(std::size_t) ch];
                const auto zs = cfz.fifo.write (cfz.canAcceptAudio() ? n0 : 0);
                if (zs.blockSize1 > 0)
                    std::memcpy (cfz.data.data() + zs.startIndex1, scratch.data(),
                                 (std::size_t) zs.blockSize1 * sizeof (float));
                if (zs.blockSize2 > 0)
                    std::memcpy (cfz.data.data() + zs.startIndex2, scratch.data(),
                                 (std::size_t) zs.blockSize2 * sizeof (float));
                const int wroteZ = zs.blockSize1 + zs.blockSize2;
                cfz.accountCapture (wroteZ, numSamples);
                if (wroteZ < numSamples)
                {
                    const int deficit = numSamples - wroteZ;
                    missedSamples     .fetch_add (deficit, std::memory_order_relaxed);
                }
                continue;
            }

            // Meter (peak + simple block RMS)
            const float peak = inputPeak;
            float sumSq = 0.0f;
            for (int i = 0; i < numSamples; ++i) sumSq += src[i] * src[i];
            const float rms = std::sqrt (sumSq / (float) juce::jmax (1, numSamples));
            const float prevPeak = t.peak.load (std::memory_order_relaxed);
            t.peak.store (juce::jmax (peak, prevPeak * 0.92f), std::memory_order_relaxed);
            t.rms .store (rms,  std::memory_order_relaxed);
            if (peak >= 0.999f)
            {
                t.clipped.store (true, std::memory_order_relaxed);
                t.clipCount.fetch_add (1, std::memory_order_relaxed);
                t.lastClipSample.store (samplesSinceStart.load (std::memory_order_relaxed),
                                        std::memory_order_relaxed);
            }

            // Feed the spectrum FIFO. When full, snapshot and signal the UI.
            {
                int idx = t.fftIndex.load (std::memory_order_relaxed);
                for (int i = 0; i < numSamples; ++i)
                {
                    if (idx < TrackState::kFftSize)
                    {
                        t.fftFifo[(std::size_t) idx++] = src[i];
                    }
                    else
                    {
                        if (! t.fftBlockReady.load (std::memory_order_acquire))
                        {
                            std::memcpy (t.fftSnapshot.data(), t.fftFifo.data(),
                                         sizeof (float) * TrackState::kFftSize);
                            t.fftBlockReady.store (true, std::memory_order_release);
                        }
                        idx = 0;
                    }
                }
                t.fftIndex.store (idx, std::memory_order_release);
            }

            // Push to ring buffer when recording. Armed channels push; the
            // R half of an armed stereo pair also pushes (pairArmed) so both
            // sides of the interleaved file advance together.
            if (rec && (armed || pairArmed))
            {
                // Live waveform overview: bin the captured input into min/max
                // pairs so the EDIT lane draws the detailed waveform AS it
                // records (lock-free; see TrackState::liveWavePush).
                t.liveWavePush (src, numSamples);

                auto& cf = *fifos[(std::size_t) ch];
                const auto scope = cf.fifo.write (cf.canAcceptAudio() ? numSamples : 0);
                const int wrote = scope.blockSize1 + scope.blockSize2;
                cf.accountCapture (wrote, numSamples);

                if (scope.blockSize1 > 0)
                    std::memcpy (cf.data.data() + scope.startIndex1,
                                 src,
                                 (std::size_t) scope.blockSize1 * sizeof (float));
                if (scope.blockSize2 > 0)
                    std::memcpy (cf.data.data() + scope.startIndex2,
                                 src + scope.blockSize1,
                                 (std::size_t) scope.blockSize2 * sizeof (float));

                if (wrote < numSamples)
                {
                    // FIFO full: the disk fell behind. Count the drop for the
                    // health readout AND record it per-channel so the drain
                    // thread can pad this track with an equal count of silence
                    // -- otherwise different channels drop different amounts and
                    // the take develops permanent inter-track sync drift.
                    const int deficit = numSamples - wrote;
                    missedSamples     .fetch_add (deficit, std::memory_order_relaxed);
                }
            }
        }

        if (rec)
        {
            samplesSinceStart.fetch_add (numSamples, std::memory_order_relaxed);
            alarmFrames.store (takeFrames, std::memory_order_relaxed);
        }
    }

    // NOTE: punch-in is triggered two ways, both via armPunchIn(): the normal
    // RECORD button when a session is loaded (MainComponent::onRecordClicked --
    // continue/overdub at the edit cursor, append at the end by default), and
    // the PUNCH-mode position window (MainComponent::servicePunch). Either way
    // this recorder just stashes + splices; it doesn't know which path armed it.
    bool MultitrackRecorder::recoverInterruptedPunches (const juce::File& audioDir,
                                                        int* recovered, bool* leadInsOwed)
    {
        if (recovered != nullptr) *recovered = 0;
        if (leadInsOwed != nullptr) *leadInsOwed = false;
        if (! audioDir.isDirectory()) return true;
        // First, so anything below that opens or archives a take reads its
        // real length. A failed repair loses nothing: the audio stays in the
        // file and the next open tries again.
        aiffrepair::repairTakeFolder (audioDir);
        flacrepair::repairTakeFolder (audioDir);
        wavrepair::repairTakeFolder (audioDir);
        // A take interrupted before STOP may still owe a copy its silent
        // lead-in (see PendingLeadIn); without it the new audio sits early.
        if (! recoverPendingLeadIns (audioDir) && leadInsOwed != nullptr) *leadInsOwed = true;

        juce::Array<juce::File> sidecars;
        for (const auto& file : audioDir.findChildFiles (juce::File::findFiles, false,
                                                        "Track_*.punchbase.*"))
        {
            const auto ext = file.getFileExtension().toLowerCase();
            if (ext != ".wav" && ext != ".aif" && ext != ".aiff" && ext != ".flac")
                continue;
            const auto stem = file.getFileNameWithoutExtension();
            if (! stem.endsWith (".punchbase")) continue;
            const auto realStem = stem.dropLastCharacters (10);
            auto suffix = realStem.fromFirstOccurrenceOf ("Track_", false, false);
            if (! realStem.startsWith ("Track_")) continue;
            const auto partAt = suffix.indexOf ("_part");
            const auto digits = partAt >= 0 ? suffix.substring (0, partAt) : suffix;
            if (digits.isEmpty() || ! digits.containsOnly ("0123456789")) continue;
            if (partAt >= 0)
            {
                const auto part = suffix.substring (partAt + 5);
                if (part.isEmpty() || ! part.containsOnly ("0123456789")) continue;
            }
            sidecars.add (file);
        }
        if (sidecars.isEmpty()) return true;

        // Archive a partially written replacement instead of overwriting it:
        // it may contain a useful performance even though the original must
        // win the interrupted transaction. The archive is outside Audio Files
        // so neither playback nor export mistakes it for a continuation part.
        const auto sessionRoot = audioDir.getFileName() == "Audio Files"
            ? audioDir.getParentDirectory() : audioDir;
        const auto archiveRoot = sessionRoot
            .getChildFile ("Session File Backups")
            .getNonexistentChildFile ("Interrupted Punch", {}, false);
        if (! archiveRoot.createDirectory().wasOk()) return false;
        sidecars.sort();
        for (const auto& sidecar : sidecars)
        {
            const auto stem = sidecar.getFileNameWithoutExtension()
                .dropLastCharacters (10);
            const auto target = audioDir.getChildFile (stem + sidecar.getFileExtension());
            const auto archived = archiveRoot.getChildFile (target.getFileName());
            // The punch file exists only once every part of the base was moved
            // aside, so then a part with no sidecar is one the punch rolled
            // into. Before that it may be base audio not yet moved: it stays.
            if (target.exists() && ! stem.contains ("_part"))
                for (const auto& part : audioDir.findChildFiles (juce::File::findFiles, false,
                                                                 stem + "_part*" + sidecar.getFileExtension()))
                {
                    if (part.getFileName().contains (".punchbase") || punchSidecar (part).exists()) continue;
                    if (! part.moveFileTo (archiveRoot.getChildFile (part.getFileName()))) return false;
                }
            if (target.exists() && ! target.moveFileTo (archived)) return false;
            if (! sidecar.moveFileTo (target))
            {
                if (archived.existsAsFile()) archived.moveFileTo (target);
                return false;
            }
            for (const auto& temporary : audioDir.findChildFiles (juce::File::findFiles, false,
                                                                  stem + ".punchtmp*"))
                temporary.deleteFile();
            if (recovered != nullptr) ++*recovered;
        }
        return true;
    }

    bool MultitrackRecorder::takeIsMultiPart (const juce::File& sessionDir, int trackIndex)
    {
        const auto audioFiles = sessionDir.getChildFile ("Audio Files");
        const auto base = audioFiles.isDirectory() ? audioFiles : sessionDir;
        const auto stem = juce::String::formatted ("Track_%02d", trackIndex + 1);
        // A take is multi-part if any Track_NN_partXX.<audio ext> sibling
        // exists. The bare "_part*" glob also matched leftover
        // Track_NN_partXX.punchbase.<ext> sidecars from an aborted punch, so a
        // single-file take could be misreported as multi-part.
        for (const auto& f : base.findChildFiles (juce::File::findFiles, false, stem + "_part*"))
        {
            const auto name = f.getFileName();
            if (name.containsIgnoreCase (".punchbase")) continue;
            const auto ext = f.getFileExtension().toLowerCase();
            if (ext == ".wav" || ext == ".flac" || ext == ".aif" || ext == ".aiff")
                return true;
        }
        return false;
    }

    std::pair<juce::File, int> MultitrackRecorder::nextContinuationPart (
        const juce::File& audioDir, const juce::String& trackName, const juce::String& ext)
    {
        // Track_NN.<ext> is part 1; continuations are Track_NN_part02, _part03 ...
        // Scan the WHOLE set on disk for the highest existing index rather than
        // stopping at the first numbering gap: if an orphaned later part exists
        // past a hole (e.g. _part04 present, _part03 missing), numbering the new
        // continuation into the hole would shadow / risk overwriting that orphan
        // and reorder the take. Numbering strictly beyond the max avoids that.
        int highest = audioDir.getChildFile (trackName + ext).existsAsFile() ? 1 : 0;
        for (const auto& f : audioDir.findChildFiles (juce::File::findFiles, false,
                                                      trackName + "_part*" + ext))
        {
            const int pn = f.getFileNameWithoutExtension()
                            .fromLastOccurrenceOf ("_part", false, false).getIntValue();
            if (pn > highest) highest = pn;
        }
        const int next = highest + 1;
        const auto file = (next == 1)
            ? audioDir.getChildFile (trackName + ext)
            : audioDir.getChildFile (trackName + "_part"
                  + juce::String::formatted ("%02d", next) + ext);
        return { file, next };
    }

    // Working state of ONE startRecording call, threaded through its stages.
    // Lives for the call only; every stage runs under structureLock.
    struct MultitrackRecorder::TakeStart
    {
        juce::File   sessionDir, audioFilesDir;
        juce::String sessionId;                  // persistent capture identity
        trackorder::Log trackOrder;              // what each copy must catch up with
        TakeFormat   primary;                    // selected, then adopted from an existing take
        bool         backupRequested { false }, backupReady { false };
        // Claimed on the first media mutation; see invalidateReportsBeforeMediaMutation.
        std::shared_ptr<std::atomic<juce::int64>> reportToken;
        std::vector<bool> hadPrimaryBase;        // Track_NN existed before this pass
        std::vector<std::pair<juce::File, juce::File>> punchStashes;   // take -> .punchbase sidecar
        std::vector<juce::int64> previousLengths;                      // continue: length already on disk
        std::vector<TakeBase> primaryBases;                            // punch: what each copy must hold
        int primaryAttempts { 0 }, primaryOpened { 0 };
    };

    // Resolves a CaptureFormat to its container code + bit-depth + file
    // extension. Container codes match the integer scheme used by
    // openWriterAtPath / WriterChannel::primaryContainer:
    //   0 = WAV, 1 = AIFF, 2 = FLAC.
    MultitrackRecorder::TakeFormat MultitrackRecorder::takeFormatFor (CaptureFormat f) noexcept
    {
        switch (f)
        {
            case CaptureFormat::Wav16:       return { 0, 16, ".wav" };
            case CaptureFormat::Wav24:       return { 0, 24, ".wav" };
            case CaptureFormat::Wav32Float:  return { 0, 32, ".wav" };
            case CaptureFormat::Aiff16:      return { 1, 16, ".aif" };
            case CaptureFormat::Aiff24:      return { 1, 24, ".aif" };
            case CaptureFormat::Aiff32Float: return { 1, 32, ".aif" };
            case CaptureFormat::Flac16:      return { 2, 16, ".flac" };
            case CaptureFormat::Flac24:      return { 2, 24, ".flac" };
        }
        return { 0, 24, ".wav" };
    }

    // True when track k is one we open a writer for: armed.
    bool MultitrackRecorder::isArmedCapture (std::size_t k) const noexcept
    {
        return tracks[k]->armed.load (std::memory_order_relaxed);
    }

    // An armed capture track that owns its Track_NN file -- i.e. not the
    // RIGHT half of an armed stereo pair, whose audio lives in the LEFT's
    // interleaved file.
    bool MultitrackRecorder::opensOwnCaptureFile (std::size_t i) const noexcept
    {
        if (! isArmedCapture (i)) return false;
        return ! (i > 0 && tracks[i - 1]->isStereo.load (std::memory_order_relaxed)
                        && isArmedCapture (i - 1));
    }

    // A take start is a sequence of stages, and all of them are the primary's:
    // RECORD waits on no other drive (see CopyOpen). Everything up to
    // continuationBasesIntact only VALIDATES (and creates folders);
    // stashPunchBases is the first that moves media, and every stage from there
    // on rolls back what it did before refusing.
    bool MultitrackRecorder::startRecording (const juce::File& sessionDir)
    {
        const juce::ScopedLock structureGuard (structureLock);
        startError.clear();
        if (recording.load() || isFinalizing()) return false;

        timeAnchor  = hasTimeHint ? timeHint : bwf::TimeAnchor::fromWallClock();
        hasTimeHint = false;
        firstLiveTicks.store (0, std::memory_order_relaxed);
        firstLiveBlock.store (0, std::memory_order_relaxed);

        // A punch/continue arm belongs to this one start. Consume it on every
        // refusal below as well as on success (see discardTakeArms).
        bool wentLive = false;
        // A refused start is not a take: the last take's failure banner stays.
        const bool hadFailed[] { recoveryMarkerFailed.load(), reportWriteFailed.load(), punchSpliceFailed.load(),
                                 primaryFailed.load(), backupFailed.load(), mirrorFailed.load() };
        const auto skippedBefore = mirrorsSkippedAtStart.load();
        const juce::ScopeGuard consumeArms { [&]
        {
            if (wentLive) return;
            discardTakeArms();
            recoveryMarkerFailed.store (hadFailed[0]);
            reportWriteFailed.store (hadFailed[1]);
            punchSpliceFailed.store (hadFailed[2]);
            primaryFailed.store (hadFailed[3]);
            backupFailed.store (hadFailed[4]);
            mirrorFailed.store (hadFailed[5]);
            mirrorsSkippedAtStart.store (skippedBefore);
        } };
        if (tracks.empty())   return false;

        // Every refusal names its reason: an empty startError made the UI
        // fall back to "could not open writer files" for unrelated causes.
        const auto refuse = [this] (const char* why)
        {
            if (startError.isEmpty()) startError = why;
            return false;
        };

        TakeStart take;
        take.sessionDir = sessionDir;
        if (! validateTakeStart (take))
            return refuse ("Recording refused: the backup folder is inside this session; choose another backup folder.");
        if (! createTakeFolders (take))
            return refuse ("Recording refused: the session folders could not be created; check the recording drive.");
        resolveTakeMirrors (take);
        prepareBackupCopy (take);
        if (! claimTakeIdentity (take))
            return refuse ("Recording refused: this session's capture identity could not be written; check the session drive.");
        if (! recoverPrimaryPunches (take))
            return refuse ("An interrupted punch was put back on this take. Reopen the session before recording.");
        if (! resolveTakeFormats (take))
            return refuse ("Recording refused: the existing take files are in mixed formats and cannot be continued.");
        if (! continuationBasesIntact (take))
            return refuse ("Recording refused: the existing take is incomplete, so it cannot be continued.");
        noteExistingPrimaryBases (take);
        if (! stashPunchBases (take))
            return refuse ("Punch refused: the existing take could not be moved aside safely; it was left untouched.");
        if (! measureContinuationBase (take))
            return refuse ("Recording refused: the existing take could not be read.");
        openTakeWriters (take);

        // Hard error: if we tried to open primary writers but EVERY one failed
        // (disk full, unmounted/read-only volume, permissions), do NOT enter
        // the recording state and do NOT report success -- the caller must not
        // show a live take that isn't being written anywhere. (Any punch
        // sidecars stay on disk under their .punchbase names, so the existing
        // take is renamed-aside, never lost.) The success path -- at least one
        // primary writer open -- is unchanged.
        const bool marked = writeLeadInMarkers (take);
        if (! marked || take.primaryAttempts == 0 || take.primaryOpened != take.primaryAttempts)
        {
            abandonTakeStart (take);
            return false;
        }

        startDiskWatch();
        goLive (take);
        wentLive = true;
        return true;
    }

    bool MultitrackRecorder::validateTakeStart (const TakeStart& take)
    {
        const auto& sessionDir = take.sessionDir;

        // Refuse corrupt/persisted overlapping pairs before creating any files.
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i]->isStereo.load()
                && (i + 1 >= tracks.size() || tracks[i + 1]->isStereo.load()))
            { startError = "Overlapping or incomplete stereo pair"; return false; }

        recoveryMarkerFailed.store (false, std::memory_order_relaxed);
        reportWriteFailed.store (false, std::memory_order_relaxed);
        punchSpliceFailed.store (false, std::memory_order_relaxed);
        primaryFailed.store (false, std::memory_order_relaxed);
        backupFailed.store (false, std::memory_order_relaxed);
        mirrorFailed.store (false, std::memory_order_relaxed);
        backupActive.store (false, std::memory_order_relaxed);
        captureArms.clear();
        for (const auto& track : tracks)
            captureArms.push_back (track->armed.load (std::memory_order_relaxed));

        return true;
    }

    bool MultitrackRecorder::createTakeFolders (TakeStart& take)
    {
        const auto& sessionDir = take.sessionDir;

        // A recorder must never report a live take it isn't actually writing:
        // if the session folder can't be created (read-only / unmounted volume,
        // permissions), fail the start rather than silently swallow it.
        if (! sessionDir.createDirectory().wasOk() && ! sessionDir.isDirectory())
            return false;
        activeSessionDir = sessionDir;

        // Folder layout: tracks live under "Audio Files/", mixdowns /
        // stem exports under "Export Files/", backups under
        // "Session File Backups/". Session-level metadata (recording.session,
        // session.report.json, the .zfproj document) stays at the root.
        take.audioFilesDir = sessionDir.getChildFile ("Audio Files");
        const auto& audioFilesDir = take.audioFilesDir;
        if (! audioFilesDir.createDirectory().wasOk() && ! audioFilesDir.isDirectory())
            return false;   // no place to write the tracks -> not a real take
        sessionDir.getChildFile ("Export Files")        .createDirectory();
        sessionDir.getChildFile ("Clip Groups")         .createDirectory();
        sessionDir.getChildFile ("Session File Backups").createDirectory();

        writers.clear();
        writers.reserve (tracks.size());
        copies.clear();
        droppedCopies.clear();
        std::erase_if (leftBehind, [] (const auto& copy) { return copy->finished.load (std::memory_order_acquire); });

        return true;
    }

    // Asks the disk, so it can block as long as a dead mount does.
    static juce::File canonicalRoot (const juce::File& f)
    {
        if (f == juce::File()) return f;
        std::error_code error;
        const auto path = std::filesystem::weakly_canonical (f.getFullPathName().toStdString(), error);
        return error ? f : juce::File (juce::String (path.string()));
    }

    // Which destinations this take will try, from what is already known. Nothing
    // here may touch a copy's path: RECORD must not wait on any drive but the
    // primary's. Each copy works out the rest on its own thread (see CopyOpen).
    void MultitrackRecorder::resolveTakeMirrors (const TakeStart&)
    {
        activeMirrors.clear();
        int skipped = 0;
        for (const auto& mc : mirrorConfigs)
        {
            if (isStillLeftBehind (mc.root))
            {
                ++skipped;
                droppedCopies.add ("Mirror " + mc.root.getFullPathName() + " is still not responding; it is not in this take");
                continue;
            }
            activeMirrors.push_back (mc);
        }
        mirrorsSkippedAtStart.store (skipped, std::memory_order_relaxed);
    }

    // A persistent identity survives relocating the primary folder. A
    // still-existing primary at another path is a fork, not this take.
    bool MultitrackRecorder::claimTakeIdentity (TakeStart& take)
    {
        take.trackOrder = trackorder::read (take.sessionDir);
        const auto idFile = take.sessionDir.getChildFile (".zynforge-capture-id");
        take.sessionId = idFile.loadFileAsString().trim();
        if (take.sessionId.isNotEmpty()) { captureIdentity = take.sessionId; return true; }
        take.sessionId = juce::Uuid().toString();
        captureIdentity = take.sessionId;
        return atomicfile::writeText (idFile, take.sessionId);
    }

    // An earlier process may have died after moving a base take aside but before
    // splicing it back. Do not start another pass against a partial replacement
    // still on disk. Each copy does the same for itself.
    bool MultitrackRecorder::recoverPrimaryPunches (const TakeStart& take)
    {
        int recovered = 0;
        bool leadInsOwed = false;
        const bool restored = recoverInterruptedPunches (take.audioFilesDir, &recovered, &leadInsOwed);
        if (leadInsOwed)
            startError = "Recording refused: a track still owes its silent lead-in and this drive will not take it. "
                         "Free some space and press RECORD again.";
        return restored && ! leadInsOwed
            && recovered == 0; // > 0: the caller must reopen the restored take
    }

    // A punch must reopen the SAME container as the existing take. If the
    // capture format changed between passes, stashing only the newly
    // selected extension leaves the old Track_NN.wav untouched and writes
    // a separate Track_NN.flac instead of replacing the punched region.
    // Continue already inherited the base format; apply that policy to
    // punch and to every configured copy. Mixed/unreadable existing media
    // is refused before any file is moved or writer opened.
    bool MultitrackRecorder::adoptExistingTakeFormat (const juce::File& copyAudioDir, TakeFormat& selected,
                                                      const std::vector<FormatCheck>& checks, double sampleRate,
                                                      juce::String& problem)
    {
        if (! copyAudioDir.isDirectory()) return true;
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        bool found = false;
        for (const auto& check : checks)
        {
            const auto name = juce::String::formatted ("Track_%02d", check.track + 1);
            for (auto* ext : { ".wav", ".aif", ".aiff", ".flac" })
            {
                const auto file = copyAudioDir.getChildFile (name + ext);
                if (! file.existsAsFile()) continue;
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
                if (reader == nullptr) return false;
                if (check.rightOfPair || reader->numChannels != (unsigned) check.channels
                    || reader->sampleRate != sampleRate)
                {
                    problem = "Recording refused: track " + juce::String (check.track + 1)
                        + " has a different recorded channel layout or sample rate. Restore the original layout or start a new session.";
                    return false;
                }
                TakeFormat actual { juce::String (ext) == ".flac" ? 2
                                       : juce::String (ext) == ".wav" ? 0 : 1,
                                  reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample,
                                  ext };
                if (found && (selected.container != actual.container
                              || selected.bitDepth != actual.bitDepth
                              || juce::String (selected.ext) != actual.ext))
                    return false;
                selected = actual;
                found = true;
            }
        }
        return true;
    }

    // The old interleaved L file also owns an armed R track's history, even
    // after the user unlinks and disarms L.
    std::vector<MultitrackRecorder::FormatCheck> MultitrackRecorder::formatChecks() const
    {
        std::vector<FormatCheck> checks;
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            if (! isArmedCapture (i) && ! (i + 1 < tracks.size() && isArmedCapture (i + 1))) continue;
            checks.push_back ({ (int) i, tracks[i]->isStereo.load() && i + 1 < tracks.size() ? 2 : 1,
                                i > 0 && tracks[i - 1]->isStereo.load() });
        }
        return checks;
    }

    bool MultitrackRecorder::resolveTakeFormats (TakeStart& take)
    {
        const auto& audioFilesDir = take.audioFilesDir;
        auto& primary = take.primary;

        take.primary = takeFormatFor (captureFormat);

        if ((continueAsPart || punchInActive)
            && ! adoptExistingTakeFormat (audioFilesDir, primary, formatChecks(), sampleRate, startError))
        {
            cancelPunchIn();
            continueAsPart = false;
            return false;
        }

        // A fresh take must never truncate media that is already present. The
        // UI normally recognises an existing take and selects continuation or
        // punch mode, but this recorder-level guard also protects daemon/API
        // callers and damaged sessions that could not be loaded by the player.
        if (! continueAsPart && ! punchInActive)
        {
            for (std::size_t i = 0; i < tracks.size(); ++i)
            {
                if (! opensOwnCaptureFile (i)) continue;
                const auto name = juce::String::formatted ("Track_%02d", (int) i + 1);
                for (auto* ext : { ".wav", ".flac", ".aif", ".aiff" })
                    if (audioFilesDir.getChildFile (name + ext).existsAsFile())
                        return false;
            }
        }
        return true;
    }

    void MultitrackRecorder::prepareBackupCopy (TakeStart& take)
    {
        take.backupRequested = backupDir.getFullPathName().isNotEmpty();
        if (take.backupRequested && isStillLeftBehind (backupDir))
        {
            backupFailed.store (true, std::memory_order_relaxed);
            droppedCopies.add ("Backup " + backupDir.getFullPathName() + " is still not responding; it is not in this take");
            return;
        }
        take.backupReady = take.backupRequested;
    }

    // A continuation part without its Track_NN base is invisible to the session
    // player, so the next part would make a damaged take harder to recover.
    bool MultitrackRecorder::orphanedContinuation (const juce::File& dir, const juce::String& stem,
                                                   const juce::String& ext)
    {
        if (dir.getChildFile (stem + ext).existsAsFile()) return false;
        for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, stem + "_part*" + ext))
            if (file.getFileNameWithoutExtension().startsWith (stem + "_part"))
                return true;
        return false;
    }

    bool MultitrackRecorder::continuationBasesIntact (const TakeStart& take)
    {
        if (! continueAsPart) return true;
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (opensOwnCaptureFile (i)
                && orphanedContinuation (take.audioFilesDir, juce::String::formatted ("Track_%02d", (int) i + 1),
                                         take.primary.ext))
                return false;
        return true;
    }

    // Validation-only refusals leave earlier integrity work intact. Once
    // this start may mutate audio, invalidate only this SESSION's snapshot.
    // A different session's job remains valid, even on the same recorder.
    void MultitrackRecorder::invalidateReportsBeforeMediaMutation (TakeStart& take)
    {
        if (take.reportToken) return;
        juce::int64 ignoredGeneration = 0;
        take.reportToken = claimReportGeneration (take.sessionDir, ignoredGeneration);
        reportAsyncState->generation.fetch_add (1, std::memory_order_acq_rel);
        reportAsyncState->failed.store (false, std::memory_order_relaxed);
    }

    void MultitrackRecorder::noteExistingPrimaryBases (TakeStart& take)
    {
        const auto& audioFilesDir = take.audioFilesDir;
        auto& primary = take.primary;
        auto& hadPrimaryBase = take.hadPrimaryBase;

        take.hadPrimaryBase.assign (tracks.size(), false);
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            const auto name = juce::String::formatted ("Track_%02d", (int) i + 1);
            hadPrimaryBase[i] = audioFilesDir.getChildFile (name + primary.ext).existsAsFile();
        }
    }

    // Punch-in: move the existing take -- AND its continuation parts -- aside
    // to sidecars BEFORE the writer truncates the target, so the WHOLE base
    // (a take built up by continue-recording is multi-part) survives to be
    // spliced + flattened back on stop. No-op when not punching or no take.
    bool MultitrackRecorder::stashForPunch (TakeStart& take, const juce::File& target)
    {
        if (! punchInActive) return true;
        invalidateReportsBeforeMediaMutation (take);
        return stashPunchParts (target, take.punchStashes);
    }

    bool MultitrackRecorder::stashPunchParts (const juce::File& target,
                                              std::vector<std::pair<juce::File, juce::File>>& stashes)
    {
        // Even when there is no main file, a sidecar may be all that
        // survived a previous interrupted punch. Never delete it.
        if (punchSidecar (target).exists()) return false;
        for (auto& part : findTakeParts (target))   // [Track_NN, _part02, ...]
        {
            const auto sidecar = punchSidecar (part);
            if (sidecar.exists()) return false;
            if (! part.moveFileTo (sidecar)) return false;
            stashes.emplace_back (part, sidecar);
        }
        return true;
    }

    void MultitrackRecorder::restorePunchStashes (TakeStart& take)
    {
        for (auto it = take.punchStashes.rbegin(); it != take.punchStashes.rend(); ++it)
        {
            if (it->first.exists() && ! it->first.deleteFile())
                continue; // keep the original in its sidecar
            it->second.moveFileTo (it->first);
        }
        cancelPunchIn();
    }

    bool MultitrackRecorder::stashPunchBases (TakeStart& take)
    {
        const auto& audioFilesDir = take.audioFilesDir;
        auto& primary = take.primary;

        // Complete ALL renames before opening ANY writer. A failed rename
        // must not be followed by openWriterAtPath's target deletion, and a
        // failure on track 2 must restore track 1 before returning.
        take.primaryBases.assign (tracks.size(), {});
        if (punchInActive)
        {
            // A copy proves it holds this take against these numbers, from its
            // own thread, long after the files below have moved aside.
            juce::AudioFormatManager punchFm; punchFm.registerBasicFormats();
            for (std::size_t i = 0; i < tracks.size(); ++i)
            {
                if (! opensOwnCaptureFile (i)) continue;
                const auto source = audioFilesDir.getChildFile (
                    juce::String::formatted ("Track_%02d", (int) i + 1) + primary.ext);
                if (! source.existsAsFile()) continue;
                auto& base = take.primaryBases[i];
                base.exists = true;
                if (const auto reader = ConcatReader::create (punchFm, findTakeParts (source)))
                {
                    base.readable = true;
                    base.length   = reader->lengthInSamples;
                    base.channels = (int) reader->numChannels;
                    base.rate     = reader->sampleRate;
                }
            }
            for (std::size_t i = 0; i < tracks.size(); ++i)
            {
                if (! opensOwnCaptureFile (i)) continue;
                const auto name = juce::String::formatted ("Track_%02d", (int) i + 1);
                if (! stashForPunch (take, audioFilesDir.getChildFile (name + primary.ext)))
                { restorePunchStashes (take); return false; }
            }
        }
        return true;
    }

    bool MultitrackRecorder::measureContinuationBase (TakeStart& take)
    {
        const auto& audioFilesDir = take.audioFilesDir;
        auto& previousLengths = take.previousLengths;

        // Continue: the timeline base is the EXISTING take's length, measured
        // from the files on disk (the longest take's Track_NN + its parts)
        // -- authoritative, so the clock / playhead continue correctly even if
        // the caller's hint was stale. Read before the writers open new parts.
        take.previousLengths.assign (tracks.size(), 0);
        if (continueAsPart)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();

            juce::int64 takeLen = recordBaseSamples; // caller's loaded-session hint
            for (std::size_t i = 0; i < tracks.size(); ++i)
            {
                const auto name = juce::String::formatted ("Track_%02d", (int) i + 1);
                for (auto* ext : { ".wav", ".aif", ".aiff", ".flac" })
                {
                    const auto mainFile = audioFilesDir.getChildFile (name + ext);
                    if (! mainFile.existsAsFile()) continue;
                    auto reader = ConcatReader::create (fm, findTakeParts (mainFile));
                    if (reader == nullptr) { continueAsPart = false; return false; }
                    previousLengths[i] = reader->lengthInSamples;
                    if (! tracks[i]->referenceMedia.load())
                        takeLen = juce::jmax (takeLen, reader->lengthInSamples);
                }
            }
            recordBaseSamples = takeLen;
        }
        return true;
    }

    // Track i's file on one copy, at the part this pass opens at. False when it
    // would not open (e.g. disk full, permissions). A copy with less audio than
    // the timeline position gets a silent lead-in, written after STOP into part
    // slots reserved here (see PendingLeadIn).
    bool MultitrackRecorder::openTrackFile (CaptureCopy& copy, std::size_t i, const juce::File& dir,
                                            const TakeFormat& fmt, const juce::String& trackName, int chans,
                                            const LeadInPlan& plan, bool primaryHadBase, juce::int64 prior,
                                            std::vector<PendingLeadIn>& leadIns)
    {
        auto& t = copy.tracks[i];
        t.baseFile = dir.getChildFile (trackName);
        t.ext = fmt.ext;
        t.container = fmt.container;
        t.bitDepth = fmt.bitDepth;
        t.bytesPerSample = fmt.bitDepth / 8;

        auto file = dir.getChildFile (trackName + fmt.ext);
        if (plan.continuing)
        {
            const auto next = nextContinuationPart (dir, trackName, fmt.ext);
            file = next.first;
            t.partNumber = next.second;
        }
        if (plan.editPass && (plan.continuing || ! primaryHadBase) && plan.timelineStart > prior)
        {
            PendingLeadIn lead;
            lead.audioDir = dir;            lead.trackName = trackName;  lead.ext = fmt.ext;
            lead.container = fmt.container; lead.bitDepth = fmt.bitDepth; lead.channels = chans;
            lead.sampleRate = plan.sampleRate; lead.priorLength = prior;
            lead.timelineStart = plan.timelineStart;
            lead.firstPart = t.partNumber;  lead.copyIndex = copy.copyIndex;
            // Enough slots to stay under the container's ceiling. A punch
            // starts capturing a little after it was armed (punchCaptureOrigin),
            // so allow a minute more than the armed position.
            const juce::int64 frameBytes = (juce::int64) juce::jmax (1, chans) * juce::jmax (1, fmt.bitDepth / 8);
            const juce::int64 maxFrames = juce::jmax ((juce::int64) 1,
                                                      (maxBytesForContainer (fmt.container) - 1) / frameBytes);
            const juce::int64 estimate = plan.timelineStart - prior
                + (plan.punching ? (juce::int64) (plan.sampleRate * 60.0) : 0);
            lead.parts = estimate <= maxFrames ? 1
                       : (int) juce::jmin ((juce::int64) 999, 1 + (estimate - 1) / maxFrames);
            leadIns.push_back (lead);
            t.partNumber += lead.parts;
            file = partFileFor (dir, trackName, fmt.ext, t.partNumber);
        }
        t.writer.reset (copy.openFile (t, file, 0));
        if (t.writer == nullptr) { t.failed = true; return false; }
        t.partFiles.add (file.getFileName());
        return true;
    }

    // What a copy must do to join a take that is already rolling, on the copy's
    // own thread, its ring holding the audio meanwhile. Plain values only, so a
    // dead mount strands that thread and nothing else. run() returns why the
    // copy cannot join, naming it; the rules for joining are the same ones
    // RECORD used to apply before go-live, and a copy that fails one is left out
    // of the take instead of refusing it.
    struct MultitrackRecorder::CopyOpen
    {
        struct TrackPlan
        {
            std::size_t  index {};
            juce::String name;
            int          channels { 1 };
            bool         primaryHadBase {};
            TakeBase     primaryBase;
        };

        juce::String label;
        int          copyIndex {};
        juce::File   root, sessionDir, backupRoot;
        juce::String sessionId;
        trackorder::Log trackOrder;
        TakeFormat   format;
        bool         continuing {}, punching {};
        LeadInPlan   leadIn;
        juce::int64  timelineBase {};
        double       sampleRate {};
        std::vector<TrackPlan>   tracks;
        std::size_t              allTrackCount {};
        std::vector<FormatCheck> formatChecks;
        std::shared_ptr<CopyRoots> roots;
        std::function<void (CaptureCopy&)> beforeOpenForTests;

        // conflict: this folder holds something that is not this session's copy,
        // and nothing may be written there. unwritable: it is ours, but the drive
        // will not take the claim (full, read-only), so it cannot carry this take.
        CopyClaim claim() const
        {
            const auto copy = root.getChildFile (sessionDir.getFileName());
            const auto owner = copy.getChildFile (".zynforge-primary");
            const auto identity = sessionDir.getFullPathName();
            if (owner.existsAsFile())
            {
                const auto saved = juce::JSON::parse (owner);
                const auto oldPath = saved.getProperty ("path", {}).toString();
                if (oldPath.isEmpty() || saved.getProperty ("id", {}).toString() != sessionId
                    || (oldPath != identity && juce::File (oldPath).exists())) return CopyClaim::conflict;
            }
            else if (! copy.getChildFile ("Audio Files").findChildFiles (
                         juce::File::findFiles, false, "Track_*").isEmpty()) return CopyClaim::conflict;
            // A strip move a crash cut short goes back first, and that restores
            // the very file written below.
            if (! TrackFileTransaction::recover (copy)) return CopyClaim::interrupted;
            // A copy with no takes has nothing to move, so it starts where the
            // session is. One from before this log began starts at its beginning.
            const bool holdsTakes = ! copy.getChildFile ("Audio Files").findChildFiles (
                juce::File::findFiles, false, "Track_*").isEmpty();
            const auto position = holdsTakes ? juce::JSON::parse (owner).getProperty ("trackOrder", "").toString()
                                             : trackOrder.current;
            return copy.createDirectory().wasOk() && writeOwner (position)
                     ? CopyClaim::claimed : CopyClaim::unwritable;
        }

        bool writeOwner (const juce::String& position) const
        {
            auto* metadata = new juce::DynamicObject();
            metadata->setProperty ("id", sessionId);
            metadata->setProperty ("path", sessionDir.getFullPathName());
            metadata->setProperty ("trackOrder", position);
            return atomicfile::writeText (root.getChildFile (sessionDir.getFileName()).getChildFile (".zynforge-primary"),
                                          juce::JSON::toString (juce::var (metadata)));
        }

        // Takes this copy's files through every strip move it has not made, one
        // transaction a step. Its position is written inside the transaction, so
        // a crash at any moment leaves files and position agreeing: a step is
        // never made twice, which would put a different channel under each name.
        // False: this copy stays as it is, in the order it was.
        bool followTrackOrder (CaptureCopy& copy) const
        {
            const auto copyDir  = root.getChildFile (sessionDir.getFileName());
            const auto audioDir = copyDir.getChildFile ("Audio Files");
            const auto position = juce::JSON::parse (copyDir.getChildFile (".zynforge-primary"))
                                      .getProperty ("trackOrder", "").toString();
            if (! trackOrder.readable) return false;
            if (position == trackOrder.current) return true;

            std::size_t next = 0;
            if (position.isNotEmpty())
            {
                while (next < trackOrder.steps.size() && trackOrder.steps[next].to != position) ++next;
                if (next == trackOrder.steps.size()) return false;   // not a place this session has been
                ++next;
            }
            for (; next < trackOrder.steps.size(); ++next)
            {
                if (copy.abandoned.load (std::memory_order_acquire)) return false;
                const auto& step = trackOrder.steps[next];
                const auto removed = copyDir.getChildFile ("Removed Tracks").getChildFile (juce::Uuid().toString());
                TrackFileTransaction files;
                if (! files.begin (copyDir, trackorder::takeMoves (audioDir, removed, step.destination)))
                    return false;
                if (! writeOwner (step.to) || ! files.commit())
                {
                    TrackFileTransaction::recoverOne (files.folder);
                    return false;
                }
                copy.noteProgress();
            }
            return true;
        }

        // The same take on both: a newly added copy cannot claim to hold the
        // original when it only contains the punch interval.
        static bool sameBase (juce::AudioFormatManager& fm, const TakeBase& primary, const juce::File& copyFile)
        {
            if (primary.exists != copyFile.existsAsFile()) return false;
            if (! primary.exists) return true;   // wholly new track on both copies
            const auto reader = ConcatReader::create (fm, findTakeParts (copyFile));
            return primary.readable && reader != nullptr
                && primary.length == reader->lengthInSamples
                && primary.channels == (int) reader->numChannels
                && std::abs (primary.rate - reader->sampleRate) <= 1.0;
        }

        juce::String run (CaptureCopy& copy)
        {
            if (beforeOpenForTests != nullptr) beforeOpenForTests (copy);

            const auto left = [this] (const juce::String& why) { return label + " " + why + "; it is not in this take"; };
            // STOP gave up on this drive while it hung. Its folder is no longer ours to touch.
            const auto givenUp = [&copy] { return copy.abandoned.load (std::memory_order_acquire); };
            if (givenUp()) return left ("stopped responding");
            const auto rejection = mirrorRootRejection (root, sessionDir, copyIndex == 1 ? juce::File() : backupRoot, {});
            if (rejection.isNotEmpty()) return left ("cannot be used (" + rejection + ")");
            if (! roots->claim (canonicalRoot (root))) return left ("is the same folder as another mirror");

            const auto audioDir = root.getChildFile (sessionDir.getFileName()).getChildFile ("Audio Files");
            if ((! root.isDirectory() && ! root.createDirectory().wasOk())
                || (! audioDir.isDirectory() && ! audioDir.createDirectory().wasOk()))
                return left ("could not be created (drive missing or read-only)");
            copy.noteProgress();
            if (givenUp()) return left ("stopped responding");

            const auto claimed = claim();
            if (claimed == CopyClaim::conflict)
                return left ("holds audio whose ownership cannot be verified, which was preserved");
            if (claimed == CopyClaim::unwritable)
                return left ("could not be written (full or read-only)");
            if (claimed == CopyClaim::interrupted)
                return left ("has a strip move that could not be put back");

            int recovered = 0;
            bool leadInsOwed = false;
            if (! recoverInterruptedPunches (audioDir, &recovered, &leadInsOwed))
                return left ("has an interrupted punch that could not be put back");
            if (leadInsOwed)
                return left ("still owes a silent lead-in it could not write");
            if (recovered > 0)
                return left ("had an interrupted punch put back, so it needs the session reopened");

            // Everything from here names files by strip: after the recovery
            // above, which reads the names this copy has now.
            if (! followTrackOrder (copy))
                return left ("holds its tracks in an older order and could not be moved to match");
            copy.noteProgress();

            auto selected = format;
            if (continuing || punching)
            {
                juce::String problem;
                if (! adoptExistingTakeFormat (audioDir, selected, formatChecks, sampleRate, problem))
                    return left ("holds this take in a different format or layout");
            }
            copy.noteProgress();

            if (continuing)
                for (const auto& track : tracks)
                    if (orphanedContinuation (audioDir, track.name, selected.ext))
                        return left ("is missing the start of this take");

            std::vector<std::pair<juce::File, juce::File>> stashes;
            const auto restoreStashes = [&stashes]
            {
                for (auto it = stashes.rbegin(); it != stashes.rend(); ++it)
                    if (! it->first.exists() || it->first.deleteFile())
                        it->second.moveFileTo (it->first);
            };
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            if (punching)
            {
                for (const auto& track : tracks)
                    if (! sameBase (fm, track.primaryBase, audioDir.getChildFile (track.name + selected.ext)))
                        return left ("does not hold the take being punched");
                for (const auto& track : tracks)
                    if (! stashPunchParts (audioDir.getChildFile (track.name + selected.ext), stashes))
                    {
                        restoreStashes();
                        return left ("could not move its take aside for the punch");
                    }
            }

            if (givenUp())
            {
                restoreStashes();
                return left ("stopped responding");
            }

            std::vector<juce::int64> lengths (allTrackCount, 0);
            if (continuing)
                for (std::size_t i = 0; i < allTrackCount; ++i)
                {
                    const auto base = audioDir.getChildFile (juce::String::formatted ("Track_%02d", (int) i + 1) + selected.ext);
                    if (! base.existsAsFile()) continue;
                    const auto reader = ConcatReader::create (fm, findTakeParts (base));
                    if (reader == nullptr || reader->lengthInSamples > timelineBase)
                        return left ("is unreadable or ahead of the primary");
                    lengths[i] = reader->lengthInSamples;
                }

            for (const auto& track : tracks)
            {
                if (! openTrackFile (copy, track.index, audioDir, selected, track.name, track.channels, leadIn,
                                     track.primaryHadBase, lengths[track.index], copy.leadIns))
                    copy.failTrackAtOpen (copy.tracks[track.index]);
                copy.noteProgress();
            }

            // Without its marker a crash would leave this copy's new audio at
            // the wrong place, so the copy is reported failed.
            std::vector<PendingLeadIn> kept;
            for (const auto& lead : copy.leadIns)
            {
                if (! partFileFor (lead.audioDir, lead.trackName, lead.ext,
                                   lead.firstPart + lead.parts).existsAsFile()) continue;
                if (! writeLeadInMarker (lead)) copy.latchFailure();
                kept.push_back (lead);
            }
            copy.leadIns = std::move (kept);
            return {};
        }
    };

    void MultitrackRecorder::openTakeWriters (TakeStart& take)
    {
        const auto& sessionDir = take.sessionDir;
        const auto& audioFilesDir = take.audioFilesDir;
        auto& primary = take.primary;
        auto& primaryAttempts = take.primaryAttempts;
        auto& primaryOpened = take.primaryOpened;

        // Track how many primary writers we TRY to open vs how many actually
        // open, so a disk-full / read-only / permissions failure at record
        // start is treated as the hard error it is instead of being swallowed.

        pendingLeadIns.clear();
        // Only what the estimate stamped at open assumes; preparePreRollDump
        // sets the real amount once the history is detached.
        preRollLimit = preRollFramesFitting (1 + (take.backupReady ? 1 : 0) + (int) activeMirrors.size(),
                                             armedCaptureChannels(),
                                             preRollSeconds > 0 && ! punchInActive && ! continueAsPart
                                                 ? (int) (sampleRate * preRollSeconds) : 0);
        preRollHistorySamples = preRollLimit;
        const bool editPass = (continueAsPart || punchInActive) && recordBaseSamples > 0;
        const LeadInPlan leadInPlan { editPass, continueAsPart, punchInActive, recordBaseSamples, sampleRate };

        const auto passStart = passStartSamples();
        const auto passOrigin = juce::Time::getCurrentTime();
        const auto addCopy = [&] (int copyIndex, const juce::String& label, const juce::File& root)
        {
            auto copy = std::make_shared<CaptureCopy>();
            copy->copyIndex   = copyIndex;
            copy->label       = label;
            copy->root        = root;
            copy->sampleRate  = sampleRate;
            copy->sessionName = sessionDir.getFileName();
            copy->passStart   = passStart;
            copy->passOrigin  = passOrigin;
            copy->latches     = latches;
            copy->beforeWriteForTests = beforeCopyWriteForTests;
            copy->threadRefusedForTests = copyThreadRefusedForTests;
            copy->tracks.resize (tracks.size());
            copies.push_back (copy);
            return copy;
        };
        const auto primaryCopy = addCopy (0, "Primary", sessionDir);
        const auto backupCopy  = take.backupReady ? addCopy (1, "Backup", backupDir) : std::shared_ptr<CaptureCopy>();
        std::vector<std::shared_ptr<CaptureCopy>> mirrorCopies;
        for (std::size_t mi = 0; mi < activeMirrors.size(); ++mi)
            mirrorCopies.push_back (addCopy (2 + (int) mi, "Mirror " + activeMirrors[mi].root.getFullPathName(),
                                             activeMirrors[mi].root));

        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            WriterChannel w;
            const auto trackName = juce::String::formatted ("Track_%02d", (int) i + 1);

            // RIGHT half of an interleaved stereo pair. When the previous
            // track is an armed stereo-L, THIS track's audio is written into
            // that L's single 2-channel file -- so it gets NO writer of its
            // own. Push an empty WriterChannel for index alignment + reset
            // its FIFO; the L channel's drain step reads this FIFO and does
            // the 2-ch write. (Checked before the arm gate: the R partner is
            // itself armed, so it would otherwise open a second mono file.)
            if (i > 0
                && tracks[i - 1]->isStereo.load (std::memory_order_relaxed)
                && isArmedCapture (i - 1))
            {
                writers.push_back (std::move (w));
                fifos[i]->fifo.reset();
                continue;
            }

            // No writer for unarmed tracks. Push
            // an empty WriterChannel so the per-channel index stays aligned
            // with tracks[i]; processBlock's arm-gate already skips both.
            //
            // CRITICAL: unarmed tracks must NOT open a writer, because
            // openWriterAtPath truncates the target file (deleteFile). When
            // a second take into the SAME session arms a different channel,
            // opening writers for the still-unarmed earlier tracks would
            // wipe their Track_NN.wav from the first take -- silent data
            // loss. Skipping them here leaves those files intact.
            if (! tracks[i]->armed.load (std::memory_order_relaxed))
            {
                writers.push_back (std::move (w));
                fifos[i]->fifo.reset();
                continue;
            }

            // LEFT half of an armed stereo pair (isStereo set + a valid R
            // partner) records ONE interleaved 2-channel file. Otherwise a
            // standalone mono track. The channel count flows into every
            // destination (primary / backup / mirrors) and the drain path.
            const int chans = (tracks[i]->isStereo.load (std::memory_order_relaxed)
                               && i + 1 < tracks.size()) ? 2 : 1;
            w.numChannels = chans;
            w.bextNameLeft = tracks[i]->getNameThreadSafe();
            if (chans == 2) w.bextNameRight = tracks[i + 1]->getNameThreadSafe();

            // Primary -- under <session>/Audio Files/Track_NN.<ext>. Continue
            // mode appends a NEW part (Track_NN_partXX) so the existing take is
            // never touched; otherwise it's the normal Track_NN.<ext>.
            invalidateReportsBeforeMediaMutation (take);
            ++primaryAttempts;
            primaryCopy->tracks[i].nameLeft  = w.bextNameLeft;
            primaryCopy->tracks[i].nameRight = w.bextNameRight;
            primaryCopy->tracks[i].channels  = chans;
            if (openTrackFile (*primaryCopy, i, audioFilesDir, primary, trackName, chans, leadInPlan,
                               take.hadPrimaryBase[i], take.previousLengths[i], pendingLeadIns))
                ++primaryOpened;

            // Every other copy opens its own files after go-live. Only what a
            // ring needs is settled here.
            for (const auto& copy : copies)
            {
                if (copy->copyIndex == 0) continue;
                copy->tracks[i].nameLeft  = w.bextNameLeft;
                copy->tracks[i].nameRight = w.bextNameRight;
                copy->tracks[i].channels  = chans;
            }

            // This channel opened (or attempted) real destinations, so it
            // participates in the take and MUST keep draining even if its
            // primary later fails -- see WriterChannel::active. Empty R slots
            // and unarmed channels `continue`d above and stay inactive.
            w.active = true;

            writers.push_back (std::move (w));

            fifos[i]->fifo.reset();
        }

        int capturedChannels = 0;
        for (const auto& w : writers)
            if (w.active) capturedChannels += w.numChannels;
        const int ringFrames = copyRingFrames ((int) copies.size(), capturedChannels) + preRollLimit;
        for (auto& copy : copies)
        {
            copy->ringFrames = ringFrames;
            for (std::size_t i = 0; i < copy->tracks.size(); ++i)
                if (copy->copyIndex == 0 ? copy->tracks[i].writer != nullptr : writers[i].active)
                    copy->openRing (copy->tracks[i]);
        }

        const auto planFor = [&] (const std::shared_ptr<CaptureCopy>& copy, TakeFormat format)
        {
            auto plan = std::make_shared<CopyOpen>();
            plan->label          = copy->copyIndex == 1 ? "Backup " + copy->root.getFullPathName() : copy->label;
            plan->copyIndex      = copy->copyIndex;
            plan->root           = copy->root;
            plan->sessionDir     = sessionDir;
            plan->backupRoot     = backupDir;
            plan->sessionId      = take.sessionId;
            plan->trackOrder     = take.trackOrder;
            plan->format         = format;
            plan->continuing     = continueAsPart;
            plan->punching       = punchInActive;
            plan->leadIn         = leadInPlan;
            plan->timelineBase   = recordBaseSamples;
            plan->sampleRate     = sampleRate;
            plan->formatChecks   = formatChecks();
            plan->roots          = openingRoots;
            plan->beforeOpenForTests = beforeCopyOpenForTests;
            for (std::size_t i = 0; i < writers.size(); ++i)
                if (writers[i].active)
                    plan->tracks.push_back ({ i, juce::String::formatted ("Track_%02d", (int) i + 1),
                                              writers[i].numChannels, take.hadPrimaryBase[i],
                                              i < take.primaryBases.size() ? take.primaryBases[i] : TakeBase {} });
            plan->allTrackCount = tracks.size();
            copy->opening.store (true, std::memory_order_release);
            copy->openForTake = [plan] (CaptureCopy& self) { return plan->run (self); };
        };
        openingRoots = std::make_shared<CopyRoots>();
        if (backupCopy != nullptr) planFor (backupCopy, takeFormatFor (backupCaptureFormat));
        for (std::size_t mi = 0; mi < mirrorCopies.size(); ++mi)
            planFor (mirrorCopies[mi], takeFormatFor (activeMirrors[mi].format));
    }

    static constexpr juce::int64 kMaxRingBytes = (juce::int64) 1 << 30;

    // Four seconds or so per copy, like the audio FIFO, until that is more than
    // a gigabyte of ring: then less, never under a second. A take with pre-roll
    // adds preRollFramesFitting on top.
    int MultitrackRecorder::copyRingFrames (int numCopies, int channels) const noexcept
    {
        if (copyRingFramesForTests > 0) return juce::jmax (copyRingFramesForTests, CaptureCopy::kChunkFrames);
        const int floorFrames = juce::jmax (1, (int) sampleRate);
        auto frames = juce::nextPowerOfTwo ((int) (sampleRate * kFifoSeconds));
        while (frames / 2 >= floorFrames
               && (juce::int64) frames * (juce::int64) sizeof (float) * channels * numCopies > kMaxRingBytes)
            frames /= 2;
        return frames;
    }

    // A copy writes its pre-roll before its first live sample, so live audio
    // waits in its ring for as long as the dump takes. A drive that keeps real
    // time takes no longer to write L frames than L frames last, so a ring with
    // room for L on top of its usual margin loses nothing to the dump, whatever
    // the page cache does. Below real time no length of pre-roll saves a take.
    // Past the memory limit the pre-roll gives way, never the live audio.
    int MultitrackRecorder::preRollFramesFitting (int numCopies, int channels, int wanted) const noexcept
    {
        const auto limit = copyRingBytesForTests > 0 ? copyRingBytesForTests : kMaxRingBytes;
        const auto bytesPerFrame = (juce::int64) sizeof (float) * juce::jmax (1, channels) * juce::jmax (1, numCopies);
        const auto room = limit / bytesPerFrame - copyRingFrames (numCopies, channels);
        return (int) juce::jlimit ((juce::int64) 0, (juce::int64) wanted, room);
    }

    int MultitrackRecorder::armedCaptureChannels() const noexcept
    {
        int channels = 0;
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            if (! tracks[i]->armed.load (std::memory_order_relaxed)) continue;
            if (i > 0 && tracks[i - 1]->isStereo.load (std::memory_order_relaxed)
                      && tracks[i - 1]->armed.load (std::memory_order_relaxed))
                continue;
            channels += tracks[i]->isStereo.load (std::memory_order_relaxed) && i + 1 < tracks.size() ? 2 : 1;
        }
        return channels;
    }

    double MultitrackRecorder::preRollSecondsThatFit() const noexcept
    {
        if (sampleRate <= 0.0) return 0.0;
        const int copyCount = 1 + (backupDir.getFullPathName().isNotEmpty() ? 1 : 0) + (int) mirrorConfigs.size();
        return preRollFramesFitting (copyCount, armedCaptureChannels(), (int) (sampleRate * preRollSeconds)) / sampleRate;
    }

    juce::String MultitrackRecorder::getPreRollNotice() const
    {
        const auto cut = preRollCutFrames.load (std::memory_order_acquire);
        if (cut <= 0 || sampleRate <= 0.0) return {};
        return "Pre-roll cut to " + juce::String ((double) preRollHistorySamples / sampleRate, 1)
             + " s of " + juce::String ((double) (preRollHistorySamples + cut) / sampleRate, 1)
             + " s -- this many channels and copies fill the capture memory";
    }

    const MultitrackRecorder::CaptureCopy* MultitrackRecorder::copyAt (int copyIndex) const noexcept
    {
        for (const auto& copy : copies)
            if (copy->copyIndex == copyIndex) return copy.get();
        return nullptr;
    }

    bool MultitrackRecorder::isStillLeftBehind (const juce::File& root) const
    {
        for (const auto& copy : leftBehind)
            if (copy->root == root && ! copy->finished.load (std::memory_order_acquire)) return true;
        return false;
    }

    bool MultitrackRecorder::isCopyOutOfTake (int copyIndex) const noexcept
    {
        const auto* copy = copyAt (copyIndex);
        return copy != nullptr && (copy->abandoned.load (std::memory_order_acquire)
                                   || copy->dropped.load (std::memory_order_acquire));
    }

    // Persist each reservation openTakeWriters made, so a crash before STOP
    // can still complete the lead-in on the next session open. A copy whose
    // writer never opened records nothing this pass and needs no silence.
    bool MultitrackRecorder::writeLeadInMarkers (TakeStart& take)
    {
        juce::ignoreUnused (take);
        bool ok = true;
        std::vector<PendingLeadIn> kept;
        for (const auto& lead : pendingLeadIns)
        {
            if (! partFileFor (lead.audioDir, lead.trackName, lead.ext,
                               lead.firstPart + lead.parts).existsAsFile()) continue;
            // Without its marker a crash would leave the new audio at the wrong
            // place, so the primary refuses the take.
            if (! writeLeadInMarker (lead)) ok = false;
            kept.push_back (lead);
        }
        pendingLeadIns = std::move (kept);
        if (! ok) startError = "Recording refused: the lead-in marker could not be written; check the recording drive.";
        return ok;
    }

    bool MultitrackRecorder::writeLeadInMarker (const PendingLeadIn& lead)
    {
        juce::DynamicObject::Ptr m (new juce::DynamicObject());
        m->setProperty ("track",         lead.trackName);
        m->setProperty ("ext",           lead.ext);
        m->setProperty ("firstPart",     lead.firstPart);
        m->setProperty ("parts",         lead.parts);
        m->setProperty ("container",     lead.container);
        m->setProperty ("bitDepth",      lead.bitDepth);
        m->setProperty ("channels",      lead.channels);
        m->setProperty ("sampleRate",    lead.sampleRate);
        m->setProperty ("priorLength",   lead.priorLength);
        m->setProperty ("timelineStart", lead.timelineStart);
        const auto marker = leadInMarkerFor (lead);
        return marker.getParentDirectory().createDirectory().wasOk()
            && atomicfile::writeText (marker, juce::JSON::toString (juce::var (m.get())));
    }

    juce::File MultitrackRecorder::partFileFor (const juce::File& audioDir, const juce::String& trackName,
                                                const juce::String& ext, int part)
    {
        return part <= 1 ? audioDir.getChildFile (trackName + ext)
                         : audioDir.getChildFile (trackName + "_part"
                               + juce::String::formatted ("%02d", part) + ext);
    }

    juce::File MultitrackRecorder::leadInMarkerFor (const PendingLeadIn& lead)
    {
        return lead.audioDir.getChildFile ("Pending Lead-ins").getChildFile (lead.trackName + ".json");
    }

    bool MultitrackRecorder::writeSilenceFile (const juce::File& file, int container, int bits,
                                               int chans, double sampleRate, juce::int64 frames)
    {
        file.deleteFile();
        const auto closeFailed = CheckedFileStream::makeFailedFlag();
        auto os = CheckedFileStream::open (file, closeFailed);
        if (os == nullptr) return false;
        const auto channels = (unsigned int) juce::jmax (1, chans);
        std::unique_ptr<juce::AudioFormatWriter> w;
        if (container == 2)
            w.reset (juce::FlacAudioFormat().createWriterFor (os.get(), sampleRate, channels, bits, {}, 5));
        else if (container == 1)
            w.reset (bits == 32 ? FloatAiffWriter::create (os.get(), sampleRate, channels)
                                : juce::AiffAudioFormat().createWriterFor (os.get(), sampleRate, channels, bits, {}, 0));
        else
            w.reset (juce::WavAudioFormat().createWriterFor (os.get(), sampleRate, channels, bits, {}, 0));
        if (w == nullptr) { os.reset(); file.deleteFile(); return false; }
        os.release();

        constexpr int kBlock = 16384;
        const std::vector<float> zeros ((std::size_t) kBlock, 0.0f);
        const float* planes[2] { zeros.data(), zeros.data() };
        bool ok = true;
        for (auto remaining = frames; ok && remaining > 0;)
        {
            const int count = (int) juce::jmin ((juce::int64) kBlock, remaining);
            ok = w->writeFromFloatArrays (planes, (int) channels, count);
            remaining -= count;
        }
        if (ok) ok = flushFloatAiffBeforeClose (w.get());
        w.reset();
        ok = ok && ! closeFailed->load();
        if (! ok) file.deleteFile();
        return ok;
    }

    bool MultitrackRecorder::writeLeadIn (const PendingLeadIn& lead, juce::int64 timelineStart)
    {
        // No audio reached this copy this pass: nothing to align.
        if (! partFileFor (lead.audioDir, lead.trackName, lead.ext,
                           lead.firstPart + lead.parts).existsAsFile()) return true;
        const auto silence = juce::jmax ((juce::int64) 0, timelineStart - lead.priorLength);
        const int parts = juce::jmax (1, lead.parts);
        // Spread evenly so no reserved slot is left empty; each share stays
        // under the container's ceiling because the slots were sized for it.
        for (int p = 0; p < parts; ++p)
        {
            const auto share = silence / parts + (p < silence % parts ? 1 : 0);
            if (! writeSilenceFile (partFileFor (lead.audioDir, lead.trackName, lead.ext, lead.firstPart + p),
                                    lead.container, lead.bitDepth, lead.channels, lead.sampleRate, share))
                return false;
        }
        return true;
    }

    bool MultitrackRecorder::leadInsPending (const juce::File& audioDir)
    {
        return ! audioDir.getChildFile ("Pending Lead-ins").findChildFiles (
            juce::File::findFiles | juce::File::ignoreHiddenFiles, false, "*.json").isEmpty();
    }

    bool MultitrackRecorder::recoverPendingLeadIns (const juce::File& audioDir)
    {
        const auto folder = audioDir.getChildFile ("Pending Lead-ins");
        if (! folder.isDirectory()) return true;
        bool ok = true;
        for (const auto& marker : folder.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                                         false, "*.json"))
        {
            // A marker nothing can read can never be finished, so it must not
            // hold every later pass. It is kept beside the others, as a record.
            const auto setAside = [&marker, &ok]
            {
                marker.moveFileTo (marker.withFileExtension ("unreadable"));
                ok = false;
            };
            const auto v = juce::JSON::parse (marker);
            if (! v.isObject()) { setAside(); continue; }
            PendingLeadIn lead;
            lead.audioDir      = audioDir;
            lead.trackName     = v["track"].toString();
            lead.ext           = v["ext"].toString();
            lead.firstPart     = (int) v["firstPart"];
            lead.parts         = (int) v["parts"];
            lead.container     = (int) v["container"];
            lead.bitDepth      = (int) v["bitDepth"];
            lead.channels      = (int) v["channels"];
            lead.sampleRate    = (double) v["sampleRate"];
            lead.priorLength   = (juce::int64) v["priorLength"];
            lead.timelineStart = (juce::int64) v["timelineStart"];
            if (! lead.trackName.startsWith ("Track_") || lead.ext.isEmpty() || lead.parts < 1
                || lead.firstPart < 1 || lead.sampleRate <= 0.0)
            { setAside(); continue; }
            if (writeLeadIn (lead, lead.timelineStart)) marker.deleteFile();
            else ok = false;
        }
        if (folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0) folder.deleteFile();
        return ok;
    }

    // Undo a start that opened writers but cannot go live: close them, delete
    // the files this pass created, and put any punch base back.
    void MultitrackRecorder::abandonTakeStart (TakeStart& take)
    {
        std::vector<juce::File> unusedNewFiles;
        // Every file in partFiles is one this start created: opening refuses
        // an existing target, so a continuation's new part is safe to remove.
        for (std::size_t i = 0; i < writers.size(); ++i)
            if (writers[i].active)
                for (const auto& copy : copies)
                    for (const auto& name : copy->tracks[i].partFiles)
                        unusedNewFiles.push_back (copy->tracks[i].baseFile.getSiblingFile (name));
        closeWriters();
        for (const auto& f : unusedNewFiles) f.deleteFile();
        for (const auto& lead : pendingLeadIns)
        {
            leadInMarkerFor (lead).deleteFile();
            partFileFor (lead.audioDir, lead.trackName, lead.ext, lead.firstPart + lead.parts).deleteFile();
        }
        pendingLeadIns.clear();
        restorePunchStashes (take);
    }

    void MultitrackRecorder::goLive (const TakeStart& take)
    {
        const auto& sessionDir = take.sessionDir;
        const bool backupRequested = take.backupRequested;
        auto& backupReady = take.backupReady;
        auto& primaryAttempts = take.primaryAttempts;
        auto& primaryOpened = take.primaryOpened;

        // Until the copy says otherwise: it opens on its own thread from here.
        backupActive.store (backupRequested && backupReady, std::memory_order_relaxed);
        latches->mirrorsDropped.store (0, std::memory_order_relaxed);
        latches->backupDropped.store (false, std::memory_order_relaxed);
        {
            const std::lock_guard<std::mutex> guard (latches->droppedLock);
            latches->dropped.clear();
        }
        // Surface a PARTIAL primary-open failure (some armed tracks opened,
        // some didn't) via the existing flag so the UI + report show it.
        primaryFailed.store (primaryAttempts > 0 && primaryOpened < primaryAttempts,
                             std::memory_order_relaxed);

        // Recovery marker -- deleted on clean stop.
        {
            const auto now = juce::Time::getCurrentTime();
            juce::DynamicObject::Ptr m (new juce::DynamicObject());
            m->setProperty ("startedAt",  now.toISO8601 (true));
            m->setProperty ("sampleRate", sampleRate);
            m->setProperty ("numTracks",  (int) tracks.size());
            recoveryMarkerFailed.store (
                ! atomicfile::writeText (sessionDir.getChildFile ("recording.session"),
                                         juce::JSON::toString (juce::var (m.get()))),
                std::memory_order_relaxed);
        }

        samplesSinceStart.store (0, std::memory_order_relaxed);
        missedSamples    .store (0, std::memory_order_relaxed);
        latches->primaryLostFrames.store (0, std::memory_order_relaxed);
        captureDeviceLost.store (false, std::memory_order_relaxed);
        deviceStallMs.store (0, std::memory_order_relaxed);
        for (auto& f : fifos) f->resetCaptureTimeline();
        alarmFrames.store (0, std::memory_order_relaxed);
        // Clear each track's live waveform overview so a new take draws fresh
        // (done before the audio thread starts pushing into it).
        for (auto& t : tracks)
        {
            t->liveWaveReset();
            t->lastInputSignal.store (-1, std::memory_order_relaxed);
        }

        // Allocate outside the callback boundary, then detach ALL histories
        // and enable live capture in one constant-time transition. Disk I/O
        // reads the detached snapshot while the callback fills the live FIFO.
        std::vector<std::unique_ptr<PreRollBuffer>> replacement;
        for (const auto& history : preRoll)
        {
            auto next = std::make_unique<PreRollBuffer>();
            next->allocate ((int) history->data.size());
            replacement.push_back (std::move (next));
        }
        {
            const juce::SpinLock::ScopedLockType guard (captureBoundary);
            preRollSnapshot.swap (preRoll);
            preRoll.swap (replacement);
            punchCaptureOrigin.store (-1, std::memory_order_relaxed);
            recording.store (true, std::memory_order_release);
        }
        preparePreRollDump();
        beginChannelAlarms();
        for (auto& copy : copies) copy->start (copy);
        writersReady.store (true,  std::memory_order_release);
        clockProbe.request (clockDevice);
    }

    // Runs after preparePreRollDump: alarmOrigin reads the pre-roll length,
    // and alarmsRunning under alarmLock is what lets another thread do that.
    void MultitrackRecorder::beginChannelAlarms()
    {
        std::vector<channelalarm::Monitor::Input> inputs;
        std::vector<std::shared_ptr<TrackState>> watched;
        for (std::size_t i = 0; i < writers.size(); ++i)
        {
            if (! writers[i].active) continue;
            for (int side = 0; side < writers[i].numChannels; ++side)
            {
                inputs.push_back ({ (int) i, writers[i].bextNameLeft });
                watched.push_back (tracks[i + (std::size_t) side]);
            }
        }
        const juce::ScopedLock sl (alarmLock);
        alarmMonitor.begin (std::move (inputs), sampleRate);
        alarmTracks = std::move (watched);
        alarmScratch.assign (alarmTracks.size(), -1);
        alarmsRunning = true;
    }

    juce::int64 MultitrackRecorder::alarmOrigin() const noexcept
    {
        return recordBaseSamples.load (std::memory_order_relaxed) + preRollHistorySamples;
    }

    // `now` is read first so a thread stalled between these loads can only see
    // a channel as more recent than `now`, never raise a false fault.
    void MultitrackRecorder::updateChannelAlarmsLocked()
    {
        const auto now = alarmFrames.load (std::memory_order_relaxed);
        for (std::size_t i = 0; i < alarmTracks.size(); ++i)
            alarmScratch[i] = alarmTracks[i]->lastInputSignal.load (std::memory_order_relaxed);
        alarmMonitor.update (alarmScratch, now);
    }

    std::vector<channelalarm::Alarm> MultitrackRecorder::evaluateChannelAlarms()
    {
        const juce::ScopedLock sl (alarmLock);
        if (! alarmsRunning) return {};
        updateChannelAlarmsLocked();
        return alarmMonitor.live (alarmOrigin());
    }

    namespace { std::atomic<juce::int64> s_autoSplitThresholdOverride { 0 }; }

    void MultitrackRecorder::setAutoSplitThresholdBytesForTests (juce::int64 bytes) noexcept
    {
        s_autoSplitThresholdOverride.store (juce::jmax ((juce::int64) 0, bytes),
                                            std::memory_order_release);
    }

    MultitrackRecorder::SmartStatus MultitrackRecorder::querySmartStatus (const juce::File& mountPoint)
    {
       #if JUCE_MAC
        if (! mountPoint.exists()) return SmartStatus::Unknown;

        juce::ChildProcess proc;
        const juce::StringArray cmd { "/usr/sbin/diskutil", "info",
                                       mountPoint.getFullPathName() };
        if (! proc.start (cmd, juce::ChildProcess::wantStdOut)) return SmartStatus::Unknown;

        // WAIT FIRST, THEN READ -- and kill on timeout. readAllProcessOutput()
        // loops on a BLOCKING pipe read until the child closes it, so it has no
        // timeout of its own; calling it first made the waitForProcessToFinish
        // below dead code and let an unresponsive volume (network mount,
        // spun-down USB, a wedged diskutil) block the caller indefinitely.
        // `diskutil info` emits a couple of KB -- far under the 64 KiB pipe
        // buffer -- so the child can always run to completion without us
        // draining it, which makes wait-then-read safe here.
        if (! proc.waitForProcessToFinish (2000))
        {
            proc.kill();
            return SmartStatus::Unknown;
        }
        const auto out = proc.readAllProcessOutput();

        for (const auto& line : juce::StringArray::fromLines (out))
        {
            if (! line.containsIgnoreCase ("SMART Status")) continue;
            if (line.containsIgnoreCase ("Verified")) return SmartStatus::Verified;
            if (line.containsIgnoreCase ("Failing"))  return SmartStatus::Failing;
            return SmartStatus::Unknown;
        }
        return SmartStatus::Unknown;
       #else
        juce::ignoreUnused (mountPoint);
        return SmartStatus::Unknown;
       #endif
    }

    void MultitrackRecorder::updateDiskHealth (juce::int64 expectedBytesPerSec) noexcept
    {
        // Only meaningful while recording AND we have an expected rate.
        if (! recording.load (std::memory_order_acquire) || expectedBytesPerSec <= 0)
        {
            diskStrugglingStreak = 0;
            diskStrugglingFlag.store (false, std::memory_order_relaxed);
            return;
        }
        const float actual = getDiskBytesPerSec();
        const float ratio  = juce::jmin (1.0f, actual / (float) expectedBytesPerSec);
        // EMA smoother so a single short stall doesn't trigger.
        diskKeepUpEma = 0.7f * diskKeepUpEma + 0.3f * ratio;
        // Below 85 % keep-up for ~3 consecutive samples (~6 s at 2 Hz
        // polling) flips the warning. Reset the streak on recovery so
        // the warning clears once the disk catches up.
        if (diskKeepUpEma < 0.85f)
            ++diskStrugglingStreak;
        else
            diskStrugglingStreak = 0;
        diskStrugglingFlag.store (diskStrugglingStreak >= 3,
                                  std::memory_order_relaxed);
    }

    bool MultitrackRecorder::setMirrors (const std::vector<MirrorConfig>& configs)
    {
        if (isRecording() || isFinalizing()) return false;   // only editable when stopped
        mirrorConfigs = configs;
        return true;
    }

    juce::String MultitrackRecorder::mirrorRootRejection (
        const juce::File& requestedRoot,
        const juce::File& requestedSession,
        const juce::File& requestedBackup,
        const std::vector<juce::File>& alreadyAccepted)
    {
        const auto root = canonicalRoot (requestedRoot);
        const auto sessionDir = canonicalRoot (requestedSession);
        const auto backupRoot = canonicalRoot (requestedBackup);
        if (root == juce::File()) return "No folder chosen";

        // A mirror writes root/<sessionName>/Audio Files/. The primary writes
        // sessionDir/Audio Files/. Those are the same path exactly when the root
        // IS the session's parent -- and with the default WAV 24-bit mirror
        // format matching the default capture format, the same filename too.
        if (sessionDir != juce::File())
        {
            if (root == sessionDir.getParentDirectory())
                return "That folder holds this session -- the mirror would "
                       "overwrite the take it is meant to protect";
            if (root == sessionDir || root.isAChildOf (sessionDir))
                return "That folder is inside the session -- a mirror there is "
                       "not a second copy";
        }

        // The backup copy uses the identical root/<sessionName>/Audio Files
        // shape, so a mirror aimed at the backup root collides with it.
        if (backupRoot != juce::File() && root == backupRoot)
            return "That is already the backup destination";

        for (const auto& other : alreadyAccepted)
            if (root == canonicalRoot (other))
                return "Another mirror already writes to that folder";

        return {};
    }

    bool MultitrackRecorder::anyMirrorFailed() const noexcept
    {
        // Every open/write/roll/padding failure latches this atomic. Live
        // telemetry must never traverse worker-owned writers during close.
        return mirrorFailed.load (std::memory_order_relaxed);
    }

    juce::int64 MultitrackRecorder::maxBytesForContainer (int containerCode) noexcept
    {
        // Test override (set via setAutoSplitThresholdBytesForTests) wins
        // when > 0. Lets unit tests force the roll path without writing
        // actual gigabytes.
        const auto override_ = s_autoSplitThresholdOverride.load (std::memory_order_acquire);
        if (override_ > 0) return override_;

        // WAV is written as RF64 once it crosses 4 GiB: JUCE's WavAudioFormat
        // reserves the ds64 chunk slot at file creation and rewrites the
        // header to RF64 on flush / close (the periodic in-take flush keeps it
        // current). So a WAV take is NEVER split into _partNN files -- it
        // stays one continuous file (an ordinary RIFF/WAV while under 4 GiB,
        // read by any DAW). Return "no ceiling" so the roll path never fires
        // for WAV.
        // NOTE: the >4 GiB RF64 promotion is exercised by JUCE but not yet
        // field-verified end-to-end on this app's write path (it needs a real
        // multi-GB capture, not a code change) -- flagged for a hardware test.
        if (containerCode == 0)
            return std::numeric_limits<juce::int64>::max();

        // AIFF + FLAC have no RF64 equivalent here, so they still roll to a
        // _partNN file at their chunk-size ceiling.
        // AIFF: 2 GiB signed-32-bit chunk-size ceiling -> roll at 1.9 GiB.
        // FLAC: 4 GiB -> roll at 3.9 GiB.
        constexpr juce::int64 kFlacMax = (juce::int64) 3900LL * 1024 * 1024;
        constexpr juce::int64 kAiffMax = (juce::int64) 1900LL * 1024 * 1024;
        return (containerCode == 1) ? kAiffMax : kFlacMax;
    }

    juce::int64 MultitrackRecorder::passStartSamples() const noexcept
    {
        const auto firstLive = firstLiveTicks.load (std::memory_order_acquire);
        const auto liveStart = (juce::int64) std::llround (
                timeAnchor.secondsAt (firstLive != 0 ? firstLive : juce::Time::getHighResolutionTicks())
                * sampleRate)
            - (firstLive != 0 ? firstLiveBlock.load (std::memory_order_relaxed) : 0);
        return liveStart - preRollHistorySamples;
    }

    bool MultitrackRecorder::beginFinalization()
    {
        const juce::ScopedLock structureGuard (structureLock);
        const juce::SpinLock::ScopedLockType guard (captureBoundary);
        if (finalizing.load() || ! recording.load()) return false;
        finalizing.store (true, std::memory_order_release);
        recording.store (false, std::memory_order_release);
        return true;
    }

    void MultitrackRecorder::stopRecording()
    {
        if (! beginFinalization()) return;
        const juce::ScopeGuard releaseLease { [this] { endFinalization(); } };
        finishFinalization();
    }

    // What finishFinalization knows about the take once its writers are
    // quiesced: the totals, and each writer's files, captured while `writers`
    // still exists (closeWriters clears it).
    struct MultitrackRecorder::TakeStop
    {
        struct MirrorReport
        {
            juce::File   root;
            juce::int64  totalSamples;
            juce::StringArray partFiles;
            bool failed;
            bool abandoned { false };   // left behind at STOP: its folder is not touched again
        };
        struct WriterReport
        {
            juce::int64 totalSamplesPrimary;
            juce::int64 totalSamplesBackup;
            juce::StringArray partFilesPrimary;
            juce::StringArray partFilesBackup;
            std::vector<MirrorReport> mirrors;
        };
        // Punch-in: the final on-disk paths of every copy (primary/backup/each
        // mirror), aligned with `writers`, so each can be spliced after the
        // writers close.
        struct PunchFiles
        {
            juce::File main;                // Track_NN.<ext>
            std::vector<juce::File> fresh;  // every part the punch wrote, in order
            juce::int64 maxBytesPerFile { 0 };
            bool complete { true };         // this copy holds every frame of the punch
        };
        struct PunchWriterFiles { PunchFiles primary, backup; std::vector<PunchFiles> mirrors; };
        // What the primary did with a track. Every copy follows it.
        enum class PunchVerdict { notPunched, nothingCaptured, committed, discarded };

        juce::Time  stoppedAt;
        juce::int64 totalSamples {}, totalMissed {};
        bool        deviceLostAtStop {};
        juce::int64 deviceStallMsAtStop {};
        double      totalSeconds {};
        bool        backupWasRunning {};
        bool        doPunch {};
        juce::int64 timelineStart {};   // where this pass sits on the timeline (punch origin aware)
        std::vector<WriterReport>     writers;
        std::vector<PunchWriterFiles> punchFiles;
        std::vector<PunchVerdict>     punchVerdicts;
        juce::int64                   punchPosition {};
        std::vector<WriterChannel::StampedPart> stampedParts;   // the primary's
        juce::int64       timeReference {};
        juce::StringArray stampFailures;
        juce::StringArray copyProblems;
        std::vector<channelalarm::Alarm> channelAlarms;
    };

    // Everything session.report.json says, captured BY VALUE -- no `this`, no
    // engine state -- so the background hashing thread can rebuild it safely
    // even if the recorder is torn down before hashing finishes.
    struct MultitrackRecorder::SessionReport
    {
        struct TrackMeta
        {
            int          index;
            juce::String name;
            int          clipCount;
            juce::int64  lastClipSample;
            int          inputRouting, outputRouting;
            bool         isStereo;
        };

        juce::File  sessionDir, bDir;
        double      sr {};
        int         fmt {}, preRoll {};
        juce::Time  stoppedAt;
        juce::int64 reportedSamples {}, reportedMissed {};
        double      reportedSeconds {};
        juce::int64 passSamples {}, passMissed {};
        double      passSeconds {};
        int         passPreRoll {}, passPreRollCut {};
        bool        backupWasRunning {}, reportedBackupFailure {}, reportedPrimaryFailure {},
                    reportedPunchFailure {}, reportedMirrorFailure {}, reportedDeviceLoss {};
        juce::int64 reportedDeviceStallMs {};
        int         reportedMirrorsSkipped {};
        juce::int64  timeReference {};
        juce::String timeReferenceSource;
        juce::StringArray stampFailures;
        juce::StringArray copyProblems;
        clocksource::Reading clock;
        juce::Array<juce::var> channelAlarms;
        std::vector<TrackMeta>               trackMetas;
        std::vector<TakeStop::WriterReport>  writerSnaps;
        juce::var                            twin;

        juce::String toJson (bool withHashes, bool hashingFailed,
                             const std::map<juce::String, juce::String>& shaByPath,
                             const juce::var& twinBlock, bool copyHashesPending = false) const;
    };

    void MultitrackRecorder::finishFinalization()
    {
        jassert (isFinalizing() && ! isRecording());
        if (beforeFinalizationForTests) beforeFinalizationForTests();

        quiesceWritersForStop();
        auto stop = snapshotTakeAtStop();

        closeWriters();
        backupActive.store (false, std::memory_order_relaxed);
        reattachWriterShards();

        // The primary's files come first and wait on nothing: what a punch did
        // to the take is settled before any copy is touched. Each copy then
        // finishes its own files on its own thread, so a drive that hangs here
        // costs that copy and a bounded wait.
        if (stop.doPunch) splicePunchedTracks (stop);
        completeLeadIns (stop);
        stampTakeFiles (stop);
        manifestSessionMedia (stop);
        finishCopyFiles (stop);

        punchInActive     = false;
        continueAsPart    = false;   // consume the continue arm
        recordBaseSamples = 0;
        punchCaptureOrigin.store (-1, std::memory_order_relaxed);

        publishSessionReport (buildSessionReport (stop));

        // Clean stop -- remove the recovery marker.
        if (activeSessionDir.isDirectory())
            activeSessionDir.getChildFile ("recording.session").deleteFile();

        std::atomic_store (&diskWatch, std::shared_ptr<const DiskWatch>());
    }

    // The exact BWF stamp, on every WAV this pass wrote: the first frame's real
    // time plus the frames before that part. The only write a closed capture
    // file ever gets. It runs after every writer closed and before
    // publishSessionReport starts the hash thread, so the hashes describe the
    // final bytes. A file that refuses keeps its estimate and goes in the
    // report; its audio is fine, so no capture failure is latched.
    void MultitrackRecorder::stampTakeFiles (TakeStop& stop)
    {
        if (beforeStampForTests) beforeStampForTests();
        const auto start = passStartSamples();
        stop.timeReference = bwf::wrapToDay (start, sampleRate);
        if (firstLiveTicks.load (std::memory_order_acquire) == 0) return;   // nothing went live

        for (const auto& part : stop.stampedParts)
        {
            const auto result = bwf::stampTimeReference (
                part.file, part.stampedAtOpen,
                (juce::uint64) bwf::wrapToDay (start + part.framesBefore, sampleRate));
            if (result != bwf::StampResult::ok)
                stop.stampFailures.add (part.file.getFullPathName() + ": " + bwf::toString (result));
        }
    }

    // Fill the lead-in slots reserved at start now that the pass's real
    // timeline start is known (a rolling punch begins where capture began).
    // Runs before the manifest, so the report lists the silence parts too.
    void MultitrackRecorder::completeLeadIns (const TakeStop& stop)
    {
        for (const auto& lead : pendingLeadIns)
            if (lead.copyIndex == 0 && ! finishLeadIn (lead, stop.timelineStart))
                primaryFailed.store (true, std::memory_order_relaxed);
    }

    // On failure the marker stays, so the next session open retries the write.
    bool MultitrackRecorder::finishLeadIn (const PendingLeadIn& lead, juce::int64 timelineStart)
    {
        if (! writeLeadIn (lead, timelineStart)) return false;
        const auto marker = leadInMarkerFor (lead);
        marker.deleteFile();
        const auto folder = marker.getParentDirectory();
        if (folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0)
            folder.deleteFile();
        return true;
    }

    void MultitrackRecorder::quiesceWritersForStop()
    {
        // Quiesce the distributor BEFORE the final flush + close.
        // removeTimeSliceClient blocks until any in-flight drain on the shard
        // returns and guarantees no further calls, so after this loop the
        // finalization caller is the sole FIFO owner. It is either the owned
        // STOP worker or the legacy synchronous caller, never a writer thread.
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());

        // Final flush of whatever is still buffered, now single-threaded.
        for (auto& sh : shards)
            drainShard (*sh);

        finishCopies();
    }

    // Every copy drains its ring and closes its files. The primary decides the
    // take, so STOP waits for it as long as it takes. A copy that stops making
    // progress, or has not drained within the total limit, is left behind
    // instead: its drive must not hold up the show, and nothing here touches
    // that copy's folder again.
    void MultitrackRecorder::finishCopies()
    {
        for (auto& copy : copies) copy->requestFinish();
        waitForCopies (copyDrainLimitMs);
    }

    void MultitrackRecorder::waitForCopies (double totalLimitMs)
    {
        const auto startedMs = juce::Time::getMillisecondCounterHiRes();
        for (;;)
        {
            bool waiting = false;
            const auto now = juce::Time::getMillisecondCounterHiRes();
            for (auto& copy : copies)
            {
                if (copy->finished.load (std::memory_order_acquire)
                    || copy->abandoned.load (std::memory_order_acquire)) continue;
                if (copy->copyIndex != 0
                    && (now - copy->lastProgressMs.load (std::memory_order_relaxed) >= copyStallLimitMs
                        || (totalLimitMs > 0.0 && now - startedMs >= totalLimitMs)))
                {
                    copy->abandon();
                    leftBehind.push_back (copy);
                    continue;
                }
                waiting = true;
            }
            if (! waiting) break;
            juce::Thread::sleep (2);
        }
        for (auto& copy : copies) copy->join();
    }

    MultitrackRecorder::TakeStop MultitrackRecorder::snapshotTakeAtStop()
    {
        TakeStop stop;
        stop.stoppedAt        = juce::Time::getCurrentTime();
        stop.totalSamples     = samplesSinceStart.load (std::memory_order_relaxed);
        stop.totalMissed      = getMissedSamples();
        stop.deviceLostAtStop = captureDeviceLost.load (std::memory_order_relaxed);
        stop.deviceStallMsAtStop = deviceStallMs.load (std::memory_order_relaxed);
        stop.totalSeconds     = sampleRate > 0.0 ? (double) stop.totalSamples / sampleRate : 0.0;
        stop.backupWasRunning = isBackupActive();
        {
            const juce::ScopedLock sl (alarmLock);
            if (alarmsRunning)
            {
                // A fault that crossed its threshold since the last status tick
                // still belongs in the report.
                updateChannelAlarmsLocked();
                alarmMonitor.finish (alarmFrames.load (std::memory_order_relaxed));
                stop.channelAlarms = alarmMonitor.history (alarmOrigin());
                alarmsRunning = false;
                alarmTracks.clear();
            }
        }

        // Snapshot per-writer file-part info BEFORE closeWriters clears
        // the writers vector. The report needs this to enumerate the
        // Track_NN.wav + Track_NN_partXX.wav parts produced by the
        // auto-split logic.
        stop.writers.reserve (writers.size());
        stop.doPunch = punchInActive;
        stop.timelineStart = recordBaseSamples;
        const auto* primaryCopy = copyAt (0);
        const auto* backupCopy  = isCopyOutOfTake (1) ? nullptr : copyAt (1);
        if (isCopyOutOfTake (1)) stop.backupWasRunning = false;
        const auto punchPath = [] (const CaptureCopy& copy, const CaptureCopy::Track& t)
        {
            TakeStop::PunchFiles files;
            if (t.baseFile == juce::File()) return files;
            files.main = t.baseFile.withFileExtension (t.ext);
            for (const auto& name : t.partFiles) files.fresh.push_back (t.baseFile.getSiblingFile (name));
            files.maxBytesPerFile = maxBytesForContainer (t.container);
            files.complete = ! t.failed && ! copy.overran.load (std::memory_order_relaxed);
            return files;
        };
        for (size_t i = 0; i < writers.size(); ++i)
        {
            TakeStop::WriterReport wr {};
            TakeStop::PunchWriterFiles pf;
            if (primaryCopy != nullptr)
            {
                const auto& t = primaryCopy->tracks[i];
                wr.totalSamplesPrimary = t.totalSamples;
                wr.partFilesPrimary    = t.partFiles;
                if (stop.doPunch) pf.primary = punchPath (*primaryCopy, t);
            }
            if (backupCopy != nullptr)
            {
                const auto& t = backupCopy->tracks[i];
                wr.totalSamplesBackup = t.totalSamples;
                wr.partFilesBackup    = t.partFiles;
                if (stop.doPunch) pf.backup = punchPath (*backupCopy, t);
            }
            // Every track, armed or not: the report lists what each folder holds.
            for (size_t mi = 0; mi < activeMirrors.size(); ++mi)
            {
                const auto* copy = copyAt (2 + (int) mi);
                if (copy == nullptr) continue;
                // A copy left behind is still its thread's: only the atomics are ours.
                if (isCopyOutOfTake (2 + (int) mi))
                {
                    wr.mirrors.push_back ({ activeMirrors[mi].root, 0, {}, true, true });
                    if (stop.doPunch) pf.mirrors.push_back ({});
                    continue;
                }
                const auto& t = copy->tracks[i];
                wr.mirrors.push_back ({ activeMirrors[mi].root, t.totalSamples, t.partFiles,
                                        t.failed || copy->overran.load (std::memory_order_relaxed) });
                if (stop.doPunch) pf.mirrors.push_back (punchPath (*copy, t));
            }
            stop.writers.push_back (std::move (wr));
            stop.punchFiles.push_back (std::move (pf));
        }
        if (primaryCopy != nullptr) stop.stampedParts = primaryCopy->stampedParts;
        return stop;
    }

    void MultitrackRecorder::reattachWriterShards()
    {
        // The finalization lease keeps the channel layout fixed. Reattach the
        // existing clients instead of replacing their vector: status readers
        // may still be loading each client's atomic telemetry during STOP.
        // Every client was detached before drain/close, so resetting its
        // private throughput accumulator here cannot race its writer thread.
        for (size_t i = 0; i < shards.size(); ++i)
        {
            auto& shard = *shards[i];
            shard.throughputBaseBytes = 0;
            shard.throughputWindowMs = 0;
            shard.shardBytesPerSec.store (0, std::memory_order_relaxed);
            shard.shardRingFillPct.store (0, std::memory_order_relaxed);
            writerThreads[i % writerThreads.size()]->addTimeSliceClient (&shard);
        }
    }

    // ── Punch-in splice ────────────────────────────────────────────────
    // Each punched track recorded a clean fresh file; its existing take was
    // stashed to a sidecar in startRecording. Splice base[0,punchIn) +
    // freshTake + base[after] over each copy (temp + atomic swap). Runs after
    // the writers close and before the hash thread, so the report hashes and
    // lengths describe the spliced files. On a splice failure the take reverts
    // to its pre-punch base (the original is never lost).
    //
    // One copy's splice of one track. All of it runs on plain values, because
    // on a backup or mirror it runs on that copy's own thread.
    struct MultitrackRecorder::CopySplice
    {
        juce::File              fresh;      // Track_NN.<ext>
        std::vector<juce::File> freshParts; // every file the punch recorded
        juce::File              tmp;        // first file of the spliced result
        std::vector<juce::File> tmpParts;   // the whole spliced result, awaiting commit
        std::vector<juce::File> baseParts;  // stashed .punchbase sidecars
        bool                    attempted { false };  // this copy was stashed
        bool                    spliced   { false };  // splice to tmp succeeded
        bool                    complete  { true };   // holds every frame of the punch

        // Every stashed base part for this copy, in order: Track_NN.punchbase +
        // Track_NN_partXX.punchbase (continue takes are multi-part). The splice
        // reads them as one seamless base.
        static CopySplice gather (const TakeStop::PunchFiles& files)
        {
            CopySplice cs;
            const auto& take = files.main;
            cs.fresh = take;
            cs.freshParts = files.fresh;
            if (take.getFullPathName().isEmpty()) return cs;
            const auto sidecar0 = punchSidecar (take);
            if (! sidecar0.existsAsFile()) return cs;    // this copy wasn't stashed
            cs.attempted = true;
            cs.baseParts.push_back (sidecar0);
            const auto dir  = take.getParentDirectory();
            const auto stem = take.getFileNameWithoutExtension();   // "Track_07"
            const auto ext  = take.getFileExtension();
            for (int p = 2; ; ++p)
            {
                const auto ps = punchSidecar (dir.getChildFile (stem + "_part"
                                              + juce::String::formatted ("%02d", p) + ext));
                if (ps.existsAsFile()) cs.baseParts.push_back (ps); else break;
            }
            return cs;
        }

        // A spliced file keeps its base's stamp (bextFieldsOf) and a copy that
        // rolled back IS its base, so neither takes this pass's time.
        void removeFromStamps (std::vector<WriterChannel::StampedPart>& parts) const
        {
            std::erase_if (parts, [this] (const auto& part)
                           { return std::find (freshParts.begin(), freshParts.end(), part.file) != freshParts.end(); });
        }

        // Spliced into a temp file, nothing committed. Splicing a capture with a
        // hole would replace audio that is on disk and good with silence the
        // engineer never played, and then delete the original. A FIFO gap sits
        // before the copies split, so it is in every one of them.
        void prepare (juce::AudioFormatManager& fm, const TakeStop::PunchFiles& files,
                      juce::int64 punchPosition, juce::int64 fifoGap, const std::function<void()>& onProgress)
        {
            complete = files.complete && fifoGap == 0;
            if (! complete) return;
            tmp = fresh.getSiblingFile (fresh.getFileNameWithoutExtension() + ".punchtmp" + fresh.getFileExtension());
            spliced = splicePunchTake (fm, baseParts, freshParts, punchPosition, tmp,
                                       files.maxBytesPerFile, tmpParts, onProgress);
        }

        bool install()
        {
            for (const auto& part : freshParts)
                if (part.existsAsFile() && ! part.deleteFile()) return false;
            for (size_t p = 0; p < tmpParts.size(); ++p)
            {
                const auto target = p == 0 ? fresh
                    : partFileFor (fresh.getParentDirectory(), fresh.getFileNameWithoutExtension(),
                                   fresh.getFileExtension(), (int) p + 1);
                if (! tmpParts[p].moveFileTo (target)) return false;
            }
            for (const auto& part : baseParts) part.deleteFile();
            return true;
        }

        // Back to the pre-punch take. The fresh file (or, if the install got as
        // far as removing it, the spliced temp) is archived. The punch's files
        // must all be out of the way before a base part comes back: a part the
        // punch rolled into has the same name as a base part.
        void rollBack (bool archive, juce::Time stoppedAt)
        {
            bool freshKept = false;
            for (const auto& part : freshParts)
            {
                if (! part.existsAsFile()) continue;
                if (archive && archiveUnspliced (part, part.getFileName(), stoppedAt)) freshKept = true;
                else part.deleteFile();
            }
            for (const auto& part : tmpParts)
            {
                if (archive && ! freshKept && part.existsAsFile())
                    if (! archiveUnspliced (part, fresh.getFileName(), stoppedAt)) part.deleteFile();
                part.deleteFile();
            }
            for (auto& part : baseParts)
                part.moveFileTo (takeOf (part));
        }

    private:
        // Track_NN.punchbase.ext -> Track_NN.ext, Track_NN_part02.punchbase.ext
        // -> Track_NN_part02.ext.
        static juce::File takeOf (const juce::File& sidecar)
        {
            const auto realStem = sidecar.getFileNameWithoutExtension()
                                         .upToLastOccurrenceOf (".punchbase", false, false);
            return sidecar.getParentDirectory().getChildFile (realStem + sidecar.getFileExtension());
        }

        // Punch audio that could not be spliced is archived beside the copy it
        // belongs to, never deleted: it is a performance the engineer may still
        // want (recoverInterruptedPunches keeps interrupted punches the same way).
        static bool archiveUnspliced (const juce::File& source, const juce::String& asName, juce::Time stoppedAt)
        {
            if (! source.existsAsFile()) return true;
            const auto audioDir = source.getParentDirectory();
            const auto copyRoot = audioDir.getFileName() == "Audio Files" ? audioDir.getParentDirectory() : audioDir;
            const auto dir = copyRoot.getChildFile ("Session File Backups")
                .getChildFile ("Unspliced Punch " + stoppedAt.formatted ("%Y-%m-%d %H-%M-%S"));
            if (! dir.isDirectory() && ! dir.createDirectory().wasOk()) return false;
            return source.moveFileTo (dir.getNonexistentChildFile (
                juce::File::createFileWithoutCheckingPath (asName).getFileNameWithoutExtension(),
                juce::File::createFileWithoutCheckingPath (asName).getFileExtension(), false));
        }
    };

    static juce::int64 readLength (juce::AudioFormatManager& fm, const juce::File& file)
    {
        const std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        return reader != nullptr ? reader->lengthInSamples : -1;
    }

    // The primary decides whether the punch landed, and nothing it does here
    // waits on a copy. A copy that cannot splice (its writer never opened, its
    // drive filled, its capture has a hole) keeps its pre-punch take and is
    // reported failed; it must not throw away a good primary punch. If the
    // primary cannot splice, every copy goes back to its base so they agree,
    // and the punch audio goes to the archive.
    void MultitrackRecorder::splicePunchedTracks (TakeStop& stop)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        const auto origin = punchCaptureOrigin.load (std::memory_order_acquire);
        stop.punchPosition = origin >= 0 ? origin : punchInPos;
        stop.punchVerdicts.assign (stop.punchFiles.size(), TakeStop::PunchVerdict::notPunched);

        bool punchDiscarded = false;
        for (size_t i = 0; i < stop.punchFiles.size() && i < stop.writers.size(); ++i)
        {
            auto primary = CopySplice::gather (stop.punchFiles[i].primary);
            if (! primary.attempted) continue;    // primary not stashed -> not a punched track
            primary.removeFromStamps (stop.stampedParts);

            // Nothing was captured (a selection punch stopped during its
            // pre-roll): there is no punch, so every copy simply keeps its take.
            if (readLength (fm, primary.fresh) == 0)
            {
                primary.rollBack (false, stop.stoppedAt);
                stop.punchVerdicts[i] = TakeStop::PunchVerdict::nothingCaptured;
                continue;
            }

            primary.prepare (fm, stop.punchFiles[i].primary, stop.punchPosition, stop.totalMissed, {});
            if (primary.spliced && primary.install())
            {
                stop.punchVerdicts[i] = TakeStop::PunchVerdict::committed;
            }
            else
            {
                punchSpliceFailed.store (true, std::memory_order_relaxed);
                punchDiscarded = punchDiscarded || ! primary.complete;
                primary.rollBack (true, stop.stoppedAt);
                stop.punchVerdicts[i] = TakeStop::PunchVerdict::discarded;
            }

            // The resulting length on disk: spliced on commit, base on rollback.
            const auto length = readLength (fm, primary.fresh);
            if (length > 0) stop.writers[i].totalSamplesPrimary = length;
        }

        if (punchDiscarded)
            noteCopyProblem ("Punch discarded: its capture has a gap, so the original take stays. "
                             "The punch audio is in Session File Backups");
    }

    void MultitrackRecorder::noteCopyProblem (const juce::String& problem)
    {
        const juce::ScopedLock sl (structureLock);
        droppedCopies.add (problem);
    }

    // One track's files and length in one copy's Audio Files folder.
    static void collectTrackMedia (juce::AudioFormatManager& fm, const juce::File& folder, int index,
                                   juce::StringArray& names, juce::int64& samples,
                                   const std::function<void()>& onProgress = {})
    {
        names.clear();
        samples = 0;
        const auto stem = juce::String::formatted ("Track_%02d", index + 1);
        for (auto* ext : { ".wav", ".aif", ".aiff", ".flac" })
        {
            const auto base = folder.getChildFile (stem + ext);
            if (! base.existsAsFile()) continue;
            for (const auto& part : findTakeParts (base))
            {
                if (part == juce::File()) continue;
                names.add (part.getFileName());
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (part));
                if (reader != nullptr) samples += reader->lengthInSamples;
                if (onProgress) onProgress();
            }
        }
    }

    // All that a copy's own files still need once its writers have closed:
    // punch, lead-ins, stamps, manifest, in that order. Plain values only, so the
    // copy's thread can run it with the recorder doing something else or gone.
    // The results are read back only if the thread finishes in time.
    struct MultitrackRecorder::CopyFinish
    {
        std::vector<TakeStop::PunchFiles>   punchFiles;   // by track; empty without a punch
        std::vector<TakeStop::PunchVerdict> verdicts;
        juce::int64 punchPosition {}, fifoGap {};
        juce::Time  stoppedAt;

        std::vector<PendingLeadIn> leadIns;
        juce::int64 timelineStart {};

        std::vector<WriterChannel::StampedPart> stampedParts;
        bool        stampWanted {};
        juce::int64 passStart {};
        double      sampleRate {};

        juce::File audioFolder;
        int        trackCount {};
        bool       manifestWanted {};

        std::function<void (CaptureCopy&)> beforeStartForTests;

        std::set<size_t>     punchFailedTracks;
        bool                 punchHadGap {}, leadInFailed {};
        juce::StringArray    stampFailures;
        std::vector<juce::StringArray> partFiles;
        std::vector<juce::int64>       samples;

        void run (CaptureCopy& copy)
        {
            if (beforeStartForTests != nullptr) beforeStartForTests (copy);
            const std::function<void()> tick = [&copy] { copy.noteProgress(); };
            juce::AudioFormatManager fm; fm.registerBasicFormats();

            for (size_t i = 0; i < punchFiles.size() && i < verdicts.size(); ++i)
            {
                using Verdict = TakeStop::PunchVerdict;
                auto splice = CopySplice::gather (punchFiles[i]);
                tick();
                if (! splice.attempted || verdicts[i] == Verdict::notPunched) continue;
                splice.removeFromStamps (stampedParts);
                if (verdicts[i] == Verdict::committed)
                    splice.prepare (fm, punchFiles[i], punchPosition, fifoGap, tick);
                if (verdicts[i] == Verdict::nothingCaptured)
                    splice.rollBack (false, stoppedAt);
                else if (verdicts[i] == Verdict::discarded)
                    splice.rollBack (true, stoppedAt);
                else if (! splice.spliced || ! splice.install())
                {
                    splice.rollBack (true, stoppedAt);
                    punchFailedTracks.insert (i);
                    punchHadGap = punchHadGap || ! splice.complete;
                }
                tick();
            }

            for (const auto& lead : leadIns)
            {
                if (! finishLeadIn (lead, timelineStart)) leadInFailed = true;
                tick();
            }

            if (stampWanted)
                for (const auto& part : stampedParts)
                {
                    const auto result = bwf::stampTimeReference (
                        part.file, part.stampedAtOpen,
                        (juce::uint64) bwf::wrapToDay (passStart + part.framesBefore, sampleRate));
                    if (result != bwf::StampResult::ok)
                        stampFailures.add (part.file.getFullPathName() + ": " + bwf::toString (result));
                    tick();
                }

            if (manifestWanted)
            {
                partFiles.resize ((size_t) trackCount);
                samples.assign ((size_t) trackCount, 0);
                for (int i = 0; i < trackCount; ++i)
                    collectTrackMedia (fm, audioFolder, i, partFiles[(size_t) i], samples[(size_t) i], tick);
            }
        }
    };

    // No total limit here, unlike the drain: a healthy splice of a long take
    // can run for minutes. What bounds this is the no-progress limit, and every
    // step of the work ticks, so only a drive that has stopped answering trips
    // it. The primary's files were finished before any of this began.
    void MultitrackRecorder::finishCopyFiles (TakeStop& stop)
    {
        struct Running { std::shared_ptr<CaptureCopy> copy; std::shared_ptr<CopyFinish> job; };
        std::vector<Running> running;

        const auto liveFrom    = firstLiveTicks.load (std::memory_order_acquire);
        const auto passStart   = passStartSamples();
        for (const auto& copy : copies)
        {
            if (copy->copyIndex == 0 || isCopyOutOfTake (copy->copyIndex)) continue;
            const auto mi = (std::size_t) juce::jmax (0, copy->copyIndex - 2);

            auto job = std::make_shared<CopyFinish>();
            if (stop.doPunch)
            {
                for (const auto& track : stop.punchFiles)
                    job->punchFiles.push_back (copy->copyIndex == 1 ? track.backup
                                               : mi < track.mirrors.size() ? track.mirrors[mi]
                                                                           : TakeStop::PunchFiles {});
                job->verdicts = stop.punchVerdicts;
            }
            job->punchPosition = stop.punchPosition;
            job->fifoGap       = stop.totalMissed;
            job->stoppedAt     = stop.stoppedAt;
            job->leadIns        = copy->leadIns;
            job->timelineStart  = stop.timelineStart;
            job->stampedParts   = copy->stampedParts;
            job->stampWanted    = liveFrom != 0;
            job->passStart      = passStart;
            job->sampleRate     = sampleRate;
            job->audioFolder    = copy->root.getChildFile (activeSessionDir.getFileName())
                                            .getChildFile ("Audio Files");
            job->trackCount     = (int) stop.writers.size();
            job->manifestWanted = copy->copyIndex == 1 ? stop.backupWasRunning : true;
            job->beforeStartForTests = beforeCopyFinishForTests;

            copy->startFinishing (copy, [job] (CaptureCopy& self) { job->run (self); });
            running.push_back ({ copy, std::move (job) });
        }
        pendingLeadIns.clear();
        waitForCopies (0.0);

        for (const auto& [copy, job] : running)
        {
            const auto mi = (std::size_t) juce::jmax (0, copy->copyIndex - 2);
            const bool isBackup = copy->copyIndex == 1;
            const bool gone = copy->abandoned.load (std::memory_order_acquire);
            if (gone && isBackup) stop.backupWasRunning = false;

            for (size_t i = 0; i < stop.writers.size(); ++i)
            {
                auto& writer = stop.writers[i];
                if (isBackup)
                {
                    if (gone)
                    {
                        writer.partFilesBackup.clear();
                        writer.totalSamplesBackup = 0;
                    }
                    else if (job->manifestWanted)
                    {
                        writer.partFilesBackup    = job->partFiles[i];
                        writer.totalSamplesBackup = job->samples[i];
                    }
                }
                else if (mi < writer.mirrors.size())
                {
                    auto& mirror = writer.mirrors[mi];
                    if (gone)
                    {
                        mirror.partFiles.clear();
                        mirror.totalSamples = 0;
                        mirror.abandoned = mirror.failed = true;
                    }
                    else
                    {
                        mirror.partFiles    = job->partFiles[i];
                        mirror.totalSamples = job->samples[i];
                        mirror.failed = mirror.failed || job->punchFailedTracks.count (i) > 0;
                    }
                }
            }
            if (gone) continue;

            stop.stampFailures.addArray (job->stampFailures);
            const bool punchFailed = ! job->punchFailedTracks.empty();
            if (punchFailed || job->leadInFailed)
            {
                (isBackup ? backupFailed : mirrorFailed).store (true, std::memory_order_relaxed);
                if (job->leadInFailed)
                    noteCopyProblem ((isBackup ? juce::String ("Backup") : copy->label)
                                     + " could not write a silent lead-in; a track on it sits early");
            }
            if (job->punchHadGap)
                noteCopyProblem ((isBackup ? juce::String ("Backup") : copy->label)
                                 + " kept its pre-punch take: its punch capture has a gap");
        }

        stop.copyProblems = getCopyProblems();
    }

    // The report describes the whole on-disk session, including parts
    // from earlier CONTINUE passes and tracks that were not armed today.
    // Listing only this pass made a valid continued take look like it had
    // unreported audio, and left older parts outside the hash manifest. The
    // primary's half: each copy lists its own in finishCopyFiles.
    void MultitrackRecorder::manifestSessionMedia (TakeStop& stop)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        const auto primaryFolder = activeSessionDir.getChildFile ("Audio Files");
        for (size_t i = 0; i < stop.writers.size(); ++i)
            collectTrackMedia (fm, primaryFolder, (int) i, stop.writers[i].partFilesPrimary,
                               stop.writers[i].totalSamplesPrimary);
    }

    // Post-show JSON report -- one file per session that captures every
    // datum a mix engineer / producer needs after the gig: total time,
    // per-track clip count, missed samples, backup status. Drop-in
    // alongside the WAVs in the session directory.
    // Snapshot the per-track metadata the report needs, so the heavy
    // SHA-256 hashing + JSON write can run on a BACKGROUND thread
    // without freezing the UI on stop (a 48 ch x several-minute take
    // is GBs to hash). Captured by value only -- no `this`, no engine
    // state -- so it's race- and lifetime-safe even if the recorder
    // is torn down before hashing finishes. The files were already
    // flushed + closed by closeWriters() above.
    MultitrackRecorder::SessionReport MultitrackRecorder::buildSessionReport (TakeStop& stop)
    {
        SessionReport report;
        report.stoppedAt        = stop.stoppedAt;
        report.backupWasRunning = stop.backupWasRunning;
        report.writerSnaps      = std::move (stop.writers);

        auto& trackMetas = report.trackMetas;
        trackMetas.reserve (tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            const auto& ts = *tracks[i];
            trackMetas.push_back ({ (int) i, ts.getNameThreadSafe(),
                                    ts.clipCount.load (std::memory_order_relaxed),
                                    (juce::int64) ts.lastClipSample.load (std::memory_order_relaxed),
                                    ts.inputRouting .load (std::memory_order_relaxed),
                                    ts.outputRouting.load (std::memory_order_relaxed),
                                    ts.isStereo.load (std::memory_order_relaxed) });
        }


        report.sessionDir = activeSessionDir;
        const auto& sessionDir = report.sessionDir;
        report.bDir    = backupDir;
        report.sr      = sampleRate;
        report.fmt     = (int) captureFormat;
        report.preRoll = preRollSeconds;
        const bool       primFailed = primaryFailed.load (std::memory_order_relaxed);
        const bool       punchFailed = punchSpliceFailed.load (std::memory_order_relaxed);
        const int        mirrorsSkipped = getMirrorsSkippedAtStart();

        // A CONTINUE pass replaces the session report. Keep the earlier
        // capture warnings and elapsed counters: the file manifest below covers
        // every pass, so a clean last pass must not certify an earlier failure.
        const auto oldReportFile = sessionDir.getChildFile ("session.report.json");
        const auto oldReport = oldReportFile.existsAsFile()
                                 ? juce::JSON::parse (oldReportFile) : juce::var();
        const auto previousInt = [&oldReport] (const char* name) -> juce::int64
        { return oldReport.isObject() ? (juce::int64) oldReport.getProperty (name, 0) : 0; };
        const auto previousBool = [&oldReport] (const char* name) -> bool
        { return oldReport.isObject() && (bool) oldReport.getProperty (name, false); };
        bool previousMirrorFailure = previousBool ("mirrorFailed");
        if (auto* oldTracks = oldReport.getProperty ("tracks", {}).getArray())
            for (const auto& track : *oldTracks)
                if (auto* mirrors = track.getProperty ("mirrors", {}).getArray())
                    for (const auto& mirror : *mirrors)
                        previousMirrorFailure = previousMirrorFailure
                            || (bool) mirror.getProperty ("failed", false);
        report.twin = oldReport.getProperty ("twin", {});
        report.reportedSamples = previousInt ("totalSamples") + stop.totalSamples;
        report.reportedSeconds = (oldReport.isObject()
                                         ? (double) oldReport.getProperty ("totalSeconds", 0.0) : 0.0)
                                     + stop.totalSeconds;
        report.reportedMissed = previousInt ("missedSamples") + stop.totalMissed;
        report.passSamples = stop.totalSamples;
        report.passSeconds = stop.totalSeconds;
        report.passMissed  = stop.totalMissed;
        report.passPreRoll = preRollHistorySamples;
        report.passPreRollCut = preRollCutFrames.load (std::memory_order_relaxed);
        report.reportedMirrorsSkipped = (int) previousInt ("mirrorsSkipped") + mirrorsSkipped;
        report.reportedDeviceLoss = previousBool ("captureDeviceLost") || stop.deviceLostAtStop;
        report.reportedDeviceStallMs = previousInt ("deviceStallMs") + stop.deviceStallMsAtStop;
        report.reportedPrimaryFailure = previousBool ("primaryFailed") || primFailed;
        // Read now: a copy's own post-take work can fail after the take stopped.
        report.reportedBackupFailure = previousBool ("backupFailed")
                                           || backupFailed.load (std::memory_order_relaxed);
        report.reportedPunchFailure = previousBool ("punchSpliceFailed") || punchFailed;
        report.reportedMirrorFailure = previousMirrorFailure
                                           || mirrorFailed.load (std::memory_order_relaxed);

        // timeReference is the newest stamp written. A punch writes none: its
        // spliced files keep the take's stamp, so the report keeps it too.
        const bool keepTakeStamp = stop.doPunch && oldReport.hasProperty ("timeReference");
        report.timeReference = keepTakeStamp ? (juce::int64) oldReport.getProperty ("timeReference", 0)
                                             : stop.timeReference;
        report.timeReferenceSource = keepTakeStamp
            ? oldReport.getProperty ("timeReferenceSource", "").toString() : timeAnchor.source;
        if (auto* oldFailures = oldReport.getProperty ("timeReferenceStampFailures", {}).getArray())
            for (const auto& failure : *oldFailures) report.stampFailures.add (failure.toString());
        report.stampFailures.addArray (stop.stampFailures);
        if (auto* oldProblems = oldReport.getProperty ("copyProblems", {}).getArray())
            for (const auto& problem : *oldProblems) report.copyProblems.add (problem.toString());
        report.copyProblems.addArray (stop.copyProblems);
        report.clock = clockProbe.latest();
        if (auto* oldAlarms = oldReport.getProperty ("channelAlarms", {}).getArray())
            report.channelAlarms.addArray (*oldAlarms);
        for (const auto& alarm : stop.channelAlarms) report.channelAlarms.add (alarm.toJson());
        return report;
    }

    // Build the report JSON. `withHashes == false` skips the (expensive,
    // GB-scale) SHA-256 pass and marks the report "sha256Pending":true,
    // so we can write a complete metadata report INSTANTLY on stop and
    // fill the hashes in afterwards. SessionReport holds only values, so
    // a copy of it travels into the background thread.
    juce::String MultitrackRecorder::SessionReport::toJson (bool withHashes, bool hashingFailed,
                                                           const std::map<juce::String, juce::String>& shaByPath,
                                                           const juce::var& twinBlock, bool copyHashesPending) const
    {
        juce::DynamicObject::Ptr report (new juce::DynamicObject());
        report->setProperty ("stoppedAt",      stoppedAt.toISO8601 (true));
        report->setProperty ("sampleRate",     sr);
        report->setProperty ("numTracks",      (int) trackMetas.size());
        report->setProperty ("totalSamples",   (juce::int64) reportedSamples);
        report->setProperty ("totalSeconds",   reportedSeconds);
        report->setProperty ("missedSamples",  (juce::int64) reportedMissed);
        // The totals above add up every pass of the session. These are this
        // pass alone, which is what a comparison with another Mac has to read.
        report->setProperty ("passSamples",    (juce::int64) passSamples);
        report->setProperty ("passSeconds",    passSeconds);
        report->setProperty ("passMissedSamples", (juce::int64) passMissed);
        report->setProperty ("passPreRollSamples", passPreRoll);
        report->setProperty ("passPreRollCutSamples", passPreRollCut);
        report->setProperty ("captureDeviceLost", reportedDeviceLoss);
        report->setProperty ("deviceStallMs", reportedDeviceStallMs);
        report->setProperty ("backupActive",   backupWasRunning);
        if (backupWasRunning) report->setProperty ("backupRoot", bDir.getFullPathName());
        report->setProperty ("backupFailed",   reportedBackupFailure);
        report->setProperty ("primaryFailed",  reportedPrimaryFailure);
        report->setProperty ("punchSpliceFailed", reportedPunchFailure);
        report->setProperty ("mirrorFailed", reportedMirrorFailure);
        report->setProperty ("mirrorsSkipped", reportedMirrorsSkipped);
        report->setProperty ("captureFormat",  fmt);
        report->setProperty ("preRollSeconds", preRoll);
        report->setProperty ("timeReference", timeReference);
        report->setProperty ("timeReferenceSource", timeReferenceSource);
        report->setProperty ("timeReferenceStampFailures", juce::var (stampFailures));
        report->setProperty ("copyProblems", juce::var (copyProblems));
        report->setProperty ("clockSource", clock.name);
        report->setProperty ("clockSourceKind", clocksource::kindName (clock.kind));
        report->setProperty ("channelAlarms", juce::var (channelAlarms));
        report->setProperty ("sha256Pending",  ! withHashes || copyHashesPending);
        report->setProperty ("sha256Failed", hashingFailed);

        // Hashes are computed off-thread BEFORE this builder runs (see the
        // background thread below) and looked up here by full path, so the
        // JSON assembly never blocks on a multi-GB read.
        const auto sha256File = [withHashes, &shaByPath] (const juce::File& f) -> juce::String
        {
            if (! withHashes) return {};
            const auto it = shaByPath.find (f.getFullPathName());
            return it != shaByPath.end() ? it->second : juce::String();
        };

        juce::Array<juce::var> trackArray;
        for (std::size_t i = 0; i < trackMetas.size(); ++i)
        {
            const auto& tm = trackMetas[i];
            juce::DynamicObject::Ptr t (new juce::DynamicObject());
            t->setProperty ("index",          tm.index);
            t->setProperty ("name",           tm.name);
            t->setProperty ("clipCount",      tm.clipCount);
            t->setProperty ("lastClipSample", tm.lastClipSample);
            t->setProperty ("inputRouting",   tm.inputRouting);
            t->setProperty ("outputRouting",  tm.outputRouting);
            t->setProperty ("isStereo",       tm.isStereo);

            if (i < writerSnaps.size())
            {
                const auto& ws = writerSnaps[i];
                t->setProperty ("totalSamplesPrimary", (juce::int64) ws.totalSamplesPrimary);

                const auto primaryAudioDir = sessionDir.getChildFile ("Audio Files");
                juce::Array<juce::var> files, shas;
                for (const auto& fn : ws.partFilesPrimary)
                {
                    files.add (juce::var (fn));
                    shas .add (juce::var (sha256File (primaryAudioDir.getChildFile (fn))));
                }
                t->setProperty ("files",  juce::var (files));
                t->setProperty ("sha256", juce::var (shas));

                if (backupWasRunning)
                {
                    const auto backupAudioDir =
                        bDir.getChildFile (sessionDir.getFileName())
                            .getChildFile ("Audio Files");
                    t->setProperty ("totalSamplesBackup", (juce::int64) ws.totalSamplesBackup);
                    juce::Array<juce::var> backupFiles, backupShas;
                    for (const auto& fn : ws.partFilesBackup)
                    {
                        backupFiles.add (juce::var (fn));
                        backupShas .add (juce::var (sha256File (backupAudioDir.getChildFile (fn))));
                    }
                    t->setProperty ("backupFiles",  juce::var (backupFiles));
                    t->setProperty ("backupSha256", juce::var (backupShas));
                }

                if (! ws.mirrors.empty())
                {
                    juce::Array<juce::var> mirrorArr;
                    for (const auto& mr : ws.mirrors)
                    {
                        juce::DynamicObject::Ptr mo (new juce::DynamicObject());
                        mo->setProperty ("root",         mr.root.getFullPathName());
                        mo->setProperty ("totalSamples", (juce::int64) mr.totalSamples);
                        mo->setProperty ("failed",       mr.failed);
                        juce::Array<juce::var> mFiles, mShas;
                        const auto mDir = mr.root.getChildFile (sessionDir.getFileName())
                                                  .getChildFile ("Audio Files");
                        for (const auto& fn : mr.partFiles)
                        {
                            mFiles.add (juce::var (fn));
                            mShas .add (juce::var (sha256File (mDir.getChildFile (fn))));
                        }
                        mo->setProperty ("files",  juce::var (mFiles));
                        mo->setProperty ("sha256", juce::var (mShas));
                        mirrorArr.add (juce::var (mo.get()));
                    }
                    t->setProperty ("mirrors", juce::var (mirrorArr));
                }
            }
            trackArray.add (juce::var (t.get()));
        }
        report->setProperty ("tracks", juce::var (trackArray));
        if (twinBlock.isObject()) report->setProperty ("twin", twinBlock);
        return juce::JSON::toString (juce::var (report.get()), true);
    }

    // Publish session.report.json: the metadata report synchronously (so it
    // exists the instant recording stops), then the SHA-256 manifest from a
    // background thread that yields to newer reports for the same session.
    void MultitrackRecorder::publishSessionReport (SessionReport report)
    {
        if (! report.sessionDir.isDirectory()) return;

        // Claim this stop's report generation for THIS SESSION. Anything a
        // previous stop's hash thread is still working on is now stale and
        // must not overwrite what we write below.
        const auto reportState = sessionReportState (report.sessionDir);
        std::unique_lock<std::mutex> publication (reportState->publication);
        const auto myGen = reportState->generation.fetch_add (1, std::memory_order_acq_rel) + 1;
        std::optional<ReportFileLock> otherProcess (std::in_place);

        // 1) Write the metadata report SYNCHRONOUSLY, right now -- this
        //    guarantees session.report.json exists the instant recording
        //    stops, even for a huge take whose hashing runs for minutes
        //    (or if the app is killed before hashing finishes).
        reportWriteFailed.store (
            ! atomicfile::writeText (report.sessionDir.getChildFile ("session.report.json"),
                                     report.toJson (false, false, {}, report.twin)),
            std::memory_order_relaxed);
        otherProcess.reset();
        publication.unlock();

        // Scan sequentially at background priority, with bounded read bursts.
        // Eight parallel hashes plus waveform scans saturated the external
        // ExFAT volume and blocked even tiny UI-thread metadata saves.
        // The immediate pending report stays durable while this scan yields.
        const auto asyncState = reportAsyncState;
        const auto asyncGen = asyncState->generation.load (std::memory_order_acquire);
        const auto testHooks = reportTestHooks;
        reportState->runningScans.fetch_add (1, std::memory_order_acq_rel);
        juce::Thread::launch (
            [report = std::move (report), myGen, reportState, asyncState, asyncGen, testHooks]
            {
                const juce::ScopeGuard scanDone { [reportState]
                {
                    reportState->runningScans.fetch_sub (1, std::memory_order_acq_rel);
                } };
                const auto& sessionDir  = report.sessionDir;
                const auto& bDir        = report.bDir;
                const auto& writerSnaps = report.writerSnaps;
                const juce::ScopeGuard finishedHook { [testHooks]
                {
                    if (testHooks && testHooks->finished) testHooks->finished();
                } };
                // Says so to a window that opens this session from another
                // process meanwhile, so it reads nothing twice. This pass runs
                // whether or not it got it: it is the one STOP owes the take.
                capture::identity::ScanClaim scanning;
                scanning.hold (sessionDir);
               #if JUCE_MAC
                pthread_set_qos_class_self_np (QOS_CLASS_BACKGROUND, 0);
               #endif

                // Collect every file the manifest will reference (primary +
                // backup + mirrors), mirroring buildReportJson's path logic.
                std::vector<juce::File> primaryFiles, copyFiles;
                const auto primDir = sessionDir.getChildFile ("Audio Files");
                const auto bkDir   = bDir.getChildFile (sessionDir.getFileName())
                                         .getChildFile ("Audio Files");
                for (const auto& ws : writerSnaps)
                {
                    for (const auto& fn : ws.partFilesPrimary) primaryFiles.push_back (primDir.getChildFile (fn));
                    if (report.backupWasRunning)
                        for (const auto& fn : ws.partFilesBackup) copyFiles.push_back (bkDir.getChildFile (fn));
                    for (const auto& mr : ws.mirrors)
                    {
                        const auto mDir = mr.root.getChildFile (sessionDir.getFileName())
                                                 .getChildFile ("Audio Files");
                        for (const auto& fn : mr.partFiles) copyFiles.push_back (mDir.getChildFile (fn));
                    }
                }

                const auto current = [=]
                {
                    return reportState->generation.load (std::memory_order_acquire) == myGen;
                };
                hashing::ReadPolicy readPolicy { current, 50 };
                std::map<juce::String, juce::String> shaByPath;
                bool hashingFailed = false;
                const auto hashAll = [&] (const std::vector<juce::File>& files)
                {
                    for (const auto& file : files)
                    {
                        if (! current()) return false;
                        const auto sha = hashing::fileSha256 (file, nullptr, readPolicy);
                        if (! current()) return false; // cancellation is not a hash failure
                        shaByPath[file.getFullPathName()] = sha;
                        hashingFailed = hashingFailed || sha.isEmpty();
                    }
                    return current();
                };
                const auto publishHashes = [&] (bool copiesStillPending)
                {
                    // The last validity check and rename share ownership with
                    // new pending reports, media mutation, and session removal.
                    const std::lock_guard<std::mutex> publish (reportState->publication);
                    if (! current()) return false;
                    const ReportFileLock otherProcess;
                    // The twin link adds its block after STOP, and in daemon mode
                    // from the other process, so the file is the only place to
                    // find it. Hashing can outlast that by many minutes.
                    const auto reportFile = sessionDir.getChildFile ("session.report.json");
                    const bool wrote = atomicfile::writeText (reportFile,
                        report.toJson (true, hashingFailed, shaByPath,
                                       juce::JSON::parse (reportFile).getProperty ("twin", {}),
                                       copiesStillPending));
                    if (! copiesStillPending && testHooks && testHooks->afterPublish) testHooks->afterPublish();
                    if ((! wrote || hashingFailed)
                        && asyncState->generation.load (std::memory_order_acquire) == asyncGen)
                        asyncState->failed.store (true, std::memory_order_release);
                    return true;
                };

                // The primary's hashes are published on their own before any
                // copy is read: a read on a dead drive can block for good. A
                // process-wide gate also bounds overlapping stops in different
                // sessions. It is acquired only on this background worker, and
                // never held across a copy, or one dead drive would stop every
                // later take's primary being hashed.
                {
                    static std::mutex reportScanMutex;
                    const std::lock_guard<std::mutex> scanGuard (reportScanMutex);
                    if (testHooks && testHooks->beforeScan) testHooks->beforeScan();
                    if (! hashAll (primaryFiles)) return;
                }
                if (! copyFiles.empty())
                {
                    if (! publishHashes (true)) return;
                    if (testHooks && testHooks->beforeCopyScan) testHooks->beforeCopyScan();
                    if (! hashAll (copyFiles)) return;
                }
                if (testHooks && testHooks->beforePublish) testHooks->beforePublish();
                publishHashes (false);
            });
    }

    std::shared_ptr<std::atomic<juce::int64>>
        MultitrackRecorder::claimReportGeneration (const juce::File& sessionDir,
                                                   juce::int64& outGeneration)
    {
        const auto state = sessionReportState (sessionDir);
        const std::lock_guard<std::mutex> lock (state->publication);
        outGeneration = state->generation.fetch_add (1, std::memory_order_acq_rel) + 1;
        return { state, &state->generation };
    }

    // A quit ends the hash thread, in this process or the daemon's, and the
    // report would stay pending for good. Opening the session picks it up from
    // the file names the report holds. A take into this session cancels it, and
    // a report that changed meanwhile is left as it is.
    void MultitrackRecorder::resumeReportHashes (const juce::File& sessionDir,
                                                 const std::function<void()>& finishedForTests,
                                                 const std::function<void()>& beforePublishForTests)
    {
        const auto state = sessionReportState (sessionDir);
        const auto file = sessionDir.getChildFile ("session.report.json");
        if (state->runningScans.load (std::memory_order_acquire) > 0 || ! file.existsAsFile()) return;
        // A helper hashes in its own process after STOP, where the count above
        // cannot see it, and two passes would read the whole take twice.
        if (capture::identity::ScanClaim::isHeld (sessionDir)) return;
        const auto report = juce::JSON::parse (file);
        if (! (bool) report.getProperty ("sha256Pending", false)) return;
        const auto stoppedAt = report.getProperty ("stoppedAt", "").toString();
        juce::int64 generation = 0;
        const auto token = claimReportGeneration (sessionDir, generation);

        state->runningScans.fetch_add (1, std::memory_order_acq_rel);
        juce::Thread::launch ([state, token, generation, report, stoppedAt, sessionDir,
                               finishedForTests, beforePublishForTests]
        {
            const juce::ScopeGuard done { [&]
            {
                state->runningScans.fetch_sub (1, std::memory_order_acq_rel);
                if (finishedForTests) finishedForTests();
            } };
            capture::identity::ScanClaim scanning;
            if (! scanning.hold (sessionDir)) return;   // another process got there first
           #if JUCE_MAC
            pthread_set_qos_class_self_np (QOS_CLASS_BACKGROUND, 0);
           #endif
            const auto current = [&] { return token->load (std::memory_order_acquire) == generation; };
            hashing::ReadPolicy policy { current, 50 };
            std::map<juce::String, juce::String> hashes;
            // The primary and the copies fail apart: a backup that is gone
            // says nothing about the take on the primary.
            bool primaryFailed = false, copyFailed = false;
            const auto hashFolder = [&] (const juce::File& folder, const juce::var& names, bool& failed)
            {
                if (auto* list = names.getArray())
                    for (const auto& name : *list)
                    {
                        if (! current()) return;
                        const auto target = folder.getChildFile (name.toString());
                        hashes[target.getFullPathName()] = hashing::fileSha256 (target, nullptr, policy);
                        failed = failed || hashes[target.getFullPathName()].isEmpty();
                    }
            };
            const auto copyAudio = [&sessionDir] (const juce::File& root)
            { return root.getChildFile (sessionDir.getFileName()).getChildFile ("Audio Files"); };
            const auto backupRoot = report.getProperty ("backupRoot", "").toString();
            auto* tracks = report.getProperty ("tracks", {}).getArray();
            if (tracks == nullptr) return;

            const auto fill = [&] (juce::DynamicObject& owner, const juce::File& folder, const char* names, const char* shas)
            {
                juce::Array<juce::var> filled;
                if (auto* list = owner.getProperty (names).getArray())
                    for (const auto& name : *list)
                        filled.add (hashes[folder.getChildFile (name.toString()).getFullPathName()]);
                owner.setProperty (shas, filled);
            };
            const auto publish = [&] (bool copiesDone)
            {
                editSessionReport (sessionDir, [&] (juce::DynamicObject& live)
                {
                    if (! current() || live.getProperty ("stoppedAt").toString() != stoppedAt) return;
                    if (auto* liveTracks = live.getProperty ("tracks").getArray())
                        for (auto& track : *liveTracks)
                        {
                            auto* t = track.getDynamicObject();
                            if (t == nullptr) continue;
                            fill (*t, sessionDir.getChildFile ("Audio Files"), "files", "sha256");
                            if (! copiesDone) continue;
                            if (backupRoot.isNotEmpty()) fill (*t, copyAudio (juce::File (backupRoot)), "backupFiles", "backupSha256");
                            if (auto* mirrors = t->getProperty ("mirrors").getArray())
                                for (auto& mirror : *mirrors)
                                    if (auto* m = mirror.getDynamicObject())
                                        fill (*m, copyAudio (juce::File (m->getProperty ("root").toString())), "files", "sha256");
                        }
                    live.setProperty ("sha256Pending", ! copiesDone);
                    live.setProperty ("sha256Failed", primaryFailed);
                    if (copiesDone) live.setProperty ("copySha256Failed", copyFailed);
                });
            };

            // The primary first, published on its own, as at STOP: a read on a
            // copy whose drive is gone or hung can block for good, and the
            // primary's hashes must not wait on it.
            bool anyCopy = false;
            for (const auto& track : *tracks)
            {
                hashFolder (sessionDir.getChildFile ("Audio Files"), track.getProperty ("files", {}), primaryFailed);
                anyCopy = anyCopy || (backupRoot.isNotEmpty() && track.getProperty ("backupFiles", {}).size() > 0)
                                  || track.getProperty ("mirrors", {}).size() > 0;
            }
            if (! current()) return;
            if (anyCopy) publish (false);
            for (const auto& track : *tracks)
            {
                if (backupRoot.isNotEmpty())
                    hashFolder (copyAudio (juce::File (backupRoot)), track.getProperty ("backupFiles", {}), copyFailed);
                if (auto* mirrors = track.getProperty ("mirrors", {}).getArray())
                    for (const auto& mirror : *mirrors)
                        hashFolder (copyAudio (juce::File (mirror.getProperty ("root", "").toString())),
                                    mirror.getProperty ("files", {}), copyFailed);
            }
            if (! current()) return;
            if (beforePublishForTests) beforePublishForTests();
            publish (true);
        });
    }

    bool MultitrackRecorder::editSessionReport (const juce::File& sessionDir,
                                                const std::function<void (juce::DynamicObject&)>& edit)
    {
        const auto state = sessionReportState (sessionDir);
        const std::lock_guard<std::mutex> lock (state->publication);
        const ReportFileLock otherProcess;
        const auto file = sessionDir.getChildFile ("session.report.json");
        auto report = file.existsAsFile() ? juce::JSON::parse (file) : juce::var();
        auto* object = report.getDynamicObject();
        if (object == nullptr) return false;
        edit (*object);
        return atomicfile::writeText (file, juce::JSON::toString (report, true));
    }

    bool MultitrackRecorder::waitForReportHashes (const juce::File& sessionDir, const std::atomic<bool>& cancel,
                                                  bool watchReportFile)
    {
        const auto state = sessionReportState (sessionDir);
        const auto giveUp = juce::Time::getMillisecondCounterHiRes() + 30.0 * 60.0 * 1000.0;
        const auto reportPending = [&]
        {
            return watchReportFile && juce::Time::getMillisecondCounterHiRes() < giveUp
                && (bool) juce::JSON::parse (sessionDir.getChildFile ("session.report.json"))
                              .getProperty ("sha256Pending", false);
        };
        while (state->runningScans.load (std::memory_order_acquire) > 0 || reportPending())
        {
            if (cancel.load (std::memory_order_relaxed)) return false;
            juce::Thread::sleep (100);
        }
        return true;
    }

    bool MultitrackRecorder::deleteSessionAfterCancellingReports (const juce::File& directory)
    {
        return removeSessionAfterCancellingReports (directory, [&directory] { return directory.deleteRecursively(); });
    }

    bool MultitrackRecorder::removeSessionAfterCancellingReports (const juce::File& directory,
                                                                  const std::function<bool()>& remove)
    {
        const auto state = sessionReportState (directory);
        const std::lock_guard<std::mutex> lock (state->publication);
        state->generation.fetch_add (1, std::memory_order_acq_rel);
        return remove();
    }

    void MultitrackRecorder::closeWriters()
    {
        writersReady.store (false, std::memory_order_release);
        // After a take the copies' threads have closed their own files, and a
        // copy left behind still owns its. This is for a start that never ran.
        for (auto& copy : copies)
            if (! copy->abandoned.load (std::memory_order_acquire)) copy->closeFilesNow();
        writers.clear();
    }

    void MultitrackRecorder::waitForCopiesOpenForTests()
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + 5000.0;
        const auto stillOpening = [this]
        {
            return std::any_of (copies.begin(), copies.end(), [] (const auto& copy)
                                { return copy->opening.load (std::memory_order_acquire); });
        };
        while (stillOpening() && juce::Time::getMillisecondCounterHiRes() < end)
            juce::Thread::sleep (1);
    }

    void MultitrackRecorder::drainPendingForTests()
    {
        drainOnce();
        for (auto& copy : copies) copy->waitUntilWritten (2000);
    }

    void MultitrackRecorder::flushOpenWritersForTests()
    {
        for (auto& copy : copies) copy->flushAndWait (5000);
    }

    juce::StringArray MultitrackRecorder::getCopyProblems() const
    {
        const juce::ScopedLock sl (structureLock);
        return describeCopyProblems();
    }

    juce::StringArray MultitrackRecorder::describeCopyProblems() const
    {
        // Only a copy that is writing and has lost nothing can be said to hold
        // what this one lost. Saying it of a take with no such copy would turn a
        // hole in the only recording into a reassurance.
        const auto healthyCopyBesides = [this] (const CaptureCopy& lossy)
        {
            return std::any_of (copies.begin(), copies.end(), [&lossy] (const auto& copy)
            {
                return copy.get() != &lossy
                    && copy->liveChannels.load (std::memory_order_relaxed) > 0
                    && ! copy->opening.load (std::memory_order_acquire)
                    && ! copy->overran.load (std::memory_order_relaxed)
                    && ! copy->abandoned.load (std::memory_order_acquire);
            });
        };
        auto problems = droppedCopies;
        {
            const std::lock_guard<std::mutex> guard (latches->droppedLock);
            problems.addArray (latches->dropped);
        }
        for (const auto& copy : copies)
        {
            if (copy->abandoned.load (std::memory_order_acquire))
                problems.add (copy->label + " stopped responding and was left behind at STOP");
            else if (copy->overran.load (std::memory_order_relaxed) && sampleRate > 0.0)
                problems.add (copy->label + " lost "
                              + juce::String ((double) copy->droppedFrames.load (std::memory_order_relaxed) / sampleRate, 1)
                              + " s of audio; "
                              + (healthyCopyBesides (*copy) ? "the other copies have it"
                                                            : "no other copy has it"));
        }
        return problems;
    }

    int MultitrackRecorder::useTimeSlice()
    {
        // Recorder is no longer registered as a client itself -- drain
        // work runs on ShardClient instances. This is here only because
        // we still inherit from TimeSliceClient (compat shim) and JUCE
        // requires the override to be present.
        return 1000;
    }

    int MultitrackRecorder::ShardClient::useTimeSlice()
    {
        // Re-join the audio device's workgroup whenever the generation
        // counter changes (set on every device start / restart). Token
        // is thread_local so its lifetime matches the worker thread.
        thread_local juce::WorkgroupToken token;
        thread_local int                  lastJoinedGen = -1;
        if (owner != nullptr)
        {
            const int gen = owner->workgroupGeneration.load (std::memory_order_acquire);
            if (gen != lastJoinedGen)
            {
                lastJoinedGen = gen;
                owner->joinAudioWorkgroupOnCurrentThread (token);
            }
            owner->drainShard (*this);
        }
        return 5; // ms
    }

    void MultitrackRecorder::setAudioWorkgroup (juce::AudioWorkgroup wg)
    {
        {
            const juce::ScopedLock sl (workgroupLock);
            currentWorkgroup = std::move (wg);
        }
        workgroupGeneration.fetch_add (1, std::memory_order_release);
    }

    void MultitrackRecorder::joinAudioWorkgroupOnCurrentThread (juce::WorkgroupToken& token)
    {
        const juce::ScopedLock sl (workgroupLock);
        currentWorkgroup.join (token);
    }

    float MultitrackRecorder::getDiskBytesPerSec() const noexcept
    {
        float total = 0.0f;
        for (auto& sh : shards)
            total += sh->shardBytesPerSec.load (std::memory_order_relaxed);
        return total;
    }

    float MultitrackRecorder::getRingFillPct() const noexcept
    {
        int worst = 0;
        for (auto& sh : shards)
        {
            const int v = sh->shardRingFillPct.load (std::memory_order_relaxed);
            if (v > worst) worst = v;
        }
        return (float) worst;
    }

    static int bytesPerSampleOf (CaptureFormat format) noexcept
    {
        switch (format)
        {
            case CaptureFormat::Wav16:  case CaptureFormat::Aiff16: case CaptureFormat::Flac16: return 2;
            case CaptureFormat::Wav32Float: case CaptureFormat::Aiff32Float:                    return 4;
            default: return 3;
        }
    }

    juce::int64 MultitrackRecorder::estimateBytesPerSecondForArmedTracks() const noexcept
    {
        // bytesPerSec for a single armed mono track = sampleRate * bitsPerSample / 8.
        const int bytesPerSampPrimary = bytesPerSampleOf (captureFormat);
        const int bytesPerSampBackup  = bytesPerSampleOf (backupCaptureFormat);
        const bool backupOn = isRecording()
                                ? (backupActive.load (std::memory_order_relaxed)
                                   && ! backupFailed.load (std::memory_order_relaxed))
                                : backupDir.isDirectory();

        const int captureChannels = armedCaptureChannels();

        // Every configured MIRROR is another full parallel copy of every armed
        // track, in its own format. Omitting them under-reported the real write
        // rate by the whole mirror count -- which made "minutes remaining"
        // optimistic and raised the bar isDiskStruggling compares against, so
        // the disk warning fired late (or not at all) on a mirrored rig.
        // While a take is rolling the honest count is what actually opened;
        // before it starts, the configured list is the right (pessimistic) one.
        int mirrorChannelBytes = 0;
        if (isRecording())
        {
            for (size_t mi = 0; mi < activeMirrors.size(); ++mi)
                if (const auto* copy = copyAt (2 + (int) mi))
                    mirrorChannelBytes += copy->liveChannels.load (std::memory_order_relaxed)
                                        * bytesPerSampleOf (activeMirrors[mi].format);
        }
        else
        {
            for (const auto& mc : mirrorConfigs)
                mirrorChannelBytes += captureChannels * bytesPerSampleOf (mc.format);
        }

        // FLAC compresses ~50% typical; treat its byte rate as the
        // worst case (uncompressed equivalent) to keep the estimate
        // pessimistic. Engineers don't want a "you have 6 h" estimate
        // that turns into 3 h because today's audio compressed poorly.
        return (juce::int64) (sampleRate
                              * (captureChannels * (bytesPerSampPrimary
                                                   + (backupOn ? bytesPerSampBackup : 0))
                                 + mirrorChannelBytes));
    }

    void MultitrackRecorder::startDiskWatch()
    {
        auto watch = std::make_shared<DiskWatch>();
        watch->sampleRate   = sampleRate;
        watch->startedMs    = juce::Time::getMillisecondCounterHiRes();
        watch->staleAfterMs = volumeStaleAfterMs;
        const VolumeProbe::Reader reader = volumeReaderForTests != nullptr ? volumeReaderForTests
                                                                           : VolumeProbe::readVolume;
        for (const auto& copy : copies)
        {
            const auto format = copy->copyIndex == 0 ? captureFormat
                              : copy->copyIndex == 1 ? backupCaptureFormat
                                                     : activeMirrors[(std::size_t) copy->copyIndex - 2].format;
            watch->volumes.push_back ({ std::make_shared<VolumeProbe> (copy->root, reader, volumeProbePeriodMs),
                                        copy, bytesPerSampleOf (format) });
        }
        std::atomic_store (&diskWatch, std::shared_ptr<const DiskWatch> (std::move (watch)));
    }

    MultitrackRecorder::DiskEstimate MultitrackRecorder::readDiskEstimate() const
    {
        DiskEstimate estimate;
        const auto watch = std::atomic_load (&diskWatch);
        if (watch == nullptr) return estimate;
        estimate.watching = true;

        // Copies on one volume drain it together; a mirror on its own drive
        // must not shorten the primary's figure.
        struct Volume { juce::uint64 id; juce::int64 freeBytes; juce::int64 bytesPerMinute; };
        std::vector<Volume> volumes;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        for (const auto& watched : watch->volumes)
        {
            const bool isPrimary = watched.copy->copyIndex == 0;
            const auto channels  = watched.copy->liveChannels.load (std::memory_order_relaxed);
            if (! isPrimary && channels <= 0) continue;   // not writing: no drive to run out of

            const auto reading = watched.probe->latest();
            if (now - (reading.atMs > 0.0 ? reading.atMs : watch->startedMs) > watch->staleAfterMs)
            {
                (isPrimary ? estimate.primaryStale : estimate.copyStale) = true;
                continue;
            }
            if (reading.atMs <= 0.0
                || ! preflight::freeSpaceKnown (reading.freeBytes, reading.totalBytes)) continue;
            if (isPrimary) estimate.primaryFreeBytes = reading.freeBytes;

            const auto rate = (juce::int64) (watch->sampleRate * channels * watched.bytesPerSample * 60.0);
            const auto volume = std::find_if (volumes.begin(), volumes.end(), [&] (const Volume& v)
                                              { return reading.volumeId != 0 && v.id == reading.volumeId; });
            if (volume != volumes.end()) volume->bytesPerMinute += rate;
            else volumes.push_back ({ reading.volumeId, reading.freeBytes, rate });
        }
        if (estimate.primaryFreeBytes < 0) return estimate;   // no primary, no figure

        int minutes = std::numeric_limits<int>::max();
        for (const auto& volume : volumes)
            if (volume.bytesPerMinute > 0)
                minutes = juce::jmin (minutes, (int) juce::jmin ((juce::int64) std::numeric_limits<int>::max(),
                                                                 volume.freeBytes / volume.bytesPerMinute));
        // 0 means no estimate. The last minute must not read as that: it would
        // take the warning down at the moment it matters most.
        estimate.minutes = minutes == std::numeric_limits<int>::max() ? 0 : juce::jmax (1, minutes);
        return estimate;
    }

    int MultitrackRecorder::estimateMinutesRemaining (const juce::File& primaryVolume,
                                                      const juce::File& backupVolume) const noexcept
    {
        const int captureChannels = armedCaptureChannels();
        if (sampleRate <= 0.0 || captureChannels <= 0) return 0;

        const auto rateFor = [&] (CaptureFormat f) -> juce::int64
        {
            return (juce::int64) (sampleRate * captureChannels * bytesPerSampleOf (f));
        };
        constexpr int kEffectivelyUnbounded = std::numeric_limits<int>::max() / 2;

        auto minutesForVolume = [&] (const juce::File& vol, juce::int64 rate) -> int
        {
            if (! vol.exists() || rate <= 0) return kEffectivelyUnbounded;
            const auto free = vol.getBytesFreeOnVolume();
            if (! preflight::freeSpaceKnown (free, vol.getVolumeTotalSize()))
                return kEffectivelyUnbounded;                  // don't pessimize
            return (int) (free / (rate * (juce::int64) 60));
        };
        // Combine rates that land on the SAME filesystem. Primary + backup on
        // one volume drain that volume twice as fast; mirrors on other drives
        // must not reduce the primary drive's estimate.
        struct VolumeRate { juce::File file; juce::uint64 id { 0 }; juce::int64 rate { 0 }; };
        std::vector<VolumeRate> volumes;
        auto addVolumeRate = [&] (const juce::File& file, juce::int64 rate)
        {
            if (! file.exists() || rate <= 0) return;
            const auto id = VolumeProbe::readVolume (file).volumeId;
            if (id != 0)
                for (auto& volume : volumes)
                    if (volume.id == id) { volume.rate += rate; return; }
            volumes.push_back ({ file, id, rate });
        };

        addVolumeRate (primaryVolume, rateFor (captureFormat));
        // A copy stuck after the last take is exactly the path that would hang
        // these stats, and the take after it will not use that destination.
        if (! isStillLeftBehind (backupVolume) && backupVolume.exists() && backupDir.isDirectory())
            addVolumeRate (backupVolume, rateFor (backupCaptureFormat));

        for (const auto& mirror : mirrorConfigs)
            if (! isStillLeftBehind (mirror.root))
                addVolumeRate (mirror.root, rateFor (mirror.format));

        int minutes = kEffectivelyUnbounded;
        for (const auto& volume : volumes)
            minutes = juce::jmin (minutes, minutesForVolume (volume.file, volume.rate));
        if (minutes == kEffectivelyUnbounded) return 0;
        return minutes;
    }

    void MultitrackRecorder::drainOnce()
    {
        // Legacy entry-point -- when called from anywhere other than the
        // shard clients, fan out to every shard so behaviour is identical
        // to the pre-shard implementation (mostly used by stopRecording's
        // final flush).
        for (auto& sh : shards) drainShard (*sh);
    }

    // The distributor: audio FIFOs to every copy's ring, no disk. Telemetry is
    // published here for the same reason. A copy stuck in a write cannot report
    // that it is stuck, but this thread sees its counters stop.
    void MultitrackRecorder::drainShard (ShardClient& shard)
    {
        if (! writersReady.load (std::memory_order_acquire)) return;

        // One drainer per shard at a time: the shard's TimeSliceThread and a
        // fan-out drainOnce() (stopRecording flush / drainPendingForTests)
        // must not read the single-reader FIFO concurrently. Consumer-side
        // only -- never contends the audio thread.
        const juce::ScopedLock sl (shard.drainLock);

        int worstFillPct = 0;

        const std::size_t first = (std::size_t) juce::jmax (0, shard.firstChannel);
        const std::size_t last  = (std::size_t) juce::jmax (0, shard.lastChannel);

        for (std::size_t i = first;
             i < last && i < fifos.size() && i < writers.size();
             ++i)
        {
            auto& cf = *fifos[i];
            const auto& w  = writers[i];
            // Skip only channels that don't participate in the take (empty
            // stereo-R slots, unarmed).
            if (! w.active) continue;

            // Track FIFO occupancy so the dashboard can warn the engineer
            // BEFORE samples are dropped.
            const int cap = (int) cf.data.size();
            if (cap > 0)
            {
                const int pct = (cf.fifo.getNumReady() * 100) / cap;
                if (pct > worstFillPct) worstFillPct = pct;
            }

            // Snapshot a finite timeline budget so live callbacks cannot keep
            // this drain running forever. Each stereo side supplies its own
            // real/silent spans, preserving alignment even for unequal drops.
            const int channels = w.numChannels == 2 && i + 1 < fifos.size() ? 2 : 1;
            auto* right = channels == 2 ? fifos[i + 1].get() : nullptr;
            juce::int64 remaining;
            {
                const juce::SpinLock::ScopedLockType guard (captureBoundary);
                remaining = cf.producedFrames - cf.consumedFrames;
                if (right != nullptr)
                    remaining = juce::jmin (remaining, right->producedFrames - right->consumedFrames);
            }
            constexpr int chunkLimit = CaptureCopy::kChunkFrames;
            if (remaining > 0)
            {
                shard.stageL.resize ((size_t) juce::jmin (remaining, (juce::int64) chunkLimit));
                if (right != nullptr) shard.stageR.resize (shard.stageL.size());
            }
            while (remaining > 0)
            {
                const int frames = (int) juce::jmin (remaining, (juce::int64) chunkLimit);
                ChannelFifo::DrainPlan leftPlan, rightPlan;
                {
                    // Only bounded cursor/descriptor operations. The reserved
                    // sample ranges stay occupied until they reach the copies,
                    // so the producer cannot overwrite a span being read.
                    const juce::SpinLock::ScopedLockType guard (captureBoundary);
                    cf.reserveTimeline (leftPlan, frames);
                    if (right != nullptr) right->reserveTimeline (rightPlan, frames);
                }
                if (afterFifoReservationForTests) afterFifoReservationForTests();
                auto materialize = [] (const ChannelFifo& source, const ChannelFifo::DrainPlan& plan,
                                       std::vector<float>& destination)
                {
                    int sourceOffset = 0;
                    for (size_t spanIndex = 0; spanIndex < plan.count; ++spanIndex)
                    {
                        const auto& span = plan.spans[spanIndex];
                        auto* out = destination.data() + span.offset;
                        if (span.silence)
                        {
                            juce::FloatVectorOperations::clear (out, span.length);
                            continue;
                        }
                        int count = span.length;
                        if (sourceOffset < plan.samples.blockSize1)
                        {
                            const int first = juce::jmin (count, plan.samples.blockSize1 - sourceOffset);
                            std::memcpy (out, source.data.data() + plan.samples.startIndex1 + sourceOffset,
                                         (size_t) first * sizeof (float));
                            sourceOffset += first; out += first; count -= first;
                        }
                        if (count > 0)
                        {
                            std::memcpy (out, source.data.data() + plan.samples.startIndex2
                                               + sourceOffset - plan.samples.blockSize1,
                                         (size_t) count * sizeof (float));
                            sourceOffset += count;
                        }
                    }
                };
                materialize (cf, leftPlan, shard.stageL);
                if (right != nullptr) materialize (*right, rightPlan, shard.stageR);
                for (auto& copy : copies)
                    copy->push (i, shard.stageL.data(), right != nullptr ? shard.stageR.data() : nullptr, frames);
                remaining -= frames;
            }
        }

        juce::int64 bytesOut = 0;
        int worstWriteMs = 0;
        for (auto& copy : copies)
        {
            bytesOut += copy->bytesOut.load (std::memory_order_relaxed);
            worstWriteMs = juce::jmax (worstWriteMs, copy->lastWriteMs.load (std::memory_order_relaxed));
            worstFillPct = juce::jmax (worstFillPct, copy->fillPercent());
        }
        lastWriteMs.store (worstWriteMs, std::memory_order_relaxed);

        // Publish this shard's worst FIFO fill so the global aggregator
        // can report the max across shards. EMA release so it doesn't
        // twitch on every drain.
        {
            const int prev = shard.shardRingFillPct.load (std::memory_order_relaxed);
            const int next = (worstFillPct > prev)
                                ? worstFillPct
                                : (int) (prev * 0.8f + worstFillPct * 0.2f);
            shard.shardRingFillPct.store (next, std::memory_order_relaxed);
        }

        // Disk throughput over a rolling window.
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (shard.throughputWindowMs <= 0.0)
        {
            shard.throughputWindowMs  = now;
            shard.throughputBaseBytes = bytesOut;
        }
        const double dtMs = now - shard.throughputWindowMs;
        if (dtMs >= 250.0)
        {
            const double bps = (double) (bytesOut - shard.throughputBaseBytes) * 1000.0 / dtMs;
            shard.shardBytesPerSec.store ((float) bps, std::memory_order_relaxed);
            shard.throughputBaseBytes = bytesOut;
            shard.throughputWindowMs  = now;
        }
    }
}
