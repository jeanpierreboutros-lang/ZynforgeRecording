#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Markers.h"
#include "VcaBus.h"
#include "../Network/RemoteControl.h"

#include <array>
#include <functional>
#include <map>
#include "ClipModel.h"
#include "EngineStatus.h"
#include "MultitrackRecorder.h"
#include "SessionPlayer.h"
#include "LoudnessMeter.h"
#include "MixKernel.h"
#include "StripColours.h"
#include "StripGains.h"
#include "StripNames.h"
#include "StripRouting.h"
#include "SongDetector.h"
#include "TimecodeChase.h"
#include "TimelineCut.h"

#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace zynforge
{
    namespace interchange { struct SessionInput; }

    class MainTransportRegressionTests;

    class AudioEngine final : public juce::AudioIODeviceCallback,
                              public juce::MidiInputCallback
    {
        friend class SettingsBatchAuditTests;
        friend class MainTransportRegressionTests; // deterministic STOP failure-latch fixtures
    public:
        AudioEngine();
        ~AudioEngine() override;

        // Remote transports (OSC, desk macros, control surfaces) all enter
        // through this one message-thread boundary.  The host may intercept a
        // command -- capture-daemon mode does this so a remote cannot silently
        // start/stop the in-process recorder instead of the protected daemon.
        enum class RemoteTransportAction
        {
            TogglePlay,
            StartPlay,
            StopPlay,
            StopAll,
            ToggleRecord,
            StartRecord,
            StopRecord
        };
        using RemoteTransportHandler = std::function<std::optional<bool>
            (RemoteTransportAction, juce::String& error)>;
        void setRemoteTransportHandler (RemoteTransportHandler handler)
        { remoteTransportHandler = std::move (handler); }
        bool performRemoteTransport (RemoteTransportAction, juce::String& error);

        // Told the final name of every marker dropped at the transport, from
        // whichever surface dropped it. Fires on the caller's thread.
        // The audio device ended a take: it has finished finalizing and the
        // session is loaded. Message thread. A take that ends this way has no
        // STOP press behind it, so nothing else would reset the transport.
        std::function<void (bool ok)> onTakeInterrupted;
        std::function<void (const juce::String&)> onMarkerDropped;

        // Queued callbacks must not capture a raw AudioEngine*.  The handle is
        // nulled at the very start of destruction and remains alive for any
        // already-queued callback that needs to notice shutdown safely.
        using AsyncHandle = std::shared_ptr<std::atomic<AudioEngine*>>;
        AsyncHandle getAsyncHandle() const noexcept { return asyncHandle; }

        // Set BEFORE constructing the engine to skip
        // AudioDeviceManager::initialise + addAudioCallback. Tests
        // use this so they can exercise the lane / persistence
        // surface without opening a 256-channel audio device.
        static void setTestModeSkipAudioInit (bool skip) noexcept;

        // Test-only: do the same setup `audioDeviceAboutToStart` does
        // (sample rate, scratch buffers, recorder/player prepare) but
        // without needing a real `juce::AudioIODevice`. Lets unit tests
        // call `audioDeviceIOCallbackWithContext` directly with
        // synthetic buffers and assert what comes out.
        void prepareForTests (double sampleRate, int blockSize);
        // Test-only: with no device open, report `n` active inputs, so the
        // smaller-device routing rules run without hardware.
        void setDeviceInputCountForTests (int n) noexcept { testDeviceInputs.store (n, std::memory_order_relaxed); }

        juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
        MultitrackRecorder&       getRecorder()      noexcept { return recorder; }
        SessionPlayer&            getPlayer()        noexcept { return player; }
        juce::PropertiesFile*     getAppProps()      noexcept       { return appProps.get(); }
        juce::PropertiesFile*     getAppProps() const noexcept       { return appProps.get(); }
        TimecodeChase&            getTimecodeChase() noexcept { return timecodeChase; }

        // ── External timecode chase (LTC from audio / MTC from MIDI) ────
        enum class ChaseMode { Off = 0, Ltc = 1, Mtc = 2 };
        void      setChaseMode (ChaseMode m) noexcept
        {
            // Clear the freewheel state on an actual mode change so a stale MTC
            // freewheel timestamp can't poison the new source. Without this,
            // using MTC then switching the master to LTC left isRunning()
            // permanently false (lastMtcTickMs stayed nonzero + aged past the
            // timeout), so LTC never chased for the rest of the run.
            // clearFreewheel() (not reset()) because the audio/MIDI feed threads
            // may still be running -- reset() writes non-atomic decoder members
            // they own, which is a data race on a live mode switch.
            if ((ChaseMode) chaseMode.exchange ((int) m, std::memory_order_relaxed) != m)
                timecodeChase.clearFreewheel();
        }
        ChaseMode getChaseMode() const noexcept       { return (ChaseMode) chaseMode.load (std::memory_order_relaxed); }
        // MTC input device (MIDI). "" = none / close. Returns success.
        juce::StringArray getMidiInputNames() const;
        bool              setMtcInputByName (const juce::String& name);
        juce::String      getMtcInputName() const { return mtcInputName; }
        // True while a chase source is selected AND that source is live:
        // decoded, consistent LTC frames, or MTC still streaming. Per mode,
        // so an MTC port left open can't make an LTC chase look locked.
        bool        isChaseLive() const noexcept
        {
            const auto mode = getChaseMode();
            return mode == ChaseMode::Ltc ? timecodeChase.isLtcLocked()
                 : mode == ChaseMode::Mtc && timecodeChase.isMtcRunning();
        }
        // The position (in the player's sample domain) the chased timecode
        // currently points at, and a HH:MM:SS:FF string for display.
        // Where the timecode puts the playhead: its label minus the session's
        // start timecode. Can fall outside the session; see getChaseParkReason.
        juce::int64  getChaseTargetSamples() const;
        // The timecode that lines up with the session's first sample, saved
        // with the session. 00:00:00:00 unless set: house timecode and any
        // show that starts at 01:00:00:00 need it, or the target is hours out.
        juce::String getChaseStartTimecode() const;
        bool         setChaseStartTimecode (const juce::String& hhmmssff);   // false = not a timecode
        // Line the session's first sample up with the first take's BWF stamp,
        // the time of day it was recorded. False when no take carries one.
        bool         setChaseStartFromTake();
        // Empty while the timecode lands inside the session. Otherwise why
        // chase is holding off instead of throwing the playhead at the end.
        juce::String getChaseParkReason() const;
        juce::String getChaseTimecodeString() const;
        // The running timecode as a time of day for a take's BWF stamp. False
        // unless chase is live right now; the take then uses the wall clock.
        bool         getTimecodeAnchor (bwf::TimeAnchor& anchor) const;
        // When the timecode master parks/drops, keep playing (free-run) instead
        // of stopping the transport. Off = stop when sync is lost.
        void setKeepRollingOnSyncLoss (bool on) noexcept { keepRollingOnSyncLoss.store (on, std::memory_order_relaxed); }
        bool isKeepRollingOnSyncLoss () const noexcept   { return keepRollingOnSyncLoss.load (std::memory_order_relaxed); }

        // juce::MidiInputCallback -- decodes incoming MTC on the MIDI thread.
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

        // -1 = no LTC source. Otherwise the strip whose device input
        // feeds the LTC zero-crossing analyzer once per audio block.
        void setLtcSourceStrip (int oneBasedIndex) noexcept;
        int  getLtcSourceStrip() const noexcept { return ltcSourceStrip.load() + 1; }

        // ── Master bus ────────────────────────────────────────────────
        // The master sums every audible channel's VSC playback plus every
        // channel whose MON flag is on (live input monitoring). Mute and
        // solo gates apply: any solo engaged → only soloed channels reach
        // the master.
        TrackState& getMasterState()  noexcept { return masterState; }
        TrackState& getMasterStateR() noexcept { return masterStateR; }

        // ── Master loudness (ITU-R BS.1770-style) ─────────────────────
        // Measured on the post-fader stereo master. Momentary/short-term
        // are live; integrated is gated program loudness since the device
        // started. True-peak is a 4x-oversampled estimate (dBTP).
        float getMomentaryLufs()  const noexcept { return loudness.getMomentaryLufs(); }
        float getShortTermLufs()  const noexcept { return loudness.getShortTermLufs(); }
        float getIntegratedLufs() const noexcept { return loudness.getIntegratedLufs(); }
        float getTruePeakDb()     const noexcept { return loudness.getTruePeakDb(); }
        float getMasterGainDb() const noexcept { return masterState.gainDb.load(); }
        bool  getMasterMuted()  const noexcept { return masterState.muted.load(); }
        void  setMasterGainDb (float dB);
        // Atomic-only master gain store for real-time / MIDI-thread callers
        // (a motor-fader move): no persistence, no file I/O. The session save
        // path still goes through setMasterGainDb.
        void  setMasterGainDbFast (float dB) noexcept { masterState.gainDb.store (dB, std::memory_order_relaxed); }
        void  setMasterMuted  (bool m);
        // setMasterOutputs (-1, -1) gives the master bus no output: it still
        // meters, and nothing lands on an interface channel.
        int   getMasterOutputL() const noexcept { return masterOutL.load(); }
        int   getMasterOutputR() const noexcept { return masterOutR.load(); }
        void  setMasterOutputs (int l, int r);
        // True while the audio callback keeps the master bus off its outputs
        // because a strip is patched to one (mix::busHeldOffOutputs).
        bool  isMasterHeldOff() const noexcept { return masterHeldOff.load (std::memory_order_relaxed); }
        bool  isStreamHeldOff() const noexcept { return streamHeldOff.load (std::memory_order_relaxed); }
        // The engineer's override: this rig's first outputs really are
        // monitors, so the mix may sit on top of strips patched there.
        void  setMixMayShareStripOutputs (bool allowed);
        bool  getMixMayShareStripOutputs() const noexcept { return mixMaySharePatchedOutputs.load (std::memory_order_relaxed); }
        // Empty, or a line saying which bus is held off its outputs and why.
        juce::String getBusRoutingNotice() const;

        // Mono / stereo mode. In mono, L+R are summed and sent only to
        // masterOutL; the meter collapses to a single bar. In stereo, L
        // goes to masterOutL and R to masterOutR; the meter shows two
        // bars driven by masterState (L) and masterStateR (R).
        bool  getMasterStereo() const noexcept { return masterStereo.load(); }
        void  setMasterStereo (bool stereo);

        bool startRecording (const juce::File& sessionDir);
        void stopRecording();
        // UI entry: accepted once, completion on UI after media/player readiness
        // and reader retirement. A false return never invokes completion.
        bool stopRecordingAsync (std::function<void (bool)> completion);
        bool isCaptureFinalizing() const noexcept { return asyncCaptureFinalizing.load(); }
        // Excludes generated reference beds whose length is unrelated to a take.
        juce::int64 getCaptureLengthSamples();
        bool isRecording() const noexcept { return recorder.isRecording() || externalRecording.load(); }
        void setExternalRecording (bool active) noexcept;
        bool isExternalRecording() const noexcept { return externalRecording.load (std::memory_order_acquire); }
        // A daemon take is rolling into sessionDir: from here on it is this
        // engine's session, for markers above all, exactly as if it had
        // started here. Only the daemon's own folder may take a marker.
        void beginExternalTake (const juce::File& sessionDir, double sampleRate);
        // A daemon take ended: its files are final, and only reading them back
        // is left. Off the message thread like a local STOP, for the same
        // reason. False if a finalization is already running.
        bool finishExternalTakeAsync (const juce::File& sessionDir, juce::int64 recordEnd,
                                      std::function<void (bool)> completion);
        void setControlsLocked (bool locked) noexcept { controlsLocked.store (locked); }
        bool areControlsLocked() const noexcept { return controlsLocked.load(); }
        void setExternalCaptureStatus (const EngineStatus& status, juce::int64 receivedAtMs = 0);
        void invalidateExternalCaptureStatus();
        void setSessionTransitionActive (bool active) noexcept
        { sessionTransitionActive.store (active, std::memory_order_release); }
        // True from the moment a take rolls until its files are final. Every
        // clip and take setter refuses then: the UI lock stops new gestures, but
        // not a menu or dialog already open, and not a remote start under it.
        bool isArrangementFrozen() const noexcept { return isRecording() || isCaptureFinalizing(); }
        bool isSessionTransitionActive() const noexcept
        { return sessionTransitionActive.load (std::memory_order_acquire) || isCaptureFinalizing(); }

        // Stereo mix bus → file. When enabled, every startRecording also
        // opens a stereo StereoMix.wav writer in the session dir which
        // captures the engine's stream-bus summing block-by-block from
        // the audio thread. Engineer uses this for archive-quality
        // streaming bounces or to hand a stereo mix to a streaming
        // platform without re-mixing.
        void setRecordStereoMix (bool enabled);
        bool getRecordStereoMix() const noexcept           { return recordStereoMixFlag.load(); }
        bool hasStereoMixWriteFailed() const noexcept      { return stereoMixWriteFailed.load(); }

        // Load a session's audio into the player + seed clips. preserveEdits
        // is false for a genuine session OPEN (wipe stale clips; the caller's
        // .zfproj playlist restore repopulates) and true for a SAME-session
        // reload that must keep the engineer's edits.
        int  loadSession (const juce::File& sessionDir, bool preserveEdits = false, bool appendRecordedAudio = false);

        // Per-SESSION strip mix state: name, colour, gain, pan, mute, solo,
        // monitor, arm, input/output routing, stereo flag, VCA + edit group.
        // These otherwise live only in global appProps keyed by strip index,
        // so they'd leak between different sessions opened at the same
        // positions. saveSessionMixTo writes them all into session_mix.json
        // in the session folder; loadSessionMixFrom sizes the recorder to the
        // saved count and applies the file, making the session authoritative
        // for its own mixer state. Returns false for a missing or malformed
        // file so callers can recover a sensible layout from recorded audio.
        juce::String captureSessionMixJson(); // message-thread snapshot; no filesystem I/O
        bool saveSessionMixTo   (const juce::File& sessionDir);
        bool loadSessionMixFrom (const juce::File& sessionDir);
        // Drop every piece of session-owned state before opening or creating
        // another session.  Keeping this in AudioEngine makes non-UI entry
        // points obey the same boundary as File > Open/New.
        void clearSessionState();
        void startPlayback()
        {
            if (isCaptureFinalizing()) return;
            // Never play back on top of a live take. This is a recorder, not
            // a DAW -- summing the previous take into the monitor/master mid-
            // record is always wrong. The UI PLAY button guards this, but
            // network/remote entry points (OSC play, MCU Play) and timecode
            // chase reach here directly, so the guard belongs at the engine.
            if (isRecording() && ! captureWindowEnabled.load (std::memory_order_acquire)) return;
            // Nothing to roll (no session, or parked at/after the end with no
            // loop to wrap): don't start.
            if (! player.isPlaying() && ! player.hasAudioAhead()) return;
            player.start();
        }
        void stopPlayback() { player.stop(); }

        // ── VCA buses ─────────────────────────────────────────────
        // 8 group "ghost faders". Strips opt-in via setTrackVcaGroup
        // and inherit the bus's gainDb / muted. Set with ramping to
        // avoid clicks when a cue snapshot recalls a different value.
        static constexpr int kNumVcas = 8;
        VcaBus&  getVca (int idx) noexcept { return vcas[(size_t) juce::jlimit (0, kNumVcas - 1, idx)]; }
        const VcaBus&  getVca (int idx) const noexcept { return vcas[(size_t) juce::jlimit (0, kNumVcas - 1, idx)]; }
        void  setVcaGainDb  (int idx, float dB);
        void  setVcaGainDbRamped (int idx, float dB, double seconds);
        void  setVcaMuted   (int idx, bool muted);
        void  setVcaSoloed  (int idx, bool soloed);
        void  setVcaName    (int idx, const juce::String& name);
        void  setVcaColour  (int idx, juce::Colour c);
        void  setTrackVcaGroup (int channelIndex, int vcaIdx);  // -1 = unassigned

        // Snap mode -- shared between the host (which sets it from
        // the Edit menu / '4' hotkey) and the edit operations that
        // should respect it (split, eventually clip move + loop in/out).
        // Off = no snap, Markers = snap to nearest marker.
        enum class SnapMode : int { Off = 0, Markers = 1 };
        void     setSnapMode (SnapMode m) noexcept { snapMode.store ((int) m, std::memory_order_relaxed); }
        SnapMode getSnapMode() const noexcept { return (SnapMode) snapMode.load (std::memory_order_relaxed); }

        // Snaps a sample position to the active snap grid. Off returns
        // the input unchanged; Markers picks the nearest marker.
        juce::int64 snapSampleToGrid (juce::int64 sample);

        // Edit Group assignment. -1 = unlinked; any non-negative id
        // links the strip with every other strip carrying the same
        // id. Broadcasting is done at the UI layer when the engineer
        // fires a fader / pan / mute / solo / arm gesture; the
        // engine just stores the group id and exposes the membership
        // query.
        void  setTrackEditGroup (int channelIndex, int groupId);
        int   getTrackEditGroup (int channelIndex) noexcept;
        std::vector<int> getStripsInEditGroup (int groupId);

        // Aux sends + bus tracks ─────────────────────────────────────
        // setTrackIsBus marks a track as a bus destination (no input,
        // no record, sums sends + routes to outputs like a regular
        // strip).
        void  setTrackIsBus (int channelIndex, bool isBus);
        // setTrackSend configures one of 4 send slots on a non-bus
        // strip. targetBus < 0 disables the send. Persisted to appProps
        // so sends survive a relaunch.
        void  setTrackSend (int channelIndex, int sendSlot,
                            int targetBus, float levelDb, bool postFader);
        bool isPlaying() const noexcept                    { return player.isPlaying(); }

        MarkersManager& getMarkers() noexcept              { return markers; }

        // Updates TrackState colour (atomic) AND persists via StripColours.
        // Pass an empty colour (alpha == 0) to revert to the default.
        void setTrackColour (int channelIndex, juce::Colour);

        // Updates TrackState::name AND persists via StripNames.
        // Empty string reverts to the default "In N" label.
        void setTrackName (int channelIndex, const juce::String&);

        // Per-channel playback gain (dB) + pan (-1..+1). Both persist.
        void  setTrackGainDb (int channelIndex, float dB);
        void  setTrackPan    (int channelIndex, float pan);
        // Soft-takeover variants -- interpolate over the given duration.
        // Used by cue recall so a setlist jump doesn't audibly click
        // when the cue's stored fader / pan values differ from the
        // engineer's current settings.
        void  setTrackGainDbRamped (int channelIndex, float dB,  double seconds);
        void  setTrackPanRamped    (int channelIndex, float pan, double seconds);
        // Per-block ramp stepper -- called from the audio callback.
        void  tickRamps (int numSamples) noexcept;

        // Routing. -1 = unrouted. The input is stored as REQUESTED (and
        // saved that way); getEffectiveInputRouting is what the strip hears
        // on the current device -- what MIXER / EDIT / PATCH display.
        void  setTrackInputRouting  (int channelIndex, int deviceCh);
        int   getEffectiveInputRouting (int channelIndex);
        void  setTrackOutputRouting (int channelIndex, int deviceCh);

        // Sets both input AND output for the strip to the same hardware
        // channel index -- the standard VSC workflow where strip N is
        // physical channel N on both sides. PATCH page + per-strip combos
        // both call this so input and output stay linked.
        void  setTrackLinkedRouting (int channelIndex, int deviceCh);

        int   getCurrentDeviceInputCount()  const;
        int   getCurrentDeviceOutputCount() const;

        // Dedicated streaming stereo bus. setStreamOutputs(-1, -1) disables.
        // Tracks with TrackState::streamSend=true mix into these outputs.
        void  setStreamOutputs (int leftCh, int rightCh);
        int   getStreamOutputL() const noexcept { return streamOutL.load (std::memory_order_relaxed); }
        int   getStreamOutputR() const noexcept { return streamOutR.load (std::memory_order_relaxed); }
        void  setTrackStream   (int channelIndex, bool enabled);

        // App-level: persisted user-chosen strip count. Defaults to 1 on
        // first launch. setStripCount safely detaches the audio callback,
        // mutates the track vector, reapplies persisted per-strip state,
        // and re-attaches.
        void setStripCount (int n);

        // Add / remove a single strip without disturbing the others.
        // The audio callback is briefly detached while the recorder
        // mutates its track vector.
        void addOneStrip();
        bool removeStripAt (int index);

        // Wipe every strip-level override stored in appProps (names,
        // colours, gains, pans, routing, stereo flags, UUIDs). Called
        // when the engineer starts a fresh session so a stale pan
        // value from a previous show doesn't carry over to a new band.
        void clearAllStripOverrides();

        // Persisted mono/stereo state for a strip. The L track holds
        // the flag; R partner is implicit at trackIndex + 1.
        void setTrackStereo (int channelIndex, bool isStereoPair);

        // Stereo topology: collapse the physical track grid (where a
        // stereo pair occupies two adjacent tracks) into the logical
        // strip ordinals every view shares, and back. Lives on the engine
        // because it's a pure function of the recorder's per-track stereo
        // flags -- not UI state -- so MIXER / EDIT / PATCH can all agree
        // without routing through MainComponent.
        int physicalFromLogical (int logical);
        int logicalFromPhysical (int physical);

        // Swap two physical tracks completely: state fields (name,
        // colour, gain, pan, routing, mute/solo/mon/arm, stereo flag),
        // persisted overrides, and the underlying Track_NN.wav files in
        // the active session's Audio Files/ folder. UI keeps holding the
        // same TrackState references -- only their contents swap -- so
        // strips don't dangle. Returns true on success.
        bool swapTracks (int a, int b);
        bool reorderTracks (const std::vector<int>& order);
        std::function<void (const std::vector<int>&)> onTracksReordered;

        // Punch in/out -- the player auto-arms enabled punch tracks when
        // the playhead enters the loop region and disarms again when it
        // leaves. The actual record state is driven on the message
        // thread by MainComponent's timer polling getPunch* and
        // toggling startRecording / stopRecording / track.armed.
        bool isPunchModeOn()      const noexcept { return punchEnabled.load (std::memory_order_relaxed); }
        // Turning it on holds loop-play off so a pass crosses the punch-out;
        // turning it off puts loop-play back. clearSessionState ends the mode.
        void setPunchModeOn (bool on) noexcept;
        // Arm the next startRecording as a punch-in at `punchInSample` -- the
        // freshly-recorded audio is spliced into the existing take there (audio
        // before the punch-in and after the punch-out is preserved). Forwarded
        // to the recorder; auto-cleared on stop. See MultitrackRecorder.
        void armPunchIn (juce::int64 punchInSample) noexcept { recorder.armPunchIn (punchInSample); }
        // A selection punch opens its writers before transport rolls, then the
        // audio callback admits only samples inside [start, end). This avoids
        // 10 Hz UI-timer quantisation and permits sub-block punch windows.
        void setCaptureWindow (juce::int64 start, juce::int64 end) noexcept
        {
            captureWindowStart.store (start, std::memory_order_relaxed);
            captureWindowEnd.store (end, std::memory_order_relaxed);
            captureWindowEnabled.store (end > start, std::memory_order_release);
        }
        void clearCaptureWindow() noexcept
        { captureWindowEnabled.store (false, std::memory_order_release); }
        bool isCaptureWindowActive() const noexcept
        { return captureWindowEnabled.load (std::memory_order_acquire); }
        // Arm the next startRecording to CONTINUE the take as a new part file
        // (append, existing files untouched), continuing the timeline from
        // `baseSamples` (the existing take length). See armContinue.
        void armContinue (juce::int64 baseSamples) noexcept { recorder.armContinue (baseSamples); }
        // Recording position on the timeline (take base + samples so far) -- for
        // the playhead / clock, so a continue carries on from where it stopped.
        // The last daemon position holds the view steady during a link loss;
        // captureStatus separately marks that snapshot unavailable.
        juce::int64 getRecordTimelineSamples()
        {
            if (! externalRecording.load (std::memory_order_acquire))
                return recorder.getRecordTimelineSamples();
            const auto status = captureStatus();
            if (status.source == "daemon") return status.positionSamples;
            std::lock_guard<std::mutex> lock (externalStatusLock);
            return externalStatus.positionSamples;
        }
        // The loop region from SessionPlayer doubles as the punch window
        // -- there's only ever one 'do this between A and B' selection.

        // Console OSC (log + channel-name capture), desk macros and the
        // MIDI control surface. Owned here so their lifetime nests inside the
        // engine's; see Source/Network/RemoteControl.h.
        RemoteControl&       getRemotes() noexcept       { return remotes; }
        const RemoteControl& getRemotes() const noexcept { return remotes; }

        // Returns recording dir if recording, else loaded playback session,
        // else an empty File.
        // ── Pair-aware, thread-safe track toggles ──────────────────────────
        // The ONE place remote surfaces (OSC, MCU) may flip
        // mute / solo / arm. Two things they all got wrong doing it themselves:
        //
        //  1. STEREO. A pair is two adjacent strips with isStereo on the L, and
        //     "mute/solo/arm mirror across the pair -- never half-update" is a
        //     documented invariant. It was implemented only in ChannelStrip
        //     (via its pairState pointer), so every non-UI entry point left one
        //     leg of a stereo pair in the opposite state.
        //  2. LIFETIME. They range-checked getNumTracks() and then called
        //     getTrack() -- a TOCTOU against the message thread shrinking the
        //     vector. These take the recorder's structure lock for both.
        //
        // Returns false when the index is out of range.
        bool setTrackMuted  (int index, bool v);
        bool setTrackSoloed (int index, bool v);
        bool setTrackArmed  (int index, bool v);
        // Read-modify-write under the same lock, for surfaces with a toggle
        // button and no state of their own (MCU). Returns the NEW value.
        bool toggleTrackMuted  (int index);
        bool toggleTrackSoloed (int index);
        bool toggleTrackArmed  (int index);

        juce::File getActiveSessionDir() const;

        // Pin the active session folder explicitly (used by New Session...
        // and Open Session...). Without this, the engine reports an active
        // session only while recording or while a player has loaded one
        // -- which left Save / Save As greyed out right after creating an
        // empty session. Cleared by passing an empty / non-directory file.
        void setActiveSessionDir (const juce::File& dir);
        // Which session the recorder's strips are the mixer of. Pinning a
        // session says nothing about this: the launch pin and a reattached
        // daemon take pin one before its mixer exists in memory. Claim only
        // after the mixer was loaded from `dir` or created for it. Every save
        // of mixer state is refused anywhere else.
        void claimMixerFor (const juce::File& dir) { mixerSession = dir; }
        juce::File getMixerSession() const { return mixerSession; }
        // The folder of the last take that really started, kept with the take's
        // capture identity so a different folder moved to that path is not
        // mistaken for it. Survives a relaunch; verify_take.sh reads it too.
        void noteLastCapture (const juce::File& dir);
        void forgetLastCapture();
        juce::File getLastCapture() const;
        bool mixerBelongsTo (const juce::File& dir) const
        { return dir != juce::File() && mixerSession == dir; }

        // Edit cursor (Pro Tools-style insertion point). Distinct from
        // the playhead: a single click in the EDIT wave pane sets it,
        // a marker drop / split / paste uses it instead of the
        // transport position. -1 means 'unset; fall back to playhead'.
        void          setEditCursorSample (juce::int64 sample) noexcept;
        juce::int64   getEditCursorSample() const noexcept;
        void          clearEditCursor() noexcept { setEditCursorSample (-1); }

        // Per-track clip list. Lazy: a track stays in 'whole file' mode
        // (no entry in trackClips, or one full-range entry) until the
        // engineer splits or trims it. The EDIT view + future playback
        // path read from here when present; falls back to the underlying
        // Track_NN.wav otherwise.
        std::vector<Clip>& clipsFor (int track);
        const std::vector<Clip>* tryClipsFor (int track) const;
        // True when the track HAS a clip list but it is empty -- i.e. the
        // engineer deleted every clip (or the track has no audio). Distinct
        // from a fresh/live take that never had a clip list (track absent
        // from trackClips). The EDIT view uses this to NOT fall back to the
        // whole-file thumbnail after a delete -- otherwise the deleted
        // waveform reappears because the Track_NN.wav is still on disk.
        bool isTrackArrangementEmpty (int track) const noexcept;
        // Mirror the live trackClips[track] into the active take so the
        // documented "active take == trackClips" invariant holds after
        // every clip edit. Without this, split / trim / move / fade /
        // gain / mute / delete / duplicate mutate trackClips but leave
        // the take stale -- which silently breaks both .zfproj
        // persistence AND playlistsToJson-based undo (the snapshot reads
        // the take, not the live clips). Creates Take 1 if none exists.
        void syncActiveTake (int track);
        // Populate every track with a single full-range clip so the EDIT
        // tools have something to grab. Called after loadSession() or
        // when stopRecording() auto-loads the just-captured files.
        //   preserveEdits == false (session OPEN): wipe every track's clips
        //     + takes and reseed defaults. The .zfproj playlist restore
        //     that follows a real session open overwrites these anyway.
        //   preserveEdits == true  (SAME-session reload: stop-recording,
        //     strip reorder): keep every edited track's splits/fades/takes
        //     verbatim and only (re)seed tracks that are still the plain
        //     full-range default -- never silently discard edits.
        void seedDefaultClips (bool preserveEdits = false, bool appendRecordedAudio = false);
        // Lazy single-track bootstrap of the above: ensure `track` has at
        // least a full-range clip. Returns false if no audio backs it.
        bool ensureClipList (int track);

        // ── Offline render (consolidate / bounce) ─────────────────────
        // Length of the edited arrangement = the furthest clip end across
        // all tracks (falls back to the loaded file length).
        juce::int64 getArrangementLengthSamples();
        // Render one track's active-take clip arrangement -- fades
        // (linear / equal-power), clip gain, mute, clip positions -- to a
        // flat mono buffer of `totalSamples`. Reads the track's source
        // file offline (NOT real-time-safe; call from a background thread).
        // Returns false if there's no source audio for the track.
        bool renderTrackArrangement (int track, juce::AudioBuffer<float>& out,
                                     juce::int64 totalSamples);
        // Render the whole edited arrangement summed to a stereo mix --
        // each track's clip render (above) through its gain / pan / mute /
        // solo + VCA, then master gain.
        // Offline; call from a background thread.
        bool renderStereoMix (juce::AudioBuffer<float>& outStereo, juce::int64 totalSamples);

        // Windowed render cores. Both render in fixed 64k-sample windows
        // and hand each window to `consume`, so memory stays O(window)
        // instead of O(arrangement length) -- a multi-hour show bounces in
        // a few MB of RAM. The two renders above are built on these;
        // consume returning false aborts the render (and the render
        // returns false).
        bool forEachArrangementWindow (int track, juce::int64 startSample, juce::int64 endSample,
                                       const std::function<bool (const float*, juce::int64, int)>& consume);
        bool forEachStereoMixWindow (juce::int64 totalSamples,
                                     const std::function<bool (const juce::AudioBuffer<float>&, juce::int64, int)>& consume);
        // The same render over [startSample, endSample) of the timeline. A
        // range is the same span of a whole render, sample for sample, whatever
        // grid it lands on. A window handed to `consume` starts at the sample
        // the second argument names.
        bool forEachStereoMixWindow (juce::int64 startSample, juce::int64 endSample,
                                     const std::function<bool (const juce::AudioBuffer<float>&, juce::int64, int)>& consume);
        // A stereo pair's two edited arrangements on one buffer's two channels.
        bool forEachStereoPairWindow (int trackL, juce::int64 startSample, juce::int64 endSample,
                                      const std::function<bool (const juce::AudioBuffer<float>&, juce::int64, int)>& consume);

        // Streaming bounce variants: identical audio to the renders above,
        // but each window is written straight to a 24-bit WAV instead of
        // accumulating in RAM. Use these for File ▸ Bounce on real
        // sessions; the buffer-returning renders are for tests / short
        // material. On failure the partial file is deleted.
        // `cancel` (optional) is polled per render window so a quit / a new
        // bounce can abort a multi-hour render promptly instead of hanging the
        // join. On cancel the partial file is deleted and false is returned.
        bool bounceTrackArrangementToWav (int track, const juce::File& dest,
                                          juce::int64 totalSamples, double sampleRate,
                                          const std::atomic<bool>* cancel = nullptr);
        // `peak`, when given, receives the render's sample peak.
        bool bounceStereoMixToWav (const juce::File& dest,
                                   juce::int64 totalSamples, double sampleRate,
                                   const std::atomic<bool>* cancel = nullptr,
                                   float* peak = nullptr);

        // Bounce a stereo-pair track (L = trackL, R = trackL+1) to ONE
        // interleaved 24-bit stereo WAV -- each side's edited clip
        // arrangement on its own channel, no pan/gain collapse. Used so a
        // paired/imported stereo strip exports as a single stereo stem.
        // Streamed window-by-window like the other bounces.
        bool bounceStereoPairToWav (int trackL, const juce::File& dest,
                                    juce::int64 totalSamples, double sampleRate,
                                    const std::atomic<bool>* cancel = nullptr);

        // ── DAW interchange and song export ───────────────────────
        // Why a session export cannot run now, or empty when it can. Local
        // takes, daemon takes and a finalizing STOP all refuse: their files
        // are not finished. Asked before a job starts and by the job itself.
        juce::String exportRefusal() const;
        // What Detect Songs listens to: one voice per logical strip, as the
        // arrangement plays it.
        std::vector<songs::Voice> songScanVoices();
        // Any strip or VCA soloed. A mix render follows solo, so its file
        // holds only what is soloed, and the engineer must be told.
        bool isAnySoloActive();
        // Plain copies of everything a DAW project carries. Message thread:
        // it reads the clip lists, which only that thread edits.
        interchange::SessionInput captureInterchange();

        // ── Playlists ─────────────────────────────
        // Each track gets a Playlist (vector<Take>). The active Take's
        // clips are what the player renders.
        int   getActiveTakeIdx (int track) const;
        juce::String getTakeName (int track, int takeIdx) const;
        // Serialise / deserialise the full playlist (every track's
        // takes + their clip lists) for .zfproj persistence. Without
        // this, Takes are RAM-only and lost on app quit.
        juce::var playlistsToJson() const;
        void      loadPlaylistsFromJson (const juce::var& v);
        // The media length of every track when these playlists were saved,
        // kept beside them in the .zfproj.
        juce::var mediaLengthsToJson() const;
        // Run after a session's playlists load. Audio past what they were
        // saved against came from a take that ended without a save (a crash
        // in a continued take); it gets a clip, as a normal STOP would give
        // it. Older projects hold no lengths, so for them `afterCrash` says
        // to measure from the end of the clips instead.
        void reconcileClipsWithMedia (const juce::var& savedLengths, bool afterCrash);
        // Undo / redo of a clip edit. Replaces the playlists with the
        // snapshot, keeping any media recorded since it was taken.
        void      restorePlaylistSnapshot (const juce::var& snapshot);
        // Split the named track at the current playhead -- creates two
        // clips that reference the same audio file with adjacent regions.
        bool splitTrackAtPlayhead (int track);
        // Split the named track at an arbitrary TIMELINE sample (no snap;
        // the caller decides). Used for selection-edge separates so the
        // range becomes its own clip. splitTrackAtPlayhead is a thin
        // snap+playhead wrapper around this.
        bool splitTrackAtSample (int track, juce::int64 timelineSample);
        // Heal Separation (Pro Tools Cmd+H): merge the two adjacent clips
        // whose shared boundary is nearest `timelineSample` and within a
        // quarter of a second of it, but ONLY when
        // they're genuinely contiguous in the same file (i.e. they were split
        // from one clip and not moved/trimmed apart). Returns true if a join
        // was healed. The range variant heals every such boundary in [a, b).
        bool healSeparationAt    (int track, juce::int64 timelineSample);
        int  healSeparationRange (int track, juce::int64 start, juce::int64 end);
        // Consolidate [start, end) on one track into a single new flat file
        // (Audio Files/Track_NN_consolidated_K.wav) referenced by one clip,
        // baking in gains / fades / clip order. Returns true on success.
        bool consolidateRange (int track, juce::int64 start, juce::int64 end);
        // Background multi-track consolidation. Rendering and file writes run
        // off the message thread; clip-list changes are committed atomically
        // back on it for the still-active session.
        void consolidateRangeAsync (std::vector<int> tracks,
                                    juce::int64 start, juce::int64 end,
                                    std::function<void (int, bool)> completion);
        // Remove the audio inside [start, end) on one track: split at both
        // edges, then drop every clip that falls fully inside the range
        // (locked clips are spared). Leaves a gap -- non-destructive, the
        // Track_NN.wav file is untouched, so it round-trips + undoes.
        // Returns true if anything was removed.
        bool clearTrackRange (int track, juce::int64 start, juce::int64 end);
        // Like clearTrackRange but RIPPLE: after dropping the clips inside
        // [start, end), every later clip slides left by (end - start) so
        // the gap closes. Returns true if anything changed.
        bool rippleDeleteRange (int track, juce::int64 start, juce::int64 end);
        // Ripple Delete across `tracks`, moving what plays with their audio:
        // every take. Naming every strip moves the session's timeline too
        // (markers). Returns the tracks
        // changed, or -1 when timelineCutRefusal refuses.
        int  rippleDeleteTracks (const std::vector<int>& tracks, juce::int64 start, juce::int64 end);
        // Why a Crop or Ripple Delete cannot run, or empty when it can. A
        // locked clip would have to move or go.
        juce::String timelineCutRefusal (const timelinecut::Cut&, const std::vector<int>& tracks) const;
        // Every strip index a whole-session edit has to touch.
        std::vector<int> allTimelineTracks() const;
        bool namesEveryStrip (const std::vector<int>& tracks) const;
        // Everything a Crop or Ripple Delete changes, for its one undo step.
        struct TimelineState
        {
            juce::var playlists;
            std::vector<Marker> markers;
        };
        TimelineState captureTimelineState() const;
        void restoreTimelineState (const TimelineState&);
        // Insert a clip referencing this track's file region into `track`
        // at `timelineStart` (used by clip paste). Returns the inserted
        // clip index, or -1. Same-track only -- clips index into the
        // track's own Track_NN.wav via the per-track reader.
        // `audioFile` empty = same-track paste (reads the track's own file);
        // set to another track's file for a cross-track paste.
        int  pasteClip (int track, juce::int64 timelineStart,
                        juce::int64 fileStart, juce::int64 fileLength,
                        juce::int64 fadeIn, juce::int64 fadeOut, float gainDb,
                        const juce::String& name, const juce::File& audioFile = {},
                        int sourceChannel = -1, int fadeCurve = 0);
        juce::File getTrackAudioFile (int track, int* channel = nullptr) const;

        // Mutate one clip's bounds and republish to the player. Three
        // edit modes the EDIT row's drag handles drive:
        //
        //   TrimLeft   -- shrinks/grows the left edge. timelineStart and
        //                fileStart move together (slip-trim style); the
        //                clip stays anchored to the same content frame.
        //   TrimRight  -- shrinks/grows the right edge. Only fileLength
        //                changes; the file content under the clip is
        //                unchanged from the left.
        //   Move       -- slides the whole clip along the timeline without
        //                changing what's inside it.
        //
        // deltaSamples is signed: +N to drag right by N samples, -N for
        // left. The engine clamps to [0, fileSize] and prevents a clip
        // from inverting (length never < 1 sample).
        enum class ClipEdit { TrimLeft, TrimRight, Move };
        juce::int64 constrainClipEditDelta (int track, int clipIndex, ClipEdit, juce::int64 deltaSamples) const;
        bool editClip (int track, int clipIndex, ClipEdit, juce::int64 deltaSamples);

        // Non-destructive crop: on EVERY track, replace its clip list with
        // the parts that fall inside the timeline range [start, end),
        // shifted so the range start becomes timeline 0. Every take and the
        // markers move with them. The
        // underlying Track_NN.wav files are never touched. Returns the number
        // of tracks left with at least one clip, or -1 when timelineCutRefusal
        // refuses.
        int cropToRange (juce::int64 startSample, juce::int64 endSample);

        // Set linear fade-in / fade-out lengths on a clip. Either side
        // can be 0 to disable that fade. SessionPlayer applies the
        // envelope sample-by-sample during the clip-aware render path.
        // Returns false when the fades overlap (in+out > length) or
        // the clip index is bad.
        bool setClipFades (int track, int clipIndex,
                           juce::int64 fadeInSamples, juce::int64 fadeOutSamples);
        // Fade shape for a clip: 0 = linear, 1 = equal-power (constant-
        // power crossfade). Persisted in the .zfproj playlists.
        bool setClipFadeCurve (int track, int clipIndex, int curve);
        // Restore a pasted clip's fade ramps (Clip::fadeInRampSamples etc.).
        bool setClipFadeRamps (int track, int clipIndex,
                               juce::int64 inRamp, juce::int64 inRampOffset,
                               juce::int64 outRamp, juce::int64 outRampOffset);

        // The rest of the clip toolkit. Every call republishes the
        // updated clip list to the player so audio honours the change
        // on the next block.
        bool setClipMuted    (int track, int clipIndex, bool muted);
        bool setClipLocked   (int track, int clipIndex, bool locked);
        bool setClipName     (int track, int clipIndex, const juce::String& name);
        bool setClipGainDb   (int track, int clipIndex, float dB);
        // Offline (read-only) nearest-zero-crossing search around a timeline
        // sample on a track, within ±windowSamples. Returns the adjusted
        // timeline sample (or the original if no crossing / no audio). Used to
        // make splits click-free.
        juce::int64 nearestZeroCrossing (int track, juce::int64 timelineSample,
                                         int windowSamples);
        bool deleteClip      (int track, int clipIndex);
        // Duplicate the clip onto the same track, placed after the end
        // of the source so the engineer doesn't have to nudge it.
        bool duplicateClip   (int track, int clipIndex);

        // Recent sessions -- maintained when loadSession / startRecording
        // succeed. Persisted in appProps as 'recentSession_<i>' (i = 0
        // most recent). Capped at kMaxRecent entries.
        static constexpr int kMaxRecent = 10;
        void rememberRecentSession (const juce::File& dir);
        juce::Array<juce::File> getRecentSessions() const;

        // Where new sessions are created. Honours the engineer's "Local
        // Storage" override (appProps "sessionsRoot", set from the New Session
        // / Welcome dialog), falling back to ~/Music/Zynforge Sessions.
        // Used by New Session / Welcome; remote capture is routed through the
        // host's active-session preflight instead of creating a hidden folder.
        juce::File getSessionsRoot() const;

        // Wipe every per-strip persisted override so all strips read
        // their defaults (name = '1', '2', '3' ..., no colour override,
        // 0 dB gain, centre pan, master routing). Used by the welcome
        // dialog's 'Create New Session' so the engineer starts with a
        // truly clean board.
        void resetAllStripState();

        // Forwards to MultitrackRecorder.
        bool setBackupDirectory (const juce::File& dir);   // persists only what the recorder took
        bool setBackupFormat (CaptureFormat format);       // likewise

        // N-way mirror destinations. Wraps recorder.setMirrors and
        // persists via appProps so the engineer's mirror setup
        // survives an app restart (mirror_count + mirror_root_<n> +
        // mirror_format_<n>).
        // Returns false when a take is rolling: the recorder refuses the change
        // and NOTHING is persisted, so the settings file can never claim a
        // redundancy the live recorder isn't providing. Callers must surface it.
        [[nodiscard]] bool setMirrors (const std::vector<MultitrackRecorder::MirrorConfig>& configs);
        std::vector<MultitrackRecorder::MirrorConfig> getMirrors() const
        {
            return recorder.getMirrors();
        }

        // Auto-arm on persistent input. When enabled, any unarmed
        // track that sees its input peak rise above the threshold for
        // a sustained run of timer ticks gets armed automatically.
        // Useful for first-time pre-show channel-discovery: walk the
        // stage shouting "check check" into each mic; the strips
        // arm as you hit them. Lives in the engine so the UI timer
        // is a thin caller. Persisted in appProps as "autoArmOnInput".
        void setAutoArmOnInputDetect (bool on);
        bool isAutoArmOnInputDetect() const noexcept
        {
            return autoArmOnInputFlag.load (std::memory_order_acquire);
        }
        // A scene recall outside a take moves the playhead to the marker of
        // that name (SceneFollow.h). Persisted in appProps as "sceneFollow".
        void setSceneFollowEnabled (bool on);
        bool isSceneFollowEnabled() const noexcept
        {
            return sceneFollowFlag.load (std::memory_order_acquire);
        }
        // Polls each track's peak; arms unarmed tracks whose peak has
        // stayed above ampThreshold for >= periodTicks consecutive
        // calls. Cheap (one atomic load per track + one int update).
        // No-op while recording -- mid-take arming is the audio
        // thread's job, not this polling shim.
        void serviceAutoArm (int periodTicks, float ampThreshold);
        juce::File getBackupDirectory() const           { return recorder.getBackupDirectory(); }
        bool       isBackupActive() const noexcept       { return recorder.isBackupActive(); }
        bool       hasBackupFailed() const noexcept      { return recorder.hasBackupFailed(); }

        // Scans the standard Sessions root and returns any directories that
        // still have a `recording.session` marker (i.e. were not stopped
        // cleanly).
        static juce::Array<juce::File> findIncompleteSessions (const juce::File& sessionsRoot);

        // Drops a marker at the current record or playback position. Pass a
        // name to set it atomically (drop re-sorts by position, so a separate
        // rename-by-index afterwards can hit the wrong marker). Returns the new
        // marker count, or -1 if no session active.
        int dropMarkerAtCurrentPosition (const juce::String& name = {},
                                         const juce::String& type = "user");

        // AudioIODeviceCallback
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                               float* const* outputs, int numOutputs,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override;

    private:
        struct CaptureStopJob
        {
            std::thread worker;
            SessionPlayer::Prepared prepared, retired;
            std::vector<Clip> explicitReaders;
            std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> mixWriter;
            juce::File directory;
            juce::int64 recordEnd = 0;
            double playbackRate = 48000.0;
            int playbackBlock = 512;
            bool ok = true;
            juce::uint64 id = 0;
            std::function<void (bool)> completion;
            bool external = false;       // the daemon owned this take
            bool sameSession = true;     // the clips in memory belong to this folder
        };
        void launchCaptureStopJob (std::function<void (bool)> completion,
                                   const juce::File& externalSession = {}, juce::int64 externalEnd = 0);
        void installFinalizedCapture (juce::uint64 id);
        void completeFinalizedCapture (juce::uint64 id);
        void applyDeviceConfiguration (double, int, juce::AudioWorkgroup);
        struct DeferredDeviceConfiguration { double rate; int block; juce::AudioWorkgroup workgroup; };
        std::mutex deviceLifecycleLock;
        std::optional<DeferredDeviceConfiguration> deferredDeviceConfiguration;
        bool deferredDeviceRelease = false;
        std::atomic<bool> deferredDeviceAudio { false };
        std::atomic<bool> asyncCaptureFinalizing { false };
        std::unique_ptr<CaptureStopJob> captureStopJob;
        juce::uint64 captureStopSequence = 0;
        AsyncHandle asyncHandle;
        juce::AudioDeviceManager deviceManager;
        MultitrackRecorder       recorder;
        SessionPlayer            player;
        MarkersManager           markers;
        juce::File               activeSession;
        juce::File               mixerSession;
        std::atomic<bool>        sessionTransitionActive { false };
        std::atomic<bool>        externalRecording { false };
        std::mutex               externalStatusLock;
        EngineStatus             externalStatus;
        juce::int64              externalStatusAtMs { 0 };
        StripColours             stripColours;
        StripNames               stripNames;
        StripGains               stripGains;
        StripRouting             stripRouting;

        std::atomic<int> streamOutL { -1 };
        std::atomic<int> streamOutR { -1 };

        TimecodeChase    timecodeChase;
        std::atomic<int> ltcSourceStrip { -1 };   // 0-based strip index, -1 = none
        std::atomic<int> testDeviceInputs { 0 };  // see setDeviceInputCountForTests
        std::atomic<int>  chaseMode      { 0 };    // 0=Off 1=LTC 2=MTC
        std::atomic<bool> keepRollingOnSyncLoss { false };
        std::unique_ptr<juce::MidiInput> mtcInput;
        juce::String                     mtcInputName;

        // Stereo mix bus → file recorder.
        std::atomic<bool>                                       recordStereoMixFlag { false };
        std::atomic<bool>                                       stereoMixWriteFailed { false };
        juce::TimeSliceThread                                   mixWriterThread { "ZF Mix Writer" };
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> stereoMixWriter;
        juce::AudioBuffer<float>                                stereoMixScratch;
        // Guards the audio-thread stereo-mix capture (check-and-write) against
        // the message-thread stereoMixWriter.reset() in stopRecording. The
        // writer holds a ScopedLock around reset(); the audio thread uses a
        // ScopedTryLock and skips the mix write for a block it can't acquire.
        juce::CriticalSection                                   stereoMixLock;

        // Double-precision accumulator for the master sum so summing N
        // hot channels can't overshoot float headroom on the way to the
        // stereo master bus.
        juce::AudioBuffer<double>                               monitorAccum;

        // 64-bit-float output accumulator. Per-strip / stream-bus
        // summing flows here in double precision; only the final
        // device-output write down-casts to float. Sized to numOutputs
        // × blockSize in audioDeviceAboutToStart.
        juce::AudioBuffer<double>                               outputAccum;

        // Master bus state. masterState is just a TrackState so the
        // master meter / fader / mute can reuse LedMeter + the strip
        // gain/pan paths without a parallel implementation. Output
        // routing is engineer-configurable and persists in appProps.
        TrackState        masterState;
        TrackState        masterStateR;
        LoudnessMeter     loudness;   // BS.1770 master loudness + true-peak
        std::atomic<juce::uint32> chaseStartLabel { 0 };   // hh mm ss ff, one byte each
        std::atomic<bool> masterHeldOff { false };
        std::atomic<bool> streamHeldOff { false };
        std::atomic<bool> mixMaySharePatchedOutputs { false };
        std::atomic<int>  masterOutL    { 0 };
        std::atomic<int>  masterOutR    { 1 };
        std::atomic<bool> masterStereo  { true };

        // PUNCH mode. The punched tracks are simply the armed tracks at the
        // moment of punch-in, so there is no per-track punch state to go
        // stale. MainComponent's timer reads this and drives startRecording.
        std::atomic<bool> punchEnabled { false };
        bool              punchLoopWasEnabled { false };

        std::atomic<bool> controlsLocked { false };
        std::array<VcaBus, kNumVcas> vcas;
        RemoteControl remotes { *this };   // OSC, desk macros, control surface
        std::unique_ptr<juce::PropertiesFile> appProps;
        RemoteTransportHandler remoteTransportHandler;

        // Live performance telemetry -- written from the audio thread
        // (audioLoadPct) and the writer threads (diskMBPerSec /
        // ringFillPct), polled from the UI to drive the header dashboard.
        // 0-100 % units.
        std::atomic<double> deviceSampleRate { 0.0 };
        std::atomic<double> sessionSampleRate { 0.0 };   // 0 = unset (no record guard)
        std::atomic<juce::int64> captureWindowStart { 0 };
        std::atomic<juce::int64> captureWindowEnd   { 0 };
        std::atomic<bool>        captureWindowEnabled { false };
        std::atomic<bool>   trimFollowEnabled { false };  // virtual-soundcheck gain compensation
        std::atomic<int>    deviceBlockSize  { 0 };
        std::atomic<float>  audioLoadPct     { 0.0f };

        // Free-space pre-flight cache. Set at startRecording, refreshed
        // periodically by the timer / UI poll. Worst-case across primary
        // + backup volumes. 0 = no estimate yet (no session / no armed
        // tracks). Exposed via getEstimatedMinutesRemaining().
        std::atomic<int>    diskMinutesRemaining { 0 };

        // Auto-arm-on-input state. Flag = engineer toggled on/off.
        // The streak vector is sized to recorder.getNumTracks() on
        // first serviceAutoArm call; entries grow when strips are
        // added.
        std::atomic<bool>   autoArmOnInputFlag { false };
        std::atomic<bool>   sceneFollowFlag { false };
        std::vector<int>    autoArmStreaks;
    public:
        // Nothing but the file so far: the take writes to it once it is live.
        struct StereoMixFile
        {
            juce::File file;
            std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        };
        StereoMixFile openStereoMix (const juce::File& sessionDir, double sampleRate);

        int   getEstimatedMinutesRemaining() const
        {
            const auto disk = recorder.readDiskEstimate();
            return disk.watching ? disk.minutes
                                 : diskMinutesRemaining.load (std::memory_order_relaxed);
        }
        // From RECORD until the take's files are final. Nothing on the message
        // thread may touch a destination's path in this window: a stat on a
        // dead mount blocks until the OS gives up, and STOP is on this thread.
        bool  isTakeOnDisk() const noexcept { return isRecording() || isCaptureFinalizing(); }
        // Whether a session folder exists, without asking the disk mid-take.
        bool  hasSessionFolder() const
        {
            const auto dir = getActiveSessionDir();
            return isTakeOnDisk() ? dir != juce::File() : dir.isDirectory();
        }
        // Between takes only, because it stats every destination. The UI calls
        // this every few seconds so the idle figure follows the free space.
        void  refreshDiskMinutesRemaining()
        {
            if (isTakeOnDisk()) return;
            const auto sessionDir = recorder.getActiveSessionDir();
            if (! sessionDir.isDirectory()) { diskMinutesRemaining.store (0); return; }
            diskMinutesRemaining.store (
                recorder.estimateMinutesRemaining (sessionDir,
                                                    recorder.getBackupDirectory()),
                std::memory_order_relaxed);
        }
    private:
        // Pro Tools-style edit cursor. -1 = unset (drop-marker /
        // split / paste use the playhead as the fallback).
        std::atomic<juce::int64> editCursorSample { -1 };
        // Active snap grid. Read by snapSampleToGrid + by the
        // splitTrackAtPlayhead path. Default Off.
        std::atomic<int> snapMode { 0 };

        // Per-track clip lists. Empty/missing entry → 'play the whole
        // Track_NN.wav' (the current behaviour). Once the engineer
        // splits or trims, the entry has one or more Clips covering
        // the audible regions.
        std::vector<std::vector<Clip>> trackClips;
        // Source lengths at the last seed, before a continuation grows files.
        std::vector<juce::int64> sourceTrackLengths;
        // Adds file span [from, to) of a track's own take to its active take,
        // at the same position on the timeline.
        void addRecordedSpan (int track, juce::int64 from, juce::int64 to);
        // Per-track Playlist storing every Take (named clip list). The
        // active take's clips mirror trackClips above so the player
        // path doesn't need to know about takes.
        std::vector<Playlist> trackPlaylists;

        void cutSessionTimeline (const timelinecut::Cut&);
        std::atomic<float>  diskMBPerSec     { 0.0f };
        std::atomic<float>  ringFillPct      { 0.0f };
    public:
        float  getAudioLoadPct() const noexcept { return audioLoadPct.load (std::memory_order_relaxed); }

        // Phase-0 status boundary: one serialisable snapshot of transport +
        // disk + per-track state, so consumers (UI readouts, the capture
        // daemon) read this instead of reaching
        // into recorder/engine internals. Fills from the atomics; call from
        // the message thread.
        EngineStatus captureStatus();
        // Clock source of the input device, for pre-flight. Waits at most
        // timeoutMs for the driver.
        clocksource::Reading readClockSource (int timeoutMs);
        float  getDiskMBPerSec() const noexcept { return recorder.getDiskBytesPerSec() / (1024.0f * 1024.0f); }
        float  getRingFillPct()  const noexcept { return recorder.getRingFillPct(); }
        // Live device format -- the actual buffer (block) size in samples and
        // sample rate negotiated with CoreAudio, captured in
        // audioDeviceAboutToStart. The buffer size is the real glitch-headroom
        // number (smaller = lower latency, less slack); 0 until the device starts.
        int    getDeviceBlockSize()  const noexcept { return deviceBlockSize.load (std::memory_order_relaxed); }
        double getDeviceSampleRate() const noexcept { return deviceSampleRate.load (std::memory_order_relaxed); }

        // The session's intended sample rate (set by the host when a session
        // is created / opened). startRecording refuses to arm if the device
        // clock disagrees with this -- the bypass-proof guard against a silent
        // wrong-speed capture. 0 = unset (no guard).
        void   setSessionSampleRate (double sr) noexcept { sessionSampleRate.store (sr, std::memory_order_relaxed); }
        double getSessionSampleRate () const noexcept    { return sessionSampleRate.load (std::memory_order_relaxed); }
        // The rate the session's audio is at, which a device held at another
        // rate does not change. Anything written into a session uses this: the
        // player has no converter and takes the last loaded file's rate as the
        // whole session's.
        double getSessionAudioRate() const noexcept
        {
            if (player.isLoaded() && player.getSampleRate() > 0.0) return player.getSampleRate();
            if (const double session = getSessionSampleRate(); session > 0.0) return session;
            return getDeviceSampleRate();
        }
        // True when a device is open and its rate diverges from the session rate.
        bool   isSampleRateMismatched() const noexcept;

        // ── Trim-follow (virtual-soundcheck gain compensation) ──────────
        // The console pushes per-channel preamp/trim gain over OSC; with
        // trim-follow ON, playback adds (live - capture) dB so soundcheck
        // input-gain moves track the recorded audio like a live mic would.
        void setTrimFollowEnabled (bool on) noexcept { trimFollowEnabled.store (on, std::memory_order_relaxed); }
        bool isTrimFollowEnabled () const noexcept   { return trimFollowEnabled.load (std::memory_order_relaxed); }
        // Console preamp/trim gain for a 0-based channel (called from OSC,
        // message thread). Updates the live value only; the capture reference
        // is fixed when a take starts.
        void setTrackLiveInputGain (int channelIndex, float dB);
        // The offset playback gets: live - capture, limited, 0 with no reference.
        float getTrackTrimDelta (int channelIndex);
        // Take the desk's current reading as each armed strip's capture
        // reference. Called when a take really starts, local or daemon.
        void snapshotCaptureInputGains();
        // Empty, or a line naming the strips Trim-Follow is not following:
        // the desk reports them but no reference was captured, or the move
        // is past the limit. For the status line.
        juce::String getTrimFollowNotice();
        int  getTrimFollowUnreferencedCount();
    private:

        void applyPersistedStripState();

        // appProps shares ONE on-disk .settings file with the four Strip*
        // modules (each its own juce::PropertiesFile). A whole-file save
        // rewrites every in-memory key, so a save here would clobber keys a
        // Strip* module wrote since appProps last loaded (and vice-versa).
        // Call this before mutating+saving appProps keys so it first picks up
        // the other writers' changes. No-op when appProps is null (tests).
        void reloadAppPropsBeforeWrite() noexcept
        {
            // REPLACE (not merge): reload() alone merges the on-disk file into
            // the in-memory set without dropping keys another writer deleted,
            // so appProps would resurrect a Strip* key it had merged when it
            // rewrites the shared file. clear()+reload() picks up others'
            // changes AND honours their deletions. Guarded on the file existing
            // so a first-run (no file yet) doesn't blank current in-memory state.
            if (appProps != nullptr) reloadSettingsReplacing (*appProps);
        }

        // Wipe every persisted per-strip override (name, colour, gain, pan,
        // routing, stereo, edit group, VCA, sends, bus flag, UUID) for the
        // half-open index range [firstIndex, lastIndexExclusive). Used to
        // give freshly-added strips a clean slate so they don't inherit a
        // previous session's layout by array position.
        void clearStripOverridesRange (int firstIndex, int lastIndexExclusive);

        // Audio-thread scratch for routed VSC playback: track i fills
        // channel i, then engine copies into the real device output that
        // strip i is routed to.
        juce::AudioBuffer<float> playerScratch { 32, 4096 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
    };
}
