#include "SessionPlayer.h"

namespace zynforge
{
    static constexpr int  kReaderBufferSeconds = 2;
    static constexpr int  kStopSettleMs        = 40;  // > one audio buffer @ any sane size

    SessionPlayer::SessionPlayer()
    {
        formatManager.registerBasicFormats();
        readerThread.startThread();
    }

    SessionPlayer::~SessionPlayer()
    {
        unload();
        readerThread.stopThread (2000);
    }

    void SessionPlayer::prepare (double sr, int block)
    {
        deviceSampleRate = sr;
        blockSize        = block;
        scratch.setSize (1, juce::jmax (block, 4096), false, true, true);
    }

    void SessionPlayer::release()
    {
        stop();
    }

    int SessionPlayer::loadSession (const juce::File& sessionDir)
    {
        // Stop playback and wait for any in-flight audio callback to drain.
        playing.store (false, std::memory_order_release);
        juce::Thread::sleep (kStopSettleMs);

        tracks.clear();
        readerCount.store (0, std::memory_order_release);
        loaded.store (false, std::memory_order_release);

        if (! sessionDir.isDirectory()) return 0;

        // Pro Tools-style: tracks live under <session>/Audio Files/. Older
        // sessions written before the named-folder refactor kept them at
        // the root, so fall back to the root scan when the subfolder is
        // empty / absent.
        const auto audioFiles = sessionDir.getChildFile ("Audio Files");
        auto files = audioFiles.isDirectory()
                       ? audioFiles.findChildFiles (juce::File::findFiles, false, "Track_*.wav")
                       : juce::Array<juce::File>();
        if (files.isEmpty())
            files = sessionDir.findChildFiles (juce::File::findFiles, false, "Track_*.wav");
        files.sort();

        juce::int64 maxLen = 0;
        double      sr     = deviceSampleRate;

        for (auto& f : files)
        {
            auto* raw = formatManager.createReaderFor (f);
            if (raw == nullptr) continue;

            const auto fileSR = raw->sampleRate;
            const auto bufferSamples = (int) (fileSR * kReaderBufferSeconds);

            Track t;
            t.length = raw->lengthInSamples;

            auto buf = std::make_unique<juce::BufferingAudioReader> (raw, readerThread, bufferSamples);
            buf->setReadTimeout (0); // non-blocking — fill silence if not buffered yet
            t.reader = std::move (buf);

            maxLen = juce::jmax (maxLen, t.length);
            sr     = fileSR;

            tracks.push_back (std::move (t));
        }

        this->sessionDir = sessionDir;
        sessionName    = sessionDir.getFileName();
        fileSampleRate = sr;
        totalLength.store (maxLen, std::memory_order_release);
        position   .store (0,      std::memory_order_release);
        readerCount.store ((int) tracks.size(), std::memory_order_release);
        loaded     .store (! tracks.empty(), std::memory_order_release);

        return (int) tracks.size();
    }

    void SessionPlayer::unload()
    {
        playing.store (false, std::memory_order_release);
        juce::Thread::sleep (kStopSettleMs);

        tracks.clear();
        readerCount.store (0, std::memory_order_release);
        loaded     .store (false, std::memory_order_release);
        sessionName.clear();
        sessionDir = juce::File();
        totalLength.store (0, std::memory_order_release);
        position   .store (0, std::memory_order_release);
    }

    void SessionPlayer::start()
    {
        if (loaded.load (std::memory_order_acquire))
            playing.store (true, std::memory_order_release);
    }

    void SessionPlayer::stop()
    {
        playing.store (false, std::memory_order_release);
    }

    void SessionPlayer::rewind()
    {
        setPositionSamples (0);
    }

    void SessionPlayer::setPositionSamples (juce::int64 s)
    {
        const auto total = totalLength.load (std::memory_order_relaxed);
        position.store (juce::jlimit<juce::int64> (0, total, s), std::memory_order_release);
    }

    void SessionPlayer::setLoopRegion (juce::int64 s, juce::int64 e) noexcept
    {
        if (e <= s) { clearLoopRegion(); return; }
        loopStart.store (s, std::memory_order_release);
        loopEnd  .store (e, std::memory_order_release);
    }

    void SessionPlayer::clearLoopRegion() noexcept
    {
        loopStart.store (-1, std::memory_order_release);
        loopEnd  .store (-1, std::memory_order_release);
    }

    bool SessionPlayer::hasLoopRegion() const noexcept
    {
        const auto s = loopStart.load (std::memory_order_relaxed);
        const auto e = loopEnd  .load (std::memory_order_relaxed);
        return s >= 0 && e > s;
    }

