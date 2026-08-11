#include "MultitrackRecorder.h"
#include "FastHash.h"
#include "PunchSplice.h"
#include "MultiPartReader.h"

#include <juce_cryptography/juce_cryptography.h>
#include <limits>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>

#if JUCE_MAC
 #include <pthread/qos.h>
#endif

namespace zynforge
{
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

    // Writer-thread pool sizing: one worker per ~32 channels, capped at 8.
    // M-series performance-cores beyond 8 give diminishing returns for
    // small-block I/O and we don't want to starve other UI work.
    static int chooseShardCount()
    {
        // Single writer thread, deliberately.
        //
        // The multi-shard design (one TimeSliceThread per channel-range)
        // was meant to parallelise disk writes, but it corrupted captures:
        // under real parallelism whole channels came back as white-noise
        // garbage, non-deterministically, worse with more channels. The
        // headless RecordingIntegrityTests reproduce it at 2+ writer
        // threads and show ZERO failures at 1 -- see decisions.md
        // "Single writer thread for capture".
        //
        // The parallelism bought us nothing real anyway: 48 tracks of
        // 24-bit/48 kHz is ~6.6 MB/s, which one thread drains with the
        // multi-second FIFO never coming close to full. Correctness of the
        // recording is non-negotiable for a recorder, so we serialise all
        // writing onto one thread until/unless the sharding race is found
        // and fixed. The shard machinery is kept (one shard covering every
        // channel) so re-enabling is a one-line change once it's proven.
        return 1;
    }

    // Crash-atomic text write: fully write a temp sibling then atomically
    // rename it over the target (POSIX rename() overwrites in place on the
    // same volume), so a crash mid-write can never leave a half-written /
    // torn report or recovery marker. Used for the recovery-session marker
    // and both session.report.json writes.
    static void writeTextAtomic (const juce::File& target, const juce::String& text)
    {
        const auto tmp = target.getSiblingFile (target.getFileName() + ".tmp");
        tmp.deleteFile();
        if (tmp.replaceWithText (text) && tmp.moveFileTo (target))
            return;
        // Fallback: best-effort direct write (e.g. rename unsupported on this
        // filesystem). Still better than leaving nothing.
        tmp.deleteFile();
        target.replaceWithText (text);
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
            th->stopThread (2000);
    }

    void MultitrackRecorder::prepare (double sr, int maxBlock, int numInputs)
    {
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
                auto t = std::make_unique<TrackState>();
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

    void MultitrackRecorder::setBackupDirectory (const juce::File& dir)
    {
        if (isRecording()) return;
        backupDir = dir;
    }

    void MultitrackRecorder::addTrack()
    {
        if (isRecording()) return;

        const int fifoSize = juce::nextPowerOfTwo ((int) (sampleRate * kFifoSeconds));

        auto t = std::make_unique<TrackState>();
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
        if (isRecording() || tracks.empty()) return;
        tracks.pop_back();
        fifos.pop_back();
        preRoll.pop_back();
        trackGen.fetch_add (1, std::memory_order_relaxed);
        rebuildShards();
    }

    void MultitrackRecorder::removeTrackAt (int index)
    {
        if (isRecording()) return;
        if (index < 0 || index >= (int) tracks.size()) return;
        tracks.erase (tracks.begin() + index);
        fifos.erase  (fifos.begin()  + index);
        preRoll.erase(preRoll.begin()+ index);
        trackGen.fetch_add (1, std::memory_order_relaxed);
        rebuildShards();
    }

    void MultitrackRecorder::setTrackCount (int n)
    {
        if (isRecording()) return;
        n = juce::jmax (0, n);
        while ((int) tracks.size() > n) removeLastTrack();
        while ((int) tracks.size() < n) addTrack();
    }

    void MultitrackRecorder::setPreRollSeconds (int seconds)
    {
        seconds = juce::jlimit (0, 30, seconds);
        if (seconds == preRollSeconds) return;
        if (isRecording()) return;
        preRollSeconds = seconds;
        allocatePreRollBuffers();
    }

    void MultitrackRecorder::allocatePreRollBuffers()
    {
        const int samples = preRollSeconds > 0
                             ? (int) (sampleRate * (preRollSeconds + kPreRollSafetySec))
                             : 0;
        for (auto& b : preRoll)
            b->allocate (samples);
    }

    void MultitrackRecorder::dumpPreRollToWriters()
    {
        if (preRollSeconds <= 0) return;

        const int wanted = (int) (sampleRate * preRollSeconds);
        std::vector<float> tmp ((std::size_t) wanted, 0.0f);

        std::vector<float> tmpR ((std::size_t) wanted, 0.0f);

        for (std::size_t i = 0; i < writers.size() && i < preRoll.size(); ++i)
        {
            auto& w = writers[i];
            // Participation gate, not `writer != nullptr` -- a channel whose
            // primary failed to open but whose backup/mirror opened still gets
            // its pre-roll history (see WriterChannel::active + the live drain).
            if (! w.active) continue;

            // Fan the SAME pre-roll history out to every destination (primary
            // + backup + all mirrors) exactly like the live drain does, so all
            // copies stay sample-identical and time-aligned -- and advance the
            // per-track sample/byte counters so the report length matches the
            // file length (pre-roll used to be written to the primary only and
            // counted nowhere, leaving the destinations divergent + the report
            // short).
            auto fanOut = [&w] (const float* const* arr, int chans, int got) noexcept
            {
                if (w.writer != nullptr && w.writer->writeFromFloatArrays (arr, chans, got))
                {
                    w.bytesWrittenPrimary += (juce::int64) got * chans * w.bytesPerSamplePrimary;
                    w.totalSamplesPrimary += got;
                }
                if (w.backupWriter != nullptr
                    && w.backupWriter->writeFromFloatArrays (arr, chans, got))
                {
                    w.bytesWrittenBackup += (juce::int64) got * chans * w.bytesPerSampleBackup;
                    w.totalSamplesBackup += got;
                }
                for (auto& m : w.mirrors)
                {
                    if (m.writer == nullptr || m.failed) continue;
                    if (m.writer->writeFromFloatArrays (arr, chans, got))
                    {
                        m.bytesWritten += (juce::int64) got * chans * m.bytesPerSample;
                        m.totalSamples += got;
                    }
                }
            };

            // Stereo pair: dump BOTH channels' history into the 2-ch copies so
            // pre-roll keeps the L/R image, matching the live path.
            if (w.numChannels == 2 && i + 1 < preRoll.size())
            {
                const int gotL = preRoll[i]    ->readHistory (tmp .data(), wanted);
                const int gotR = preRoll[i + 1]->readHistory (tmpR.data(), wanted);
                const int got  = juce::jmin (gotL, gotR);
                if (got <= 0) continue;
                const float* const arr[] = { tmp.data(), tmpR.data() };
                fanOut (arr, 2, got);
                continue;
            }

            const int got = preRoll[i]->readHistory (tmp.data(), wanted);
            if (got <= 0) continue;
            const float* const arr[] = { tmp.data() };
            fanOut (arr, 1, got);
        }
    }

    void MultitrackRecorder::release()
    {
        // Called from AudioEngine::audioDeviceStopped on every device
        // stop / restart (device swap, sample-rate or buffer-size change).
        // Detach the device- and thread-bound writer shards, but PRESERVE
        // the per-track state (tracks / fifos / preRoll). Every
        // ChannelStrip / LedMeter / MiniSpectrum / EditPage row caches a
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
                                           int numSamples) noexcept
    {
        const int n = juce::jmin (numChannels, (int) fifos.size());
        const bool rec = recording.load (std::memory_order_acquire);

        for (int ch = 0; ch < n; ++ch)
        {
            auto& t = *tracks[(std::size_t) ch];

            // Arm-or-monitor gate. Pro Tools convention:
            //   ARM     = will record when you hit RECORD
            //   MONITOR = listen to the input through the monitor bus
            // Either of those modes should drive the strip's meter so
            // the engineer can gain-stage WITHOUT having to arm the
            // strip first. Pre-roll history + ring buffer push are
            // still arm-gated below.
            const bool armed   = t.armed  .load (std::memory_order_relaxed);
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
                         && lft.armed   .load (std::memory_order_relaxed)
                         && ! lft.isBus .load (std::memory_order_relaxed);
            }

            if (! armed && ! monitor && ! pairArmed)
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
                // The RIGHT half of an armed stereo pair MUST keep its FIFO fed
                // even with no input routed to it. The pair writes ONE
                // interleaved file and the drain consumes min(L,R) frames, so a
                // starved R side pinned `avail` at 0 forever: L overflowed, the
                // 2-ch write never fired, and the whole pair came back as
                // silence padding. Push zeros so the pair stays in lockstep and
                // the take is L-plus-silence instead of nothing.
                if (! (rec && pairArmed)) continue;
                if (t.isBus.load (std::memory_order_relaxed)) continue;
                const int  n0  = juce::jmin (numSamples, (int) scratch.size());
                if (n0 <= 0) continue;
                auto& cfz = *fifos[(std::size_t) ch];
                const auto zs = cfz.fifo.write (n0);
                if (zs.blockSize1 > 0)
                    std::memcpy (cfz.data.data() + zs.startIndex1, scratch.data(),
                                 (std::size_t) zs.blockSize1 * sizeof (float));
                if (zs.blockSize2 > 0)
                    std::memcpy (cfz.data.data() + zs.startIndex2, scratch.data(),
                                 (std::size_t) zs.blockSize2 * sizeof (float));
                const int wroteZ = zs.blockSize1 + zs.blockSize2;
                if (wroteZ < numSamples)
                {
                    const int deficit = numSamples - wroteZ;
                    missedSamples     .fetch_add (deficit, std::memory_order_relaxed);
                    cfz.droppedSamples.fetch_add (deficit, std::memory_order_relaxed);
                }
                continue;
            }

            // Meter (peak + simple block RMS)
            float peak = 0.0f, sumSq = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                const float s = src[i];
                const float a = std::abs (s);
                if (a > peak) peak = a;
                sumSq += s * s;
            }
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

