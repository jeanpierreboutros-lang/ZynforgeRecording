#include "AudioEngine.h"
#include "SettingsFile.h"
#include "TrackFileTransaction.h"
#include "AtomicFile.h"
#include "SessionMixFile.h"
#include "FastAccumulate.h"
#include "MultiPartReader.h"

namespace zynforge
{
    void AudioEngine::rememberRecentSession (const juce::File& dir)
    {
        if (appProps == nullptr || (! isTakeOnDisk() && ! dir.isDirectory())) return;
        const auto path = dir.getFullPathName();

        // Pull existing list, drop any prior occurrence of this path,
        // push this path on the front, cap at kMaxRecent.
        juce::StringArray entries;
        for (int i = 0; i < kMaxRecent; ++i)
        {
            const auto p = appProps->getValue ("recentSession_" + juce::String (i));
            if (p.isNotEmpty() && p != path) entries.add (p);
        }
        entries.insert (0, path);
        while (entries.size() > kMaxRecent) entries.remove (entries.size() - 1);

        reloadAppPropsBeforeWrite();
        for (int i = 0; i < kMaxRecent; ++i)
            appProps->setValue ("recentSession_" + juce::String (i),
                                i < entries.size() ? entries[i] : juce::String());
        appProps->saveIfNeeded();
    }