    void SessionPlayer::setTrackClips (int trackIdx, std::vector<Clip> clips)
    {
        if (trackIdx < 0) return;
        const juce::ScopedLock sl (clipsLock);
        if (trackIdx >= (int) activeClips.size()) activeClips.resize ((size_t) trackIdx + 1);
        activeClips[(size_t) trackIdx] = std::move (clips);
    }

    void SessionPlayer::clearAllClips()
    {
        const juce::ScopedLock sl (clipsLock);
        activeClips.clear();
    }

    void SessionPlayer::processBlock (float* const* outputs, int numOutputs, int numSamples) noexcept
    {
        if (! playing.load (std::memory_order_acquire)) return;

        const auto numTracks = readerCount.load (std::memory_order_acquire);
        if (numTracks == 0) return;

        juce::int64       startPos = position.load (std::memory_order_relaxed);
        const juce::int64 total    = totalLength.load (std::memory_order_relaxed);

        const juce::int64 lStart = loopStart.load (std::memory_order_relaxed);
        const juce::int64 lEnd   = loopEnd  .load (std::memory_order_relaxed);
        const bool        looping = (lStart >= 0 && lEnd > lStart);

        if (looping && startPos >= lEnd)
        {
            startPos = lStart;
            position.store (startPos, std::memory_order_release);
        }

        if (startPos >= total)
        {
            playing.store (false, std::memory_order_release);
            return;
        }

        juce::int64 cap = total - startPos;
        if (looping) cap = juce::jmin (cap, lEnd - startPos);
        const int playableThisBlock = (int) juce::jmin ((juce::int64) numSamples, cap);
        const int n                 = juce::jmin (numOutputs, numTracks);

        if (scratch.getNumSamples() < playableThisBlock)
            scratch.setSize (1, playableThisBlock, false, false, true);

        // Snapshot the clip lists under the lock — held only long enough
        // to take a const reference. After release we iterate read-only
        // and the UI may modify the (different) live vector.
        const juce::ScopedTryLock stl (clipsLock);

        for (int i = 0; i < n; ++i)
        {
            float* out = outputs[i];
            if (out == nullptr) continue;

            auto& t = tracks[(std::size_t) i];
            if (t.reader == nullptr || startPos >= t.length)
            {
                juce::FloatVectorOperations::clear (out, numSamples);
                continue;
            }

            // Clip-aware path: if this track has an active clip list,
            // render only the time spans inside clips; silence elsewhere.
            // Otherwise fall through to the legacy 'whole file' read.
            const std::vector<Clip>* clips = nullptr;
            if (stl.isLocked()
                && i < (int) activeClips.size()
                && ! activeClips[(size_t) i].empty())
            {
                clips = &activeClips[(size_t) i];
            }

            if (clips != nullptr)
            {
                juce::FloatVectorOperations::clear (out, numSamples);
                for (const auto& c : *clips)
                {
                    const auto clipEndTL = c.timelineStartSamples + c.fileLengthSamples;
                    // Overlap of [startPos, startPos+playableThisBlock)
                    // with [c.timelineStartSamples, clipEndTL).
                    const juce::int64 ovStartTL = juce::jmax (startPos, c.timelineStartSamples);
                    const juce::int64 ovEndTL   = juce::jmin (startPos + playableThisBlock, clipEndTL);
                    if (ovEndTL <= ovStartTL) continue;
                    const int outOffset  = (int) (ovStartTL - startPos);
                    const int spanLen    = (int) (ovEndTL  - ovStartTL);
                    const juce::int64 fileReadStart =
                        c.fileStartSamples + (ovStartTL - c.timelineStartSamples);

                    if (scratch.getNumSamples() < spanLen)
                        scratch.setSize (1, spanLen, false, false, true);
                    scratch.clear (0, 0, spanLen);
                    t.reader->read (&scratch, 0, spanLen, fileReadStart, true, true);
                    juce::FloatVectorOperations::copy (out + outOffset,
                                                       scratch.getReadPointer (0),
                                                       spanLen);
                }
                if (playableThisBlock < numSamples)
                    juce::FloatVectorOperations::clear (out + playableThisBlock,
                                                        numSamples - playableThisBlock);
                continue;
            }

            // Legacy whole-file path.
            const int avail = (int) juce::jmin ((juce::int64) playableThisBlock, t.length - startPos);

            scratch.clear (0, 0, avail);
            t.reader->read (&scratch, 0, avail, startPos, true, true);

            juce::FloatVectorOperations::copy (out, scratch.getReadPointer (0), avail);
            if (avail < numSamples)
                juce::FloatVectorOperations::clear (out + avail, numSamples - avail);
        }

        // Silence outputs beyond loaded tracks
        for (int i = n; i < numOutputs; ++i)
            if (outputs[i] != nullptr)
                juce::FloatVectorOperations::clear (outputs[i], numSamples);

        position.store (startPos + playableThisBlock, std::memory_order_release);
    }
}
