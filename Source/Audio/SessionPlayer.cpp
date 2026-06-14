#include "SessionPlayer.h"
#include "MultiPartReader.h"

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
        // 2 channels: a stereo file is read in full (L+R) and the wanted
        // channel selected, so the scratch needs room for both.
        scratch.setSize (2, juce::jmax (block, 4096), false, true, true);
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
        {
            // New session = new tracks; drop any clip state from the
            // previous one so a stale authoritative flag can't silence a
            // freshly loaded track before seedDefaultClips repopulates.
            const juce::ScopedLock sl (clipsLock);
            activeClips.clear();
            clipsAuthoritative.clear();
            extraReaders.clear();   // cross-track clip readers belong to the old session
        }

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

        // CRITICAL: place each file at the track index encoded in its
        // FILENAME (Track_NN -> index N-1), NOT at its position in the sorted
        // file list. Strips can be created but never recorded (no file on
        // disk), so load-order indexing would shift every later file onto the
        // wrong strip -- e.g. importing a stereo track into a session whose
        // earlier mono strips were never recorded made the stereo play out of
        // those mono strips and left the real stereo strip silent. Gaps are
        // filled with empty (silent) tracks so player index == strip index.
        struct Pending
        {
            int  index;   // 0-based strip index (L slot for a stereo file)
            std::shared_ptr<juce::BufferingAudioReader> reader;
            juce::int64 length;
            bool stereo;
        };
        std::vector<Pending> pending;
        int maxIndex = -1;

        // Group every file by its track index. files.sort() already orders a
        // group as main-first then parts ascending ("Track_07.wav" < "..._part02"
        // < "..._part03"), so each group is in playback order. A take split
        // across parts is read back as ONE seamless stream via ConcatReader.
        std::map<int, std::vector<juce::File>> byIndex;
        for (auto& f : files)
        {
            // "Track_04.wav" -> 4 ; "Track_04_part02.wav" -> 4 (getIntValue
            // stops at the underscore). 1-based on disk, so index = num - 1.
            const int num = f.getFileNameWithoutExtension()
                              .fromFirstOccurrenceOf ("Track_", false, false)
                              .getIntValue();
            if (num < 1) continue;
            byIndex[num - 1].push_back (f);
        }

        for (auto& [idx, partFiles] : byIndex)
        {
            std::vector<std::unique_ptr<juce::AudioFormatReader>> partReaders;
            double      fileSR = deviceSampleRate;
            bool        stereo = false;
            juce::int64 takeLen = 0;
            for (auto& pf : partFiles)
            {
                std::unique_ptr<juce::AudioFormatReader> raw (formatManager.createReaderFor (pf));
                if (raw == nullptr) continue;
                if (partReaders.empty()) { fileSR = raw->sampleRate; stereo = raw->numChannels >= 2; }
                takeLen += (juce::int64) raw->lengthInSamples;
                partReaders.push_back (std::move (raw));
            }
            if (partReaders.empty()) continue;

            // One file -> use it directly; many -> stitch with ConcatReader.
            std::unique_ptr<juce::AudioFormatReader> reader;
            if (partReaders.size() == 1) reader = std::move (partReaders.front());
            else                         reader = std::make_unique<ConcatReader> (std::move (partReaders));

            const auto bufferSamples = (int) (fileSR * kReaderBufferSeconds);
            auto buf = std::make_shared<juce::BufferingAudioReader> (reader.release(), readerThread, bufferSamples);
            buf->setReadTimeout (0); // non-blocking -- fill silence if not buffered yet

            pending.push_back ({ idx, std::move (buf), takeLen, stereo });
            maxIndex = juce::jmax (maxIndex, idx + (stereo ? 1 : 0));
            maxLen   = juce::jmax (maxLen, takeLen);
            sr       = fileSR;
        }

        // Build the index-aligned track list. Empty slots (no file) get a
        // default Track (null reader) which processBlock renders as silence.
        tracks.assign ((std::size_t) (maxIndex + 1), Track{});
        for (auto& p : pending)
        {
            if (p.stereo)
            {
                if (p.index + 1 >= (int) tracks.size()) continue;
                // L reads file channel 0, R reads channel 1 (shared reader).
                tracks[(std::size_t) p.index]     = Track { p.reader, p.length, true,  false };
                tracks[(std::size_t) p.index + 1] = Track { p.reader, p.length, false, true  };
            }
            else
            {
                tracks[(std::size_t) p.index] = Track { p.reader, p.length, true, true };
            }
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

        {
            const juce::ScopedLock sl (clipsLock);
            // Clear ALL clip state, not just the cross-track readers -- otherwise
            // a new/empty session inherits the previous session's clips and draws
            // its audio on a freshly-added channel.
            extraReaders.clear();
            activeClips.clear();
            clipsAuthoritative.clear();
        }
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

        // Build readers for any clip that references a file other than its
        // track's own -- OUTSIDE the lock, since opening a file is slow.
        std::vector<std::pair<juce::String, std::unique_ptr<juce::BufferingAudioReader>>> built;
        for (const auto& c : clips)
        {
            if (c.audioFile == juce::File()) continue;
            const auto key = c.audioFile.getFullPathName();
            {
                const juce::ScopedLock sl (clipsLock);
                if (extraReaders.find (key) != extraReaders.end()) continue;
            }
            if (auto* raw = formatManager.createReaderFor (c.audioFile))
            {
                const auto bufSamples = (int) (raw->sampleRate * kReaderBufferSeconds);
                auto br = std::make_unique<juce::BufferingAudioReader> (raw, readerThread, bufSamples);
                br->setReadTimeout (0);
                built.emplace_back (key, std::move (br));
            }
        }

        // Readers whose file no clip references any more (e.g. superseded
        // consolidates) are moved here and destroyed AFTER the lock is
        // released -- tearing down a BufferingAudioReader unregisters it
        // from the read thread, which is too slow to hold the lock for.
        std::vector<std::unique_ptr<juce::BufferingAudioReader>> stale;
        {
            const juce::ScopedLock sl (clipsLock);
            for (auto& b : built)
                if (extraReaders.find (b.first) == extraReaders.end())
                    extraReaders.emplace (b.first, std::move (b.second));
            if (trackIdx >= (int) activeClips.size())        activeClips.resize ((size_t) trackIdx + 1);
            if (trackIdx >= (int) clipsAuthoritative.size()) clipsAuthoritative.resize ((size_t) trackIdx + 1, 0);
            activeClips[(size_t) trackIdx]        = std::move (clips);
            clipsAuthoritative[(size_t) trackIdx] = 1;   // explicit list (even if empty)

            // Prune cross-track readers nothing references across ANY
            // track -- otherwise every consolidate / cross-track paste
            // leaks a buffered reader + file handle until session close.
            for (auto it = extraReaders.begin(); it != extraReaders.end();)
            {
                bool wanted = false;
                for (const auto& list : activeClips)
                {
                    for (const auto& c : list)
                        if (c.audioFile != juce::File()
                            && c.audioFile.getFullPathName() == it->first) { wanted = true; break; }
                    if (wanted) break;
                }
                if (wanted) ++it;
                else { stale.push_back (std::move (it->second)); it = extraReaders.erase (it); }
            }
        }
    }

    void SessionPlayer::clearAllClips()
    {
        const juce::ScopedLock sl (clipsLock);
        activeClips.clear();
        clipsAuthoritative.clear();
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
        // Loop playback only when the ⟲ loop-play toggle is on AND a region is
        // set -- so selecting a range for editing doesn't force play to loop.
        const bool        looping = loopEnabled.load (std::memory_order_relaxed)
                                 && lStart >= 0 && lEnd > lStart;

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
            scratch.setSize (2, playableThisBlock, false, false, true);

        // Snapshot the clip lists under the lock -- held only long enough
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
                && i < (int) clipsAuthoritative.size()
                && clipsAuthoritative[(size_t) i]
                && i < (int) activeClips.size())
            {
                // Authoritative list -- honour it even when empty (every
                // clip deleted => the track is silent, not whole-file).
                clips = &activeClips[(size_t) i];
            }

            if (clips != nullptr)
            {
                juce::FloatVectorOperations::clear (out, numSamples);
                for (const auto& c : *clips)
                {
                    if (c.muted) continue;   // muted clips render silence
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
                        scratch.setSize (2, spanLen, false, false, true);
                    scratch.clear (0, spanLen);   // both channels
                    // Cross-track clips read from their own file's reader
                    // (resolved under the clipsLock we already hold); clips
                    // with no audioFile use the track's default reader.
                    // A stereo file is read in full; readChan picks the L (0)
                    // or R (1) half for THIS logical track. Cross-track clips
                    // reference mono Track files, so they always read ch 0.
                    juce::AudioFormatReader* rd = t.reader.get();
                    int readChan = t.useRight ? 1 : 0;
                    if (c.audioFile != juce::File())
                    {
                        auto it = extraReaders.find (c.audioFile.getFullPathName());
                        if (it != extraReaders.end() && it->second != nullptr)
                        {
                            rd = it->second.get();
                            readChan = 0;
                        }
                    }
                    if (rd != nullptr)
                        rd->read (&scratch, 0, spanLen, fileReadStart, true, true);

                    // Apply linear fade envelopes (if any). The clip-relative
                    // offset of this span's first sample is (ovStartTL -
                    // c.timelineStartSamples); we walk from there.
                    auto* src = scratch.getReadPointer (readChan);
                    auto* dst = out + outOffset;
                    const juce::int64 spanOffsetInClip = ovStartTL - c.timelineStartSamples;
                    const juce::int64 fIn  = c.fadeInSamples;
                    const juce::int64 fOut = c.fadeOutSamples;
                    const juce::int64 fOutStart = c.fileLengthSamples - fOut;
                    const float clipGain = juce::Decibels::decibelsToGain (c.gainDb, -60.0f);
                    if (fIn == 0 && fOut == 0)
                    {
                        if (std::abs (clipGain - 1.0f) < 0.001f)
                        {
                            juce::FloatVectorOperations::copy (dst, src, spanLen);
                        }
                        else
                        {
                            for (int i = 0; i < spanLen; ++i)
                                dst[i] = src[i] * clipGain;
                        }
                    }
                    else
                    {
                        const bool equalPower = (c.fadeCurve == 1);
                        for (int i = 0; i < spanLen; ++i)
                        {
                            const juce::int64 clipPos = spanOffsetInClip + i;
                            float gain = clipGain;
                            if (fIn > 0 && clipPos < fIn)
                            {
                                const float t = (float) clipPos / (float) fIn;
                                gain *= equalPower ? std::sin (t * 1.57079633f) : t;
                            }
                            else if (fOut > 0 && clipPos >= fOutStart)
                            {
                                const float t = (float) (c.fileLengthSamples - clipPos) / (float) fOut;
                                gain *= equalPower ? std::sin (t * 1.57079633f) : t;
                            }
                            dst[i] = src[i] * gain;
                        }
                    }
                }
                if (playableThisBlock < numSamples)
                    juce::FloatVectorOperations::clear (out + playableThisBlock,
                                                        numSamples - playableThisBlock);
                continue;
            }

            // Legacy whole-file path. For a stereo file both channels are
            // read; useRight selects which half routes to this track's out.
            const int avail = (int) juce::jmin ((juce::int64) playableThisBlock, t.length - startPos);

            scratch.clear (0, avail);   // both channels
            t.reader->read (&scratch, 0, avail, startPos, true, true);

            juce::FloatVectorOperations::copy (out, scratch.getReadPointer (t.useRight ? 1 : 0), avail);
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
