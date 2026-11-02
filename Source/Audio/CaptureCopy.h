#pragma once

#include "CheckedFileStream.h"
#include "FloatAiffWriter.h"
#include "MultitrackRecorder.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace zynforge
{
    // One destination of a take: the primary, the backup, or a mirror. Each has
    // its own thread and its own ring, so a drive that stalls costs only its own
    // copy. The recorder's distributor thread does no disk work: it hands every
    // copy each chunk and moves on, and a copy that has fallen a whole ring
    // behind loses its oldest audio to silence, on that copy alone.
    //
    // Everything a stalled thread can touch lives here and is shared, never in
    // the recorder: STOP leaves a copy behind that stops making progress, and
    // that thread may come back from its write() long after the recorder is gone.
    struct MultitrackRecorder::CaptureCopy final : std::enable_shared_from_this<CaptureCopy>
    {
        static constexpr int kChunkFrames = 8192;

        // One track's files on this copy, and its ring. head, tail, lostFrames
        // and accepting are guarded by ringLock. The rest belongs to the copy's
        // thread while it runs, and to whoever joined it afterwards.
        struct Track
        {
            std::unique_ptr<juce::AudioFormatWriter> writer;
            juce::File   baseFile;                 // "Audio Files/Track_01" (no extension)
            juce::String ext, nameLeft, nameRight;
            int          container { 0 }, bitDepth { 24 }, bytesPerSample { 3 };
            int          channels { 1 }, partNumber { 1 };
            juce::int64  bytesWritten { 0 };       // resets at each roll
            juce::int64  totalSamples { 0 };       // never resets
            juce::StringArray partFiles;
            bool         failed { false };
            CheckedFileStream::Failed closeFailed { CheckedFileStream::makeFailedFlag() };

            juce::HeapBlock<float> ring;           // ringFrames per channel, planar
            juce::int64  head { 0 }, tail { 0 }, lostFrames { 0 }, countedLost { 0 };
            bool         accepting { false };
        };

        int          copyIndex { 0 };              // 0 primary, 1 backup, 2+ mirror
        juce::String label;                        // what the banner and report call it
        juce::File   root;                         // the folder the engineer configured
        double       sampleRate { 48000.0 };
        juce::String sessionName;
        juce::int64  passStart { 0 };              // estimate for the stamp at open
        juce::Time   passOrigin;                   // one wall-clock time for every copy of the pass
        int          ringFrames { 0 };
        std::shared_ptr<CaptureLatches> latches;
        std::function<void (CaptureCopy&)> beforeWriteForTests;
        std::function<bool (CaptureCopy&, bool finishing)> threadRefusedForTests;

        std::vector<Track> tracks;                 // aligned with the recorder's tracks
        std::vector<WriterChannel::StampedPart> stampedParts;
        std::vector<PendingLeadIn> leadIns;

        // Run first on this copy's thread: whatever joining the take takes. A
        // non-empty answer says why this copy cannot, and it leaves the take.
        std::function<juce::String (CaptureCopy&)> openForTake;

        std::shared_ptr<std::vector<std::unique_ptr<PreRollBuffer>>> history;
        int historySamples { 0 };

        std::atomic<juce::int64> bytesOut { 0 }, framesPushed { 0 }, framesDone { 0 };
        std::atomic<juce::int64> droppedFrames { 0 };
        std::atomic<int>         liveChannels { 0 };
        std::atomic<int>         lastWriteMs { 0 };
        std::atomic<double>      lastProgressMs { 0.0 };
        std::atomic<bool>        overran { false }, finishing { false }, finished { false };
        std::atomic<bool>        abandoned { false }, flushWanted { false }, dropped { false }, opening { false };

        std::mutex              ringLock;
        std::condition_variable wake;
        std::thread             worker;

        ~CaptureCopy() { jassert (! worker.joinable()); }

        void latchFailure() noexcept
        {
            if (abandoned.load (std::memory_order_acquire)) return;
            auto& flag = copyIndex == 0 ? latches->primaryFailed
                       : copyIndex == 1 ? latches->backupFailed : latches->mirrorFailed;
            flag.store (true, std::memory_order_relaxed);
        }

        void failTrack (Track& t)
        {
            t.failed = true;
            latchFailure();
        }

        // This track's file is finished, by choice or by failure.
        void stopTrack (Track& t)
        {
            closeWriter (t);
            const std::lock_guard<std::mutex> g (ringLock);
            if (t.accepting) liveChannels.fetch_sub (t.channels, std::memory_order_relaxed);
            t.accepting = false;
        }

        // The stream can only say a close came up short once the writer is gone.
        void closeWriter (Track& t)
        {
            const bool wasOpen = t.writer != nullptr;
            t.writer.reset();
            if (wasOpen && t.closeFailed->load()) failTrack (t);
        }

        // Opens one file. framesBefore: what this pass already wrote to earlier
        // parts of this copy. A WAV joins stampedParts. Null on failure.
        juce::AudioFormatWriter* openFile (Track& t, const juce::File& target, juce::int64 framesBefore)
        {
            // All callers choose a new part or stash a punch target first.
            // Never truncate an unexpected file, including a foreign backup.
            if (target.exists()) return nullptr;
            auto* out = CheckedFileStream::open (target, t.closeFailed).release();
            if (out == nullptr) return nullptr;

            const auto chans = (unsigned int) juce::jmax (1, t.channels);
            juce::StringPairArray meta;
            juce::int64 timeReference = 0;
            if (t.container == 0)   // WAV gets the BWF bext chunk so mix engineers can identify the take.
            {
                // Files open one by one and a late copy opens later still, so
                // the clock is read once per pass. Every copy of a take then
                // says the same thing, and a rolled part says when it began.
                const auto now = passOrigin + juce::RelativeTime::seconds ((double) framesBefore / sampleRate);
                // A crash survivor keeps this estimate; finalization stamps the
                // exact value (see bwf::stampTimeReference).
                timeReference = bwf::wrapToDay (passStart + framesBefore, sampleRate);
                meta.set (juce::WavAudioFormat::bwavDescription,
                          bwf::channelDescription ("Zynforge Recording -- " + sessionName
                                                     + " -- " + target.getFileNameWithoutExtension(),
                                                   t.nameLeft, t.nameRight));
                meta.set (juce::WavAudioFormat::bwavOriginator,      "Zynforge Recording");
                meta.set (juce::WavAudioFormat::bwavOriginatorRef,   sessionName);
                meta.set (juce::WavAudioFormat::bwavOriginationDate, now.formatted ("%Y-%m-%d"));
                meta.set (juce::WavAudioFormat::bwavOriginationTime, now.formatted ("%H:%M:%S"));
                meta.set (juce::WavAudioFormat::bwavTimeReference,   juce::String (timeReference));
            }

            juce::AudioFormatWriter* w = nullptr;
            if      (t.container == 2) w = juce::FlacAudioFormat().createWriterFor (out, sampleRate, chans, t.bitDepth, meta, 5);
            else if (t.container == 1) w = t.bitDepth == 32 ? FloatAiffWriter::create (out, sampleRate, chans)
                                                            : PcmAiffWriter::create (out, sampleRate, chans, t.bitDepth);
            else                       w = juce::WavAudioFormat().createWriterFor (out, sampleRate, chans, t.bitDepth, meta, 0);
            if (w == nullptr) delete out;
            else if (t.container == 0)
                stampedParts.push_back ({ target, framesBefore, (juce::uint64) timeReference });
            return w;
        }

        // Allocates without touching the pages: a short take on many channels
        // never pays for ring it does not reach, and RECORD does not wait for
        // gigabytes to be zeroed.
        void openRing (Track& t)
        {
            t.ring.malloc ((size_t) ringFrames * (size_t) t.channels);
            t.accepting = t.ring != nullptr;
            if (t.accepting)
                liveChannels.fetch_add (t.channels, std::memory_order_relaxed);
            else
            {
                t.writer.reset();
                failTrack (t);
            }
        }

        // ── Distributor side: memory only, never a disk ────────────────────
        void push (std::size_t index, const float* left, const float* right, int frames)
        {
            auto& t = tracks[index];
            const std::lock_guard<std::mutex> g (ringLock);
            if (! t.accepting) return;
            const auto start = (int) (t.head % ringFrames);
            const int  first = juce::jmin (frames, ringFrames - start);
            const auto copyIn = [&] (float* plane, const float* source)
            {
                std::memcpy (plane + start, source, (size_t) first * sizeof (float));
                if (first < frames)
                    std::memcpy (plane, source + first, (size_t) (frames - first) * sizeof (float));
            };
            copyIn (t.ring.get(), left);
            if (t.channels == 2) copyIn (t.ring.get() + ringFrames, right != nullptr ? right : left);
            t.head += frames;
            framesPushed.fetch_add (frames, std::memory_order_relaxed);

            const auto over = t.head - t.tail - ringFrames;
            if (over > 0)
            {
                // Latched here, not when the thread comes back: it may never.
                overran.store (true, std::memory_order_relaxed);
                latchFailure();
                const auto lost = t.lostFrames + over;
                if (lost > droppedFrames.load (std::memory_order_relaxed))
                    droppedFrames.store (lost, std::memory_order_relaxed);
                // A hole in the primary is a hole in the take, so it counts
                // where every reading of missed samples looks. Per track, like
                // the capture FIFOs count theirs.
                if (copyIndex == 0 && lost > t.countedLost)
                    latches->primaryLostFrames.fetch_add (lost - t.countedLost, std::memory_order_relaxed);
                t.countedLost = juce::jmax (t.countedLost, lost);
            }
        }

        int fillPercent()
        {
            const std::lock_guard<std::mutex> g (ringLock);
            juce::int64 worst = 0;
            for (const auto& t : tracks)
                if (t.accepting) worst = juce::jmax (worst, t.head - t.tail);
            return ringFrames > 0 ? (int) juce::jmin ((juce::int64) 100, worst * 100 / ringFrames) : 0;
        }

        // ── The copy's thread ──────────────────────────────────────────────
        void start (std::shared_ptr<CaptureCopy> self)
        {
            lastProgressMs.store (juce::Time::getMillisecondCounterHiRes());
            try
            {
                refuseThreadForTests (false);
                worker = std::thread ([self = std::move (self)] { self->run(); });
            }
            catch (const std::system_error&)
            {
                // Nothing will ever drain this copy: stop feeding it and say so.
                closeFilesNow();
                for (auto& t : tracks)
                    if (t.baseFile != juce::File()) t.failed = true;
                latchFailure();
                if (opening.exchange (false)) leaveTake (label + " could not start; it is not in this take");
                finished.store (true);
            }
        }

        void run()
        {
            if (openForTake != nullptr)
            {
                const auto refusal = openForTake (*this);
                openForTake = nullptr;
                opening.store (false, std::memory_order_release);
                if (refusal.isNotEmpty())
                {
                    leaveTake (refusal);
                    finished.store (true, std::memory_order_release);
                    wakeWaiters();
                    return;
                }
            }
            writeHistory();
            std::vector<float> left ((size_t) kChunkFrames), right ((size_t) kChunkFrames), zeros ((size_t) kChunkFrames, 0.0f);
            auto lastFlush = juce::Time::getMillisecondCounterHiRes();
            while (! abandoned.load (std::memory_order_acquire))
            {
                const bool last = finishing.load (std::memory_order_acquire);
                bool wrote = false;
                for (auto& t : tracks)
                    wrote = writeOneChunk (t, left, right, zeros) || wrote;

                const auto now = juce::Time::getMillisecondCounterHiRes();
                if (flushWanted.load (std::memory_order_acquire) || now - lastFlush >= 5000.0)
                {
                    lastFlush = now;
                    flushOpenFiles();
                    flushWanted.store (false, std::memory_order_release);
                    wakeWaiters();
                }
                if (last && ! wrote) break;
                if (wrote) continue;
                std::unique_lock<std::mutex> lock (ringLock);
                wake.wait_for (lock, std::chrono::milliseconds (5));
            }
            closeFilesNow();
            finished.store (true, std::memory_order_release);
            wakeWaiters();
        }

        // Out of the take, by name. Everything the rings hold for it goes too.
        void leaveTake (const juce::String& why)
        {
            for (auto& t : tracks) stopTrack (t);
            dropped.store (true, std::memory_order_release);
            if (abandoned.load (std::memory_order_acquire)) return;
            latchFailure();
            if (copyIndex == 1) latches->backupDropped.store (true, std::memory_order_relaxed);
            else                latches->mirrorsDropped.fetch_add (1, std::memory_order_relaxed);
            const std::lock_guard<std::mutex> guard (latches->droppedLock);
            latches->dropped.add (why);
        }

        void failTrackAtOpen (Track& t)
        {
            failTrack (t);
            stopTrack (t);
        }

        // Under the lock, so a waiter between its test and its sleep is not missed.
        void wakeWaiters()
        {
            const std::lock_guard<std::mutex> g (ringLock);
            wake.notify_all();
        }

        // Pre-roll goes in front of the first live sample. The ring holds live
        // audio meanwhile, so a slow dump costs this copy alone.
        void writeHistory()
        {
            const auto detached = std::move (history);
            if (detached == nullptr || historySamples <= 0) return;
            std::vector<float> left ((size_t) historySamples, 0.0f), right ((size_t) historySamples, 0.0f);
            const auto read = [&] (std::size_t channel, std::vector<float>& destination)
            {
                const int got = (*detached)[channel]->readHistory (destination.data(), historySamples);
                // A newly added track may have less history than the others.
                // Pad its leading edge, keeping the live seam at the same sample.
                if (got < historySamples)
                {
                    std::memmove (destination.data() + historySamples - got, destination.data(),
                                  (size_t) got * sizeof (float));
                    std::fill_n (destination.data(), historySamples - got, 0.0f);
                }
            };
            for (std::size_t i = 0; i < tracks.size() && i < detached->size(); ++i)
            {
                if (abandoned.load (std::memory_order_acquire)) return;
                auto& t = tracks[i];
                if (t.writer == nullptr) continue;
                read (i, left);
                if (t.channels == 2 && i + 1 < detached->size()) read (i + 1, right);
                const float* const audio[] { left.data(), right.data() };
                writeFrames (t, audio, historySamples);
            }
        }

        bool writeOneChunk (Track& t, std::vector<float>& left, std::vector<float>& right,
                            const std::vector<float>& zeros)
        {
            juce::int64 lost = 0;
            int frames = 0;
            {
                const std::lock_guard<std::mutex> g (ringLock);
                const auto behind = t.head - t.tail;
                if (behind <= 0) return false;
                lost = juce::jmax ((juce::int64) 0, behind - ringFrames);
                t.tail += lost;
                t.lostFrames += lost;
                frames = (int) juce::jmin (behind - lost, (juce::int64) kChunkFrames);
                const auto start = (int) (t.tail % ringFrames);
                const int  first = juce::jmin (frames, ringFrames - start);
                const auto copyOut = [&] (const float* plane, float* destination)
                {
                    std::memcpy (destination, plane + start, (size_t) first * sizeof (float));
                    if (first < frames)
                        std::memcpy (destination + first, plane, (size_t) (frames - first) * sizeof (float));
                };
                copyOut (t.ring.get(), left.data());
                if (t.channels == 2) copyOut (t.ring.get() + ringFrames, right.data());
                t.tail += frames;
            }
            // Silence for what was lost, so this copy stays sample-aligned with
            // every other copy and with the take's timeline.
            const float* const silent[] { zeros.data(), zeros.data() };
            for (auto remaining = lost; remaining > 0;)
            {
                if (abandoned.load (std::memory_order_acquire)) return false;
                const int count = (int) juce::jmin (remaining, (juce::int64) kChunkFrames);
                writeFrames (t, silent, count);
                remaining -= count;
            }
            const float* const audio[] { left.data(), right.data() };
            writeFrames (t, audio, frames);
            framesDone.fetch_add (frames + lost, std::memory_order_relaxed);
            return true;
        }

        void rollIfNeeded (Track& t, int frames)
        {
            if (t.writer == nullptr) return;
            // A stereo pair's file grows twice as fast per frame, so the
            // projection counts every channel in the file.
            const juce::int64 projected = t.bytesWritten
                + (juce::int64) frames * t.channels * t.bytesPerSample;
            if (projected < maxBytesForContainer (t.container)) return;
            // Closing finalises the header, so the file left behind is valid.
            if (! flushFloatAiffBeforeClose (t.writer.get())) failTrack (t);
            closeWriter (t);
            ++t.partNumber;
            const auto next = t.baseFile.getParentDirectory()
                .getChildFile (t.baseFile.getFileName() + "_part"
                               + juce::String::formatted ("%02d", t.partNumber) + t.ext);
            t.writer.reset (openFile (t, next, t.totalSamples));
            t.bytesWritten = 0;
            if (t.writer != nullptr)
                t.partFiles.add (next.getFileName());
            else
            {
                failTrack (t);    // the next part would not open (disk full at the ceiling)
                stopTrack (t);
            }
        }

        void writeFrames (Track& t, const float* const* audio, int frames)
        {
            rollIfNeeded (t, frames);
            if (t.writer == nullptr) return;
            if (beforeWriteForTests) beforeWriteForTests (*this);
            if (t.writer == nullptr) return;
            const auto started = juce::Time::getMillisecondCounterHiRes();
            if (t.writer->writeFromFloatArrays (audio, t.channels, frames))
            {
                const auto bytes = (juce::int64) frames * t.channels * t.bytesPerSample;
                t.bytesWritten += bytes;
                t.totalSamples += frames;
                bytesOut.fetch_add (bytes, std::memory_order_relaxed);
            }
            else
            {
                failTrack (t);
                stopTrack (t);
            }
            const auto now = juce::Time::getMillisecondCounterHiRes();
            lastWriteMs.store ((int) (now - started), std::memory_order_relaxed);
            lastProgressMs.store (now, std::memory_order_relaxed);
        }

        // Rewrites each open file's header so a crash mid-take leaves a
        // self-describing file, and promotes a WAV past 4 GiB to RF64.
        void flushOpenFiles()
        {
            for (auto& t : tracks)
            {
                if (t.writer == nullptr) continue;
                const bool checked = dynamic_cast<FloatAiffWriter*> (t.writer.get()) != nullptr
                                  || dynamic_cast<PcmAiffWriter*> (t.writer.get()) != nullptr;
                // Other JUCE codecs may report unsupported.
                if (! t.writer->flush() && checked) failTrack (t);
            }
        }

        void closeFilesNow()
        {
            for (auto& t : tracks)
            {
                if (t.writer != nullptr && ! flushFloatAiffBeforeClose (t.writer.get())) failTrack (t);
                stopTrack (t);      // a copy still opening has a ring and no writer
            }
            const std::lock_guard<std::mutex> g (ringLock);
            for (auto& t : tracks) t.ring.free();
        }

        // ── Recorder side ──────────────────────────────────────────────────
        void requestFinish()
        {
            lastProgressMs.store (juce::Time::getMillisecondCounterHiRes());
            finishing.store (true, std::memory_order_release);
            wakeWaiters();
        }

        // After this the recorder never looks at `tracks` again, and the thread
        // ends by itself whenever its write() returns.
        void abandon()
        {
            latchFailure();
            abandoned.store (true, std::memory_order_release);
            wakeWaiters();
            if (worker.joinable()) worker.detach();
        }

        void join()
        {
            if (worker.joinable()) worker.join();
        }

        // The copy's own work once its files have closed, on a thread of its
        // own for the same reason the writing was: a drive that hangs strands
        // that thread. `work` may hold only the copy and plain values.
        void startFinishing (std::shared_ptr<CaptureCopy> self, std::function<void (CaptureCopy&)> work)
        {
            finished.store (false, std::memory_order_release);
            noteProgress();
            try
            {
                refuseThreadForTests (true);
                worker = std::thread ([self = std::move (self), work = std::move (work)]
                {
                    work (*self);
                    self->finished.store (true, std::memory_order_release);
                });
            }
            catch (const std::system_error&)
            {
                // None of the work ran, so nothing it would report exists. Reads
                // as a copy left behind, and its folder is recovered next take.
                latchFailure();
                abandoned.store (true, std::memory_order_release);
                finished.store (true, std::memory_order_release);
            }
        }

        void refuseThreadForTests (bool finishing)
        {
            if (threadRefusedForTests != nullptr && threadRefusedForTests (*this, finishing))
                throw std::system_error (std::make_error_code (std::errc::resource_unavailable_try_again));
        }

        void noteProgress() noexcept
        {
            lastProgressMs.store (juce::Time::getMillisecondCounterHiRes());
        }

        void flushAndWait (int timeoutMs)
        {
            flushWanted.store (true, std::memory_order_release);
            std::unique_lock<std::mutex> lock (ringLock);
            wake.notify_all();
            wake.wait_for (lock, std::chrono::milliseconds (timeoutMs), [this]
            {
                return ! flushWanted.load() || finished.load() || abandoned.load();
            });
        }

        void waitUntilWritten (int timeoutMs)
        {
            wakeWaiters();
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (framesDone.load() < framesPushed.load() && ! finished.load() && ! abandoned.load()
                   && juce::Time::getMillisecondCounterHiRes() < end)
                juce::Thread::sleep (1);
        }
    };
}