    juce::Array<juce::File> AudioEngine::getRecentSessions() const
    {
        juce::Array<juce::File> out;
        if (appProps == nullptr) return out;
        // Skip entries that no longer exist OR live under the temp dir (a
        // scratch / test session) -- those shouldn't show as "recent".
        const auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory);
        for (int i = 0; i < kMaxRecent; ++i)
        {
            const auto p = appProps->getValue ("recentSession_" + juce::String (i));
            if (p.isEmpty()) continue;
            juce::File f (p);
            if (f.isDirectory() && ! f.isAChildOf (tempRoot)) out.add (f);
        }
        return out;
    }

    juce::File AudioEngine::getSessionsRoot() const
    {
        if (appProps != nullptr)
        {
            const auto override_ = appProps->getValue ("sessionsRoot", {});
            if (override_.isNotEmpty())
            {
                juce::File f (override_);
                if (f.isDirectory() || f.createDirectory().wasOk())
                    return f;
            }
        }
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Zynforge Sessions");
    }

    void AudioEngine::resetAllStripState()
    {
        // A live take records from inputRouting and captures stereo pairs by
        // isStereo; resetting them mid-take would change what it records.
        if (isRecording()) return;
        const int n = recorder.getNumTracks();
        // Each domain reloads the shared file once, preserving sibling keys
        // and updating its own cache before the next domain publishes.
        stripNames.clearRange (0, n);
        stripColours.clearRange (0, n);
        stripGains.resetRange (0, n);
        stripRouting.resetRange (0, n);
        for (int i = 0; i < n; ++i)
        {
            // Live atomics so the UI flips back immediately.
            auto& t = recorder.getTrack (i);
            t.gainDb.store (0.0f); t.pan.store (0.0f);
            t.rampTargetGainDb.store (0.0f);
            t.inputRouting.store (i); t.outputRouting.store (-1);
            t.outputBeforeVsc.store (TrackState::kNoSavedOutput);
            t.setNameThreadSafe (juce::String (i + 1));
            t.colourARGB.store (0, std::memory_order_relaxed);
            t.armed   .store (false, std::memory_order_relaxed);
            t.muted   .store (false, std::memory_order_relaxed);
            t.soloed  .store (false, std::memory_order_relaxed);
            t.monitor .store (false, std::memory_order_relaxed);
            t.isStereo.store (false, std::memory_order_relaxed);
            t.editGroup.store (-1);
            t.outputMuted.store (false); t.streamSend.store (false);
            t.liveInputGainDb.store (0); t.captureInputGainDb.store (0);
            t.liveInputGainKnown.store (false); t.captureInputGainKnown.store (false);
            t.referenceMedia.store (false); t.inputPeak.store (0);
            t.rampSamplesRemaining.store (0);
        }
        // Reload AFTER the Strip* writes above so our whole-file save doesn't
        // undo them, THEN apply the appProps-owned stereo flags and save once.
        reloadAppPropsBeforeWrite();
        if (appProps != nullptr)
        {
            for (int i = 0; i < n; ++i)
                appProps->setValue (juce::String ("strip_stereo_") + juce::String (i), false);
            appProps->saveIfNeeded();
        }
    }

    bool AudioEngine::isSampleRateMismatched() const noexcept
    {
        const double target = sessionSampleRate.load (std::memory_order_relaxed);
        if (target <= 0.0) return false;   // no session rate set -> no guard
        if (auto* device = deviceManager.getCurrentAudioDevice())
        {
            const double devSR = device->getCurrentSampleRate();
            return devSR > 0.0 && std::abs (devSR - target) > 1.0;
        }
        return false;
    }

    void AudioEngine::setTrackLiveInputGain (int channelIndex, float dB)
    {
        // Console trim arrives unauthenticated, and with trim-follow on it
        // drives playback gain: +inf / NaN / 200 dB went straight to the PA.
        // Drop non-finite readings; clamp to the travel real desks have, from
        // -96 dB (Yamaha digital-gain floor; attenuation only) to +72 dB (above
        // the widest supported preamp, Yamaha's +66 dB HA).
        if (! std::isfinite (dB)) return;
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        auto& t = recorder.getTrack (channelIndex);
        t.liveInputGainDb.store (juce::jlimit (-96.0f, 72.0f, dB), std::memory_order_relaxed);
        t.liveInputGainKnown.store (true, std::memory_order_relaxed);
        // The reference is fixed at capture start. Zero dB is a valid gain,
        // never a sentinel permitting a console update to rebaseline a take.
    }

    void AudioEngine::snapshotCaptureInputGains()
    {
        const juce::ScopedLock sl (recorder.getStructureLock());
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);
            if (! t.armed.load()) continue;
            t.captureInputGainDb.store (t.liveInputGainDb.load (std::memory_order_relaxed),
                                        std::memory_order_relaxed);
            t.captureInputGainKnown.store (t.liveInputGainKnown.load (std::memory_order_relaxed),
                                           std::memory_order_relaxed);
        }
    }

    int AudioEngine::getTrimFollowUnreferencedCount()
    {
        const juce::ScopedLock sl (recorder.getStructureLock());
        int count = 0;
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);
            if (t.liveInputGainKnown.load (std::memory_order_relaxed)
                && ! t.captureInputGainKnown.load (std::memory_order_relaxed)) ++count;
        }
        return count;
    }

    juce::String AudioEngine::getTrimFollowNotice()
    {
        juce::StringArray noReference, limited;
        {
            const juce::ScopedLock sl (recorder.getStructureLock());
            for (int i = 0; i < recorder.getNumTracks(); ++i)
            {
                auto& t = recorder.getTrack (i);
                const auto name = t.getNameThreadSafe();
                const auto label = name.isNotEmpty() ? name : juce::String (i + 1);
                if (! t.captureInputGainKnown.load (std::memory_order_relaxed))
                {
                    if (t.liveInputGainKnown.load (std::memory_order_relaxed)) noReference.add (label);
                    continue;
                }
                const float move = t.liveInputGainDb.load (std::memory_order_relaxed)
                                 - t.captureInputGainDb.load (std::memory_order_relaxed);
                if (move > mix::kTrimFollowMaxUpDb || move < mix::kTrimFollowMaxDownDb)
                    limited.add (label);
            }
        }
        const auto names = [] (const juce::StringArray& list)
        {
            constexpr int kNamed = 4;
            juce::StringArray shown;
            for (int i = 0; i < juce::jmin (kNamed, list.size()); ++i) shown.add (list[i]);
            auto text = shown.joinIntoString (", ");
            if (list.size() > kNamed) text << " +" << (list.size() - kNamed) << " more";
            return text;
        };
        juce::String notice;
        if (! noReference.isEmpty())
            notice << "Trim-Follow: no capture reference on " << names (noReference)
                   << " -- held at 0 dB offset";
        if (! limited.isEmpty())
            notice << (notice.isEmpty() ? "Trim-Follow: " : "; ")
                   << "move past the " << (int) mix::kTrimFollowMaxUpDb << " dB up / "
                   << (int) -mix::kTrimFollowMaxDownDb << " dB down limit on " << names (limited);
        return notice;
    }

    float AudioEngine::getTrackTrimDelta (int channelIndex)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return 0.0f;
        auto& t = recorder.getTrack (channelIndex);
        return mix::trimFollowDb (t.captureInputGainKnown.load (std::memory_order_relaxed),
                                  t.liveInputGainDb.load (std::memory_order_relaxed),
                                  t.captureInputGainDb.load (std::memory_order_relaxed));
    }

    juce::int64 AudioEngine::getCaptureLengthSamples()
    {
        juce::int64 length = 0;
        for (int i = 0; i < recorder.getNumTracks(); ++i)
            if (! recorder.getTrack (i).referenceMedia.load())
                length = juce::jmax (length, player.getTrackLengthSamples (i));
        return length;
    }

    bool AudioEngine::startRecording (const juce::File& sessionDir)
    {
        // Every refusal here also drops a punch/continue arm the caller set
        // for this start; discardTakeArms is a no-op while a take is live.
        if (isCaptureFinalizing()) { recorder.discardTakeArms(); return false; }
        stereoMixWriteFailed.store (false, std::memory_order_relaxed);
        if (isRecording() || sessionTransitionActive.load (std::memory_order_acquire))
        {
            recorder.discardTakeArms();
            return false;
        }
        // Bypass-proof guard: refuse to capture if the device clock disagrees
        // with the session rate (silent wrong-speed/pitch take). Covers every
        // caller -- record button, transport bar, punch auto-record, remotes.
        if (isSampleRateMismatched())
        {
            recorder.discardTakeArms();
            DBG ("startRecording refused: sample-rate mismatch (device "
                 << (deviceManager.getCurrentAudioDevice()
                        ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 0.0)
                 << " vs session " << sessionSampleRate.load (std::memory_order_relaxed) << ")");
            return false;
        }

        // Here and not on a button, because PUNCH mode, a rolling press and a
        // selection all end up at this one door.
        if (recorder.isPunchInArmed())
        {
            const auto refusal = punchRefusal (recorder.getRecordBaseSamples(),
                                               isCaptureWindowActive()
                                                   ? captureWindowEnd.load (std::memory_order_relaxed) : -1);
            if (refusal.isNotEmpty())
            {
                recorder.refuseStart (refusal);
                return false;
            }
        }

        bwf::TimeAnchor timecode;
        if (getTimecodeAnchor (timecode)) recorder.setTimeReferenceHint (timecode);
        else                              recorder.clearTimeReferenceHint();
        recorder.setClockDevice (deviceManager.getAudioDeviceSetup().inputDeviceName);

        // Both of these read or write the session's volume, so they go first:
        // once the take rolls this thread is STOP and must not touch that volume.
        double sr = 48000.0;
        if (auto* device = deviceManager.getCurrentAudioDevice())
            sr = device->getCurrentSampleRate();
        const auto markerSession = markers.getSessionDir();
        const auto markerRate    = markers.getSampleRate();
        markers.setContext (sessionDir, sr);
        auto mix = openStereoMix (sessionDir, sr);

        if (! recorder.startRecording (sessionDir))
        {
            markers.setContext (markerSession, markerRate);
            if (mix.writer != nullptr)
            {
                mix.writer.reset();
                mix.file.deleteFile();
            }
            return false;
        }
        if (recordStereoMixFlag.load (std::memory_order_acquire))
        {
            stereoMixWriteFailed.store (mix.writer == nullptr, std::memory_order_relaxed);
            const juce::ScopedLock sl (stereoMixLock);
            stereoMixWriter = std::move (mix.writer);
        }

        continueFileBase = recorder.isContinueArmed() ? recorder.getRecordBaseSamples() : 0;
        continueShift.store (recorder.isContinueArmed() && player.isLoaded()
                                 ? continueFileBase - continueLandsAt() : 0,
                             std::memory_order_release);

        // These strips are what this take was recorded with, so from here on
        // they are this folder's mixer, whichever session they came from.
        claimMixerFor (sessionDir);
        noteLastCapture (sessionDir);
        invalidateExternalCaptureStatus();
        // A punch take's capture window and splice point are player positions:
        // hold the playhead (and loop) still until the take stops.
        player.setSeekLocked (recorder.isPunchInArmed());

        // A take makes the live capture position authoritative -- drop any
        // sticky EDIT cursor left from soundcheck so markers land at the take.
        clearEditCursor();

        // Trim-follow: snapshot each channel's current console input gain as the
        // capture reference AFTER the take actually starts. Doing it before the
        // guards above meant a refused start (sample-rate mismatch, or a double
        // record-start from a re-sent OSC/tablet command) still re-baselined the
        // references mid-take, so trim-follow deltas were measured wrong for the
        // rest of the session.
        snapshotCaptureInputGains();

        rememberRecentSession (sessionDir);
        return true;
    }

    AudioEngine::StereoMixFile AudioEngine::openStereoMix (const juce::File& sessionDir, double sr)
    {
        StereoMixFile mix;
        if (! recordStereoMixFlag.load (std::memory_order_acquire)) return mix;

        if (! mixWriterThread.isThreadRunning())
            mixWriterThread.startThread();

        // Stereo bus mix lands in Export Files/ -- the location for any
        // rendered mix / stem export.
        auto exportDir = sessionDir.getChildFile ("Export Files");
        if (! exportDir.isDirectory() && ! exportDir.createDirectory().wasOk()) return mix;

        // NEVER overwrite a previous take's mix. startRecording runs again
        // for every continue / second take into the SAME session, and the
        // old code deleteFile()d StereoMix.wav each time -- silently
        // destroying the earlier take's archive. Number subsequent takes
        // (StereoMix.wav, StereoMix_02.wav, ...); the first take keeps the
        // historic name so existing workflows are unchanged.
        auto path = exportDir.getChildFile ("StereoMix.wav");
        for (int take = 2; path.existsAsFile(); ++take)
            path = exportDir.getChildFile ("StereoMix_"
                                           + juce::String (take).paddedLeft ('0', 2) + ".wav");

        juce::WavAudioFormat wav;
        if (auto* out = path.createOutputStream().release())
        {
            juce::StringPairArray meta;
            if (auto* w = wav.createWriterFor (out, sr, 2, 24, meta, 0))
            {
                mix.file = path;
                mix.writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>
                                 (w, mixWriterThread, 32768);
            }
            else
            {
                delete out;
            }
        }
        return mix;
    }

    void AudioEngine::stopRecording()
    {
        if (isCaptureFinalizing()) return;
        // Fail closed when not recording. Otherwise a stray transport-stop
        // from a network source (OSC/console recordStop, a tablet double-tap)
        // would still run the whole tail below -- reload the last-recorded
        // session, reseed clips, yank the playhead -- destroying clip
        // edits on a session that is merely idle or playing back. The UI STOP
        // button is already gated, but the engine entry point was not.
        if (! recorder.isRecording()) return;

        const auto sessionDir = recorder.getActiveSessionDir();
        // Where the take actually ends, captured BEFORE stopRecording()
        // resets the recorder's base offset to 0. Live multitrack recorders
        // (unlike a DAW, whose Stop snaps the edit cursor back to where the
        // pass began) leave the transport parked at the END of what was just
        // captured, so the next RECORD continues the take from here instead
        // of looking like it starts over from the beginning.
        const auto recordEnd = getRecordTimelineSamples();
        recorder.stopRecording();
        clearCaptureWindow();
        player.setSeekLocked (false);
        // Drop the threaded writer -- its destructor flushes the queue and
        // closes the underlying AudioFormatWriter cleanly. Hold stereoMixLock
        // so the audio thread can't be mid check-and-write against the writer
        // we're destroying (it uses a ScopedTryLock around that region).
        {
            const juce::ScopedLock sl (stereoMixLock);
            stereoMixWriter.reset();
        }

        // Auto-load the just-recorded session into the SessionPlayer so
        // PLAY / spacebar can play back immediately without an explicit
        // 'open session' step. Skip if the dir somehow vanished.
        if (sessionDir.isDirectory())
        {
            player.loadSession (sessionDir);
            setActiveSessionDir (sessionDir);
            // Preserve every take / split / fade the engineer made
            // during soundcheck. A stop after a punch or continue must not
            // wipe edits on OTHER tracks; only the just-captured track's
            // plain default clip is refreshed to the new take length.
            seedDefaultClips (/*preserveEdits*/ true, /*appendRecordedAudio*/ true);
            // loadSession() rewinds the player to 0; park the playhead back
            // at the take's end so the engineer can hit RECORD again and
            // continue seamlessly (or PLAY to review from this point).
            player.setPositionSamples (juce::jlimit ((juce::int64) 0,
                                                     player.getTotalLengthSamples(),
                                                     recordEnd));
        }
    }

    bool AudioEngine::stopRecordingAsync (std::function<void (bool)> completion)
    {
        const std::lock_guard<std::mutex> lifecycle (deviceLifecycleLock);
        if (isCaptureFinalizing() || ! recorder.beginFinalization()) return false;
        launchCaptureStopJob (std::move (completion));
        return true;
    }

    // The caller holds deviceLifecycleLock and the recorder's finalization lease.
    void AudioEngine::launchCaptureStopJob (std::function<void (bool)> completion,
                                            const juce::File& externalSession, juce::int64 externalEnd)
    {
        asyncCaptureFinalizing.store (true, std::memory_order_release);
        auto job = std::make_unique<CaptureStopJob>();
        job->external = externalSession != juce::File();
        job->directory = job->external ? externalSession : recorder.getActiveSessionDir();
        job->recordEnd = job->external ? externalEnd : getRecordTimelineSamples();
        job->sameSession = ! job->external || player.getSessionDir() == externalSession;
        job->playbackRate = deviceSampleRate.load();
        job->playbackBlock = deviceBlockSize.load();
        job->explicitReaders = player.snapshotExplicitReaderRequests();
        job->completion = std::move (completion);
        job->id = ++captureStopSequence;
        clearCaptureWindow();
        player.setSeekLocked (false);
        player.stop();
        {
            const juce::ScopedLock sl (stereoMixLock);
            job->mixWriter = std::move (stereoMixWriter);
        }
        captureStopJob = std::move (job);
        auto* work = captureStopJob.get();
        const auto handle = asyncHandle;
        work->worker = std::thread ([this, work, handle]
        {
            if (work->external)
            {
                // A daemon that died left this unfinished, and nothing else
                // here will read the session before the player does.
                const auto audioDir = work->directory.getChildFile ("Audio Files");
                MultitrackRecorder::recoverInterruptedPunches (audioDir.isDirectory() ? audioDir : work->directory);
            }
            else
                recorder.finishFinalization();
            work->mixWriter.reset();
            work->prepared = player.prepareSessionLoad (work->directory, work->playbackRate,
                                                        work->playbackBlock, work->explicitReaders);
            work->ok = SessionPlayer::preparedLoadComplete (work->prepared)
                && (work->external
                    || (! recorder.hasPrimaryFailed() && ! recorder.hasCaptureDeviceLost()
                        && ! recorder.hasBackupFailed() && ! recorder.anyMirrorFailed()
                        && recorder.getMirrorsSkippedAtStart() == 0 && ! recorder.hasRecoveryMarkerFailed()
                        && ! recorder.hasReportWriteFailed() && ! recorder.hasPunchSpliceFailed()
                        && ! hasStereoMixWriteFailed()));
            const auto id = work->id;
            juce::MessageManager::callAsync ([handle, id]
            {
                if (auto* engine = handle->load (std::memory_order_acquire)) engine->installFinalizedCapture (id);
            });
        });
    }

    bool AudioEngine::finishExternalTakeAsync (const juce::File& sessionDir, juce::int64 recordEnd,
                                               std::function<void (bool)> completion)
    {
        const std::lock_guard<std::mutex> lifecycle (deviceLifecycleLock);
        if (isCaptureFinalizing()) return false;
        launchCaptureStopJob (std::move (completion), sessionDir, recordEnd);
        return true;
    }

    void AudioEngine::installFinalizedCapture (juce::uint64 id)
    {
        if (! captureStopJob || captureStopJob->id != id) return;
        auto& job = *captureStopJob;
        // The worker has completed every disk operation before posting here.
        // Joining only its return tail establishes ownership of the result.
        if (job.worker.joinable()) job.worker.join();
        job.retired = player.installPreparedSession (std::move (job.prepared), true);
        activeSession = job.directory;
        updateSettings (appProps.get(), [this] (juce::PropertiesFile& props)
                        { props.setValue ("activeSessionDir", activeSession.getFullPathName()); });
        // A daemon RECORD opens a new folder: clips still in memory belong to
        // the session before it.
        seedDefaultClips (job.sameSession, job.sameSession);
        if (job.external)
        {
            markers.setContext (job.directory, player.getSampleRate());
            rememberRecentSession (job.directory);
        }
        player.finishPreparedClipPublication();
        player.setPositionSamples (juce::jlimit ((juce::int64) 0, player.getTotalLengthSamples(), job.recordEnd));
        auto* work = &job;
        const auto handle = asyncHandle;
        job.worker = std::thread ([work, handle]
        {
            work->retired.reset();
            const auto jobId = work->id;
            juce::MessageManager::callAsync ([handle, jobId]
            {
                if (auto* engine = handle->load (std::memory_order_acquire)) engine->completeFinalizedCapture (jobId);
            });
        });
    }

    void AudioEngine::completeFinalizedCapture (juce::uint64 id)
    {
        if (! captureStopJob || captureStopJob->id != id) return;
        if (captureStopJob->worker.joinable()) captureStopJob->worker.join();
        auto completion = std::move (captureStopJob->completion);
        const bool ok = captureStopJob->ok;
        {
            const std::lock_guard<std::mutex> lifecycle (deviceLifecycleLock);
            recorder.endFinalization();
            if (deferredDeviceRelease) recorder.release();
            deferredDeviceRelease = false;
            if (deferredDeviceConfiguration)
            {
                const auto configuration = *deferredDeviceConfiguration;
                deferredDeviceConfiguration.reset();
                applyDeviceConfiguration (configuration.rate, configuration.block, configuration.workgroup);
            }
            asyncCaptureFinalizing.store (false, std::memory_order_release);
            deferredDeviceAudio.store (false, std::memory_order_release);
        }
        captureStopJob.reset();
        if (completion) completion (ok);
    }

    bool AudioEngine::performRemoteTransport (RemoteTransportAction action,
                                              juce::String& error)
    {
        error.clear();
        if (areControlsLocked()) { error = "session is locked; unlock on the host"; return false; }

        // MainComponent installs this for both local and daemon capture.
        // std::nullopt means "use the ordinary in-process implementation";
        // true/false means the host handled (or explicitly refused) it.
        if (remoteTransportHandler)
            if (auto handled = remoteTransportHandler (action, error))
                return *handled;

        switch (action)
        {
            case RemoteTransportAction::TogglePlay:
                if (player.isPlaying()) stopPlayback(); else startPlayback();
                return true;
            case RemoteTransportAction::StartPlay:
                startPlayback();
                return true;
            case RemoteTransportAction::StopPlay:
                stopPlayback();
                return true;
            case RemoteTransportAction::StopAll:
                if (isRecording()) break; // no host: cannot bypass two-tap STOP
                stopPlayback();
                player.rewind();
                return true;
            case RemoteTransportAction::ToggleRecord:
            case RemoteTransportAction::StartRecord:
            case RemoteTransportAction::StopRecord:
                break;
        }
        error = "record transport requires host pre-flight and STOP confirmation";
        return false;
    }

    void AudioEngine::setMasterGainDb (float dB)
    {
        masterState.gainDb.store (dB, std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("masterGainDb", (double) dB);
            appProps->saveIfNeeded();
        }
    }
    void AudioEngine::setMasterMuted (bool m)
    {
        masterState.muted.store (m, std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("masterMuted", m);
            appProps->saveIfNeeded();
        }
    }
    void AudioEngine::setMasterOutputs (int l, int r)
    {
        masterOutL.store (l, std::memory_order_relaxed);
        masterOutR.store (r, std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("masterOutL", l);
            appProps->setValue ("masterOutR", r);
            appProps->saveIfNeeded();
        }
    }
    void AudioEngine::setMixMayShareStripOutputs (bool allowed)
    {
        mixMaySharePatchedOutputs.store (allowed, std::memory_order_relaxed);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("mixMaySharePatchedOutputs", allowed);
            appProps->saveIfNeeded();
        }
    }

    juce::String AudioEngine::getBusRoutingNotice() const
    {
        const auto outs = [] (int l, int r)
        {
            return "Out " + juce::String (l + 1) + (r != l ? "-" + juce::String (r + 1) : juce::String());
        };
        juce::String notice;
        if (isMasterHeldOff())
            notice << "Master mix is off " << outs (getMasterOutputL(), getMasterOutputR())
                   << ": a strip is patched there. Pick a spare pair on the master strip";
        if (isStreamHeldOff())
            notice << (notice.isEmpty() ? "" : "; ")
                   << "Stream bus is off " << outs (getStreamOutputL(), getStreamOutputR())
                   << ": a strip is patched there";
        return notice;
    }

    void AudioEngine::setMasterStereo (bool s)
    {
        masterStereo.store (s, std::memory_order_release);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("masterStereo", s);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setLtcSourceStrip (int oneBasedIndex) noexcept
    {
        ltcSourceStrip.store (oneBasedIndex - 1, std::memory_order_release);
        if (oneBasedIndex <= 0) timecodeChase.reset();
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("ltcSourceStrip", oneBasedIndex);
            appProps->saveIfNeeded();
        }
    }

    juce::StringArray AudioEngine::getMidiInputNames() const
    {
        juce::StringArray names;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            names.add (d.name);
        return names;
    }

    bool AudioEngine::setMtcInputByName (const juce::String& name)
    {
        if (mtcInput != nullptr) { mtcInput->stop(); mtcInput.reset(); }
        mtcInputName.clear();
        if (name.isEmpty()) return true;   // closed = success

        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name == name)
            {
                mtcInput = juce::MidiInput::openDevice (d.identifier, this);
                if (mtcInput != nullptr) { mtcInput->start(); mtcInputName = name; return true; }
                return false;
            }
        return false;
    }

    void AudioEngine::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
    {
        // Decoded on the MIDI thread -> TimecodeChase atomics (no locks). The
        // message-thread chase poll reads getChaseTargetSamples() to seek.
        // Only while MTC is the chase source: LTC shares the time readout.
        if (getChaseMode() != ChaseMode::Mtc) return;
        if (m.isQuarterFrame())
            timecodeChase.feedMtcQuarterFrame (m.getRawData()[1]);
        else if (m.isFullFrame())
        {
            int h, mn, s, f;
            juce::MidiMessage::SmpteTimecodeType tc;
            m.getFullFrameParameters (h, mn, s, f, tc);
            const float fps = (tc == juce::MidiMessage::fps24)     ? 24.0f
                            : (tc == juce::MidiMessage::fps25)     ? 25.0f
                            : (tc == juce::MidiMessage::fps30drop) ? 29.97f : 30.0f;
            timecodeChase.feedMtcFullFrame ((juce::uint8) h, (juce::uint8) mn,
                                            (juce::uint8) s, (juce::uint8) f,
                                            fps, tc == juce::MidiMessage::fps30drop);
        }
    }

    juce::int64 AudioEngine::getChaseTargetSamples() const
    {
        double fps = timecodeChase.getFps();
        if (fps <= 0.0) fps = 30.0;   // not yet learned -> sane default
        const double rate = player.getSampleRate() > 0.0 ? player.getSampleRate()
                          : (deviceManager.getCurrentAudioDevice() != nullptr
                                ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate() : 48000.0);
        const auto start = chaseStartLabel.load (std::memory_order_relaxed);
        double secs = timecodeChase.timeOfDaySeconds (0)
                    - ((double) (start >> 24) * 3600.0 + (double) ((start >> 16) & 0xFF) * 60.0
                       + (double) ((start >> 8) & 0xFF) + (double) (start & 0xFF) / fps);
        // House timecode wraps at midnight; a show running across it is still
        // the same show.
        constexpr double kDay = 24.0 * 3600.0;
        if (secs < -kDay / 2.0) secs += kDay;
        return (juce::int64) std::floor (secs * rate + 0.5);
    }

    juce::String AudioEngine::getChaseStartTimecode() const
    {
        const auto start = chaseStartLabel.load (std::memory_order_relaxed);
        return juce::String::formatted ("%02d:%02d:%02d:%02d", (int) (start >> 24), (int) ((start >> 16) & 0xFF),
                                        (int) ((start >> 8) & 0xFF), (int) (start & 0xFF));
    }

    bool AudioEngine::setChaseStartTimecode (const juce::String& hhmmssff)
    {
        juce::StringArray fields;
        fields.addTokens (hhmmssff.trim(), ":;.", {});
        if (fields.size() != 4) return false;
        int value[4] {};
        for (int i = 0; i < 4; ++i)
        {
            if (fields[i].isEmpty() || ! fields[i].containsOnly ("0123456789")) return false;
            value[i] = fields[i].getIntValue();
        }
        if (value[0] > 23 || value[1] > 59 || value[2] > 59 || value[3] > 29) return false;
        chaseStartLabel.store (((juce::uint32) value[0] << 24) | ((juce::uint32) value[1] << 16)
                                   | ((juce::uint32) value[2] << 8) | (juce::uint32) value[3],
                               std::memory_order_relaxed);
        return true;
    }

    bool AudioEngine::setChaseStartFromTake()
    {
        const auto folder = getActiveSessionDir().getChildFile ("Audio Files");
        auto files = folder.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles, false, "Track_*");
        files.sort();
        const double rate = player.getSampleRate();
        double fps = timecodeChase.getFps();
        if (fps <= 0.0) fps = 30.0;
        if (rate <= 0.0) return false;
        for (const auto& file : files)
        {
            juce::uint64 stamp = 0;
            if (file.getFileName().contains (".punchbase") || mainTakeTrackNumber (file) <= 0
                || bwf::readTimeReference (file, stamp) != bwf::StampResult::ok) continue;
            const double seconds = (double) stamp / rate;
            const int whole = (int) seconds;
            return setChaseStartTimecode (juce::String::formatted ("%02d:%02d:%02d:%02d",
                (whole / 3600) % 24, (whole / 60) % 60, whole % 60,
                juce::jmin (29, (int) ((seconds - whole) * fps))));
        }
        return false;
    }

    juce::String AudioEngine::getChaseParkReason() const
    {
        const auto total  = player.getTotalLengthSamples();
        const auto target = getChaseTargetSamples();
        if (target >= 0 && target < total) return {};
        if (total <= 0) return "Chase is holding: no audio is loaded";
        return "Chase is holding: timecode " + getChaseTimecodeString()
             + (target < 0 ? " is before the session start (" : " is past the end of the session (starts at ")
             + getChaseStartTimecode() + "). Set the start in Transport Sync";
    }

    bool AudioEngine::getTimecodeAnchor (bwf::TimeAnchor& anchor) const
    {
        if (! isChaseLive()) return false;
        const bool ltc = getChaseMode() == ChaseMode::Ltc;
        anchor = { timecodeChase.timeOfDaySeconds (ltc ? 1 : 2),
                   juce::Time::getHighResolutionTicks(), ltc ? "ltc" : "mtc" };
        return true;
    }

    clocksource::Reading AudioEngine::readClockSource (int timeoutMs)
    {
        return recorder.readClockSource (deviceManager.getAudioDeviceSetup().inputDeviceName, timeoutMs);
    }

    juce::String AudioEngine::getChaseTimecodeString() const
    {
        return juce::String::formatted ("%02d:%02d:%02d:%02d",
                                        timecodeChase.getHours(), timecodeChase.getMinutes(),
                                        timecodeChase.getSeconds(), timecodeChase.getFrames());
    }

    void AudioEngine::setRecordStereoMix (bool enabled)
    {
        recordStereoMixFlag.store (enabled, std::memory_order_release);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("recordStereoMix", enabled);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setAutoArmOnInputDetect (bool on)
    {
        autoArmOnInputFlag.store (on, std::memory_order_release);
        recorder.setMeterUnarmedInputs (on);
        if (! on) std::fill (autoArmStreaks.begin(), autoArmStreaks.end(), 0);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("autoArmOnInput", on);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setSceneFollowEnabled (bool on)
    {
        sceneFollowFlag.store (on, std::memory_order_release);
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("sceneFollow", on);
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::serviceAutoArm (int periodTicks, float ampThreshold)
    {
        if (! autoArmOnInputFlag.load (std::memory_order_acquire)) return;
        if (isRecording() || isCaptureFinalizing() || areControlsLocked()) return;

        const int n = recorder.getNumTracks();
        if ((int) autoArmStreaks.size() < n)
            autoArmStreaks.resize ((size_t) n, 0);

        for (int i = 0; i < n; ++i)
        {
            auto& t = recorder.getTrack (i);
            if (t.armed.load (std::memory_order_relaxed)) { autoArmStreaks[(size_t) i] = 0; continue; }

            const float pk = t.inputPeak.load (std::memory_order_relaxed);
            if (pk >= ampThreshold)
                ++autoArmStreaks[(size_t) i];
            else
                autoArmStreaks[(size_t) i] = 0;

            if (autoArmStreaks[(size_t) i] >= periodTicks)
            {
                setTrackArmed (i, true);
                autoArmStreaks[(size_t) i] = 0;
            }
        }
    }

    bool AudioEngine::setMirrors (const std::vector<MultitrackRecorder::MirrorConfig>& configs)
    {
        // The recorder REFUSES a mirror change mid-take. Persisting one it
        // refused left the settings file claiming a redundancy the live
        // recorder wasn't providing: the engineer adds a mirror drive during
        // the show, the dialog closes without a word, nothing mirrors, and
        // after the next restart it "works" -- which reads as a flake rather
        // than a rule. Report the refusal instead, and write nothing.
        if (! recorder.setMirrors (configs)) return false;
        if (appProps == nullptr) return true;

        reloadAppPropsBeforeWrite();
        // Wipe any prior persisted mirror slots so we don't leave a
        // stale entry behind when the engineer reduces the count.
        for (int i = 0; i < 16; ++i)
        {
            appProps->removeValue ("mirror_root_"   + juce::String (i));
            appProps->removeValue ("mirror_format_" + juce::String (i));
        }
        appProps->setValue ("mirror_count", (int) configs.size());
        for (size_t i = 0; i < configs.size() && i < 16; ++i)
        {
            appProps->setValue ("mirror_root_"   + juce::String ((int) i),
                                configs[i].root.getFullPathName());
            appProps->setValue ("mirror_format_" + juce::String ((int) i),
                                (int) configs[i].format);
        }
        appProps->saveIfNeeded();
        return true;
    }

    int AudioEngine::loadSession (const juce::File& sessionDir, bool preserveEdits, bool appendRecordedAudio)
    {
        if (isCaptureFinalizing()) return 0;
        if (! TrackFileTransaction::recover (sessionDir)) return -1;
        const auto audioDir = sessionDir.getChildFile ("Audio Files");
        if (! MultitrackRecorder::recoverInterruptedPunches (
                audioDir.isDirectory() ? audioDir : sessionDir)) return -1;
        const auto n = player.loadSession (sessionDir);
        if (n > 0)
        {
            // Loading through the engine is a session switch, not a temporary
            // player preview. Keep the explicit pin in sync so subsequent
            // Save/Export/edit operations cannot continue targeting the
            // previously active session.
            setActiveSessionDir (sessionDir);
            markers.setContext (sessionDir, player.getSampleRate());
            rememberRecentSession (sessionDir);
            seedDefaultClips (preserveEdits, appendRecordedAudio);
        }
        return n;
    }

    // Resolve a strip's stereo partner: L (isStereo) -> N+1, R -> N-1.
    // Called with the recorder's structure lock already held.
    static int stereoPartnerIndexLocked (MultitrackRecorder& rec, int index)
    {
        if (index >= 0 && index + 1 < rec.getNumTracks()
            && rec.getTrack (index).isStereo.load (std::memory_order_relaxed))
            return index + 1;
        if (index > 0
            && rec.getTrack (index - 1).isStereo.load (std::memory_order_relaxed))
            return index - 1;
        return -1;
    }

    template <typename Member>
    static bool applyToPair (MultitrackRecorder& rec, int index, Member member,
                             bool value, const juce::CriticalSection& lock)
    {
        const juce::ScopedLock sl (lock);
        if (index < 0 || index >= rec.getNumTracks()) return false;
        (rec.getTrack (index).*member).store (value, std::memory_order_relaxed);
        const int partner = stereoPartnerIndexLocked (rec, index);
        if (partner >= 0 && partner < rec.getNumTracks())
            (rec.getTrack (partner).*member).store (value, std::memory_order_relaxed);
        return true;
    }

    bool AudioEngine::setTrackMuted (int i, bool v)
    { return applyToPair (recorder, i, &TrackState::muted,  v, recorder.getStructureLock()); }
    bool AudioEngine::setTrackSoloed (int i, bool v)
    { return applyToPair (recorder, i, &TrackState::soloed, v, recorder.getStructureLock()); }
    bool AudioEngine::setTrackArmed (int i, bool v)
    {
        // Test the take state UNDER the structure lock. startRecording holds it
        // while it snapshots the arms, so a remote arm racing a start either
        // lands before the snapshot or is refused -- never silently after it,
        // where the R button would light for a track that is not recording.
        const juce::ScopedLock sl (recorder.getStructureLock());
        return ! isRecording() && applyToPair (recorder, i, &TrackState::armed, v, recorder.getStructureLock());
    }

    template <typename Member>
    static bool togglePair (MultitrackRecorder& rec, int index, Member member,
                            const juce::CriticalSection& lock)
    {
        const juce::ScopedLock sl (lock);
        if (index < 0 || index >= rec.getNumTracks()) return false;
        const bool next = ! (rec.getTrack (index).*member).load (std::memory_order_relaxed);
        (rec.getTrack (index).*member).store (next, std::memory_order_relaxed);
        const int partner = stereoPartnerIndexLocked (rec, index);
        if (partner >= 0 && partner < rec.getNumTracks())
            (rec.getTrack (partner).*member).store (next, std::memory_order_relaxed);
        return next;
    }

    bool AudioEngine::toggleTrackMuted (int i)
    { return togglePair (recorder, i, &TrackState::muted,  recorder.getStructureLock()); }
    bool AudioEngine::toggleTrackSoloed (int i)
    { return togglePair (recorder, i, &TrackState::soloed, recorder.getStructureLock()); }
    bool AudioEngine::toggleTrackArmed (int i)
    { return ! isRecording() && togglePair (recorder, i, &TrackState::armed, recorder.getStructureLock()); }

    void AudioEngine::setExternalRecording (bool active) noexcept
    {
        if (! active)
        {
            std::lock_guard<std::mutex> lock (externalStatusLock);
            if (! externalStatus.captureDeviceLost) externalStatusAtMs = 0;
        }
        externalRecording.store (active, std::memory_order_release);
    }

    void AudioEngine::beginExternalTake (const juce::File& sessionDir, double sampleRate)
    {
        setExternalRecording (true);
        setActiveSessionDir (sessionDir);
        if (sessionDir == juce::File()) return;
        // The window loads these before asking the daemon to roll, off this
        // take's clock. Only a reattach that found the take running lands here.
        if (markers.getSessionDir() != sessionDir) markers.setContext (sessionDir, sampleRate);
        clearEditCursor();
    }

    void AudioEngine::setExternalCaptureStatus (const EngineStatus& status, juce::int64 receivedAtMs)
    {
        std::lock_guard<std::mutex> lock (externalStatusLock);
        externalStatus = status;
        externalStatusAtMs = receivedAtMs != 0 ? receivedAtMs : juce::Time::currentTimeMillis();
    }

    void AudioEngine::invalidateExternalCaptureStatus()
    {
        std::lock_guard<std::mutex> lock (externalStatusLock);
        externalStatusAtMs = 0;
    }

    EngineStatus AudioEngine::captureStatus()
    {
        constexpr juce::uint32 kDefaultSwatch = 0xff3a3f44;
        EngineStatus status;
        juce::int64 receivedAt;
        {
            std::lock_guard<std::mutex> lock (externalStatusLock);
            status = externalStatus;
            receivedAt = externalStatusAtMs;
        }
        if (externalRecording.load (std::memory_order_acquire)
            || (receivedAt != 0 && status.captureDeviceLost))
        {
            const auto age = juce::Time::currentTimeMillis() - receivedAt;
            if (receivedAt != 0 && age >= 0 && (status.captureDeviceLost || age <= 2000))
            {
                status.source = "daemon";
                status.statusAgeMs = juce::jmax ((juce::int64) 0,
                    juce::Time::currentTimeMillis() - receivedAt);
                // The daemon owns the capture meters and writer health, but
                // mute/solo/name/colour are controlled by this GUI. Overlay
                // those fields so the snapshot reflects their true state.
                const juce::ScopedLock sl (recorder.getStructureLock());
                const int count = juce::jmin ((int) status.tracks.size(), recorder.getNumTracks());
                for (int i = 0; i < count; ++i)
                {
                    const auto& t = recorder.getTrack (i);
                    auto& ts = status.tracks[(size_t) i];
                    ts.name = t.getNameThreadSafe();
                    ts.muted = t.muted.load (std::memory_order_relaxed);
                    ts.soloed = t.soloed.load (std::memory_order_relaxed);
                    ts.monitor = t.monitor.load (std::memory_order_relaxed);
                    ts.stereoLeft = t.isStereo.load (std::memory_order_relaxed);
                    const auto argb = (juce::uint32) t.colourARGB.load (std::memory_order_relaxed);
                    ts.colourARGB = argb != 0 ? argb : kDefaultSwatch;
                }
                return status;
            }
            EngineStatus unavailable;
            unavailable.source = "daemon-unavailable";
            unavailable.statusAgeMs = -1;
            unavailable.recording = true;
            unavailable.primaryDiskStale = true;
            return unavailable;
        }
        // Metrics also traverse recorder shards and tracks. Protect the entire
        // snapshot, not just the final per-track walk, against structural edits.
        const juce::ScopedLock sl (recorder.getStructureLock());
        EngineStatus s;
        s.captureDeviceLost = recorder.hasCaptureDeviceLost();
        s.recording        = isRecording();
        if (s.recording)
            s.sessionPath = recorder.getActiveSessionDir().getFullPathName();
        s.playing          = player.isPlaying();
        s.positionSamples  = player.getPositionSamples();
        s.elapsedSamples   = recorder.getSamplesSinceStart();
        s.sampleRate       = getDeviceSampleRate();
        s.blockSize        = getDeviceBlockSize();
        s.audioLoadPct     = getAudioLoadPct();
        s.ringFillPct      = (float) getRingFillPct();
        s.diskMBPerSec     = (double) getDiskMBPerSec();
        s.lastWriteMs      = recorder.getLastWriteMs();
        const auto disk    = recorder.readDiskEstimate();
        s.minutesRemaining = disk.watching ? disk.minutes
                                           : diskMinutesRemaining.load (std::memory_order_relaxed);
        s.primaryFreeBytes = disk.primaryFreeBytes;
        s.primaryDiskStale = disk.primaryStale;
        s.copyDiskStale    = disk.copyStale;
        s.missedSamples    = recorder.getMissedSamples();
        s.integratedLufs   = getIntegratedLufs();
        s.momentaryLufs    = getMomentaryLufs();
        s.truePeakDb       = getTruePeakDb();
        s.numTracks        = recorder.getNumTracks();
        s.backupActive     = recorder.isBackupActive();
        s.primaryFailed    = recorder.hasPrimaryFailed();
        s.backupFailed     = recorder.hasBackupFailed();
        s.mirrorFailed     = recorder.anyMirrorFailed();
        s.mirrorsSkipped   = recorder.getMirrorsSkippedAtStart();
        s.recoveryMarkerFailed = recorder.hasRecoveryMarkerFailed();
        s.reportWriteFailed = recorder.hasReportWriteFailed();
        s.stereoMixFailed = hasStereoMixWriteFailed();
        s.diskStruggling   = recorder.isDiskStruggling();
        s.captureFormat    = (int) recorder.getCaptureFormat();
        s.channelAlarms    = recorder.evaluateChannelAlarms();
        s.copyProblems     = recorder.getCopyProblems();

        s.tracks.reserve ((size_t) s.numTracks);
        for (int i = 0; i < s.numTracks; ++i)
        {
            auto& t = recorder.getTrack (i);
            TrackStatus ts;
            ts.name       = t.getNameThreadSafe();   // locked: read off the message thread too
            ts.peak       = t.peak  .load (std::memory_order_relaxed);
            ts.rms        = t.rms   .load (std::memory_order_relaxed);
            ts.armed      = t.armed .load (std::memory_order_relaxed);
            ts.muted      = t.muted .load (std::memory_order_relaxed);
            ts.soloed     = t.soloed.load (std::memory_order_relaxed);
            ts.monitor    = t.monitor.load (std::memory_order_relaxed);
            ts.stereoLeft = t.isStereo.load (std::memory_order_relaxed);
            const auto argb = (juce::uint32) t.colourARGB.load (std::memory_order_relaxed);
            ts.colourARGB = argb != 0 ? argb : kDefaultSwatch;
            if (ts.armed) ++s.armedTracks;
            s.tracks.push_back (std::move (ts));
        }
        return s;
    }

    juce::String AudioEngine::captureSessionMixJson()
    {
        const juce::ScopedLock structure (recorder.getStructureLock());

        juce::Array<juce::var> arr;
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);
            juce::DynamicObject::Ptr o (new juce::DynamicObject());
            o->setProperty ("index",     i);
            o->setProperty ("uid",       t.stripId);
            o->setProperty ("name",      t.getNameThreadSafe());
            o->setProperty ("colour",    (juce::int64) t.colourARGB.load (std::memory_order_relaxed));
            o->setProperty ("gainDb",    (double) t.gainDb.load (std::memory_order_relaxed));
            o->setProperty ("pan",       (double) t.pan.load    (std::memory_order_relaxed));
            o->setProperty ("muted",     t.muted  .load (std::memory_order_relaxed));
            o->setProperty ("soloed",    t.soloed .load (std::memory_order_relaxed));
            o->setProperty ("monitor",   t.monitor.load (std::memory_order_relaxed));
            o->setProperty ("armed",     t.armed  .load (std::memory_order_relaxed));
            o->setProperty ("inRoute",   t.inputRouting .load (std::memory_order_relaxed));
            o->setProperty ("outRoute",  t.outputRouting.load (std::memory_order_relaxed));
            if (t.outputBeforeVsc.load (std::memory_order_relaxed) != TrackState::kNoSavedOutput)
                o->setProperty ("outputBeforeVsc", t.outputBeforeVsc.load (std::memory_order_relaxed));
            o->setProperty ("outputMuted", t.outputMuted.load (std::memory_order_relaxed));
            o->setProperty ("streamSend",  t.streamSend .load (std::memory_order_relaxed));
            o->setProperty ("stereo",    t.isStereo .load (std::memory_order_relaxed));
            o->setProperty ("referenceMedia", t.referenceMedia.load());
            o->setProperty ("captureInputGainDb", (double) t.captureInputGainDb.load());
            o->setProperty ("captureInputGainKnown", t.captureInputGainKnown.load());
            o->setProperty ("editGroup", t.editGroup.load (std::memory_order_relaxed));
            arr.add (juce::var (o.get()));
        }

        juce::DynamicObject::Ptr root (new juce::DynamicObject());
        // Schema version for session_mix.json. Absence => pre-v1 (legacy):
        // every field is still read with hasProperty guards, so old files
        // load fine and missing fields fall back to safe defaults.
        root->setProperty ("formatVersion", 2);
        root->setProperty ("trackCount", recorder.getNumTracks());
        root->setProperty ("strips", arr);

        root->setProperty ("chaseStartTimecode", getChaseStartTimecode());
        root->setProperty ("streamOutL", streamOutL.load (std::memory_order_relaxed));
        root->setProperty ("streamOutR", streamOutR.load (std::memory_order_relaxed));
        return juce::JSON::toString (juce::var (root.get()), true);
    }

    bool AudioEngine::saveSessionMixTo (const juce::File& sessionDir)
    {
        return sessionDir.isDirectory()
            && sessionmix::write (sessionDir, mixerSession, captureSessionMixJson());
    }

    bool AudioEngine::loadSessionMixFrom (const juce::File& sessionDir)
    {
        if (isRecording() || isCaptureFinalizing()) return false;
        const auto f = sessionDir.getChildFile (sessionmix::kFileName);
        if (! f.existsAsFile()) return false;

        const auto parsed = juce::JSON::parse (f);
        auto* root = parsed.getDynamicObject();
        if (root == nullptr || ! root->hasProperty ("trackCount")) return false;

        // The saved session is authoritative in both directions.  Growing only
        // left channels from the previous, larger session alive and exposed
        // them to the user after Open.
        const int want = (int) root->getProperty ("trackCount");
        if (want < 0) return false;
        if (want != recorder.getNumTracks())
            setStripCount (want);

        // Replace pair layout, rather than rejecting valid saved pairs against
        // stale neighbouring flags from the previous session.
        for (int i = 0; i < recorder.getNumTracks(); ++i)
            recorder.getTrack (i).isStereo.store (false);

        if (! setChaseStartTimecode (root->getProperty ("chaseStartTimecode").toString()))
            chaseStartLabel.store (0, std::memory_order_relaxed);

        // An older session has no stream outputs: the bus stays off them
        // rather than landing on whatever pair the last session used.
        setStreamOutputs (root->hasProperty ("streamOutL") ? (int) root->getProperty ("streamOutL") : -1,
                          root->hasProperty ("streamOutR") ? (int) root->getProperty ("streamOutR") : -1);

        if (auto* arr = root->getProperty ("strips").getArray())
            for (auto& v : *arr)
                if (auto* o = v.getDynamicObject())
                {
                    const int idx = (int) o->getProperty ("index");
                    if (idx < 0 || idx >= recorder.getNumTracks()) continue;
                    auto& t = recorder.getTrack (idx);

                    if (o->getProperty ("uid").toString().isNotEmpty())
                    {
                        t.stripId = o->getProperty ("uid").toString();
                        if (appProps != nullptr)
                            appProps->setValue ("strip_uid_" + juce::String (idx), t.stripId);
                    }

                    if (o->hasProperty ("name"))     setTrackName          (idx, o->getProperty ("name").toString());
                    if (o->hasProperty ("colour"))   setTrackColour        (idx, juce::Colour ((juce::uint32) (juce::int64) o->getProperty ("colour")));
                    if (o->hasProperty ("gainDb"))   setTrackGainDb        (idx, (float) (double) o->getProperty ("gainDb"));
                    if (o->hasProperty ("pan"))      setTrackPan           (idx, (float) (double) o->getProperty ("pan"));
                    if (o->hasProperty ("inRoute"))  setTrackInputRouting  (idx, (int) o->getProperty ("inRoute"));
                    if (o->hasProperty ("outRoute")) setTrackOutputRouting (idx, (int) o->getProperty ("outRoute"));
                    t.outputBeforeVsc.store (o->hasProperty ("outputBeforeVsc") ? (int) o->getProperty ("outputBeforeVsc")
                                                                                : TrackState::kNoSavedOutput,
                                             std::memory_order_relaxed);
                    if (o->hasProperty ("stereo"))   setTrackStereo        (idx, (bool) o->getProperty ("stereo"));
                    t.referenceMedia.store (o->hasProperty ("referenceMedia")
                        ? (bool) o->getProperty ("referenceMedia")
                        : t.getNameThreadSafe() == "Click" && t.inputRouting.load() < 0);
                    const float captureGain = (float) (double) o->getProperty ("captureInputGainDb");
                    const bool  captureFinite = std::isfinite (captureGain);
                    t.captureInputGainDb.store (captureFinite ? captureGain : 0.0f);
                    // A session from before the flag cannot tell a real 0 dB
                    // reference from a strip the desk never reported, so 0
                    // reads as no reference: no follow is the safe failure.
                    t.captureInputGainKnown.store (captureFinite
                        && (o->hasProperty ("captureInputGainKnown")
                                ? (bool) o->getProperty ("captureInputGainKnown")
                                : captureGain != 0.0f));
                    // Until a new desk reading arrives, reopening has no trim delta.
                    t.liveInputGainDb.store (t.captureInputGainDb.load());
                    t.liveInputGainKnown.store (false);
                    setTrackEditGroup (idx, o->hasProperty ("editGroup") ? (int) o->getProperty ("editGroup") : -1);

                    // mute / solo / monitor / arm have no dedicated engine
                    // setter -- the UI flips the atomics directly, so do the same.
                    t.muted  .store (o->hasProperty ("muted")   && (bool) o->getProperty ("muted"),   std::memory_order_relaxed);
                    t.soloed .store (o->hasProperty ("soloed")  && (bool) o->getProperty ("soloed"),  std::memory_order_relaxed);
                    t.monitor.store (o->hasProperty ("monitor") && (bool) o->getProperty ("monitor"), std::memory_order_relaxed);
                    t.armed  .store (o->hasProperty ("armed")   && (bool) o->getProperty ("armed"),   std::memory_order_relaxed);
                    t.outputMuted.store (o->hasProperty ("outputMuted")
                                           && (bool) o->getProperty ("outputMuted"),
                                         std::memory_order_relaxed);
                    t.streamSend.store (o->hasProperty ("streamSend")
                                          && (bool) o->getProperty ("streamSend"),
                                        std::memory_order_relaxed);

                }

        // The strip shows L, so L is what the engineer set. A session saved
        // while these controls wrote L alone can hold R out of step with it.
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);
            if (! t.isStereo.load (std::memory_order_relaxed)) continue;
            setTrackStream (i, t.streamSend.load (std::memory_order_relaxed));
        }

        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            for (int i = 0; i < recorder.getNumTracks(); ++i)
                appProps->setValue ("strip_uid_" + juce::String (i), recorder.getTrack (i).stripId);
            appProps->saveIfNeeded();
        }
        return true;
    }

    void AudioEngine::setEditCursorSample (juce::int64 sample) noexcept
    {
        editCursorSample.store (sample, std::memory_order_release);
    }
    juce::int64 AudioEngine::getEditCursorSample() const noexcept
    {
        return editCursorSample.load (std::memory_order_acquire);
    }

    int AudioEngine::dropMarkerAtCurrentPosition (const juce::String& name, const juce::String& type)
    {
        if (! markers.hasContext()) return -1;
        if (externalRecording.load (std::memory_order_acquire)
            && captureStatus().source != "daemon") return -1;

        // Pro Tools-style: when the engineer has placed the edit
        // cursor in the EDIT view, the marker lands there. Otherwise
        // fall back to the live transport position (recorder while
        // recording, player otherwise) so the M-key shortcut still
        // works during a take.
        // While RECORDING the live capture position wins over any EDIT cursor.
        // The edit cursor is sticky (set by a wave-pane click, e.g. during
        // soundcheck) and was preferred unconditionally, so every marker dropped
        // during the take piled onto that frozen soundcheck position instead of
        // where the recording actually is. Record start also clears it below.
        juce::int64 pos = -1;
        const auto cursor = editCursorSample.load (std::memory_order_acquire);
        if (isRecording())
        {
            // Timeline position (recordBaseSamples + samplesSinceStart), NOT the
            // 0-based getSamplesSinceStart(): on a continue/punch the take starts
            // at recordBaseSamples, so the raw offset would drop every marker
            // recordBaseSamples too early. See MultitrackRecorder.h.
            pos = isCaptureWindowActive() && player.isPlaying()
                ? player.getPositionSamples()
                : getRecordTimelineSamples();
            if (pos < 0) return -1;
        }
        else if (cursor >= 0)
            pos = cursor;
        else if (player.isLoaded())
            pos = player.getPositionSamples();
        else
            return -1;

        markers.drop (pos, name, type);
        if (onMarkerDropped)
            for (const auto& dropped : markers.getAll())
                if (dropped.runtimeId == markers.lastDroppedId) { onMarkerDropped (dropped.name); break; }
        return markers.getCount();
    }

    void AudioEngine::clearAllStripOverrides()
    {
        // Wipe every per-strip key in appProps that survives across
        // runs -- gains, pans, colours, names, routing, stereo flags,
        // strip UUIDs. Called on new-session creation so the engineer
        // starts from a clean slate rather than inheriting (e.g.) a
        // hard-pan they set up for last weekend's gig.
        // That goes for the way back from a soundcheck too.
        for (int i = 0; i < recorder.getNumTracks(); ++i)
            recorder.getTrack (i).outputBeforeVsc.store (TrackState::kNoSavedOutput, std::memory_order_relaxed);
        // Each module replaces its cache from the latest shared file, clears
        // its whole domain, then saves once. Per-field clears parsed/rewrote
        // the same XML 1,536 times even when most strip keys were absent.
        stripGains.clearRange (0, 256);
        stripColours.clearRange (0, 256);
        stripNames.clearRange (0, 256);
        stripRouting.clearRange (0, 256);
        if (appProps != nullptr)
        {
            // Reload AFTER the Strip* clears above (they rewrote the shared
            // .settings file) so our save here doesn't undo them.
            reloadAppPropsBeforeWrite();
            for (int i = 0; i < 256; ++i)
            {
                const auto suffix = juce::String (i);
                appProps->removeValue ("strip_stereo_"    + suffix);
                appProps->removeValue ("strip_uid_"       + suffix);
                appProps->removeValue ("strip_editgroup_" + suffix);
                appProps->removeValue ("strip_outmute_"   + suffix);
            }
            // CRITICAL: also reset the persisted strip COUNT. Wiping only the
            // per-index overrides left `stripCount` pointing at last gig's
            // number, so a "new / cleared session" silently revived that many
            // (now blank) strips on the next launch -- the gig-prep "why are
            // there tracks?" bug. A cleared/new session is empty; callers that
            // build real strips (New-from-CSV/console, or opening a session
            // with a saved mix) set the count explicitly afterward.
            appProps->setValue ("stripCount", juce::var (0));
            appProps->saveIfNeeded();
        }

        // Reset live state on whatever strips currently exist so the
        // change is visible immediately, not only after the recorder
        // grows.
        for (int i = 0, n = recorder.getNumTracks(); i < n; ++i)
        {
            auto& t = recorder.getTrack (i);
            t.gainDb  .store (0.0f,  std::memory_order_relaxed);
            t.pan     .store (0.0f,  std::memory_order_relaxed);
            t.muted   .store (false, std::memory_order_relaxed);
            t.soloed  .store (false, std::memory_order_relaxed);
            t.armed   .store (false, std::memory_order_relaxed);
            t.monitor .store (false, std::memory_order_relaxed);
            t.isStereo.store (false, std::memory_order_relaxed);
            t.colourARGB.store (0,   std::memory_order_relaxed);
            t.stripId.clear();
        }
    }

    void AudioEngine::clearStripOverridesRange (int firstIndex, int lastIndexExclusive)
    {
        stripGains.clearRange (firstIndex, lastIndexExclusive);
        stripColours.clearRange (firstIndex, lastIndexExclusive);
        stripNames.clearRange (firstIndex, lastIndexExclusive);
        stripRouting.clearRange (firstIndex, lastIndexExclusive);
        // Reload AFTER the Strip* clears above (they rewrote the shared
        // .settings file) so our appProps save doesn't undo them, THEN wipe
        // the appProps-owned per-strip keys and save once.
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            for (int i = firstIndex; i < lastIndexExclusive; ++i)
            {
                const auto suffix = juce::String (i);
                appProps->removeValue ("strip_stereo_"    + suffix);
                appProps->removeValue ("strip_uid_"       + suffix);
                appProps->removeValue ("strip_editgroup_" + suffix);
                appProps->removeValue ("strip_outmute_"   + suffix);
            }
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::setStripCount (int n)
    {
        if (isRecording() || isCaptureFinalizing()) return;
        n = juce::jlimit (0, 256, n);

        const int oldCount = recorder.getNumTracks();

        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("stripCount", n);
            appProps->saveIfNeeded();
        }

        deviceManager.removeAudioCallback (this);
        recorder.setTrackCount (n);
        // Freshly-added strips must NOT inherit a previous session's per-index
        // metadata that lingers in appProps -- names, colours, and especially
        // edit-group assignments. Edit groups are stored globally
        // by array position (not in the .zfproj), so without this, adding a
        // channel (or a fresh session re-adding strips after the empty-on-open
        // start) silently revived last gig's edit-group links -- strips showed
        // an unexpected "EG" badge and moved together. Wipe the new slots
        // before applyPersistedStripState re-reads them; callers that import a
        // real session set names/routing explicitly afterwards.
        if (n > oldCount)
            clearStripOverridesRange (oldCount, n);
        applyPersistedStripState();
        deviceManager.addAudioCallback (this);
    }

    void AudioEngine::addOneStrip()
    {
        setStripCount (recorder.getNumTracks() + 1);
    }

    bool AudioEngine::removeStripAt (int index)
    {
        const int n = recorder.getNumTracks();
        if (index < 0 || index >= n) return false;
        std::vector<int> order;
        for (int i = 0; i < n; ++i) if (i != index) order.push_back (i);
        return reorderTracks (order);
    }

    void AudioEngine::applyPersistedStripState()
    {
        for (int i = 0; i < recorder.getNumTracks(); ++i)
        {
            auto& t = recorder.getTrack (i);
            if (stripColours.hasColour (i))
                t.colourARGB.store (stripColours.getColour (i).getARGB(),
                                    std::memory_order_relaxed);
            if (stripNames.hasName (i))
                t.setNameThreadSafe (stripNames.getName (i));
            if (stripGains.hasGain (i))
                t.gainDb.store (stripGains.getGainDb (i), std::memory_order_relaxed);
            if (stripGains.hasPan (i))
                t.pan.store    (stripGains.getPan (i), std::memory_order_relaxed);

            // Stable per-strip UUID -- generated once on first sight,
            // persisted under strip_uid_<n>. Cue snapshots reference
            // strips by this ID instead of array index, so a reorder
            // doesn't silently break every cue in the show.
            if (appProps != nullptr)
            {
                const auto key = "strip_uid_" + juce::String (i);
                auto uid = appProps->getValue (key, juce::String());
                if (uid.isEmpty())
                {
                    uid = juce::Uuid().toString();
                    reloadAppPropsBeforeWrite();
                    // reload() may have populated the key from disk (another
                    // path wrote it); re-read before committing a fresh one.
                    auto existing = appProps->getValue (key, juce::String());
                    if (existing.isNotEmpty()) uid = existing;
                    else { appProps->setValue (key, uid); appProps->saveIfNeeded(); }
                }
                t.stripId = uid;
            }

            // The edit group is NOT read here.
            // It lives in session_mix.json (loadSessionMixFrom) and is
            // applied there. Reading an index-keyed *global* appProps copy
            // would (a) leak the previous session's grouping into
            // this one, and (b) clobber the freshly-loaded value on a device
            // restart, which re-runs this function. (The appProps copies were
            // also never swapped by swapTracks, so they went stale after any
            // reorder anyway.) loadSessionMixFrom sets editGroup
            // authoritatively for every strip on session open; new strips
            // default to "ungrouped / normal channel" via TrackState
            // construction.

            // Default to channel i. A request past a smaller device's last
            // input is KEPT: the callback wraps it live (strip i hears
            // i % numInputs, effectiveInputRouting), so a 4-strip session on a
            // 1-input device still captures sound, while every save path keeps
            // writing the patch the show rig needs. Storing the wrapped value
            // here leaked it into session_mix.json on the next save or reorder.
            const int wantIn = stripRouting.hasInput (i) ? stripRouting.getInput (i) : i;
            t.inputRouting .store (wantIn, std::memory_order_relaxed);
            // Default per-channel output is -1 (unrouted) → audio only
            // reaches the hardware via the master bus. Engineer picks a
            // specific output explicitly (e.g. for Virtual Soundcheck)
            // when they need a dedicated per-track destination.
            t.outputRouting.store (stripRouting.hasOutput (i) ? stripRouting.getOutput (i) : -1,
                                   std::memory_order_relaxed);

            // Restore the mono / stereo flag from appProps. The key is
            // 'strip_stereo_N' → bool. Defaults to false.
            if (appProps != nullptr)
            {
                const auto key = juce::String ("strip_stereo_") + juce::String (i);
                t.isStereo.store (appProps->getBoolValue (key, false),
                                  std::memory_order_relaxed);
            }
        }
    }

    void AudioEngine::setTrackStereo (int channelIndex, bool isStereoPair)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        // The recorder's writer layout (mono vs one interleaved 2-ch file) is
        // fixed at record start, and the capture feed gates on isStereo per
        // block, so flipping this mid-take desyncs the on-disk layout from the
        // flag. Refuse while recording -- like setStripCount / removeStripAt.
        if (isRecording() || isCaptureFinalizing()) return;
        if (isStereoPair
            && (channelIndex + 1 >= recorder.getNumTracks()
                || (channelIndex > 0 && recorder.getTrack (channelIndex - 1).isStereo.load())
                || recorder.getTrack (channelIndex + 1).isStereo.load())) return;
        const bool wasStereo = recorder.getTrack (channelIndex).isStereo.load();
        recorder.getTrack (channelIndex).isStereo.store (isStereoPair,
                                                          std::memory_order_release);
        // Linking makes the pair ONE strip in every view, and that strip
        // shows and edits the L half -- so bring the R half onto it now.
        // Otherwise R kept playing (and recording) by its own mute, fader,
        // arm and routing until each control happened to be touched.
        // Pan stays per channel: that is the stereo image.
        if (isStereoPair && ! wasStereo)
        {
            auto& l = recorder.getTrack (channelIndex);
            auto& r = recorder.getTrack (channelIndex + 1);
            for (auto flag : { &TrackState::muted, &TrackState::soloed, &TrackState::armed,
                               &TrackState::monitor, &TrackState::outputMuted, &TrackState::streamSend })
                (r.*flag).store ((l.*flag).load (std::memory_order_relaxed), std::memory_order_relaxed);
            setTrackGainDb   (channelIndex + 1, l.gainDb.load (std::memory_order_relaxed));
            setTrackEditGroup (channelIndex + 1, l.editGroup.load (std::memory_order_relaxed));
            const int in  = l.inputRouting .load (std::memory_order_relaxed);
            const int out = l.outputRouting.load (std::memory_order_relaxed);
            if (in  != -2) setTrackInputRouting  (channelIndex + 1, in  >= 0 ? in  + 1 : -1);
            if (out != -2) setTrackOutputRouting (channelIndex + 1, out >= 0 ? out + 1 : -1);
        }
        // Bump the track generation so EVERY view (MIXER, EDIT, and the
        // meterbridge) rebuilds and re-collapses the pair -- a stereo link from
        // the PATCH page changed the engine but left the other views showing two
        // mono strips until some unrelated add/delete forced a rebuild.
        recorder.bumpTrackGeneration();
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            const auto key = juce::String ("strip_stereo_") + juce::String (channelIndex);
            appProps->setValue (key, isStereoPair);
            appProps->saveIfNeeded();
        }
    }

    std::vector<Clip>& AudioEngine::clipsFor (int track)
    {
        if (track < 0) track = 0;
        if (track >= (int) trackClips.size()) trackClips.resize ((size_t) track + 1);
        return trackClips[(size_t) track];
    }

    int AudioEngine::physicalFromLogical (int logical)
    {
        int phys = 0;
        for (int k = 0; k < logical && phys < recorder.getNumTracks(); ++k)
            phys += recorder.getTrack (phys).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
        return phys;
    }

    int AudioEngine::logicalFromPhysical (int physical)
    {
        int logical = 0, p = 0;
        while (p < physical && p < recorder.getNumTracks())
        {
            p += recorder.getTrack (p).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
            ++logical;
        }
        return logical;
    }

    void AudioEngine::setPunchModeOn (bool on) noexcept
    {
        if (punchEnabled.exchange (on, std::memory_order_relaxed) == on) return;
        if (on)
        {
            punchLoopWasEnabled = player.isLoopEnabled();
            player.setLoopEnabled (false);   // a loop must not wrap before punch-out
        }
        else
            player.setLoopEnabled (punchLoopWasEnabled);
    }

    bool AudioEngine::swapTracks (int a, int b)
    {
        const int n = recorder.getNumTracks();
        if (a < 0 || b < 0 || a >= n || b >= n || a == b) return false;
        std::vector<int> order;
        for (int i = 0; i < n; ++i) order.push_back (i == a ? b : i == b ? a : i);
        return reorderTracks (order);
    }

    bool AudioEngine::reorderTracks (const std::vector<int>& order)
    {
        if (isRecording() || isSessionTransitionActive()) return false;
        // Refused before any take file is renamed: the indices below describe
        // the strips in memory, which only match this folder's takes if they
        // are its mixer, and emptying a saved mixer is not a save we allow.
        if (activeSession.isDirectory()
            && (! mixerBelongsTo (activeSession)
                || (order.empty() && sessionmix::stripCountIn (activeSession) > 0)))
            return false;
        const int n = recorder.getNumTracks();
        std::vector<int> destination ((size_t) n, -1);
        for (int i = 0; i < (int) order.size(); ++i)
        {
            const int old = order[(size_t) i];
            if (old < 0 || old >= n || destination[(size_t) old] >= 0) return false;
            destination[(size_t) old] = i;
        }
        player.stop();
        deviceManager.removeAudioCallback (this);
        const juce::ScopedLock structureGuard (recorder.getStructureLock());
        std::vector<std::unique_ptr<TrackState>> states;
        reloadAppPropsBeforeWrite();
        for (int i = 0; i < n; ++i)
        {
            auto s = std::make_unique<TrackState>();
            s->copySettingsFrom (recorder.getTrack (i));
            if (appProps != nullptr)
                s->stripId = appProps->getValue ("strip_uid_" + juce::String (i), s->stripId);
            states.push_back (std::move (s));
        }
        auto oldClips = trackClips;
        auto oldPlaylists = trackPlaylists;
        const auto session = activeSession;
        auto audio = session.getChildFile ("Audio Files");
        if (! audio.isDirectory()) audio = session;
        std::vector<TrackFileTransaction::Move> moves;
        const auto removedDir = session.getChildFile ("Removed Tracks").getChildFile (juce::Uuid().toString());
        // Each track's complete take (all containers and continuation parts)
        // moves together. Removed audio is archived, never deleted.
        if (session.isDirectory())
            for (int old = 0; old < n; ++old)
            {
                const auto prefix = juce::String::formatted ("Track_%02d", old + 1);
                for (const auto& f : audio.findChildFiles (juce::File::findFiles, false, prefix + ".*;" + prefix + "_part*"))
                {
                    if (f.getFileName().containsIgnoreCase (".punchbase")) continue;
                    if (! f.hasFileExtension ("wav;flac;aif;aiff")) continue;
                    const int dest = destination[(size_t) old];
                    const auto target = dest >= 0
                        ? audio.getChildFile (juce::String::formatted ("Track_%02d", dest + 1)
                                               + f.getFileName().substring (prefix.length()))
                        : removedDir.getChildFile (f.getFileName());
                    if (target != f) moves.emplace_back (f, target);
                }
            }
        auto remapClip = [&] (Clip& c, int oldTrack)
        {
            // Make implicit references explicit before filenames move, so a
            // surviving cross-track clip can still read an archived take.
            if (c.audioFile == juce::File())
            {
                int channel = 0;
                auto source = getTrackAudioFile (oldTrack, &channel);
                if (destination[(size_t) oldTrack] >= 0
                    && ! (channel == 1 && oldTrack > 0 && destination[(size_t) oldTrack - 1] < 0)) return;
                c.audioFile = source; c.sourceChannel = channel;
            }
            for (const auto& m : moves)
                if (c.audioFile == m.first) { c.audioFile = m.second; break; }
        };
        auto nextClips = trackClips;
        auto nextPlaylists = trackPlaylists;
        nextClips.resize ((size_t) n); nextPlaylists.resize ((size_t) n);
        for (int old = 0; old < n; ++old)
        {
            for (auto& c : nextClips[(size_t) old]) remapClip (c, old);
            for (auto& take : nextPlaylists[(size_t) old].takes)
                for (auto& c : take.clips) remapClip (c, old);
        }
        TrackFileTransaction files;
        const bool disk = session.isDirectory();
        if (disk && ! files.begin (session, moves))
        { deviceManager.addAudioCallback (this); return false; }

        trackClips.clear(); trackPlaylists.clear();
        for (int i = 0; i < (int) order.size(); ++i)
        {
            const int old = order[(size_t) i];
            auto& t = recorder.getTrack (i);
            t.copySettingsFrom (*states[(size_t) old]);
            trackClips.push_back (nextClips[(size_t) old]);
            trackPlaylists.push_back (nextPlaylists[(size_t) old]);
        }
        // Persist through the actual zero-based stores, not hand-built keys.
        auto persist = [this] (int count)
        {
            for (int i = 0; i < count; ++i)
            {
                auto& t = recorder.getTrack (i);
                setTrackName (i, t.getNameThreadSafe()); setTrackColour (i, juce::Colour (t.colourARGB.load()));
                setTrackGainDb (i, t.gainDb.load()); setTrackPan (i, t.pan.load());
                setTrackInputRouting (i, t.inputRouting.load()); setTrackOutputRouting (i, t.outputRouting.load());
                setTrackStereo (i, t.isStereo.load());
                setTrackEditGroup (i, t.editGroup.load());
            }
            if (appProps != nullptr)
            {
                reloadAppPropsBeforeWrite();
                for (int i = 0; i < count; ++i)
                    appProps->setValue ("strip_uid_" + juce::String (i), recorder.getTrack (i).stripId);
                for (int i = count; i < 256; ++i)
                {
                    appProps->removeValue ("strip_uid_" + juce::String (i));
                    appProps->removeValue ("strip_stereo_" + juce::String (i));
                }
                appProps->setValue ("stripCount", count); appProps->saveIfNeeded();
            }
        };
        // Retain objects until metadata is safely written; shrink only on commit.
        bool saved = true;
        if (disk)
        {
            // The recorder still holds the removed strips until commit.
            auto mix = juce::JSON::parse (captureSessionMixJson());
            mix.getDynamicObject()->setProperty ("trackCount", (int) order.size());
            if (auto* arr = mix["strips"].getArray())
                while (arr->size() > (int) order.size()) arr->removeLast();
            saved = sessionmix::write (session, mixerSession, juce::JSON::toString (mix));
            for (const auto& f : session.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                                          false, "*.zfproj"))
            {
                auto project = juce::JSON::parse (f);
                if (auto* root = project.getDynamicObject())
                {
                    root->setProperty ("playlists", playlistsToJson());
                    saved = atomicfile::writeText (f, juce::JSON::toString (project)) && saved;
                }
                else saved = false;
            }
            if (saved) saved = files.commit();
        }
        if (! saved)
        {
            TrackFileTransaction::recoverOne (files.folder);
            for (int i = 0; i < n; ++i) recorder.getTrack (i).copySettingsFrom (*states[(size_t) i]);
            trackClips = std::move (oldClips); trackPlaylists = std::move (oldPlaylists);
            persist (n);
        }
        else
        {
            recorder.setTrackCount ((int) order.size());
            persist ((int) order.size());
            // The LTC source follows its strip; deleting that strip clears it.
            const int ltc = ltcSourceStrip.load();
            if (ltc >= 0 && ltc < n && destination[(size_t) ltc] != ltc)
                setLtcSourceStrip (destination[(size_t) ltc] + 1);
            // A reorder can move a stereo pair without changing the count;
            // every view rebuilds on a generation change.
            recorder.bumpTrackGeneration();
        }
        if (disk) player.loadSession (session);
        sourceTrackLengths.clear();
        for (int i = 0; i < player.getNumTracks(); ++i)
            sourceTrackLengths.push_back (player.getTrackLengthSamples (i));
        player.clearAllClips();
        for (int i = 0; i < (int) trackClips.size(); ++i) player.setTrackClips (i, trackClips[(size_t) i]);
        deviceManager.addAudioCallback (this);
        if (saved && onTracksReordered) onTracksReordered (destination);
        return saved;
    }

    namespace { std::atomic<bool> s_testSkipAudioInit { false }; }

    void AudioEngine::setTestModeSkipAudioInit (bool skip) noexcept
    {
        if (skip) isolateSettings();
        s_testSkipAudioInit.store (skip, std::memory_order_release);
    }

    void AudioEngine::prepareForTests (double sr, int blockSize)
    {
        const std::lock_guard<std::mutex> lifecycle (deviceLifecycleLock);
        if (isCaptureFinalizing())
        {
            deferredDeviceAudio.store (true, std::memory_order_release);
            deferredDeviceConfiguration = DeferredDeviceConfiguration { sr, blockSize, {} };
            return;
        }
        applyDeviceConfiguration (sr, blockSize, {});
    }

    AudioEngine::AudioEngine()
        : asyncHandle (std::make_shared<std::atomic<AudioEngine*>> (this))
    {
        markers.deferDiskWhile = [this] { return isTakeOnDisk(); };
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Zynforge Recording";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Zynforge Recording";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat       = juce::PropertiesFile::storeAsXML;

        appProps = makeSettingsFile (opts);

        // Restore the stereo-mix-recording flag from the prefs file so the
        // engineer's preference survives restart.
        recordStereoMixFlag.store (appProps->getBoolValue ("recordStereoMix", false),
                                   std::memory_order_release);
        autoArmOnInputFlag.store (appProps->getBoolValue ("autoArmOnInput", false),
                                   std::memory_order_release);
        sceneFollowFlag.store (appProps->getBoolValue ("sceneFollow", false),
                               std::memory_order_release);

        recorder.setMeterUnarmedInputs (autoArmOnInputFlag.load());

        // Keep the configured path even while a removable backup drive is
        // offline. Readiness can then report it as unavailable, and recording
        // resumes using it automatically when the volume returns.
        {
            const auto savedBackup = appProps->getValue ("backupDirectory", {});
            if (savedBackup.isNotEmpty())
                recorder.setBackupDirectory (juce::File (savedBackup));
            // WAV 24 unless the engineer chose otherwise; a value this build
            // does not know leaves it there.
            const int savedFormat = appProps->getIntValue ("backupFormat", (int) CaptureFormat::Wav24);
            if (savedFormat >= 0 && savedFormat <= (int) CaptureFormat::Flac24)
                recorder.setBackupCaptureFormat ((CaptureFormat) savedFormat);
        }

        // Retain configured mirrors while removable volumes are offline;
        // readiness must report the missing copy and retry after reconnect.
        {
            const int count = appProps->getIntValue ("mirror_count", 0);
            std::vector<MultitrackRecorder::MirrorConfig> mirrors;
            for (int i = 0; i < count && i < 16; ++i)
            {
                const auto rootStr = appProps->getValue ("mirror_root_" + juce::String (i), {});
                if (rootStr.isEmpty()) continue;
                juce::File f (rootStr);
                MultitrackRecorder::MirrorConfig c;
                c.root   = f;
                c.format = (CaptureFormat) appProps->getIntValue (
                    "mirror_format_" + juce::String (i), (int) CaptureFormat::Wav24);
                mirrors.push_back (c);
            }
            if (! mirrors.empty()) recorder.setMirrors (mirrors);
        }

        // Restore the active session folder (set by New Session... / Open
        // Session...) so Save / Save As stay enabled across app restarts.
        {
            const auto saved = appProps->getValue ("activeSessionDir", {});
            if (saved.isNotEmpty())
            {
                juce::File f (saved);
                // Only restore a real, still-present session folder. Reject a
                // path under the temp dir (a leftover scratch / test session)
                // so a stale entry can't make the app reopen a deleted folder.
                const auto tempRoot = juce::File::getSpecialLocation (juce::File::tempDirectory);
                if (f.isDirectory() && ! f.isAChildOf (tempRoot))
                    activeSession = f;
                else
                {
                    reloadAppPropsBeforeWrite();
                    appProps->removeValue ("activeSessionDir");   // tidy the stale key
                    appProps->saveIfNeeded();                     // flush so it's gone on disk too
                }
            }
        }

        // Restore the LTC source strip (1-based stored, internal 0-based).
        ltcSourceStrip.store (appProps->getIntValue ("ltcSourceStrip", 0) - 1,
                              std::memory_order_release);

        // Master bus state -- gain + mute + L/R output routing + stereo mode.
        masterState.name  = "MASTER";
        masterStateR.name = "MASTER R";
        masterState.gainDb.store ((float) appProps->getDoubleValue ("masterGainDb", 0.0));
        masterState.muted .store (appProps->getBoolValue ("masterMuted", false));
        mixMaySharePatchedOutputs.store (appProps->getBoolValue ("mixMaySharePatchedOutputs", false));
        masterOutL  .store (appProps->getIntValue  ("masterOutL", 0));
        masterOutR  .store (appProps->getIntValue  ("masterOutR", 1));
        masterStereo.store (appProps->getBoolValue ("masterStereo", true),
                            std::memory_order_release);

        // Open with up to 256 inputs / 64 outputs by default -- adjust later from UI.
        if (! s_testSkipAudioInit.load (std::memory_order_acquire))
        {
            auto err = deviceManager.initialise (/*numInputs*/ 256, /*numOutputs*/ 64,
                                                 /*savedState*/ nullptr,
                                                 /*selectDefault*/ true);
            if (err.isNotEmpty())
                DBG ("AudioDeviceManager init: " << err);
        }

        if (! s_testSkipAudioInit.load (std::memory_order_acquire))
            deviceManager.addAudioCallback (this);
    }

    AudioEngine::~AudioEngine()
    {
        // Invalidate every MessageManager::callAsync callback before any
        // member teardown begins.  The shared handle itself stays alive in
        // queued lambdas, but they will observe nullptr and become no-ops.
        asyncHandle->store (nullptr, std::memory_order_release);
        if (! s_testSkipAudioInit.load (std::memory_order_acquire))
            deviceManager.removeAudioCallback (this);
        // No queued callback owns readers. Finish whichever worker phase is
        // active, then destroy uninstalled/retired readers while player lives.
        if (captureStopJob && captureStopJob->worker.joinable()) captureStopJob->worker.join();
        captureStopJob.reset();
        recorder.endFinalization();
        asyncCaptureFinalizing.store (false);
        // ~TimeSliceThread would kill it after two seconds, mid-write.
        mixWriterThread.stopThread (-1);
    }

    void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
    {
        const std::lock_guard<std::mutex> lifecycle (deviceLifecycleLock);
        const auto sr = device->getCurrentSampleRate();
        const auto blockSize = device->getCurrentBufferSizeSamples();
        if (isCaptureFinalizing())
        {
            deferredDeviceAudio.store (true, std::memory_order_release);
            deferredDeviceConfiguration = DeferredDeviceConfiguration { sr, blockSize, device->getWorkgroup() };
            return;
        }
        applyDeviceConfiguration (sr, blockSize, device->getWorkgroup());
    }

    void AudioEngine::applyDeviceConfiguration (double sr, int blockSize, juce::AudioWorkgroup workgroup)
    {
        deviceSampleRate.store (sr, std::memory_order_relaxed);
        deviceBlockSize .store (blockSize, std::memory_order_relaxed);
        audioLoadPct    .store (0.0f, std::memory_order_relaxed);

        // Hand the device's audio workgroup to the recorder so its
        // writer threads can join it. On Apple Silicon the macOS
        // scheduler then co-schedules the writers with the CoreAudio
        // IO thread -- no priority inversion under load.
        recorder.setAudioWorkgroup (std::move (workgroup));

        loudness.prepare (sr);

        // Strip count is user-controlled: the app opens with NO channels
        // (engineer adds them via +CH), and the count is preserved across
        // device restarts -- it is NOT restored from a previous session.
        recorder.prepare (sr, blockSize, recorder.getNumTracks());   // start empty; preserve count across device restarts
        player  .prepare (sr, blockSize);

        // Pre-allocate the stereo mix scratch buffer at the device block
        // size so the audio thread never allocates when mix capture is on.
        stereoMixScratch.setSize (2, blockSize, false, true, true);
        monitorAccum    .setSize (2, blockSize, false, true, true);
        // 64-bit output accumulator -- sized to the max supported channel
        // count (kMaxStrips) so a wide device (>64 outs) never forces an
        // allocation inside the callback.
        outputAccum     .setSize (256, blockSize, false, true, true);
        // Per-track playback scratch -- pre-size to the max supported strip
        // count so a >32-track session never grows it inside the callback.
        playerScratch   .setSize (256, blockSize, false, true, true);

        applyPersistedStripState();
    }

    juce::Array<juce::File> AudioEngine::findIncompleteSessions (const juce::File& sessionsRoot)
    {
        juce::Array<juce::File> incomplete;
        if (! sessionsRoot.isDirectory()) return incomplete;

        // Match every session subdir, not just the auto-named
        // `Session_*` ones. User-named sessions (created via
        // File > New Session with a custom name) carry the same
        // `recording.session` marker when they crash, and were
        // previously invisible to the recovery scan.
        const auto dirs = sessionsRoot.findChildFiles (juce::File::findDirectories, false);
        for (auto& d : dirs)
            if (d.getChildFile ("recording.session").existsAsFile())
                incomplete.add (d);
        return incomplete;
    }

    juce::File AudioEngine::getActiveSessionDir() const
    {
        if (recorder.isRecording())
            return recorder.getActiveSessionDir();

        // An explicitly pinned session is authoritative even when an older
        // session remains loaded in the player.  This matters for daemon
        // recording: the local recorder is idle while activeSession points at
        // the new take, and the player may still contain the preceding take.
        // Keep returning the pin if its volume disappears too, so callers can
        // report the missing target instead of silently falling back to a
        // stale player session and writing metadata into the wrong folder.
        if (activeSession != juce::File())
            return activeSession;
        if (player.isLoaded())
            return player.getSessionDir();
        return {};
    }

    void AudioEngine::noteLastCapture (const juce::File& dir)
    {
        updateSettings (appProps.get(), [&dir] (juce::PropertiesFile& props)
        {
            props.setValue ("lastCaptureDir", dir.getFullPathName());
            props.setValue ("lastCaptureId", dir.getChildFile (".zynforge-capture-id").loadFileAsString().trim());
        });
    }

    void AudioEngine::forgetLastCapture()
    {
        updateSettings (appProps.get(), [] (juce::PropertiesFile& props)
        {
            props.removeValue ("lastCaptureDir");
            props.removeValue ("lastCaptureId");
        });
    }

    juce::File AudioEngine::getLastCapture() const
    {
        if (appProps == nullptr) return {};
        const auto path = appProps->getValue ("lastCaptureDir");
        if (path.isEmpty()) return {};
        const juce::File dir (path);
        const auto id = appProps->getValue ("lastCaptureId");
        const bool sameCapture = dir.isDirectory()
            && (id.isEmpty() || dir.getChildFile (".zynforge-capture-id").loadFileAsString().trim() == id);
        return sameCapture ? dir : juce::File();
    }

    void AudioEngine::setActiveSessionDir (const juce::File& dir)
    {
        if (isCaptureFinalizing()) return;
        activeSession = isTakeOnDisk() || dir.isDirectory() ? dir : juce::File();
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("activeSessionDir", activeSession.getFullPathName());
            appProps->saveIfNeeded();
        }
    }

    void AudioEngine::clearSessionState()
    {
        if (isCaptureFinalizing()) return;
        // Never under a live take: resetAllStripState re-patches its inputs.
        if (isRecording()) return;
        stopPlayback();
        player.unload();
        player.clearAllClips();
        player.clearLoopRegion();
        player.setLoopEnabled (false);
        // A punch belongs to the session it was set up in. Left on, the next
        // session's first selection would punch without anyone asking.
        punchEnabled.store (false, std::memory_order_relaxed);

        trackClips.clear();
        trackPlaylists.clear();
        sourceTrackLengths.clear();
        resetAllStripState();
        setStreamOutputs (-1, -1);
        chaseStartLabel.store (0, std::memory_order_relaxed);
        markers.clearContext();
        editCursorSample.store (-1, std::memory_order_release);
        claimMixerFor ({});
        setActiveSessionDir ({});
    }

    void AudioEngine::setStreamOutputs (int l, int r)
    {
        streamOutL.store (l, std::memory_order_relaxed);
        streamOutR.store (r, std::memory_order_relaxed);
    }

    bool AudioEngine::setBackupDirectory (const juce::File& dir)
    {
        if (! recorder.setBackupDirectory (dir)) return false;
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("backupDirectory", dir.getFullPathName());
            appProps->saveIfNeeded();
        }
        return true;
    }

    bool AudioEngine::setBackupFormat (CaptureFormat format)
    {
        recorder.setBackupCaptureFormat (format);
        if (recorder.getBackupCaptureFormat() != format) return false;
        if (appProps != nullptr)
        {
            reloadAppPropsBeforeWrite();
            appProps->setValue ("backupFormat", (int) format);
            appProps->saveIfNeeded();
        }
        return true;
    }

    void AudioEngine::setTrackStream (int channelIndex, bool enabled)
    {
        applyToPair (recorder, channelIndex, &TrackState::streamSend, enabled,
                     recorder.getStructureLock());
    }

    int AudioEngine::getCurrentDeviceInputCount() const
    {
        if (auto* d = deviceManager.getCurrentAudioDevice())
            return d->getActiveInputChannels().countNumberOfSetBits();
        return testDeviceInputs.load (std::memory_order_relaxed);
    }

    int AudioEngine::getEffectiveInputRouting (int channelIndex)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return -1;
        return effectiveInputRouting (channelIndex,
                                      recorder.getTrack (channelIndex).inputRouting.load (std::memory_order_relaxed),
                                      getCurrentDeviceInputCount());
    }

    int AudioEngine::getCurrentDeviceOutputCount() const
    {
        if (auto* d = deviceManager.getCurrentAudioDevice())
            return d->getActiveOutputChannels().countNumberOfSetBits();
        return 0;
    }

    void AudioEngine::setTrackInputRouting (int channelIndex, int deviceCh)
    {
        if (isRecording() || isCaptureFinalizing()) return;
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        deviceCh = juce::jmax (-1, deviceCh);
        recorder.getTrack (channelIndex).inputRouting.store (deviceCh, std::memory_order_relaxed);
        stripRouting.setInput (channelIndex, deviceCh);
    }

    void AudioEngine::setTrackOutputRouting (int channelIndex, int deviceCh)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        deviceCh = juce::jmax (-1, deviceCh);
        recorder.getTrack (channelIndex).outputRouting.store (deviceCh, std::memory_order_relaxed);
        stripRouting.setOutput (channelIndex, deviceCh);
    }

    void AudioEngine::setTrackLinkedRouting (int channelIndex, int deviceCh)
    {
        // Single source of truth -- sets BOTH sides of the patch so the
        // PATCH page + the per-strip dropdowns stay in lockstep. The input
        // side is refused during a take, so the output must be too, or the
        // link breaks for the next virtual soundcheck.
        if (isRecording()) return;
        setTrackInputRouting  (channelIndex, deviceCh);
        setTrackOutputRouting (channelIndex, deviceCh);
    }

    void AudioEngine::setTrackGainDb (int channelIndex, float dB)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        // Fader range is -60..+12 dB -- earlier clamp at 0 silently
        // snapped any positive value back to unity, which looked like
        // the fader was 'resetting itself' when the engineer pushed
        // it past the centre point.
        dB = juce::jlimit (-60.0f, 12.0f, dB);
        auto& t = recorder.getTrack (channelIndex);
        t.gainDb.store (dB, std::memory_order_relaxed);
        // Direct set cancels any in-flight ramp so the engineer's slow
        // physical fader move doesn't fight a finishing cue ramp.
        t.rampTargetGainDb.store (dB, std::memory_order_relaxed);
        t.rampSamplesRemaining.store (0, std::memory_order_relaxed);
        stripGains.setGainDb (channelIndex, dB);
    }

    void AudioEngine::setTrackGainDbRamped (int channelIndex, float dB, double seconds)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        dB = juce::jlimit (-60.0f, 12.0f, dB);
        auto& t = recorder.getTrack (channelIndex);
        const auto sr = deviceSampleRate.load (std::memory_order_relaxed);
        const juce::int64 samples = sr > 0.0
            ? (juce::int64) (sr * juce::jmax (0.0, seconds))
            : 0;
        t.rampTargetGainDb     .store (dB,      std::memory_order_relaxed);
        t.rampSamplesRemaining .store (samples, std::memory_order_relaxed);
        if (samples == 0)
            t.gainDb.store (dB, std::memory_order_relaxed);   // instant
        stripGains.setGainDb (channelIndex, dB);              // persisted target
    }

    void AudioEngine::setTrackPanRamped (int channelIndex, float pan, double seconds)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        auto& t = recorder.getTrack (channelIndex);
        const auto sr = deviceSampleRate.load (std::memory_order_relaxed);
        const juce::int64 samples = sr > 0.0
            ? (juce::int64) (sr * juce::jmax (0.0, seconds))
            : 0;
        t.rampTargetPan       .store (pan,     std::memory_order_relaxed);
        // Reuse rampSamplesRemaining for pan -- gain + pan ramp together
        // during a cue recall, same duration, so one counter is enough.
        // (Pan ramp uses the same countdown as the gain ramp.)
        t.rampSamplesRemaining.store (samples, std::memory_order_relaxed);
        if (samples == 0)
            t.pan.store (pan, std::memory_order_relaxed);
        // Persisted target, like the gain ramp: applyPersistedStripState
        // re-reads StripGains on +CH / device restart, which otherwise
        // snapped a cue-recalled pan back to the last manual value.
        stripGains.setPan (channelIndex, pan);
    }

    juce::int64 AudioEngine::snapSampleToGrid (juce::int64 sample)
    {
        const auto mode = getSnapMode();
        if (mode == SnapMode::Off || sample < 0) return sample;

        // Markers: pick the nearest marker. Empty marker list -> no snap.
        const auto& list = markers.getAll();
        if (list.empty()) return sample;
        juce::int64 best = list.front().sampleOffset;
        juce::int64 bestDist = std::abs (best - sample);
        for (const auto& m : list)
        {
            const auto d = std::abs (m.sampleOffset - sample);
            if (d < bestDist) { bestDist = d; best = m.sampleOffset; }
        }
        return best;
    }

    void AudioEngine::setTrackEditGroup (int channelIndex, int groupId)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        recorder.getTrack (channelIndex).editGroup.store (groupId, std::memory_order_relaxed);
        // Edit group is authoritative in session_mix.json -- not written to
        // global appProps (an index-keyed global copy leaked across sessions
        // and went stale after a reorder).
    }

    int AudioEngine::getTrackEditGroup (int channelIndex) noexcept
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return -1;
        return recorder.getTrack (channelIndex).editGroup.load (std::memory_order_relaxed);
    }

    std::vector<int> AudioEngine::getStripsInEditGroup (int groupId)
    {
        std::vector<int> out;
        if (groupId < 0) return out;
        const int n = recorder.getNumTracks();
        for (int i = 0; i < n; ++i)
            if (recorder.getTrack (i).editGroup.load (std::memory_order_relaxed) == groupId)
                out.push_back (i);
        return out;
    }

    void AudioEngine::tickRamps (int numSamples) noexcept
    {
        if (numSamples <= 0) return;

        const int n = recorder.getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            auto& t = recorder.getTrack (i);
            const juce::int64 remaining = t.rampSamplesRemaining.load (std::memory_order_relaxed);
            if (remaining <= 0) continue;

            const float curG = t.gainDb.load (std::memory_order_relaxed);
            const float tgtG = t.rampTargetGainDb.load (std::memory_order_relaxed);
            const float curP = t.pan   .load (std::memory_order_relaxed);
            const float tgtP = t.rampTargetPan   .load (std::memory_order_relaxed);

            // Defensive: clamp denominator so a stale remaining can't
            // produce NaN/inf gain values.
            const juce::int64 denom = juce::jmax ((juce::int64) numSamples, remaining);
            const float frac = (float) numSamples / (float) denom;
            const float newG = curG + (tgtG - curG) * frac;
            const float newP = curP + (tgtP - curP) * frac;

            const juce::int64 next = remaining - (juce::int64) numSamples;
            if (next <= 0)
            {
                t.gainDb.store (tgtG, std::memory_order_relaxed);
                t.pan   .store (tgtP, std::memory_order_relaxed);
                t.rampSamplesRemaining.store (0, std::memory_order_relaxed);
            }
            else
            {
                t.gainDb.store (newG, std::memory_order_relaxed);
                t.pan   .store (newP, std::memory_order_relaxed);
                t.rampSamplesRemaining.store (next, std::memory_order_relaxed);
            }
        }
    }

    void AudioEngine::setTrackPan (int channelIndex, float pan)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        pan = juce::jlimit (-1.0f, 1.0f, pan);
        recorder.getTrack (channelIndex).pan.store (pan, std::memory_order_relaxed);
        stripGains.setPan (channelIndex, pan);
    }

    void AudioEngine::setTrackName (int channelIndex, const juce::String& name)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        if (name.isEmpty())
        {
            stripNames.clearName (channelIndex);
            recorder.getTrack (channelIndex).setNameThreadSafe (juce::String (channelIndex + 1));
        }
        else
        {
            stripNames.setName (channelIndex, name);
            recorder.getTrack (channelIndex).setNameThreadSafe (name);
        }
    }

    void AudioEngine::setTrackColour (int channelIndex, juce::Colour c)
    {
        if (channelIndex < 0 || channelIndex >= recorder.getNumTracks()) return;
        if (c.getAlpha() == 0)
        {
            stripColours.clearColour (channelIndex);
            recorder.getTrack (channelIndex).colourARGB.store (0, std::memory_order_relaxed);
        }
        else
        {
            stripColours.setColour (channelIndex, c);
            recorder.getTrack (channelIndex).colourARGB.store (
                c.getARGB(), std::memory_order_relaxed);
        }
    }

    void AudioEngine::audioDeviceStopped()
    {
        const std::lock_guard<std::mutex> lifecycle (deviceLifecycleLock);
        if (isCaptureFinalizing())
        {
            deferredDeviceAudio.store (true, std::memory_order_release);
            deferredDeviceRelease = true;
            deferredDeviceConfiguration.reset();
            player.stop();
            return;
        }
        if (recorder.isRecording())
        {
            recorder.markCaptureDeviceLost();
            // Closing a take can take minutes (a punch rewrites whole files, a
            // lead-in writes silence), and JUCE calls this on the message
            // thread holding its audio lock. It goes to the STOP worker, and
            // what the device does next waits for it like any other STOP.
            if (recorder.beginFinalization())
            {
                launchCaptureStopJob ([this] (bool ok) { if (onTakeInterrupted) onTakeInterrupted (ok); });
                deferredDeviceAudio.store (true, std::memory_order_release);
                deferredDeviceRelease = true;
                deferredDeviceConfiguration.reset();
                return;
            }
        }
        recorder.release();
        player.release();
        {
            const juce::ScopedLock sl (stereoMixLock);
            stereoMixWriter.reset();
        }
    }

    void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                        float* const* outputs, int numOutputs,
                                                        int numSamples,
                                                        const juce::AudioIODeviceCallbackContext&)
    {
        if (deferredDeviceAudio.load (std::memory_order_acquire))
        {
            for (int channel = 0; channel < numOutputs; ++channel)
                if (outputs[channel]) juce::FloatVectorOperations::clear (outputs[channel], numSamples);
            return;
        }

        // CPU-load measurement: capture start time so we can report
        // (callback time / available time) as a load percentage. The
        // dashboard polls audioLoadPct at 4 Hz to drive the LED.
        const auto cbStart = juce::Time::getHighResolutionTicks();

        // 64-bit output accumulator -- every per-strip / stream
        // sum lands here in double precision; the final cast to the
        // device's float buffers happens at the end of the callback.
        // Cheap to ensure the size every block -- JUCE no-ops when
        // already big enough.
        if (outputAccum.getNumChannels() < numOutputs || outputAccum.getNumSamples() < numSamples)
            outputAccum.setSize (juce::jmax (outputAccum.getNumChannels(), numOutputs),
                                 juce::jmax (outputAccum.getNumSamples(), numSamples),
                                 false, false, true);
        for (int ch = 0; ch < numOutputs; ++ch)
            juce::FloatVectorOperations::clear (outputAccum.getWritePointer (ch), numSamples);

        // Always clear outputs first; player + monitor sum into them.
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        // Build a routed-input pointer array: routedInputs[stripIndex] points
        // to the device input that strip is patched from, or nullptr if -1.
        const int numTracks = recorder.getNumTracks();
        constexpr int kMaxStrips = 256;
        const float* routedInputs[kMaxStrips] = {};
        for (int i = 0; i < numTracks && i < kMaxStrips; ++i)
        {
            const int dev = effectiveInputRouting (
                i, recorder.getTrack (i).inputRouting.load (std::memory_order_relaxed), numInputs);
            routedInputs[i] = (dev >= 0 && dev < numInputs) ? inputs[dev] : nullptr;
        }

        // Soft-takeover ramps (cue recall) -- step gain / pan per block
        // toward their target values before they're read downstream by
        // the monitor / per-channel output / stream-bus loops.
        tickRamps (numSamples);

        // Have the player render into a scratch buffer per track, then we
        // route each track to its configured output channel.
        if (playerScratch.getNumChannels() < numTracks || playerScratch.getNumSamples() < numSamples)
            playerScratch.setSize (juce::jmax (numTracks, 1), juce::jmax (numSamples, 64),
                                   false, false, true);
        playerScratch.clear (0, numSamples);
        const bool playerWasRolling = player.isPlaying();
        const auto playbackBlockStart = player.getPositionSamples();
        player.processBlock (playerScratch.getArrayOfWritePointers(),
                             juce::jmin (playerScratch.getNumChannels(), numTracks),
                             numSamples);
        const auto playbackBlockEnd = player.getPositionSamples();

        const bool windowedPunch = captureWindowEnabled.load (std::memory_order_acquire)
                                && recorder.isRecording();
        int punchOffset = 0, punchSamples = numSamples;
        if (windowedPunch)
        {
            const auto in  = captureWindowStart.load (std::memory_order_relaxed);
            const auto out = captureWindowEnd.load (std::memory_order_relaxed);
            const auto played = playerWasRolling
                ? juce::jlimit ((juce::int64) 0, (juce::int64) numSamples,
                                playbackBlockEnd - playbackBlockStart)
                : (juce::int64) 0;
            const auto first = juce::jmax (playbackBlockStart, in);
            const auto last  = juce::jmin (playbackBlockStart + played, out);
            punchOffset = (int) juce::jlimit ((juce::int64) 0, played,
                                              first - playbackBlockStart);
            punchSamples = last > first ? (int) (last - first) : 0;
        }

        if (punchSamples > 0)
        {
            const float* captureInputs[kMaxStrips] {};
            for (int i = 0; i < numTracks && i < kMaxStrips; ++i)
                captureInputs[i] = routedInputs[i] != nullptr
                    ? routedInputs[i] + punchOffset : nullptr;
            recorder.processBlock (captureInputs, juce::jmin (numTracks, kMaxStrips),
                                   punchSamples,
                                   playerWasRolling ? playbackBlockStart + punchOffset
                                                    : (juce::int64) -1);
        }

        // While punching, the recorded input replaces the old take on armed
        // tracks in every monitor/direct/stream route. Outside a scheduled
        // window the old take remains audible for pre/post-roll. This also
        // prevents the master from summing old + live performances together.
        if (playerWasRolling && recorder.isRecording())
            for (int i = 0; i < numTracks && i < kMaxStrips; ++i)
            {
                if (! recorder.getTrack (i).armed.load (std::memory_order_relaxed)) continue;
                auto* dst = playerScratch.getWritePointer (i) + punchOffset;
                if (punchSamples <= 0) continue;
                if (routedInputs[i] != nullptr)
                    juce::FloatVectorOperations::copy (dst,
                        routedInputs[i] + punchOffset, punchSamples);
                else
                    juce::FloatVectorOperations::clear (dst, punchSamples);
            }

        // Trim-Follow moves recorded playback only. A take with the transport
        // stopped feeds live input to the stream bus on every strip, and a
        // punch puts it in the armed strip's own playback row.
        const bool trimFollowing = trimFollowEnabled.load (std::memory_order_relaxed);
        const bool capturing     = recorder.isRecording();
        const auto trimFollowDb = [this, trimFollowing, capturing, playerWasRolling] (int i) noexcept -> float
        {
            if (! trimFollowing) return 0.0f;
            const auto& t = recorder.getTrack (i);
            if (capturing && (! playerWasRolling || t.armed.load (std::memory_order_relaxed))) return 0.0f;
            return mix::trimFollowDb (t.captureInputGainKnown.load (std::memory_order_relaxed),
                                      t.liveInputGainDb.load (std::memory_order_relaxed),
                                      t.captureInputGainDb.load (std::memory_order_relaxed));
        };

        // Strip's effective dB. Defined once here so every consumer below
        // (per-channel mix, stream bus) shares one definition instead of
        // re-deriving it.
        const auto effectiveGainDb = [this, &trimFollowDb] (int i) noexcept -> float
        {
            const float dB = mix::stripDb (recorder.getTrack (i).gainDb.load (std::memory_order_relaxed));
            // Trim-follow: track the console's input-gain change since capture
            // so soundcheck preamp moves shift the recorded track like a live mic.
            return dB + trimFollowDb (i);
        };

        // Drive per-strip meters from the player's output ONLY when the
        // player is actually rolling. The recorder has already written
        // the input-side peak / rms for this block; we max the player
        // contribution on top so a strip with BOTH input AND playback
        // shows whichever is louder. When the player is stopped we
        // leave the recorder's values alone -- otherwise this would
        // clobber the live input meters with zeros from an idle
        // playerScratch buffer.
        if (player.isPlaying())
        {
            const int playerTracks = juce::jmin (playerScratch.getNumChannels(), numTracks);
            for (int ch = 0; ch < playerTracks; ++ch)
            {
                const float* src = playerScratch.getReadPointer (ch);
                if (src == nullptr) continue;
                float pk = 0.0f, sumSq = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float a = std::abs (src[i]);
                    if (a > pk) pk = a;
                    sumSq += src[i] * src[i];
                }
                const float rms = std::sqrt (sumSq / juce::jmax (1, numSamples));
                auto& t = recorder.getTrack (ch);
                // Apply the same 0.92 release decay the recorder uses, so
                // when a transient passes the meter falls back to the
                // ambient level instead of latching at the loudest peak
                // ever observed. Without this, any stereo track whose R
                // partner had no input routing would stick at full scale.
                const float prevPk  = t.peak.load (std::memory_order_relaxed);
                const float prevRms = t.rms .load (std::memory_order_relaxed);
                t.peak.store (juce::jmax (pk,  prevPk  * 0.92f), std::memory_order_relaxed);
                t.rms .store (juce::jmax (rms, prevRms * 0.85f), std::memory_order_relaxed);
                if (pk >= 0.999f)
                {
                    t.clipped.store (true, std::memory_order_relaxed);
                    t.clipCount.fetch_add (1, std::memory_order_relaxed);
                }
            }
        }

        // Mute / solo: when any channel is soloed, only soloed channels pass.
        bool anySolo = false;
        for (int i = 0; i < numTracks; ++i)
            if (recorder.getTrack (i).soloed.load (std::memory_order_relaxed))
            { anySolo = true; break; }

        auto channelAudible = [&] (int i) -> bool
        {
            auto& t = recorder.getTrack (i);
            return mix::audible (t.muted.load (std::memory_order_relaxed),
                                 t.soloed.load (std::memory_order_relaxed), anySolo);
        };

        // Mix the routed track scratch onto the device outputs honoring
        // mute/solo and per-channel gain.
        for (int i = 0; i < numTracks; ++i)
        {
            if (! channelAudible (i)) continue;

            auto& t = recorder.getTrack (i);
            // outputMuted gates the physical output independently of the
            // monitor-side `muted` flag -- so an engineer can drop a track
            // from FOH while still hearing it in their cans (or the
            // reverse).
            if (t.outputMuted.load (std::memory_order_relaxed)) continue;
            const int devOut = t.outputRouting.load (std::memory_order_relaxed);
            if (devOut < 0 || devOut >= numOutputs || outputs[devOut] == nullptr) continue;

            const float dB   = effectiveGainDb (i);
            const double gain = (double) juce::Decibels::decibelsToGain (dB, -60.0f);
            const float* src = playerScratch.getReadPointer (i);
            double* dst = outputAccum.getWritePointer (devOut);

            // Prefetch the next strip's playerScratch row + its target
            // output column into L1 -- at 256+ tracks this hides the
            // ~10-cycle memory miss on each iteration boundary.
            if (i + 1 < numTracks)
            {
                __builtin_prefetch (playerScratch.getReadPointer (i + 1), 0, 1);
                const int nextOut = recorder.getTrack (i + 1).outputRouting
                                      .load (std::memory_order_relaxed);
                if (nextOut >= 0 && nextOut < numOutputs)
                    __builtin_prefetch (outputAccum.getWritePointer (nextOut), 1, 1);
            }

            // NEON-vectorised float-into-double FMA -- about 2.5x
            // faster than the auto-vectorised scalar loop on M1+.
            fastaccum::addFloatScaledIntoDouble (dst, src, gain, numSamples);
        }

        const int trackCount = numTracks;

        // Dedicated streaming stereo bus: sum every strip flagged as
        // streamSend into the configured outputs with their gain+pan.
        // Also captured to StereoMix.wav when recordStereoMix is on, so
        // the bounce hits disk alongside the per-channel multitrack.
        {
        const int sL = streamOutL.load (std::memory_order_relaxed);
        const int sR = streamOutR.load (std::memory_order_relaxed);
        // Try-lock the mix-capture writer: stopRecording() resets it on the
        // message thread while holding stereoMixLock. If we can't acquire it,
        // treat capture as off for this block (the stream bus into outputAccum
        // is unaffected -- only the archive StereoMix.wav write is skipped).
        // Held to the end of this scope so the writer stays valid through the
        // accumulate + write below.
        const juce::ScopedTryLock mixStl (stereoMixLock);
        const bool wantMixCapture = mixStl.isLocked() && recorder.isRecording() && (stereoMixWriter != nullptr);

        if (wantMixCapture)
        {
            stereoMixScratch.setSize (2, numSamples, false, false, true);
            stereoMixScratch.clear();
        }

        const bool mayShare = mixMaySharePatchedOutputs.load (std::memory_order_relaxed);
        bool stripOnStreamOutput = false;
        for (int i = 0; i < trackCount; ++i)
        {
            const int out = recorder.getTrack (i).outputRouting.load (std::memory_order_relaxed);
            stripOnStreamOutput = stripOnStreamOutput || (out >= 0 && (out == sL || out == sR));
        }
        const bool streamHeld = mix::busHeldOffOutputs (stripOnStreamOutput, numOutputs, mayShare);
        streamHeldOff.store (streamHeld, std::memory_order_relaxed);
        const bool haveStreamOutputs = sL >= 0 && sR >= 0 && sL < numOutputs && sR < numOutputs && ! streamHeld;
        // File capture must not depend on physical stream outputs being
        // configured. A recording-only rig often has no spare output pair but
        // still expects StereoMix.wav to contain the selected live inputs.
        if (haveStreamOutputs || wantMixCapture)
        {
            for (int i = 0; i < trackCount; ++i)
            {
                auto& t = recorder.getTrack (i);
                if (! t.streamSend.load (std::memory_order_relaxed)) continue;
                if (! mix::streamAudible (t.muted.load (std::memory_order_relaxed)))
                    continue;

                const float dB   = effectiveGainDb (i);
                const double gain = (double) juce::Decibels::decibelsToGain (dB, -60.0f);
                const auto [gL, gR] = mix::constantPowerPan (gain, t.pan.load (std::memory_order_relaxed));

                const float* src = playerScratch.getReadPointer (i);
                if (recorder.isRecording() && ! playerWasRolling
                    && i < kMaxStrips && routedInputs[i] != nullptr)
                    src = routedInputs[i];
                // Stream bus also goes through the 64-bit accumulator
                // so the L/R sum stays full precision across N strips.
                // Same NEON helper as the per-channel sum above.
                if (haveStreamOutputs && gL > 0.00001)
                    fastaccum::addFloatScaledIntoDouble (outputAccum.getWritePointer (sL),
                                                         src, gL, numSamples);
                if (haveStreamOutputs && gR > 0.00001)
                    fastaccum::addFloatScaledIntoDouble (outputAccum.getWritePointer (sR),
                                                         src, gR, numSamples);

                if (wantMixCapture)
                {
                    juce::FloatVectorOperations::addWithMultiply (stereoMixScratch.getWritePointer (0),
                                                                  src, (float) gL, numSamples);
                    juce::FloatVectorOperations::addWithMultiply (stereoMixScratch.getWritePointer (1),
                                                                  src, (float) gR, numSamples);
                }
            }
        }

        if (wantMixCapture)
        {
            const float* chans[2] = { stereoMixScratch.getReadPointer (0),
                                      stereoMixScratch.getReadPointer (1) };
            if (! stereoMixWriter->write (chans, numSamples))
                stereoMixWriteFailed.store (true, std::memory_order_relaxed);
        }
        }   // release stereoMixLock

        // LTC decode -- analyzes the input of a designated strip every
        // block while LTC is the chase source. Engineer routes the desk's
        // timecode line to that strip and picks it in Transport Sync.
        {
            const int srcStrip = ltcSourceStrip.load (std::memory_order_relaxed);
            if (srcStrip >= 0 && srcStrip < trackCount && routedInputs[srcStrip] != nullptr
                && chaseMode.load (std::memory_order_relaxed) == (int) ChaseMode::Ltc)
            {
                const auto sr = deviceManager.getCurrentAudioDevice() != nullptr
                                ? deviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                                : 48000.0;
                timecodeChase.feedLtc (routedInputs[srcStrip], numSamples, sr);
            }
        }

        // ── Master bus ────────────────────────────────────────────────
        // Every audible channel's VSC playback contributes to the master
        // by default. Channels with MON=on ALSO contribute their live
        // input on top, so the engineer can hear mics during a take.
        // Mute / solo gates apply: if any channel is soloed, only soloed
        // channels contribute (this is what gives the user "solo a track
        // and hear it via the master fader" behaviour).
        const int blk = juce::jmin (numSamples, monitorAccum.getNumSamples());
        monitorAccum.clear (0, 0, blk);
        monitorAccum.clear (1, 0, blk);

        auto* accL = monitorAccum.getWritePointer (0);
        auto* accR = monitorAccum.getWritePointer (1);

        for (int ch = 0; ch < trackCount; ++ch)
        {
            auto& t = recorder.getTrack (ch);
            if (! channelAudible (ch)) continue;

const double dB = (double) mix::stripDb (t.gainDb.load (std::memory_order_relaxed));
            const double gain = juce::Decibels::decibelsToGain (dB, -60.0);
            const auto [gL, gR] = mix::constantPowerPan (gain, t.pan.load (std::memory_order_relaxed));

            // (a) VSC playback -- always summed to master when audible.
            // Trim-follow applies HERE (recorded content) but NOT to live
            // input (b): the desk's input-gain change since capture is added
            // so soundcheck preamp moves track the recorded track. Computed as
            // a separate gain so the live mic path stays untouched.
            if (ch < playerScratch.getNumChannels())
            {
                const float* psrc = playerScratch.getReadPointer (ch);
                if (psrc != nullptr)
                {
                    double gLp = gL, gRp = gR;
                    if (trimFollowing)
                    {
                        const double trim = (double) trimFollowDb (ch);
                        const double m = juce::Decibels::decibelsToGain (dB + trim, -60.0) / juce::jmax (1.0e-9, gain);
                        gLp = gL * m; gRp = gR * m;
                    }
                    for (int i = 0; i < blk; ++i)
                    {
                        const double s = (double) psrc[i];
                        accL[i] += s * gLp;
                        accR[i] += s * gRp;
                    }
                }
            }

            // (b) Live input -- reaches the master when the channel is
            //     armed (so the engineer hears what's about to hit disk)
            //     OR monitor is on (live audition without recording).
            const bool punchedInputIsInPlayback = playerWasRolling && recorder.isRecording()
                && t.armed.load (std::memory_order_relaxed);
            const bool wantInput = ! punchedInputIsInPlayback
                && (t.armed.load (std::memory_order_relaxed)
                    || t.monitor.load (std::memory_order_relaxed));
            if (wantInput)
            {
                const float* isrc = (ch < kMaxStrips) ? routedInputs[ch] : nullptr;
                if (isrc != nullptr)
                    for (int i = 0; i < blk; ++i)
                    {
                        const double s = (double) isrc[i];
                        accL[i] += s * gL;
                        accR[i] += s * gR;
                    }
            }
        }

        // Master fader + master mute act on the summed bus.
        const bool   mMute = masterState.muted.load (std::memory_order_relaxed);
        const double mGain = mix::masterGain (mMute, masterState.gainDb.load (std::memory_order_relaxed));

        // If mono, collapse L+R into accL (and zero accR) so a single
        // output gets the sum and the meter reads as one bar. A one-output
        // device is mono whatever the mode says.
        const auto masterPair = mix::masterOutputs (masterOutL.load (std::memory_order_relaxed),
                                                    masterOutR.load (std::memory_order_relaxed),
                                                    numOutputs);
        const bool stereo = masterStereo.load (std::memory_order_relaxed)
                         && masterPair.left != masterPair.right;
        if (! stereo)
        {
            for (int i = 0; i < blk; ++i)
            {
                accL[i] = (accL[i] + accR[i]) * 0.5;   // average to keep headroom
                accR[i] = 0.0;
            }
        }

        // Drive the L/R master meters from the accumulator BEFORE mGain.
        {
            double mPeakL = 0.0, mPeakR = 0.0;
            double mRmsL  = 0.0, mRmsR  = 0.0;
            for (int i = 0; i < blk; ++i)
            {
                const double aL = std::abs (accL[i]);
                const double aR = std::abs (accR[i]);
                if (aL > mPeakL) mPeakL = aL;
                if (aR > mPeakR) mPeakR = aR;
                mRmsL += accL[i] * accL[i];
                mRmsR += accR[i] * accR[i];
            }
            const double inv = 1.0 / (double) juce::jmax (1, blk);
            const float pkL  = (float) mPeakL;
            const float pkR  = (float) mPeakR;
            const float rmsL = (float) std::sqrt (mRmsL * inv);
            const float rmsR = (float) std::sqrt (mRmsR * inv);

            const float prevPkL  = masterState .peak.load (std::memory_order_relaxed);
            const float prevPkR  = masterStateR.peak.load (std::memory_order_relaxed);
            const float prevRmsL = masterState .rms .load (std::memory_order_relaxed);
            const float prevRmsR = masterStateR.rms .load (std::memory_order_relaxed);

            masterState .peak.store (juce::jmax (pkL,  prevPkL  * 0.92f), std::memory_order_relaxed);
            masterStateR.peak.store (juce::jmax (pkR,  prevPkR  * 0.92f), std::memory_order_relaxed);
            masterState .rms .store (juce::jmax (rmsL, prevRmsL * 0.85f), std::memory_order_relaxed);
            masterStateR.rms .store (juce::jmax (rmsR, prevRmsR * 0.85f), std::memory_order_relaxed);
            if (pkL >= 0.999f) masterState .clipped.store (true, std::memory_order_relaxed);
            if (pkR >= 0.999f) masterStateR.clipped.store (true, std::memory_order_relaxed);
        }

        // Feed the BS.1770 loudness + true-peak meter from the post-fader
        // master (mGain applied inside). RT-safe -- the meter keeps only
        // fixed-size filter / histogram state.
        loudness.processMasterDouble (accL, accR, mGain, blk);

        const int outL = masterPair.left;
        const int outR = stereo ? masterPair.right : -1;

        // Whether the mix reaches its pair, decided from the patch and not
        // from what is audible, so a strip muted for a moment cannot let the
        // whole mix drop onto its console channel.
        bool stripOnMasterOutput = false;
        for (int i = 0; i < trackCount; ++i)
        {
            const int out = recorder.getTrack (i).outputRouting.load (std::memory_order_relaxed);
            stripOnMasterOutput = stripOnMasterOutput || (out >= 0 && (out == outL || out == outR));
        }
        const bool masterHeld = mix::busHeldOffOutputs (stripOnMasterOutput, numOutputs,
                                                        mixMaySharePatchedOutputs.load (std::memory_order_relaxed));
        masterHeldOff.store (masterHeld, std::memory_order_relaxed);
        const bool masterAudible = outL >= 0 && ! masterHeld;

        if (masterAudible)
        {
            // Monitor sum lands in the 64-bit accumulator so the master
            // bus stays double-precision until the final downcast below.
            double* dstL = outputAccum.getWritePointer (outL);
            for (int i = 0; i < blk; ++i)
                dstL[i] += accL[i] * mGain;
        }

        if (masterAudible && outR >= 0)
        {
            double* dstR = outputAccum.getWritePointer (outR);
            for (int i = 0; i < blk; ++i)
                dstR[i] += accR[i] * mGain;
        }

        // Final downcast: the 64-bit accumulator → device-output float
        // buffers. Done in one place at the very end so per-strip /
        // stream / monitor sums all stayed at double precision.
        // NEON-vectorised on Apple Silicon; scalar fallback elsewhere.
        for (int ch = 0; ch < numOutputs; ++ch)
        {
            if (outputs[ch] == nullptr) continue;
            // Master mute is the emergency kill for the selected speaker pair.
            // Per-strip and stream routes are accumulated before the master bus;
            // when one shares this pair, multiplying only the master sum by zero
            // leaves that earlier contribution audible. Silence the pair at the
            // device boundary while preserving unrelated direct/FOH outputs.
            if (mMute && (ch == outL || ch == outR))
            {
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);
                continue;
            }
            fastaccum::downcastDoubleToFloat (outputs[ch],
                                               outputAccum.getReadPointer (ch),
                                               numSamples);
        }

        // CPU-load: (callback wall time) / (block period). EMA-smoothed
        // with a fast attack and slower release so the engineer sees
        // spikes immediately but the meter doesn't twitch on each block.
        {
            const auto cbEnd       = juce::Time::getHighResolutionTicks();
            const double cbSeconds = juce::Time::highResolutionTicksToSeconds (cbEnd - cbStart);
            const double sr        = deviceSampleRate.load (std::memory_order_relaxed);
            const double available = (sr > 0.0) ? ((double) numSamples / sr) : 0.0;
            if (available > 0.0)
            {
                const float pct  = (float) juce::jlimit (0.0, 100.0, (cbSeconds / available) * 100.0);
                const float prev = audioLoadPct.load (std::memory_order_relaxed);
                const float ema  = (pct > prev) ? pct : (prev * 0.85f + pct * 0.15f);
                audioLoadPct.store (ema, std::memory_order_relaxed);
            }
        }
    }
}