            // Always feed the pre-roll history if it's been allocated.
            if (ch < (int) preRoll.size() && ! preRoll[(std::size_t) ch]->data.empty())
                preRoll[(std::size_t) ch]->push (src, numSamples);

            // Push to ring buffer when recording. Armed channels push; the
            // R half of an armed stereo pair also pushes (pairArmed) so both
            // sides of the interleaved file advance together. A BUS never pushes
            // -- it has no input + gets no writer (active stays false), so an
            // armed bus (e.g. armed over OSC/MCU) would feed an undrained FIFO
            // that overflows into a false "samples dropped" alarm.
            if (rec && ! t.isBus.load (std::memory_order_relaxed)
                    && (t.armed.load (std::memory_order_relaxed) || pairArmed))
            {
                // Live waveform overview: bin the captured input into min/max
                // pairs so the EDIT lane draws the detailed waveform AS it
                // records (lock-free; see TrackState::liveWavePush).
                t.liveWavePush (src, numSamples);

                auto& cf = *fifos[(std::size_t) ch];
                const auto scope = cf.fifo.write (numSamples);
                const int wrote = scope.blockSize1 + scope.blockSize2;

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
                    cf.droppedSamples .fetch_add (deficit, std::memory_order_relaxed);
                }
            }
        }

        if (rec)
            samplesSinceStart.fetch_add (numSamples, std::memory_order_relaxed);
    }

    // NOTE: punch-in is triggered two ways, both via armPunchIn(): the normal
    // RECORD button when a session is loaded (MainComponent::onRecordClicked --
    // continue/overdub at the edit cursor, append at the end by default), and
    // the PUNCH-mode position window (MainComponent::servicePunch). Either way
    // this recorder just stashes + splices; it doesn't know which path armed it.
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

    bool MultitrackRecorder::startRecording (const juce::File& sessionDir)
    {
        if (recording.load()) return false;
        if (tracks.empty())   return false;

        // A recorder must never report a live take it isn't actually writing:
        // if the session folder can't be created (read-only / unmounted volume,
        // permissions), fail the start rather than silently swallow it.
        if (! sessionDir.createDirectory().wasOk() && ! sessionDir.isDirectory())
            return false;
        activeSessionDir = sessionDir;
        // Space the first periodic header flush ~5 s into the take.
        lastWriterFlushMs.store (juce::Time::getMillisecondCounterHiRes(),
                                 std::memory_order_relaxed);

        // Folder layout: tracks live under "Audio Files/", mixdowns /
        // stem exports under "Export Files/", backups under
        // "Session File Backups/". Session-level metadata (recording.session,
        // session.report.json, the .zfproj document) stays at the root.
        const auto audioFilesDir = sessionDir.getChildFile ("Audio Files");
        if (! audioFilesDir.createDirectory().wasOk() && ! audioFilesDir.isDirectory())
            return false;   // no place to write the tracks -> not a real take
        sessionDir.getChildFile ("Export Files")        .createDirectory();
        sessionDir.getChildFile ("Clip Groups")         .createDirectory();
        sessionDir.getChildFile ("Session File Backups").createDirectory();

        writers.clear();
        writers.reserve (tracks.size());

        // Resolves a CaptureFormat to its container code + bit-depth +
        // file extension. Container codes match the integer scheme used
        // by openWriterAtPath / WriterChannel::primaryContainer:
        //   0 = WAV, 1 = AIFF, 2 = FLAC.
        struct Resolved { int container; int bitDepth; const char* ext; };
        auto resolve = [] (CaptureFormat f) -> Resolved
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
        };
        auto       primary = resolve (captureFormat);
        const auto backup  = resolve (backupCaptureFormat);

        // True when track k is one we open a writer for: not a bus, and armed.
        auto isArmedCapture = [this] (std::size_t k)
        {
            return ! tracks[k]->isBus .load (std::memory_order_relaxed)
                   &&  tracks[k]->armed.load (std::memory_order_relaxed);
        };

        // Punch-in: move the existing take -- AND its continuation parts -- aside
        // to sidecars BEFORE the writer truncates the target, so the WHOLE base
        // (a take built up by continue-recording is multi-part) survives to be
        // spliced + flattened back on stop. No-op when not punching or no take.
        auto stashForPunch = [this] (const juce::File& target)
        {
            if (! punchInActive) return;
            for (auto& part : findTakeParts (target))   // [Track_NN, _part02, ...]
            {
                const auto sidecar = punchSidecar (part);
                sidecar.deleteFile();
                part.moveFileTo (sidecar);
            }
        };

        // Continue: the timeline base is the EXISTING take's length, measured
        // from the files on disk (the longest armed take's Track_NN + its parts)
        // -- authoritative, so the clock / playhead continue correctly even if
        // the caller's hint was stale. Read before the writers open new parts.
        if (continueAsPart)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();

            // A CONTINUE must extend the take in the container it's already in.
            // The scan below used to look only for `primary.ext`, so if the
            // engineer changed the capture format between takes (WAV -> FLAC,
            // say) it found nothing: recordBaseSamples stayed 0 and the
            // "continuation" was written as Track_NN.flac PART 1 alongside the
            // existing Track_NN.wav -- two files claiming the same track index,
            // which SessionPlayer then globs as one confused take. Adopt the
            // existing take's container + bit depth for this take instead.
            static const char* kContainerExts[] = { ".wav", ".aif", ".aiff", ".flac" };
            for (std::size_t i = 0; i < tracks.size() && continueAsPart; ++i)
            {
                if (tracks[i]->isBus .load (std::memory_order_relaxed)) continue;
                if (! tracks[i]->armed.load (std::memory_order_relaxed)) continue;
                const auto name = juce::String::formatted ("Track_%02d", (int) i + 1);
                if (audioFilesDir.getChildFile (name + primary.ext).existsAsFile())
                    break;                       // already the right container
                for (auto* ext : kContainerExts)
                {
                    const auto f = audioFilesDir.getChildFile (name + ext);
                    if (! f.existsAsFile()) continue;
                    if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (f) })
                    {
                        const juce::String e (ext);
                        primary.container = e == ".flac" ? 2 : (e == ".wav" ? 0 : 1);
                        primary.ext       = ext;
                        primary.bitDepth  = r->usesFloatingPointData
                                              ? 32 : (int) r->bitsPerSample;
                    }
                    break;
                }
                break;   // one armed take is enough to pin the container
            }

            juce::int64 takeLen = 0;
            for (std::size_t i = 0; i < tracks.size(); ++i)
            {
                if (tracks[i]->isBus .load (std::memory_order_relaxed)) continue;
                if (! tracks[i]->armed.load (std::memory_order_relaxed)) continue;
                const auto name = juce::String::formatted ("Track_%02d", (int) i + 1);
                const auto mainFile = audioFilesDir.getChildFile (name + primary.ext);
                juce::int64 len = 0;
                for (auto& pf : findTakeParts (mainFile))
                    if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (pf) })
                        len += r->lengthInSamples;
                takeLen = juce::jmax (takeLen, len);
            }
            recordBaseSamples = takeLen;
        }

        // Track how many primary writers we TRY to open vs how many actually
        // open, so a disk-full / read-only / permissions failure at record
        // start is treated as the hard error it is instead of being swallowed.
        int primaryAttempts = 0;
        int primaryOpened   = 0;

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

            // No writer for bus tracks (no input) OR unarmed tracks. Push
            // an empty WriterChannel so the per-channel index stays aligned
            // with tracks[i]; processBlock's arm-gate already skips both.
            //
            // CRITICAL: unarmed tracks must NOT open a writer, because
            // openWriterAtPath truncates the target file (deleteFile). When
            // a second take into the SAME session arms a different channel,
            // opening writers for the still-unarmed earlier tracks would
            // wipe their Track_NN.wav from the first take -- silent data
            // loss. Skipping them here leaves those files intact.
            if (tracks[i]->isBus .load (std::memory_order_relaxed)
                || ! tracks[i]->armed.load (std::memory_order_relaxed))
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

            // Primary writer -- under <session>/Audio Files/Track_NN.<ext>.
            // Continue mode appends a NEW part (Track_NN_partXX) so the existing
            // take is never touched; otherwise it's the normal Track_NN.<ext>.
            auto primaryFile = audioFilesDir.getChildFile (trackName + primary.ext);
            if (continueAsPart)
            {
                auto [pf, pn] = nextContinuationPart (audioFilesDir, trackName, primary.ext);
                primaryFile = pf;
                w.partNumberPrimary = pn;
            }
            stashForPunch (primaryFile);
            w.writer.reset (openWriterAtPath (primaryFile, primary.container, primary.bitDepth, chans));
            ++primaryAttempts;
            if (w.writer != nullptr) ++primaryOpened;
            w.primaryBaseFile       = audioFilesDir.getChildFile (trackName);
            w.primaryExt            = primary.ext;
            w.primaryBitDepth       = primary.bitDepth;
            w.primaryContainer      = primary.container;
            w.bytesPerSamplePrimary = primary.bitDepth / 8;
            if (w.writer != nullptr)
                w.partFilesPrimary.add (primaryFile.getFileName());

            // Optional second copy -- may be in a different format from
            // the primary, so the engineer can run e.g. WAV/24 to the
            // main drive AND FLAC/24 to the backup drive simultaneously.
            // Backups mirror the same Audio Files/ layout under the chosen
            // backup root.
            if (backupDir.isDirectory())
            {
                auto backupSession = backupDir.getChildFile (sessionDir.getFileName())
                                              .getChildFile ("Audio Files");
                backupSession.createDirectory();
                auto backupFile = backupSession.getChildFile (trackName + backup.ext);
                if (continueAsPart)
                {
                    auto [bf, bn] = nextContinuationPart (backupSession, trackName, backup.ext);
                    backupFile = bf;
                    w.partNumberBackup = bn;
                }
                stashForPunch (backupFile);
                w.backupWriter.reset (openWriterAtPath (backupFile, backup.container, backup.bitDepth, chans));
                w.backupBaseFile       = backupSession.getChildFile (trackName);
                w.backupExt            = backup.ext;
                w.backupBitDepth       = backup.bitDepth;
                w.backupContainer      = backup.container;
                w.bytesPerSampleBackup = backup.bitDepth / 8;
                if (w.backupWriter != nullptr)
                    w.partFilesBackup.add (backupFile.getFileName());
            }

            // Extra N-way mirror destinations beyond primary + backup.
            for (const auto& mc : mirrorConfigs)
            {
                if (! mc.root.isDirectory())
                {
                    // Try to create -- root might be a path the
                    // engineer set up before plugging in the drive.
                    if (! mc.root.createDirectory().wasOk()) continue;
                }
                const auto mirrorSession = mc.root.getChildFile (sessionDir.getFileName())
                                                    .getChildFile ("Audio Files");
                mirrorSession.createDirectory();
                const auto mFmt = resolve (mc.format);
                auto mFile = mirrorSession.getChildFile (trackName + mFmt.ext);
                int  mPart = 1;
                if (continueAsPart)
                {
                    auto [mf, mn] = nextContinuationPart (mirrorSession, trackName, mFmt.ext);
                    mFile = mf;
                    mPart = mn;
                }
                stashForPunch (mFile);
                WriterChannel::Mirror m;
                m.partNumber = mPart;
                m.writer.reset (openWriterAtPath (mFile, mFmt.container, mFmt.bitDepth, chans));
                m.baseFile       = mirrorSession.getChildFile (trackName);
                m.ext            = mFmt.ext;
                m.container      = mFmt.container;
                m.bitDepth       = mFmt.bitDepth;
                m.bytesPerSample = mFmt.bitDepth / 8;
                if (m.writer != nullptr)
                    m.partFiles.add (mFile.getFileName());
                w.mirrors.push_back (std::move (m));
            }

            // This channel opened (or attempted) real destinations, so it
            // participates in the take and MUST keep draining even if its
            // primary later fails -- see WriterChannel::active. Empty R slots
            // and unarmed/bus channels `continue`d above and stay inactive.
            w.active = true;

            writers.push_back (std::move (w));

            fifos[i]->fifo.reset();
        }

        // Hard error: if we tried to open primary writers but EVERY one failed
        // (disk full, unmounted/read-only volume, permissions), do NOT enter
        // the recording state and do NOT report success -- the caller must not
        // show a live take that isn't being written anywhere. (Any punch
        // sidecars stay on disk under their .punchbase names, so the existing
        // take is renamed-aside, never lost.) The success path -- at least one
        // primary writer open -- is unchanged.
        if (primaryAttempts > 0 && primaryOpened == 0)
        {
            closeWriters();
            return false;
        }

        backupActive.store (backupDir.isDirectory(), std::memory_order_relaxed);
        backupFailed.store (false, std::memory_order_relaxed);
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
            writeTextAtomic (sessionDir.getChildFile ("recording.session"),
                             juce::JSON::toString (juce::var (m.get())));
        }

        samplesSinceStart.store (0, std::memory_order_relaxed);
        missedSamples    .store (0, std::memory_order_relaxed);
        samplesSinceFlush.store (0, std::memory_order_relaxed);
        for (auto& f : fifos) f->droppedSamples.store (0, std::memory_order_relaxed);
        // Clear each track's live waveform overview so a new take draws fresh
        // (done before the audio thread starts pushing into it).
        for (auto& t : tracks) t->liveWaveReset();

        // Ordering closes the pre-roll/live seam gap. Enable capture FIRST
        // (recording=true) so the audio thread starts filling the FIFOs with
        // no sample able to fall between the pre-roll snapshot and live start;
        // the drain stays gated OFF (writersReady=false) so no writer thread
        // can race the pre-roll writes below. THEN dump the pre-roll history to
        // every destination, and only then arm the drain (writersReady=true).
        // At worst the seam duplicates a sub-block, uniformly across all armed
        // channels (so no inter-track drift) -- strictly preferable to losing
        // audio at the boundary, which the old dump-then-arm order allowed.
        recording   .store (true,  std::memory_order_release);
        dumpPreRollToWriters();
        writersReady.store (true,  std::memory_order_release);
        return true;
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

    void MultitrackRecorder::setMirrors (const std::vector<MirrorConfig>& configs)
    {
        if (isRecording()) return;     // only editable when stopped
        mirrorConfigs = configs;
    }

    bool MultitrackRecorder::anyMirrorFailed() const noexcept
    {
        for (const auto& w : writers)
            for (const auto& m : w.mirrors)
                if (m.failed) return true;
        return false;
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

    juce::AudioFormatWriter* MultitrackRecorder::openWriterAtPath (const juce::File& target,
                                                                   int containerCode,
                                                                   int bits,
                                                                   int numChannels) noexcept
    {
        // numChannels: 1 = mono (the default, every standalone track),
        // 2 = interleaved stereo (a stereo pair captured to one file).
        const unsigned int chans = (unsigned int) juce::jmax (1, numChannels);

        target.deleteFile();
        auto* out = target.createOutputStream().release();
        if (out == nullptr) return nullptr;

        juce::WavAudioFormat  wav;
        juce::AiffAudioFormat aiff;
        juce::FlacAudioFormat flac;

        juce::StringPairArray meta;
        if (containerCode == 0)   // WAV gets the BWF bext chunk so mix
        {                         // engineers can identify the take.
            const auto now = juce::Time::getCurrentTime();
            meta.set (juce::WavAudioFormat::bwavDescription,
                      "Zynforge Recording -- " + activeSessionDir.getFileName()
                        + " -- " + target.getFileNameWithoutExtension());
            meta.set (juce::WavAudioFormat::bwavOriginator,      "Zynforge Recording");
            meta.set (juce::WavAudioFormat::bwavOriginatorRef,   activeSessionDir.getFileName());
            meta.set (juce::WavAudioFormat::bwavOriginationDate, now.formatted ("%Y-%m-%d"));
            meta.set (juce::WavAudioFormat::bwavOriginationTime, now.formatted ("%H:%M:%S"));
            meta.set (juce::WavAudioFormat::bwavTimeReference,   "0");
        }

        juce::AudioFormatWriter* w = nullptr;
        if      (containerCode == 2) w = flac.createWriterFor (out, sampleRate, chans, bits, meta, 5);
        else if (containerCode == 1) w = aiff.createWriterFor (out, sampleRate, chans, bits, meta, 0);
        else                         w = wav .createWriterFor (out, sampleRate, chans, bits, meta, 0);
        if (w == nullptr) delete out;
        return w;
    }

    void MultitrackRecorder::stopRecording()
    {
        if (! recording.exchange (false, std::memory_order_acq_rel))
            return;

        // Quiesce the writer threads BEFORE the final flush + close.
        // Each shard runs on its own TimeSliceThread draining the
        // single-consumer FIFOs and writing to the AudioFormatWriters.
        // If we flushed (drainOnce) and closed (writers.clear()) here
        // while those threads were still ticking, we got two consumers
        // on one FIFO and writers.clear() freeing an AudioFormatWriter
        // mid-writeFromFloatArrays -- which truncated files at random
        // per-channel lengths while their WAV headers kept the full
        // intended sample count (header says 24 s, data is 8 s).
        // removeTimeSliceClient blocks until any in-flight drain on the
        // shard returns and guarantees no further calls, so after this
        // loop the message thread is the sole owner of the FIFOs and
        // writers. (We are always on the message thread here, never on
        // a writer thread, so this can't deadlock.)
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());

        // Final flush of whatever is still buffered, now single-threaded.
        for (auto& sh : shards)
            drainShard (*sh);

        const auto stoppedAt        = juce::Time::getCurrentTime();
        const auto totalSamples     = samplesSinceStart.load (std::memory_order_relaxed);
        const auto totalMissed      = missedSamples    .load (std::memory_order_relaxed);
        const auto totalSeconds     = sampleRate > 0.0 ? (double) totalSamples / sampleRate : 0.0;
        const bool backupWasRunning = backupDir.isDirectory();
        const bool backupHadFailure = backupFailed.load (std::memory_order_relaxed);

        // Snapshot per-writer file-part info BEFORE closeWriters clears
        // the writers vector. The report needs this to enumerate the
        // Track_NN.wav + Track_NN_partXX.wav parts produced by the
        // auto-split logic.
        struct MirrorReport
        {
            juce::File   root;
            juce::int64  totalSamples;
            juce::StringArray partFiles;
            bool failed;
        };
        struct WriterReport
        {
            juce::int64 totalSamplesPrimary;
            juce::int64 totalSamplesBackup;
            juce::StringArray partFilesPrimary;
            juce::StringArray partFilesBackup;
            std::vector<MirrorReport> mirrors;
        };
        std::vector<WriterReport> writerSnapshots;
        writerSnapshots.reserve (writers.size());
        // Punch-in: the final on-disk paths of every copy (primary/backup/each
        // mirror), aligned with writerSnapshots, so we can splice each after the
        // writers close. Captured here while `writers` still exists.
        struct PunchWriterFiles { juce::File primary, backup; std::vector<juce::File> mirrors; };
        std::vector<PunchWriterFiles> punchFiles;
        const bool doPunch = punchInActive;
        for (auto& wc : writers)
        {
            WriterReport wr { wc.totalSamplesPrimary, wc.totalSamplesBackup,
                              wc.partFilesPrimary, wc.partFilesBackup, {} };
            for (size_t mi = 0; mi < wc.mirrors.size(); ++mi)
            {
                const auto& m = wc.mirrors[mi];
                const juce::File root = (mi < mirrorConfigs.size())
                                            ? mirrorConfigs[mi].root
                                            : juce::File();
                wr.mirrors.push_back ({ root, m.totalSamples, m.partFiles, m.failed });
            }
            writerSnapshots.push_back (std::move (wr));

            PunchWriterFiles pf;
            if (doPunch)
            {
                pf.primary = wc.primaryBaseFile.withFileExtension (wc.primaryExt);
                if (wc.backupBaseFile.getFullPathName().isNotEmpty())
                    pf.backup = wc.backupBaseFile.withFileExtension (wc.backupExt);
                for (auto& m : wc.mirrors)
                    pf.mirrors.push_back (m.baseFile.withFileExtension (m.ext));
            }
            punchFiles.push_back (std::move (pf));
        }

        closeWriters();
        backupActive.store (false, std::memory_order_relaxed);

        // Re-attach the shard clients to their writer threads so the next
        // take drains again (we detached them above to flush + close
        // single-threaded).
        rebuildShards();

        // ── Punch-in splice ────────────────────────────────────────────────
        // Each punched track recorded a clean fresh file; its existing take was
        // stashed to a sidecar in startRecording. Splice base[0,punchIn) +
        // freshTake + base[after] over each copy (temp + atomic swap). Runs
        // synchronously here -- AFTER the writers close, BEFORE the async SHA
        // thread below -- so the report hashes + lengths describe the spliced
        // files. On the rare splice failure the take reverts to its pre-punch
        // base (the original is never lost). Cleared so the next take is normal.
        if (doPunch)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            const auto punchPos = punchInPos;
            // Map a stashed sidecar back to its real take file:
            //   Track_NN.punchbase.ext      -> Track_NN.ext
            //   Track_NN_part02.punchbase.ext -> Track_NN_part02.ext
            auto sidecarToTake = [] (const juce::File& s) -> juce::File
            {
                const auto realStem = s.getFileNameWithoutExtension()
                                       .upToLastOccurrenceOf (".punchbase", false, false);
                return s.getParentDirectory().getChildFile (realStem + s.getFileExtension());
            };
            // One copy's (primary / backup / a mirror) splice, prepared into a
            // temp file but NOT yet committed. The splice for a track must be
            // ATOMIC ACROSS all its copies: if any copy fails, every copy rolls
            // back to its pre-punch base, so primary and backup never disagree
            // about whether the punch landed. So we split the work into a
            // PREPARE phase (splice each copy to its temp), then a single
            // commit-or-rollback decision for the whole track.
            struct CopySplice
            {
                juce::File              fresh;      // the freshly-recorded punch file
                juce::File              tmp;        // spliced result awaiting commit
                std::vector<juce::File> baseParts;  // stashed .punchbase sidecars
                bool                    attempted { false };  // this copy was stashed
                bool                    spliced   { false };  // splice to tmp succeeded
            };
            auto prepareCopy = [&] (const juce::File& fresh) -> CopySplice
            {
                CopySplice cs;
                cs.fresh = fresh;
                if (fresh.getFullPathName().isEmpty()) return cs;
                // Gather every stashed base part for this copy, in order:
                // Track_NN.punchbase + Track_NN_partXX.punchbase (continue takes
                // are multi-part). The splice reads them as one seamless base.
                const auto sidecar0 = punchSidecar (fresh);
                if (! sidecar0.existsAsFile()) return cs;    // this copy wasn't stashed
                cs.attempted = true;
                cs.baseParts.push_back (sidecar0);
                {
                    const auto dir  = fresh.getParentDirectory();
                    const auto stem = fresh.getFileNameWithoutExtension();   // "Track_07"
                    const auto ext  = fresh.getFileExtension();
                    for (int p = 2; ; ++p)
                    {
                        const auto part = dir.getChildFile (stem + "_part"
                                            + juce::String::formatted ("%02d", p) + ext);
                        const auto ps = punchSidecar (part);
                        if (ps.existsAsFile()) cs.baseParts.push_back (ps); else break;
                    }
                }
                cs.tmp = fresh.getSiblingFile (
                    fresh.getFileNameWithoutExtension() + ".punchtmp" + fresh.getFileExtension());
                cs.spliced = splicePunchParts (fm, cs.baseParts, fresh, punchPos, cs.tmp);
                return cs;
            };
            auto readLen = [&fm] (const juce::File& f) -> juce::int64
            {
                juce::int64 len = -1;
                if (auto* r = fm.createReaderFor (f)) { len = r->lengthInSamples; delete r; }
                return len;
            };

            for (size_t i = 0; i < punchFiles.size() && i < writerSnapshots.size(); ++i)
            {
                // Prepare a splice for every copy of this track: [primary,
                // backup, mirror0, mirror1, ...].
                std::vector<CopySplice> copies;
                copies.push_back (prepareCopy (punchFiles[i].primary));
                copies.push_back (prepareCopy (punchFiles[i].backup));
                for (auto& mf : punchFiles[i].mirrors)
                    copies.push_back (prepareCopy (mf));

                if (! copies[0].attempted) continue;    // primary not stashed -> not a punched track

                // Commit only if EVERY stashed copy spliced OK; otherwise roll
                // them ALL back to the pre-punch base so no two copies diverge.
                bool allOk = true;
                for (auto& c : copies)
                    if (c.attempted && ! c.spliced) allOk = false;

                for (auto& c : copies)
                {
                    if (! c.attempted) { c.tmp.deleteFile(); continue; }
                    if (allOk)
                    {
                        // Success: the spliced tmp holds the WHOLE flattened take.
                        c.fresh.deleteFile();
                        c.tmp.moveFileTo (c.fresh);
                        for (auto& s : c.baseParts) s.deleteFile();   // folded into `fresh`
                    }
                    else
                    {
                        // Roll back: discard the temp AND the fresh punch file,
                        // restore the original (possibly multi-part) base take.
                        c.tmp.deleteFile();
                        c.fresh.deleteFile();
                        for (auto& s : c.baseParts) s.moveFileTo (sidecarToTake (s));
                    }
                }

                // Report the resulting on-disk length of each copy (spliced
                // length on commit, base length on rollback).
                const auto primLen = readLen (copies[0].fresh);
                if (primLen > 0) writerSnapshots[i].totalSamplesPrimary = primLen;
                if (copies.size() > 1 && copies[1].attempted)
                {
                    const auto bl = readLen (copies[1].fresh);
                    if (bl > 0) writerSnapshots[i].totalSamplesBackup = bl;
                }
                for (size_t mi = 0; mi < punchFiles[i].mirrors.size()
                                    && mi < writerSnapshots[i].mirrors.size(); ++mi)
                {
                    const size_t ci = mi + 2;    // mirrors follow primary + backup
                    if (ci < copies.size() && copies[ci].attempted)
                    {
                        const auto ml = readLen (copies[ci].fresh);
                        if (ml > 0) writerSnapshots[i].mirrors[mi].totalSamples = ml;
                    }
                }
            }
        }
        punchInActive     = false;
        continueAsPart    = false;   // consume the continue arm
        recordBaseSamples = 0;

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
        struct TrackMeta
        {
            int          index;
            juce::String name;
            int          clipCount;
            juce::int64  lastClipSample;
            int          inputRouting, outputRouting;
            bool         isStereo;
        };
        std::vector<TrackMeta> trackMetas;
        trackMetas.reserve (tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            const auto& ts = *tracks[i];
            trackMetas.push_back ({ (int) i, ts.name,
                                    ts.clipCount.load (std::memory_order_relaxed),
                                    (juce::int64) ts.lastClipSample.load (std::memory_order_relaxed),
                                    ts.inputRouting .load (std::memory_order_relaxed),
                                    ts.outputRouting.load (std::memory_order_relaxed),
                                    ts.isStereo.load (std::memory_order_relaxed) });
        }

        const juce::File sessionDir = activeSessionDir;
        const juce::File bDir       = backupDir;
        const double     sr         = sampleRate;
        const int        fmt        = (int) captureFormat;
        const int        preRoll    = preRollSeconds;
        const bool       primFailed = primaryFailed.load (std::memory_order_relaxed);

        // Build the report JSON. `withHashes == false` skips the (expensive,
        // GB-scale) SHA-256 pass and marks the report "sha256Pending":true,
        // so we can write a complete metadata report INSTANTLY on stop and
        // fill the hashes in afterwards. Captures only scalars by value, so
        // it's copyable into the background thread below.
        auto buildReportJson =
            [sessionDir, bDir, sr, fmt, preRoll, primFailed, stoppedAt,
             totalSamples, totalSeconds, totalMissed, backupWasRunning,
             backupHadFailure]
            (const std::vector<TrackMeta>&   trackMetas,
             const std::vector<WriterReport>& writerSnaps,
             bool withHashes,
             const std::map<juce::String, juce::String>& shaByPath) -> juce::String
        {
            juce::DynamicObject::Ptr report (new juce::DynamicObject());
            report->setProperty ("stoppedAt",      stoppedAt.toISO8601 (true));
            report->setProperty ("sampleRate",     sr);
            report->setProperty ("numTracks",      (int) trackMetas.size());
            report->setProperty ("totalSamples",   (juce::int64) totalSamples);
            report->setProperty ("totalSeconds",   totalSeconds);
            report->setProperty ("missedSamples",  (juce::int64) totalMissed);
            report->setProperty ("backupActive",   backupWasRunning);
            report->setProperty ("backupFailed",   backupHadFailure);
            report->setProperty ("primaryFailed",  primFailed);
            report->setProperty ("captureFormat",  fmt);
            report->setProperty ("preRollSeconds", preRoll);
            report->setProperty ("sha256Pending",  ! withHashes);

            // Hashes are computed in parallel BEFORE this builder runs (see the
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
            return juce::JSON::toString (juce::var (report.get()), true);
        };

        if (sessionDir.isDirectory())
        {
            // 1) Write the metadata report SYNCHRONOUSLY, right now -- this
            //    guarantees session.report.json exists the instant recording
            //    stops, even for a huge take whose hashing runs for minutes
            //    (or if the app is killed before hashing finishes).
            writeTextAtomic (sessionDir.getChildFile ("session.report.json"),
                             buildReportJson (trackMetas, writerSnapshots, false, {}));

            // 2) Hash all recorded audio off-thread and rewrite the report with
            //    the SHA-256s. The hashing is (a) hardware-accelerated
            //    (CC_SHA256 -> ARMv8 crypto / SHA-NI, see FastHash.h) and
            //    (b) parallelised across files, so a multi-GB pass that used to
            //    take minutes finishes in seconds and is disk-bound, not
            //    CPU-bound. Runs at UTILITY QoS: still yields to the UI / next
            //    take, but isn't throttled to a trickle the way BACKGROUND was.
            //    If interrupted, the metadata report from step 1 still stands.
            juce::Thread::launch (
                [buildReportJson, sessionDir, bDir, backupWasRunning,
                 trackMetas  = std::move (trackMetas),
                 writerSnaps = std::move (writerSnapshots)]
                {
                   #if JUCE_MAC
                    pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
                   #endif

                    // Collect every file the manifest will reference (primary +
                    // backup + mirrors), mirroring buildReportJson's path logic.
                    std::vector<juce::File> files;
                    const auto primDir = sessionDir.getChildFile ("Audio Files");
                    const auto bkDir   = bDir.getChildFile (sessionDir.getFileName())
                                             .getChildFile ("Audio Files");
                    for (const auto& ws : writerSnaps)
                    {
                        for (const auto& fn : ws.partFilesPrimary) files.push_back (primDir.getChildFile (fn));
                        if (backupWasRunning)
                            for (const auto& fn : ws.partFilesBackup) files.push_back (bkDir.getChildFile (fn));
                        for (const auto& mr : ws.mirrors)
                        {
                            const auto mDir = mr.root.getChildFile (sessionDir.getFileName())
                                                     .getChildFile ("Audio Files");
                            for (const auto& fn : mr.partFiles) files.push_back (mDir.getChildFile (fn));
                        }
                    }

                    // Hash them in parallel (bounded), then build the path->sha map.
                    std::map<juce::String, juce::String> shaByPath;
                    std::mutex mapMutex;
                    std::atomic<size_t> next { 0 };
                    const int nThreads = (int) juce::jlimit (1u, 8u,
                                            std::max (1u, std::thread::hardware_concurrency()));
                    std::vector<std::thread> pool;
                    for (int t = 0; t < nThreads; ++t)
                        pool.emplace_back ([&]
                        {
                            for (;;)
                            {
                                const size_t i = next.fetch_add (1);
                                if (i >= files.size()) break;
                                const auto sha  = zynforge::hashing::fileSha256 (files[i]);
                                const auto path = files[i].getFullPathName();
                                const std::lock_guard<std::mutex> lk (mapMutex);
                                shaByPath[path] = sha;
                            }
                        });
                    for (auto& th : pool) th.join();

                    writeTextAtomic (sessionDir.getChildFile ("session.report.json"),
                                     buildReportJson (trackMetas, writerSnaps, true, shaByPath));
                });
        }

        // Clean stop -- remove the recovery marker.
        if (activeSessionDir.isDirectory())
            activeSessionDir.getChildFile ("recording.session").deleteFile();
    }

    void MultitrackRecorder::closeWriters()
    {
        writersReady.store (false, std::memory_order_release);
        writers.clear();
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

    juce::int64 MultitrackRecorder::estimateBytesPerSecondForArmedTracks() const noexcept
    {
        // bytesPerSec for a single armed mono track = sampleRate * bitsPerSample / 8.
        auto bitsFor = [] (CaptureFormat f) -> int
        {
            switch (f)
            {
                case CaptureFormat::Wav16:  case CaptureFormat::Aiff16: case CaptureFormat::Flac16: return 16;
                case CaptureFormat::Wav32Float: case CaptureFormat::Aiff32Float:                    return 32;
                default: return 24;
            }
        };
        const int bytesPerSampPrimary = bitsFor (captureFormat)       / 8;
        const int bytesPerSampBackup  = bitsFor (backupCaptureFormat) / 8;
        const bool backupOn = backupDir.isDirectory();

        int armedCount = 0;
        for (const auto& t : tracks)
            if (t->armed.load (std::memory_order_relaxed)
                && ! t->isBus.load (std::memory_order_relaxed))
                ++armedCount;

        // Every configured MIRROR is another full parallel copy of every armed
        // track, in its own format. Omitting them under-reported the real write
        // rate by the whole mirror count -- which made "minutes remaining"
        // optimistic and raised the bar isDiskStruggling compares against, so
        // the disk warning fired late (or not at all) on a mirrored rig.
        int bytesPerSampMirrors = 0;
        for (const auto& mc : mirrorConfigs)
            bytesPerSampMirrors += bitsFor (mc.format) / 8;

        // FLAC compresses ~50% typical; treat its byte rate as the
        // worst case (uncompressed equivalent) to keep the estimate
        // pessimistic. Engineers don't want a "you have 6 h" estimate
        // that turns into 3 h because today's audio compressed poorly.
        return (juce::int64) (sampleRate
                              * (juce::int64) armedCount
                              * (bytesPerSampPrimary
                                 + (backupOn ? bytesPerSampBackup : 0)
                                 + bytesPerSampMirrors));
    }

    int MultitrackRecorder::estimateMinutesRemaining (const juce::File& primaryVolume,
                                                      const juce::File& backupVolume) const noexcept
    {
        const auto rate = estimateBytesPerSecondForArmedTracks();
        if (rate <= 0) return 0;
        constexpr int kEffectivelyUnbounded = std::numeric_limits<int>::max() / 2;

        auto minutesForVolume = [&] (const juce::File& vol) -> int
        {
            if (! vol.exists()) return kEffectivelyUnbounded;
            const auto free = vol.getBytesFreeOnVolume();
            if (free <= 0) return kEffectivelyUnbounded;       // unknown -> don't pessimize
            return (int) (free / (rate * (juce::int64) 60));
        };
        int minutes = minutesForVolume (primaryVolume);
        if (backupVolume.exists() && backupDir.isDirectory())
            minutes = juce::jmin (minutes, minutesForVolume (backupVolume));
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

    void MultitrackRecorder::flushOpenWriters (std::size_t first, std::size_t last)
    {
        // Rewrite each open writer's header in place. JUCE's flush() seeks
        // back to the header, writes it (RIFF, or RF64 once bytesWritten
        // crosses 4 GiB -- the ds64 slot was reserved at creation), then
        // seeks back to the data end to keep appending. Runs on the writer
        // thread, never the audio thread.
        for (std::size_t i = first; i < last && i < writers.size(); ++i)
        {
            auto& w = writers[i];
            if (w.writer       != nullptr) w.writer->flush();
            if (w.backupWriter != nullptr) w.backupWriter->flush();
            for (auto& m : w.mirrors)
                if (m.writer != nullptr && ! m.failed) m.writer->flush();
        }
    }

    void MultitrackRecorder::drainShard (ShardClient& shard)
    {
        if (! writersReady.load (std::memory_order_acquire)) return;

        // One drainer per shard at a time: the shard's TimeSliceThread and a
        // fan-out drainOnce() (stopRecording flush / drainPendingForTests)
        // must not read the single-reader FIFO concurrently. Consumer-side
        // only -- never contends the audio thread.
        const juce::ScopedLock sl (shard.drainLock);

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        juce::int64 totalWritten = 0;
        int         worstFillPct = 0;

        const std::size_t first = (std::size_t) juce::jmax (0, shard.firstChannel);
        const std::size_t last  = (std::size_t) juce::jmax (0, shard.lastChannel);

        for (std::size_t i = first;
             i < last && i < fifos.size() && i < writers.size();
             ++i)
        {
            auto& cf = *fifos[i];
            auto& w  = writers[i];
            // Skip only channels that don't participate in the take (empty
            // stereo-R slots, unarmed/bus). A participating channel with a
            // failed (nulled) primary MUST still drain so its backup + mirrors
            // keep capturing and its FIFO doesn't overflow -- the failover
            // guarantee. Each write below is already individually null-guarded.
            if (! w.active) continue;

            // Track FIFO occupancy so the dashboard can warn the engineer
            // BEFORE samples are dropped -- at 80%+ the disk is falling
            // behind the audio thread.
            const int cap = (int) cf.data.size();
            if (cap > 0)
            {
                const int pct = (cf.fifo.getNumReady() * 100) / cap;
                if (pct > worstFillPct) worstFillPct = pct;
            }

            // Roll a writer over to the next part file when it's about
            // to cross its format's chunk-size ceiling. Closing the
            // current writer (.reset()) finalises the header so the
            // file we're leaving behind is valid; opening the next
            // part keeps recording continuous from the engineer's POV.
            auto rollIfNeeded = [this] (WriterChannel& wc, int samplesPending) noexcept
            {
                // Byte projection counts ALL channels in the file: a stereo
                // pair's interleaved 2-ch file grows twice as fast per frame,
                // so it must roll (or promote to RF64) at the same wall-clock
                // size as two mono files would.
                const juce::int64 chans = (juce::int64) juce::jmax (1, wc.numChannels);
                if (wc.writer != nullptr)
                {
                    const juce::int64 projected = wc.bytesWrittenPrimary
                        + (juce::int64) samplesPending * chans * wc.bytesPerSamplePrimary;
                    if (projected >= maxBytesForContainer (wc.primaryContainer))
                    {
                        wc.writer.reset();   // closes + finalises header
                        ++wc.partNumberPrimary;
                        const auto nextFile = wc.primaryBaseFile.getParentDirectory()
                            .getChildFile (wc.primaryBaseFile.getFileName()
                                           + "_part"
                                           + juce::String::formatted ("%02d", wc.partNumberPrimary)
                                           + wc.primaryExt);
                        wc.writer.reset (openWriterAtPath (nextFile, wc.primaryContainer,
                                                           wc.primaryBitDepth, wc.numChannels));
                        wc.bytesWrittenPrimary = 0;
                        if (wc.writer != nullptr)
                            wc.partFilesPrimary.add (nextFile.getFileName());
                        else
                            primaryFailed.store (true, std::memory_order_relaxed);   // next part wouldn't open (disk full at ceiling)
                    }
                }
                if (wc.backupWriter != nullptr)
                {
                    const juce::int64 projected = wc.bytesWrittenBackup
                        + (juce::int64) samplesPending * chans * wc.bytesPerSampleBackup;
                    if (projected >= maxBytesForContainer (wc.backupContainer))
                    {
                        wc.backupWriter.reset();
                        ++wc.partNumberBackup;
                        const auto nextFile = wc.backupBaseFile.getParentDirectory()
                            .getChildFile (wc.backupBaseFile.getFileName()
                                           + "_part"
                                           + juce::String::formatted ("%02d", wc.partNumberBackup)
                                           + wc.backupExt);
                        wc.backupWriter.reset (openWriterAtPath (nextFile, wc.backupContainer,
                                                                  wc.backupBitDepth, wc.numChannels));
                        wc.bytesWrittenBackup = 0;
                        if (wc.backupWriter != nullptr)
                            wc.partFilesBackup.add (nextFile.getFileName());
                        else
                            backupFailed.store (true, std::memory_order_relaxed);
                    }
                }
                // Mirror writers -- same per-destination roll logic.
                for (auto& m : wc.mirrors)
                {
                    if (m.writer == nullptr || m.failed) continue;
                    const juce::int64 projected = m.bytesWritten
                        + (juce::int64) samplesPending * chans * m.bytesPerSample;
                    if (projected >= maxBytesForContainer (m.container))
                    {
                        m.writer.reset();
                        ++m.partNumber;
                        const auto nextFile = m.baseFile.getParentDirectory()
                            .getChildFile (m.baseFile.getFileName()
                                           + "_part"
                                           + juce::String::formatted ("%02d", m.partNumber)
                                           + m.ext);
                        m.writer.reset (openWriterAtPath (nextFile, m.container, m.bitDepth, wc.numChannels));
                        m.bytesWritten = 0;
                        if (m.writer != nullptr)
                            m.partFiles.add (nextFile.getFileName());
                        else
                            m.failed = true;   // next mirror part wouldn't open
                    }
                }
            };

            // Length-preserving overflow compensation. When the audio thread
            // had to drop samples on a full FIFO it counted them per-channel;
            // here we write that many silence frames to EVERY destination so
            // this track's file advances by the same total sample count as
            // every other track. Without this, different channels lose
            // different counts and the take develops permanent inter-track
            // sync drift (not merely a glitch). Runs on the writer thread, so
            // the reusable zero buffer + writes are fine here.
            auto writeSilencePad = [&] (WriterChannel& wc, juce::int64 frames) noexcept
            {
                const int chans = juce::jmax (1, wc.numChannels);
                while (frames > 0)
                {
                    const int chunk = (int) juce::jmin ((juce::int64) 8192, frames);
                    frames -= chunk;
                    if ((int) shard.silence.size() < chunk)
                        shard.silence.assign ((std::size_t) chunk, 0.0f);  // grows once, stays all-zero
                    const float* z = shard.silence.data();
                    const float* const arr[] = { z, z };

                    rollIfNeeded (wc, chunk);
                    if (wc.writer != nullptr)
                    {
                        if (! wc.writer->writeFromFloatArrays (arr, chans, chunk))
                        {
                            wc.writer.reset();
                            primaryFailed.store (true, std::memory_order_relaxed);
                        }
                        else
                        {
                            wc.bytesWrittenPrimary += (juce::int64) chunk * chans * wc.bytesPerSamplePrimary;
                            wc.totalSamplesPrimary += chunk;
                        }
                    }
                    if (wc.backupWriter != nullptr)
                    {
                        if (! wc.backupWriter->writeFromFloatArrays (arr, chans, chunk))
                        {
                            wc.backupWriter.reset();
                            backupFailed.store (true, std::memory_order_relaxed);
                        }
                        else
                        {
                            wc.bytesWrittenBackup += (juce::int64) chunk * chans * wc.bytesPerSampleBackup;
                            wc.totalSamplesBackup += chunk;
                        }
                    }
                    for (auto& m : wc.mirrors)
                    {
                        if (m.writer == nullptr || m.failed) continue;
                        if (! m.writer->writeFromFloatArrays (arr, chans, chunk))
                        {
                            m.writer.reset();
                            m.failed = true;
                            continue;
                        }
                        m.bytesWritten += (juce::int64) chunk * chans * m.bytesPerSample;
                        m.totalSamples += chunk;
                    }
                    totalWritten += chunk;
                }
            };

            // ── Interleaved stereo pair ──────────────────────────────────
            // A stereo-L writer (numChannels == 2) owns BOTH this FIFO and
            // the R partner's (i + 1). Drain the SAME number of frames from
            // each -- min(ready) so neither runs ahead -- gather them into
            // two contiguous planar staging buffers (the FIFO read may wrap),
            // then do ONE 2-channel write. The R partner's WriterChannel is
            // empty (writer == nullptr), so it's skipped on its own iteration
            // and its FIFO is consumed only here -- never left to overflow.
            if (w.numChannels == 2 && i + 1 < fifos.size())
            {
                auto& cfR = *fifos[i + 1];
                const int avail = juce::jmin (cf.fifo.getNumReady(), cfR.fifo.getNumReady());
                if (avail > 0)
                {
                    const auto scopeL = cf .fifo.read (avail);
                    const auto scopeR = cfR.fifo.read (avail);

                    // Copy a (possibly wrapped) ScopedRead into a contiguous array.
                    auto gather = [avail] (std::vector<float>& dst, const ChannelFifo& src,
                                           const auto& sc) noexcept
                    {
                        dst.resize ((std::size_t) avail);
                        if (sc.blockSize1 > 0)
                            std::memcpy (dst.data(),
                                         src.data.data() + sc.startIndex1,
                                         (std::size_t) sc.blockSize1 * sizeof (float));
                        if (sc.blockSize2 > 0)
                            std::memcpy (dst.data() + sc.blockSize1,
                                         src.data.data() + sc.startIndex2,
                                         (std::size_t) sc.blockSize2 * sizeof (float));
                    };
                    gather (shard.stageL, cf,  scopeL);
                    gather (shard.stageR, cfR, scopeR);

                    rollIfNeeded (w, avail);
                    const float* const channels[] = { shard.stageL.data(), shard.stageR.data() };

                    if (w.writer != nullptr)
                    {
                        if (! w.writer->writeFromFloatArrays (channels, 2, avail))
                        {
                            w.writer.reset();
                            primaryFailed.store (true, std::memory_order_relaxed);
                        }
                        else
                        {
                            w.bytesWrittenPrimary += (juce::int64) avail * 2 * w.bytesPerSamplePrimary;
                            w.totalSamplesPrimary += avail;
                        }
                    }
                    if (w.backupWriter != nullptr
                        && ! w.backupWriter->writeFromFloatArrays (channels, 2, avail))
                    {
                        w.backupWriter.reset();
                        backupFailed.store (true, std::memory_order_relaxed);
                    }
                    else if (w.backupWriter != nullptr)
                    {
                        w.bytesWrittenBackup += (juce::int64) avail * 2 * w.bytesPerSampleBackup;
                        w.totalSamplesBackup += avail;
                    }
                    for (auto& m : w.mirrors)
                    {
                        if (m.writer == nullptr || m.failed) continue;
                        if (! m.writer->writeFromFloatArrays (channels, 2, avail))
                        {
                            m.writer.reset();
                            m.failed = true;
                            continue;
                        }
                        m.bytesWritten += (juce::int64) avail * 2 * m.bytesPerSample;
                        m.totalSamples += avail;
                    }
                    totalWritten += avail;
                }

                // Compensate any per-side FIFO overflow with silence so the
                // pair's file stays aligned with every other track. Both sides
                // are consumed here (padMax keeps the interleaved frame count
                // uniform across all tracks); consume both counters either way.
                const juce::int64 padL = cf .droppedSamples.exchange (0, std::memory_order_relaxed);
                const juce::int64 padR = cfR.droppedSamples.exchange (0, std::memory_order_relaxed);
                writeSilencePad (w, juce::jmax (padL, padR));
                continue;
            }

            const int available = cf.fifo.getNumReady();
            if (available > 0)
            {
            const auto scope = cf.fifo.read (available);

            if (scope.blockSize1 > 0)
            {
                rollIfNeeded (w, scope.blockSize1);
                const float* ptr = cf.data.data() + scope.startIndex1;
                const float* const channels[] = { ptr };
                if (w.writer != nullptr)
                {
                    if (! w.writer->writeFromFloatArrays (channels, 1, scope.blockSize1))
                    {
                        // Primary write failed mid-take (disk full,
                        // path disappeared, permissions). Close the
                        // writer so we don't keep hammering a bad
                        // handle, flip the failure flag so UI + report
                        // surface it, and let backup + mirrors keep
                        // capturing without interruption.
                        w.writer.reset();
                        primaryFailed.store (true, std::memory_order_relaxed);
                    }
                    else
                    {
                        w.bytesWrittenPrimary += (juce::int64) scope.blockSize1 * w.bytesPerSamplePrimary;
                        w.totalSamplesPrimary += scope.blockSize1;
                    }
                }
                if (w.backupWriter != nullptr
                    && ! w.backupWriter->writeFromFloatArrays (channels, 1, scope.blockSize1))
                {
                    w.backupWriter.reset();
                    backupFailed.store (true, std::memory_order_relaxed);
                }
                else if (w.backupWriter != nullptr)
                {
                    w.bytesWrittenBackup += (juce::int64) scope.blockSize1 * w.bytesPerSampleBackup;
                    w.totalSamplesBackup += scope.blockSize1;
                }
                for (auto& m : w.mirrors)
                {
                    if (m.writer == nullptr || m.failed) continue;
                    if (! m.writer->writeFromFloatArrays (channels, 1, scope.blockSize1))
                    {
                        m.writer.reset();
                        m.failed = true;
                        continue;
                    }
                    m.bytesWritten += (juce::int64) scope.blockSize1 * m.bytesPerSample;
                    m.totalSamples += scope.blockSize1;
                }
                totalWritten += scope.blockSize1;
            }
            if (scope.blockSize2 > 0)
            {
                rollIfNeeded (w, scope.blockSize2);
                const float* ptr = cf.data.data() + scope.startIndex2;
                const float* const channels[] = { ptr };
                if (w.writer != nullptr)
                {
                    if (! w.writer->writeFromFloatArrays (channels, 1, scope.blockSize2))
                    {
                        w.writer.reset();
                        primaryFailed.store (true, std::memory_order_relaxed);
                    }
                    else
                    {
                        w.bytesWrittenPrimary += (juce::int64) scope.blockSize2 * w.bytesPerSamplePrimary;
                        w.totalSamplesPrimary += scope.blockSize2;
                    }
                }
                if (w.backupWriter != nullptr
                    && ! w.backupWriter->writeFromFloatArrays (channels, 1, scope.blockSize2))
                {
                    w.backupWriter.reset();
                    backupFailed.store (true, std::memory_order_relaxed);
                }
                else if (w.backupWriter != nullptr)
                {
                    w.bytesWrittenBackup += (juce::int64) scope.blockSize2 * w.bytesPerSampleBackup;
                    w.totalSamplesBackup += scope.blockSize2;
                }
                for (auto& m : w.mirrors)
                {
                    if (m.writer == nullptr || m.failed) continue;
                    if (! m.writer->writeFromFloatArrays (channels, 1, scope.blockSize2))
                    {
                        m.writer.reset();
                        m.failed = true;
                        continue;
                    }
                    m.bytesWritten += (juce::int64) scope.blockSize2 * m.bytesPerSample;
                    m.totalSamples += scope.blockSize2;
                }
                totalWritten += scope.blockSize2;
            }
            }

            // Compensate this channel's FIFO overflow with an equal count of
            // silence so its file stays length-aligned with every other track.
            writeSilencePad (w, cf.droppedSamples.exchange (0, std::memory_order_relaxed));
        }

        // Periodic header flush (~every 5 s). Keeps each open file self-
        // describing so a crash mid-take leaves a readable recording (header
        // valid up to the last flush) instead of one with a stale/zero-length
        // header -- and promotes a WAV past 4 GiB to RF64 on the way. Cheap: a
        // seek + small header write per writer, on the writer thread.
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            const double lastF = lastWriterFlushMs.load (std::memory_order_relaxed);
            if (nowMs - lastF >= 5000.0)
            {
                lastWriterFlushMs.store (nowMs, std::memory_order_relaxed);
                flushOpenWriters ((std::size_t) juce::jmax (0, shard.firstChannel),
                                  (std::size_t) juce::jmax (0, shard.lastChannel));
            }
        }

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

        // Disk throughput: count bytes written in a rolling 1 s window.
        // bitsPerSample is derived from the active capture format below.
        auto bitsForFormat = [] (CaptureFormat f) -> int
        {
            switch (f)
            {
                case CaptureFormat::Wav16:  case CaptureFormat::Aiff16: case CaptureFormat::Flac16: return 16;
                case CaptureFormat::Wav32Float: case CaptureFormat::Aiff32Float:                   return 32;
                default: return 24;
            }
        };
        const int  bytesPerSampPrimary = bitsForFormat (captureFormat)       / 8;
        const int  bytesPerSampBackup  = bitsForFormat (backupCaptureFormat) / 8;
        const bool backupActiveNow     = backupActive.load (std::memory_order_relaxed) && ! backupFailed.load (std::memory_order_relaxed);
        const juce::int64 bytesThisDrain = totalWritten *
                                           (bytesPerSampPrimary + (backupActiveNow ? bytesPerSampBackup : 0));
        shard.throughputAccumBytes += bytesThisDrain;

        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (shard.throughputWindowMs <= 0.0) shard.throughputWindowMs = now;
        const double dtMs = now - shard.throughputWindowMs;
        if (dtMs >= 250.0)
        {
            const double bps = (dtMs > 0.0)
                                  ? ((double) shard.throughputAccumBytes * 1000.0 / dtMs)
                                  : 0.0;
            shard.shardBytesPerSec.store ((float) bps, std::memory_order_relaxed);
            shard.throughputAccumBytes = 0;
            shard.throughputWindowMs   = now;
        }

        if (totalWritten > 0)
        {
            const auto elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
            lastWriteMs.store ((int) elapsed, std::memory_order_relaxed);
        }
        // NOTE: the periodic header flush is the wall-clock block ABOVE
        // (lastWriterFlushMs). A second, audio-time flush used to live here
        // doing the same job on the same ~5 s cadence -- and it iterated EVERY
        // writer rather than this shard's range, so with more than one shard
        // each thread flushed other shards' writers behind their backs. One
        // timer, scoped to the shard, is the whole job.
    }
}
