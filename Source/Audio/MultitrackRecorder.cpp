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

        enum class Container { Wav, Aiff, Flac } container;
        int bitDepth;
        switch (captureFormat)
        {
            case CaptureFormat::Wav16:       container = Container::Wav;  bitDepth = 16; break;
            case CaptureFormat::Wav24:       container = Container::Wav;  bitDepth = 24; break;
            case CaptureFormat::Wav32Float:  container = Container::Wav;  bitDepth = 32; break;
            case CaptureFormat::Aiff16:      container = Container::Aiff; bitDepth = 16; break;
            case CaptureFormat::Aiff24:      container = Container::Aiff; bitDepth = 24; break;
            case CaptureFormat::Aiff32Float: container = Container::Aiff; bitDepth = 32; break;
            case CaptureFormat::Flac16:      container = Container::Flac; bitDepth = 16; break;
            case CaptureFormat::Flac24:      container = Container::Flac; bitDepth = 24; break;
        }
        const bool useFlac = (container == Container::Flac);
        const bool useAiff = (container == Container::Aiff);
        const char* ext = useFlac ? ".flac" : (useAiff ? ".aif" : ".wav");

        const auto now = juce::Time::getCurrentTime();

        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            // BWF (bext) metadata — only meaningful for WAV.
            juce::StringPairArray meta;
            if (! useFlac && ! useAiff)
            {
                meta.set (juce::WavAudioFormat::bwavDescription,
                          "Zynforge Recording — " + sessionDir.getFileName()
                            + " — track " + juce::String ((int) i + 1));
                meta.set (juce::WavAudioFormat::bwavOriginator,      "Zynforge Recording");
                meta.set (juce::WavAudioFormat::bwavOriginatorRef,   sessionDir.getFileName());
                meta.set (juce::WavAudioFormat::bwavOriginationDate, now.formatted ("%Y-%m-%d"));
                meta.set (juce::WavAudioFormat::bwavOriginationTime, now.formatted ("%H:%M:%S"));
                meta.set (juce::WavAudioFormat::bwavTimeReference,   "0");
            }

            WriterChannel w;
            const auto name = juce::String::formatted ("Track_%02d", (int) i + 1) + ext;
            auto file = sessionDir.getChildFile (name);
            file.deleteFile();

            if (auto* out = file.createOutputStream().release())
            {
                juce::AudioFormatWriter* awRaw = nullptr;
                if      (useFlac) awRaw = flac.createWriterFor (out, sampleRate, 1, bitDepth, meta, 5);
                else if (useAiff) awRaw = aiff.createWriterFor (out, sampleRate, 1, bitDepth, meta, 0);
                else              awRaw = wav .createWriterFor (out, sampleRate, 1, bitDepth, meta, 0);

                if (awRaw == nullptr) { delete out; }
                w.writer.reset (awRaw);
            }

            // Optional second copy to a backup directory.
            if (backupDir.isDirectory())
            {
                auto backupSession = backupDir.getChildFile (sessionDir.getFileName());
                backupSession.createDirectory();
                auto backupFile = backupSession.getChildFile (name);
                backupFile.deleteFile();
                if (auto* bout = backupFile.createOutputStream().release())
                {
                    juce::AudioFormatWriter* bw = nullptr;
                    if      (useFlac) bw = flac.createWriterFor (bout, sampleRate, 1, bitDepth, meta, 5);
                    else if (useAiff) bw = aiff.createWriterFor (bout, sampleRate, 1, bitDepth, meta, 0);
                    else              bw = wav .createWriterFor (bout, sampleRate, 1, bitDepth, meta, 0);
                    if (bw == nullptr) { delete bout; }
                    w.backupWriter.reset (bw);
                }
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

        closeWriters();
        backupActive.store (false, std::memory_order_relaxed);

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
