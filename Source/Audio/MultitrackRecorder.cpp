#include "MultitrackRecorder.h"

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
        const int hw = juce::SystemStats::getNumPhysicalCpus();
        return juce::jlimit (2, 8, hw > 0 ? hw / 2 : 4);
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
        // Detach existing shards from their threads — they'll be rebuilt.
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());
        shards.clear();

        sampleRate = sr;
        blockSize  = maxBlock;

        const int fifoSize = juce::nextPowerOfTwo ((int) (sr * kFifoSeconds));
        scratch.assign ((std::size_t) maxBlock, 0.0f);

        // Preserve existing TrackState objects when the count hasn't
        // changed — destroying them here would leave every ChannelStrip
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
                t->name = juce::String (i + 1);
                tracks.push_back (std::move (t));

                auto f = std::make_unique<ChannelFifo>();
                f->resize (fifoSize);
                fifos.push_back (std::move (f));

                preRoll.push_back (std::make_unique<PreRollBuffer>());
            }
        }
        else
        {
            // Same track count — just resize the existing FIFOs to the
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
        // Detach + recreate. We never resize the thread pool itself —
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
        t->name = juce::String ((int) tracks.size() + 1);
        tracks.push_back (std::move (t));

        auto f = std::make_unique<ChannelFifo>();
        f->resize (fifoSize);
        fifos.push_back (std::move (f));

        preRoll.push_back (std::make_unique<PreRollBuffer>());
        allocatePreRollBuffers();
        rebuildShards();
    }

    void MultitrackRecorder::removeLastTrack()
    {
        if (isRecording() || tracks.empty()) return;
        tracks.pop_back();
        fifos.pop_back();
        preRoll.pop_back();
        rebuildShards();
    }

    void MultitrackRecorder::removeTrackAt (int index)
    {
        if (isRecording()) return;
        if (index < 0 || index >= (int) tracks.size()) return;
        tracks.erase (tracks.begin() + index);
        fifos.erase  (fifos.begin()  + index);
        preRoll.erase(preRoll.begin()+ index);
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

        for (std::size_t i = 0; i < writers.size() && i < preRoll.size(); ++i)
        {
            if (writers[i].writer == nullptr) continue;
            const int got = preRoll[i]->readHistory (tmp.data(), wanted);
            if (got <= 0) continue;
            const float* const arr[] = { tmp.data() };
            writers[i].writer->writeFromFloatArrays (arr, 1, got);
        }
    }

    void MultitrackRecorder::release()
    {
        stopRecording();
        for (auto& sh : shards)
            for (auto& th : writerThreads)
                th->removeTimeSliceClient (sh.get());
        shards.clear();
        tracks.clear();
        fifos.clear();
        preRoll.clear();
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

            // Arm gate: a strip whose REC button is OFF doesn't let input
            // enter the software. Meter stays silent (decays cleanly so a
            // previously-hot meter doesn't latch), FFT FIFO is left alone,
            // and the FIFO push below is skipped too. Engineer arms a
            // channel when they actually want to see / record it.
            if (! t.armed.load (std::memory_order_relaxed))
            {
                const float prevPeak = t.peak.load (std::memory_order_relaxed);
                const float prevRms  = t.rms .load (std::memory_order_relaxed);
                t.peak.store (prevPeak * 0.92f, std::memory_order_relaxed);
                t.rms .store (prevRms  * 0.85f, std::memory_order_relaxed);
                continue;
            }

            const float* src = inputs[ch];
            if (src == nullptr) continue;

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

            // Push to ring buffer when recording
            if (rec && t.armed.load (std::memory_order_relaxed))
            {
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
                    missedSamples.fetch_add (numSamples - wrote, std::memory_order_relaxed);
            }
        }

        if (rec)
            samplesSinceStart.fetch_add (numSamples, std::memory_order_relaxed);
    }

    bool MultitrackRecorder::startRecording (const juce::File& sessionDir)
    {
        if (recording.load()) return false;
        if (tracks.empty())   return false;

        sessionDir.createDirectory();
        activeSessionDir = sessionDir;

        // Pro Tools-style folder layout: tracks live under "Audio Files/",
        // mixdowns under "Bounced Files/", backups under
        // "Session File Backups/". Session-level metadata (recording.session,
        // session.report.json, the .zfproj document) stays at the root.
        const auto audioFilesDir = sessionDir.getChildFile ("Audio Files");
        audioFilesDir.createDirectory();
        sessionDir.getChildFile ("Bounced Files")       .createDirectory();
        sessionDir.getChildFile ("Clip Groups")         .createDirectory();
        sessionDir.getChildFile ("Session File Backups").createDirectory();
        sessionDir.getChildFile ("Video Files")         .createDirectory();

        writers.clear();
        writers.reserve (tracks.size());

        juce::WavAudioFormat  wav;
        juce::AiffAudioFormat aiff;
        juce::FlacAudioFormat flac;

        // Resolves a CaptureFormat to its container + bit-depth + file
        // extension so primary and backup writers can use different
        // formats and the rest of the open-writer code stays uniform.
        enum class Container { Wav, Aiff, Flac };
        auto resolve = [] (CaptureFormat f)
        {
            struct Resolved { Container container; int bitDepth; const char* ext; };
            switch (f)
            {
                case CaptureFormat::Wav16:       return Resolved { Container::Wav,  16, ".wav" };
                case CaptureFormat::Wav24:       return Resolved { Container::Wav,  24, ".wav" };
                case CaptureFormat::Wav32Float:  return Resolved { Container::Wav,  32, ".wav" };
                case CaptureFormat::Aiff16:      return Resolved { Container::Aiff, 16, ".aif" };
                case CaptureFormat::Aiff24:      return Resolved { Container::Aiff, 24, ".aif" };
                case CaptureFormat::Aiff32Float: return Resolved { Container::Aiff, 32, ".aif" };
                case CaptureFormat::Flac16:      return Resolved { Container::Flac, 16, ".flac" };
                case CaptureFormat::Flac24:      return Resolved { Container::Flac, 24, ".flac" };
            }
            return Resolved { Container::Wav, 24, ".wav" };
        };
        const auto primary = resolve (captureFormat);
        const auto backup  = resolve (backupCaptureFormat);

        const auto now = juce::Time::getCurrentTime();

        auto openWriter = [&] (const juce::File& target, Container c, int bits) -> juce::AudioFormatWriter*
        {
            target.deleteFile();
            auto* out = target.createOutputStream().release();
            if (out == nullptr) return nullptr;

            juce::StringPairArray meta;
            if (c == Container::Wav)
            {
                meta.set (juce::WavAudioFormat::bwavDescription,
                          "Zynforge Recording — " + sessionDir.getFileName()
                            + " — " + target.getFileNameWithoutExtension());
                meta.set (juce::WavAudioFormat::bwavOriginator,      "Zynforge Recording");
                meta.set (juce::WavAudioFormat::bwavOriginatorRef,   sessionDir.getFileName());
                meta.set (juce::WavAudioFormat::bwavOriginationDate, now.formatted ("%Y-%m-%d"));
                meta.set (juce::WavAudioFormat::bwavOriginationTime, now.formatted ("%H:%M:%S"));
                meta.set (juce::WavAudioFormat::bwavTimeReference,   "0");
            }

            juce::AudioFormatWriter* w = nullptr;
            if      (c == Container::Flac) w = flac.createWriterFor (out, sampleRate, 1, bits, meta, 5);
            else if (c == Container::Aiff) w = aiff.createWriterFor (out, sampleRate, 1, bits, meta, 0);
            else                           w = wav .createWriterFor (out, sampleRate, 1, bits, meta, 0);
            if (w == nullptr) delete out;
            return w;
        };

        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            WriterChannel w;
            const auto trackName = juce::String::formatted ("Track_%02d", (int) i + 1);

            // Bus tracks have no input — no writer. Push an empty
            // WriterChannel so the per-channel index stays aligned with
            // tracks[i]; processBlock's arm-gate already skips bus
            // tracks (they have armed=false).
            if (tracks[i]->isBus.load (std::memory_order_relaxed))
            {
                writers.push_back (std::move (w));
                fifos[i]->fifo.reset();
                continue;
            }

            // Primary writer — under <session>/Audio Files/Track_NN.<ext>.
            const auto primaryFile = audioFilesDir.getChildFile (trackName + primary.ext);
            w.writer.reset (openWriter (primaryFile, primary.container, primary.bitDepth));

            // Optional second copy — may be in a different format from
            // the primary, so the engineer can run e.g. WAV/24 to the
            // main drive AND FLAC/24 to the backup drive simultaneously.
            // Backups mirror the same Audio Files/ layout under the chosen
            // backup root.
            if (backupDir.isDirectory())
            {
                auto backupSession = backupDir.getChildFile (sessionDir.getFileName())
                                              .getChildFile ("Audio Files");
                backupSession.createDirectory();
                const auto backupFile = backupSession.getChildFile (trackName + backup.ext);
                w.backupWriter.reset (openWriter (backupFile, backup.container, backup.bitDepth));
            }

            writers.push_back (std::move (w));

            fifos[i]->fifo.reset();
        }

        backupActive.store (backupDir.isDirectory(), std::memory_order_relaxed);
        backupFailed.store (false, std::memory_order_relaxed);

        // Recovery marker — deleted on clean stop.
        {
            juce::DynamicObject::Ptr m (new juce::DynamicObject());
            m->setProperty ("startedAt",  now.toISO8601 (true));
            m->setProperty ("sampleRate", sampleRate);
            m->setProperty ("numTracks",  (int) tracks.size());
            sessionDir.getChildFile ("recording.session")
                      .replaceWithText (juce::JSON::toString (juce::var (m.get())));
        }

        samplesSinceStart.store (0, std::memory_order_relaxed);
        missedSamples    .store (0, std::memory_order_relaxed);
        samplesSinceFlush = 0;

        // Pre-roll: dump history into each writer BEFORE enabling live capture.
        dumpPreRollToWriters();

        writersReady.store (true,  std::memory_order_release);
        recording   .store (true,  std::memory_order_release);
        return true;
    }

    void MultitrackRecorder::stopRecording()
    {
        if (! recording.exchange (false, std::memory_order_acq_rel))
            return;

        // Drain remaining samples before closing.
        for (int i = 0; i < 8; ++i) drainOnce();

        const auto stoppedAt        = juce::Time::getCurrentTime();
        const auto totalSamples     = samplesSinceStart.load (std::memory_order_relaxed);
        const auto totalMissed      = missedSamples    .load (std::memory_order_relaxed);
        const auto totalSeconds     = sampleRate > 0.0 ? (double) totalSamples / sampleRate : 0.0;
        const bool backupWasRunning = backupDir.isDirectory();
        const bool backupHadFailure = backupFailed.load (std::memory_order_relaxed);

        closeWriters();
        backupActive.store (false, std::memory_order_relaxed);

        // Post-show JSON report — one file per session that captures every
        // datum a mix engineer / producer needs after the gig: total time,
        // per-track clip count, missed samples, backup status. Drop-in
        // alongside the WAVs in the session directory.
        if (activeSessionDir.isDirectory())
        {
            juce::DynamicObject::Ptr report (new juce::DynamicObject());
            report->setProperty ("stoppedAt",      stoppedAt.toISO8601 (true));
            report->setProperty ("sampleRate",     sampleRate);
            report->setProperty ("numTracks",      (int) tracks.size());
            report->setProperty ("totalSamples",   (juce::int64) totalSamples);
            report->setProperty ("totalSeconds",   totalSeconds);
            report->setProperty ("missedSamples",  (juce::int64) totalMissed);
            report->setProperty ("backupActive",   backupWasRunning);
            report->setProperty ("backupFailed",   backupHadFailure);
            report->setProperty ("captureFormat",  (int) captureFormat);
            report->setProperty ("preRollSeconds", preRollSeconds);

            juce::Array<juce::var> trackArray;
            for (std::size_t i = 0; i < tracks.size(); ++i)
            {
                juce::DynamicObject::Ptr t (new juce::DynamicObject());
                const auto& ts = *tracks[i];
                t->setProperty ("index",          (int) i);
                t->setProperty ("name",           ts.name);
                t->setProperty ("clipCount",      ts.clipCount.load (std::memory_order_relaxed));
                t->setProperty ("lastClipSample",
                                (juce::int64) ts.lastClipSample.load (std::memory_order_relaxed));
                t->setProperty ("inputRouting",   ts.inputRouting .load (std::memory_order_relaxed));
                t->setProperty ("outputRouting",  ts.outputRouting.load (std::memory_order_relaxed));
                t->setProperty ("isStereo",       ts.isStereo.load (std::memory_order_relaxed));
                trackArray.add (juce::var (t.get()));
            }
            report->setProperty ("tracks", juce::var (trackArray));

            activeSessionDir.getChildFile ("session.report.json")
                            .replaceWithText (juce::JSON::toString (juce::var (report.get()), true));
        }

        // Clean stop — remove the recovery marker.
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
        // Recorder is no longer registered as a client itself — drain
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

    void MultitrackRecorder::drainOnce()
    {
        // Legacy entry-point — when called from anywhere other than the
        // shard clients, fan out to every shard so behaviour is identical
        // to the pre-shard implementation (mostly used by stopRecording's
        // final flush).
        for (auto& sh : shards) drainShard (*sh);
    }

    void MultitrackRecorder::drainShard (ShardClient& shard)
    {
        if (! writersReady.load (std::memory_order_acquire)) return;

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
            if (w.writer == nullptr) continue;

            // Track FIFO occupancy so the dashboard can warn the engineer
            // BEFORE samples are dropped — at 80%+ the disk is falling
            // behind the audio thread.
            const int cap = (int) cf.data.size();
            if (cap > 0)
            {
                const int pct = (cf.fifo.getNumReady() * 100) / cap;
                if (pct > worstFillPct) worstFillPct = pct;
            }

            const int available = cf.fifo.getNumReady();
            if (available <= 0) continue;

            const auto scope = cf.fifo.read (available);

            if (scope.blockSize1 > 0)
            {
                const float* ptr = cf.data.data() + scope.startIndex1;
                const float* const channels[] = { ptr };
                w.writer->writeFromFloatArrays (channels, 1, scope.blockSize1);
                if (w.backupWriter != nullptr
                    && ! w.backupWriter->writeFromFloatArrays (channels, 1, scope.blockSize1))
                {
                    w.backupWriter.reset();
                    backupFailed.store (true, std::memory_order_relaxed);
                }
                totalWritten += scope.blockSize1;
            }
            if (scope.blockSize2 > 0)
            {
                const float* ptr = cf.data.data() + scope.startIndex2;
                const float* const channels[] = { ptr };
                w.writer->writeFromFloatArrays (channels, 1, scope.blockSize2);
                if (w.backupWriter != nullptr
                    && ! w.backupWriter->writeFromFloatArrays (channels, 1, scope.blockSize2))
                {
                    w.backupWriter.reset();
                    backupFailed.store (true, std::memory_order_relaxed);
                }
                totalWritten += scope.blockSize2;
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

            samplesSinceFlush += totalWritten;
            // Flush WAV headers every ~5 s of audio so a crash mid-record
            // still leaves a playable file with current data sizes.
            if (samplesSinceFlush >= (juce::int64) (sampleRate * 5.0))
            {
                for (auto& w : writers)
                {
                    if (w.writer       != nullptr) w.writer      ->flush();
                    if (w.backupWriter != nullptr) w.backupWriter->flush();
                }
                samplesSinceFlush = 0;
            }
        }
    }
}
