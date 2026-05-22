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

    MultitrackRecorder::MultitrackRecorder()
    {
        formatManager.registerBasicFormats();
        writerThread.startThread();
    }

    MultitrackRecorder::~MultitrackRecorder()
    {
        stopRecording();
        writerThread.removeTimeSliceClient (this);
        writerThread.stopThread (2000);
    }

    void MultitrackRecorder::prepare (double sr, int maxBlock, int numInputs)
    {
        stopRecording();
        writerThread.removeTimeSliceClient (this);

        sampleRate = sr;
        blockSize  = maxBlock;

        const int fifoSize = juce::nextPowerOfTwo ((int) (sr * kFifoSeconds));
        scratch.assign ((std::size_t) maxBlock, 0.0f);

        tracks.clear();
        fifos.clear();
        preRoll.clear();
        for (int i = 0; i < numInputs; ++i)
        {
            auto t = std::make_unique<TrackState>();
            t->name = "In " + juce::String (i + 1);
            tracks.push_back (std::move (t));

            auto f = std::make_unique<ChannelFifo>();
            f->resize (fifoSize);
            fifos.push_back (std::move (f));

            preRoll.push_back (std::make_unique<PreRollBuffer>());
        }
        allocatePreRollBuffers();

        writerThread.addTimeSliceClient (this);
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
        t->name = "In " + juce::String ((int) tracks.size() + 1);
        tracks.push_back (std::move (t));

        auto f = std::make_unique<ChannelFifo>();
        f->resize (fifoSize);
        fifos.push_back (std::move (f));

        preRoll.push_back (std::make_unique<PreRollBuffer>());
        allocatePreRollBuffers();
    }

    void MultitrackRecorder::removeLastTrack()
    {
        if (isRecording() || tracks.empty()) return;
        tracks.pop_back();
        fifos.pop_back();
        preRoll.pop_back();
    }

    void MultitrackRecorder::removeTrackAt (int index)
    {
        if (isRecording()) return;
        if (index < 0 || index >= (int) tracks.size()) return;
        tracks.erase (tracks.begin() + index);
        fifos.erase  (fifos.begin()  + index);
        preRoll.erase(preRoll.begin()+ index);
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
        writerThread.removeTimeSliceClient (this);
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
            auto& t = *tracks[(std::size_t) ch];
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

            // Primary writer.
            const auto primaryFile = sessionDir.getChildFile (trackName + primary.ext);
            w.writer.reset (openWriter (primaryFile, primary.container, primary.bitDepth));

            // Optional second copy — may be in a different format from
            // the primary, so the engineer can run e.g. WAV/24 to the
            // main drive AND FLAC/24 to the backup drive simultaneously.
            if (backupDir.isDirectory())
            {
                auto backupSession = backupDir.getChildFile (sessionDir.getFileName());
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
        drainOnce();
        return 5; // ms
    }

    void MultitrackRecorder::drainOnce()
    {
        if (! writersReady.load (std::memory_order_acquire)) return;

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        juce::int64 totalWritten = 0;

        for (std::size_t i = 0; i < fifos.size() && i < writers.size(); ++i)
        {
            auto& cf = *fifos[i];
            auto& w  = writers[i];
            if (w.writer == nullptr) continue;

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
