#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Markers.h"
#include "ClickEngine.h"
#include "MidiClockOut.h"
#include "VcaBus.h"
#include "../Network/NDIBridge.h"

#include <array>
#include "ClipModel.h"
#include "MultitrackRecorder.h"
#include "SessionPlayer.h"
#include "StripColours.h"
#include "StripGains.h"
#include "StripNames.h"
#include "StripRouting.h"
#include "TimecodeChase.h"

#include <memory>

namespace zynforge
{
    class OscRemote;
    class CompanionServer;

    class AudioEngine final : public juce::AudioIODeviceCallback
    {
    public:
        AudioEngine();
        ~AudioEngine() override;

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

        juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
        MultitrackRecorder&       getRecorder()      noexcept { return recorder; }
        SessionPlayer&            getPlayer()        noexcept { return player; }
        juce::PropertiesFile*     getAppProps()      noexcept       { return appProps.get(); }
        juce::PropertiesFile*     getAppProps() const noexcept       { return appProps.get(); }
        TimecodeChase&            getTimecodeChase() noexcept { return timecodeChase; }

        // -1 = no LTC source. Otherwise the strip whose device input
        // feeds the LTC zero-crossing analyzer once per audio block.
        void setLtcSourceStrip (int oneBasedIndex) noexcept;
        int  getLtcSourceStrip() const noexcept { return ltcSourceStrip.load() + 1; }

        // Companion HTTP server (read-only iPad / remote-audition).
        bool startCompanionServer (int port);
        bool startCompanionServerOnLan (int port);
        juce::String getCompanionAccessUrl() const;
        bool         isCompanionExposedOnLan() const;
        void stopCompanionServer();
        bool isCompanionServerRunning() const noexcept;
        int  getCompanionServerPort() const noexcept;

        // ── Master bus ────────────────────────────────────────────────
        // The master sums every audible channel's VSC playback plus every
        // channel whose MON flag is on (live input monitoring). Mute and
        // solo gates apply: any solo engaged → only soloed channels reach
        // the master.
        TrackState& getMasterState()  noexcept { return masterState; }
        TrackState& getMasterStateR() noexcept { return masterStateR; }
        float getMasterGainDb() const noexcept { return masterState.gainDb.load(); }
        bool  getMasterMuted()  const noexcept { return masterState.muted.load(); }
        void  setMasterGainDb (float dB);
        void  setMasterMuted  (bool m);
        int   getMasterOutputL() const noexcept { return masterOutL.load(); }
        int   getMasterOutputR() const noexcept { return masterOutR.load(); }
        void  setMasterOutputs (int l, int r);

        // Mono / stereo mode. In mono, L+R are summed and sent only to
        // masterOutL; the meter collapses to a single bar. In stereo, L
        // goes to masterOutL and R to masterOutR; the meter shows two
        // bars driven by masterState (L) and masterStateR (R).
        bool  getMasterStereo() const noexcept { return masterStereo.load(); }
        void  setMasterStereo (bool stereo);

        bool startRecording (const juce::File& sessionDir);
        void stopRecording();
        bool isRecording() const noexcept                  { return recorder.isRecording(); }

        // Stereo mix bus → file. When enabled, every startRecording also
        // opens a stereo StereoMix.wav writer in the session dir which
        // captures the engine's stream-bus summing block-by-block from
        // the audio thread. Engineer uses this for archive-quality
        // streaming bounces or to hand a stereo mix to a streaming
        // platform without re-mixing.
        void setRecordStereoMix (bool enabled);
        bool getRecordStereoMix() const noexcept           { return recordStereoMixFlag.load(); }

        int  loadSession (const juce::File& sessionDir);
        void startPlayback()
        {
            const bool wasPlaying = player.isPlaying();
            player.start();
            if (! wasPlaying) midiClockOut.sendStart();
            else              midiClockOut.sendContinue();
        }
        void stopPlayback()
        {
            player.stop();
            midiClockOut.sendStop();
        }

        // MIDI clock master (24 PPQN) -- drives outboard synths /
        // drum machines / DAW slaves from the session tempo.
        MidiClockOut& getMidiClockOut() noexcept { return midiClockOut; }
        NDIBridge&    getNDIBridge()    noexcept { return ndi; }

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
        // Off = no snap, Markers = snap to nearest marker. Bars used
        // to exist for tempo-map-grid snapping; removed in 2026-05-25
        // along with the Bars|Beats ruler -- live recorders don't
        // navigate by bars.
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
        StripColours&   getStripColours() noexcept         { return stripColours; }

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

        // Routing. -1 = unrouted; values clamped to current device's range.
        void  setTrackInputRouting  (int channelIndex, int deviceCh);
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
        int  getStripCount() const;
        void setStripCount (int n);

        // Add / remove a single strip without disturbing the others.
        // The audio callback is briefly detached while the recorder
        // mutates its track vector.
        void addOneStrip();
        void removeStripAt (int index);

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

        // Punch in/out -- the player auto-arms enabled punch tracks when
        // the playhead enters the loop region and disarms again when it
        // leaves. The actual record state is driven on the message
        // thread by MainComponent's timer polling getPunch* and
        // toggling startRecording / stopRecording / track.armed.
        bool isPunchModeOn()      const noexcept { return punchEnabled.load (std::memory_order_relaxed); }
        void setPunchModeOn (bool en) noexcept   { punchEnabled.store (en, std::memory_order_relaxed); }
        bool isTrackPunchArmed (int channel) const noexcept;
        void setTrackPunchArmed (int channel, bool armed);
        // The loop region from SessionPlayer doubles as the punch window
        // -- there's only ever one 'do this between A and B' selection.

        // OSC remote: starts/stops a juce::OSCReceiver bound to UDP port,
        // with a dialect parser for DiGiCo / A&H / SSL / Yamaha consoles
        // plus a generic /zynforge/* schema for tablet apps.
        bool  startOsc (int udpPort, int dialectIndex);
        void  stopOsc();
        bool  isOscListening() const;
        int   getOscPort() const;
        int   getOscDialect() const;

        // Returns recording dir if recording, else loaded playback session,
        // else an empty File.
        juce::File getActiveSessionDir() const;

        // Pin the active session folder explicitly (used by New Session...
        // and Open Session...). Without this, the engine reports an active
        // session only while recording or while a player has loaded one
        // -- which left Save / Save As greyed out right after creating an
        // empty session. Cleared by passing an empty / non-directory file.
        void setActiveSessionDir (const juce::File& dir);

        // Edit cursor (Pro Tools-style insertion point). Distinct from
        // the playhead: a single click in the EDIT wave pane sets it,
        // a marker drop / split / paste uses it instead of the
        // transport position. -1 means 'unset; fall back to playhead'.
        void          setEditCursorSample (juce::int64 sample) noexcept;
        juce::int64   getEditCursorSample() const noexcept;
        void          clearEditCursor() noexcept { setEditCursorSample (-1); }

        // Tab-to-transient (Pro Tools-style). Returns the nearest
        // detected onset sample position after / before `fromSample`,
        // or -1 if none. First call lazily scans every Track_NN.wav
        // in the active session and caches the result; subsequent
        // calls reuse the cache. invalidateTransientCache() forces
        // a rescan on the next query (call after a fresh record).
        //
        // restrictToTrack >= 0 limits the search to onsets detected
        // on that single Track_NN.wav (1-based file index, matching
        // the recorder's numbering). Pass -1 to search the pooled
        // list across every track (legacy behaviour).
        juce::int64 nextTransientSample (juce::int64 fromSample, int restrictToTrack = -1);
        juce::int64 prevTransientSample (juce::int64 fromSample, int restrictToTrack = -1);
        void invalidateTransientCache();

        // Returns the transient list for a single track index
        // (1-based, matching Track_NN.wav). Empty if not detected
        // yet OR if the track has no audio file.
        const std::vector<juce::int64>& getTransientsForTrack (int trackIdx);

        // Session tempo. setSessionTempoBpm() is the canonical setter --
        // it updates currentTempoBpm + persists to appProps. The tempo
        // map is a sorted list of (samplePos, bpm) change points; the
        // engine doesn't consume it on the audio thread yet (no MIDI
        // click out), but TempoBar / EDIT view / cue snapshot all read
        // and write it through these getters.
        struct TempoChange { juce::int64 samplePos; float bpm; };
        float  getSessionTempoBpm() const noexcept { return currentTempoBpm.load (std::memory_order_relaxed); }
        void   setSessionTempoBpm (float bpm);

        // Time signature -- clicks accent beat 1 of each bar; the click
        // engine consumes (numerator, denominator). Persisted to appProps.
        int   getTimeSignatureNumerator()   const noexcept { return timeSigNumerator  .load (std::memory_order_relaxed); }
        int   getTimeSignatureDenominator() const noexcept { return timeSigDenominator.load (std::memory_order_relaxed); }
        void  setTimeSignature (int numerator, int denominator);

        const std::vector<TempoChange>& getTempoMap() const noexcept { return tempoMap; }
        void   setTempoMap (std::vector<TempoChange> newMap);
        void   addTempoChange (juce::int64 samplePos, float bpm);
        void   removeTempoChangeNear (juce::int64 samplePos, juce::int64 tolerance);
        void   clearTempoMap();

        // Per-track automation. Three discrete parameter lanes:
        //   Volume (-60..+12 dB)
        //   Pan    (-1..+1)
        //   Mute   (0/1, but stored as float so the lane editor can
        //          fade between values if we ever extend the renderer)
        enum class AutomationParam : int { Volume = 0, Pan = 1, Mute = 2 };

        // Pro Tools-style automation write mode. Off = playback only
        // reads existing points; Touch / Latch / Write progressively
        // expose more aggressive write behaviour. Today the engine
        // honours Off vs anything-else (writes a point whenever the
        // engineer moves the fader during playback); future commits
        // distinguish the three write flavours.
        enum class AutomationWriteMode : int { Off = 0, Touch = 1, Latch = 2, Write = 3 };
        void setAutomationWriteMode (AutomationWriteMode m) noexcept
        {
            automationWriteMode.store ((int) m, std::memory_order_release);
        }
        AutomationWriteMode getAutomationWriteMode() const noexcept
        {
            return (AutomationWriteMode) automationWriteMode.load (std::memory_order_acquire);
        }
        bool isAutomationWriting() const noexcept
        {
            return getAutomationWriteMode() != AutomationWriteMode::Off
                && player.isPlaying();
        }

        // TRIM mode (mutually exclusive with WRITE on the toolbar).
        // When on + playing, fader / pan moves nudge the per-track
        // trim atomic instead of dropping new automation points.
        void setAutomationTrimMode (bool on) noexcept
        {
            automationTrimMode.store (on, std::memory_order_release);
        }
        bool isAutomationTrimming() const noexcept
        {
            return automationTrimMode.load (std::memory_order_acquire)
                && player.isPlaying();
        }

        // Global read suspend. When on, automationValueAt returns the
        // caller's fallback value (the live fader) regardless of lane
        // contents. Lets the engineer hear raw faders without
        // disturbing the stored automation.
        void setAutomationReadSuspended (bool on) noexcept
        {
            automationReadSuspended.store (on, std::memory_order_release);
        }
        bool isAutomationReadSuspended() const noexcept
        {
            return automationReadSuspended.load (std::memory_order_acquire);
        }

        // Punch range. When on AND a valid [in, out) range is set,
        // automation writes only fire while the playhead is inside
        // the region. Outside the region, write calls are no-ops.
        // Set both endpoints to -1 to clear.
        void setAutomationPunchEnabled (bool on) noexcept
        {
            automationPunchEnabled.store (on, std::memory_order_release);
        }
        bool isAutomationPunchEnabled() const noexcept
        {
            return automationPunchEnabled.load (std::memory_order_acquire);
        }
        void setAutomationPunchRange (juce::int64 inSample, juce::int64 outSample) noexcept
        {
            automationPunchIn .store (inSample,  std::memory_order_release);
            automationPunchOut.store (outSample, std::memory_order_release);
        }
        juce::int64 getAutomationPunchIn()  const noexcept { return automationPunchIn .load (std::memory_order_acquire); }
        juce::int64 getAutomationPunchOut() const noexcept { return automationPunchOut.load (std::memory_order_acquire); }
        bool isInsideAutomationPunchRange (juce::int64 samplePos) const noexcept
        {
            const auto in  = getAutomationPunchIn();
            const auto out = getAutomationPunchOut();
            if (in < 0 || out <= in) return true;  // no valid range = always in
            return samplePos >= in && samplePos < out;
        }

        // Per-track Automation Safe lock. Engine refuses Add /
        // Remove / Paste / WRITE / TRIM writes to a safe track.
        void setTrackAutomationSafe (int track, bool safe);
        bool isTrackAutomationSafe  (int track) const;

        // WRITE-thinned point drop. Skips the call when the previous
        // write to the same (track, param) was less than 'minSamples'
        // ago. Used by the WRITE-mode fader callback so a 60 Hz
        // fader.onValueChange stream doesn't fan out into 60
        // points/sec of automation cruft.
        void writeAutomationPointThinned (int track, AutomationParam,
                                          juce::int64 samplePos, float value,
                                          juce::int64 minSamplesBetween);
        // Curve type controls how the value evolves from this point
        // to the NEXT point. Hold = step (old behaviour); Linear =
        // straight ramp; SCurve = smoothstep; ExpUp/ExpDown = legacy
        // presets, now expressed as a non-zero `tension` on a Linear
        // curve and re-mapped on load (kept in the enum so old .zfproj
        // files round-trip without losing shape). Mute always ignores
        // both fields and behaves as Hold so it never fades through
        // 0.5.
        //
        // `tension` is a continuous knob bent by the EDIT lane's
        // drag handle. 0 = linear, +1 = strongly ease-out (slow
        // start, fast end), -1 = strongly ease-in (fast start, slow
        // end). Only honoured when curve == Linear.
        enum class AutomationCurve : int { Hold = 0, Linear = 1, SCurve = 2, ExpUp = 3, ExpDown = 4 };
        struct AutomationPoint
        {
            juce::int64     samplePos { 0 };
            float           value     { 0.0f };
            AutomationCurve curve     { AutomationCurve::Linear };
            float           tension   { 0.0f };
        };

        // Returns the points for (track, parameter). Empty when none.
        const std::vector<AutomationPoint>& getAutomation (int track, AutomationParam) const;

        // Drop a new point. Replaces an existing one if it lands within
        // a tolerance window (so dragging never produces fan-out clouds).
        // For Mute, the value is snapped to 0 or 1 with curve=Hold.
        void addAutomationPoint (int track, AutomationParam, juce::int64 samplePos, float value);

        // Remove the nearest point inside the given sample tolerance.
        void removeAutomationPointNear (int track, AutomationParam,
                                        juce::int64 samplePos, juce::int64 tolerance);

        // Per-point curve setter. Affects the segment FROM this point.
        void setAutomationCurveAt (int track, AutomationParam,
                                   juce::int64 samplePos, juce::int64 tolerance,
                                   AutomationCurve);
        // Per-segment tension setter. Bends the Linear segment that
        // starts at the matched point into an ease-in (-1..0) or
        // ease-out (0..+1) curve. No-op when the point's curve is
        // Hold or SCurve. Clamps tension to [-1, +1].
        void setAutomationTensionAt (int track, AutomationParam,
                                     juce::int64 samplePos, juce::int64 tolerance,
                                     float tension);

        // Snapshot every track's automation lanes into a JSON-compatible
        // var so the engine can round-trip through .zfproj.
        juce::var      automationToJson() const;
        void           loadAutomationFromJson (const juce::var&);

        // ── Range operations on one lane ────────────────────────
        // copyAutomationRange returns every point whose samplePos
        // lies inside [startSample, endSample]. Points are returned
        // with samplePos rebased to (samplePos - startSample) so the
        // caller / paste path can position them at an arbitrary
        // anchor without preserving session-absolute times.
        std::vector<AutomationPoint> copyAutomationRange (int track, AutomationParam,
                                                          juce::int64 startSample,
                                                          juce::int64 endSample) const;
        // Wipe every point in [startSample, endSample] for (track, p).
        void clearAutomationRange (int track, AutomationParam,
                                   juce::int64 startSample, juce::int64 endSample);
        // Drop every point in `points` at anchor + point.samplePos.
        // Points should be in 'relative' form (samplePos = offset
        // from the original copy range start) -- the format returned
        // by copyAutomationRange.
        void pasteAutomationRange (int track, AutomationParam,
                                   juce::int64 anchorSample,
                                   const std::vector<AutomationPoint>& points);

        // ── Trim mode -- per (track, param) offset added to the
        // automation value at playback. Defaults to 0 (no trim).
        // Volume trim is in dB. Pan trim is added then clamped to
        // [-1, +1]. Mute trim is ignored.
        void  setAutomationTrim (int track, AutomationParam, float trim);
        float getAutomationTrim (int track, AutomationParam) const;
        void  clearAllAutomationTrims();

        void clearAutomation (AutomationParam);
        void clearAutomationForTrack (int track, AutomationParam);

        // RT-safe lookup of the automated parameter value at the given
        // sample position. Holds automationLock with tryEnter on the
        // audio thread; if contended (UI thread mid-edit), returns the
        // static fallback so the mix doesn't glitch.
        float automationValueAt (int track, AutomationParam, juce::int64 samplePos,
                                 float fallback) const noexcept;

        // Per-track clip list. Lazy: a track stays in 'whole file' mode
        // (no entry in trackClips, or one full-range entry) until the
        // engineer splits or trims it. The EDIT view + future playback
        // path read from here when present; falls back to the underlying
        // Track_NN.wav otherwise.
        std::vector<Clip>& clipsFor (int track);
        const std::vector<Clip>* tryClipsFor (int track) const;
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
        void seedDefaultClips();

        // ── Comp playlists (Take swap) ─────────────────────────────
        // Each track gets a Playlist (vector<Take>). The active Take's
        // clips are what the player renders. Engineer captures the
        // current clip list as a named take, switches between takes for
        // comping, deletes takes they don't want.
        int   getTakeCount     (int track) const;
        int   getActiveTakeIdx (int track) const;
        juce::String getTakeName (int track, int takeIdx) const;
        void  setActiveTake   (int track, int takeIdx);
        int   newTakeFromCurrent (int track, const juce::String& name);
        void  deleteTake      (int track, int takeIdx);
        void  renameTake      (int track, int takeIdx, const juce::String& name);
        // Serialise / deserialise the full playlist (every track's
        // takes + their clip lists) for .zfproj persistence. Without
        // this, Takes are RAM-only and lost on app quit.
        juce::var playlistsToJson() const;
        void      loadPlaylistsFromJson (const juce::var& v);
        // Split the named track at the current playhead -- creates two
        // clips that reference the same audio file with adjacent regions.
        bool splitTrackAtPlayhead (int track);

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
        bool editClip (int track, int clipIndex, ClipEdit, juce::int64 deltaSamples);

        // Non-destructive crop: on EVERY track, replace its clip list with
        // the parts that fall inside the timeline range [start, end),
        // shifted so the range start becomes timeline 0. The underlying
        // Track_NN.wav files are never touched -- only the clip references
        // change -- so it round-trips through the .zfproj playlists and is
        // fully reversible (re-crop to the full range, or reload). Returns
        // the number of tracks left with at least one clip.
        int cropToRange (juce::int64 startSample, juce::int64 endSample);

        // Set linear fade-in / fade-out lengths on a clip. Either side
        // can be 0 to disable that fade. SessionPlayer applies the
        // envelope sample-by-sample during the clip-aware render path.
        // Returns false when the fades overlap (in+out > length) or
        // the clip index is bad.
        bool setClipFades (int track, int clipIndex,
                           juce::int64 fadeInSamples, juce::int64 fadeOutSamples);

        // The rest of the clip toolkit. Every call republishes the
        // updated clip list to the player so audio honours the change
        // on the next block.
        bool setClipMuted    (int track, int clipIndex, bool muted);
        bool setClipLocked   (int track, int clipIndex, bool locked);
        bool setClipGainDb   (int track, int clipIndex, float dB);
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
        void clearRecentSessions();

        // Wipe every per-strip persisted override so all strips read
        // their defaults (name = '1', '2', '3' ..., no colour override,
        // 0 dB gain, centre pan, master routing). Used by the welcome
        // dialog's 'Create New Session' so the engineer starts with a
        // truly clean board.
        void resetAllStripState();

        // Forwards to MultitrackRecorder.
        void setBackupDirectory (const juce::File& dir) { recorder.setBackupDirectory (dir); }

        // N-way mirror destinations. Wraps recorder.setMirrors and
        // persists via appProps so the engineer's mirror setup
        // survives an app restart (mirror_count + mirror_root_<n> +
        // mirror_format_<n>).
        void setMirrors (const std::vector<MultitrackRecorder::MirrorConfig>& configs);
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

        // Phase correlation between two input channels (live, smoothed).
        // Channels are 1-based for display, stored 0-based.
        void setPhasePair (int leftCh1Based, int rightCh1Based) noexcept;
        int  getPhaseLeftChannel()  const noexcept { return phaseLeft .load (std::memory_order_relaxed) + 1; }
        int  getPhaseRightChannel() const noexcept { return phaseRight.load (std::memory_order_relaxed) + 1; }
        float getPhaseCorrelation() const noexcept { return phaseCorrelation.load (std::memory_order_relaxed); }

        // Drops a marker at the current record or playback position.
        // Returns the new marker count, or -1 if no session active.
        int dropMarkerAtCurrentPosition();

        // AudioIODeviceCallback
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                               float* const* outputs, int numOutputs,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override;

    private:
        juce::AudioDeviceManager deviceManager;
        MultitrackRecorder       recorder;
        SessionPlayer            player;
        MarkersManager           markers;
        juce::File               activeSession;
        StripColours             stripColours;
        StripNames               stripNames;
        StripGains               stripGains;
        StripRouting             stripRouting;

        std::atomic<int>   phaseLeft         { 0 };  // 0-based
        std::atomic<int>   phaseRight        { 1 };
        std::atomic<float> phaseCorrelation  { 0.0f };

        std::atomic<int> streamOutL { -1 };
        std::atomic<int> streamOutR { -1 };

        TimecodeChase    timecodeChase;
        std::atomic<int> ltcSourceStrip { -1 };   // 0-based strip index, -1 = none

        // Stereo mix bus → file recorder.
        std::atomic<bool>                                       recordStereoMixFlag { false };
        juce::TimeSliceThread                                   mixWriterThread { "ZF Mix Writer" };
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> stereoMixWriter;
        juce::AudioBuffer<float>                                stereoMixScratch;

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
        std::atomic<int>  masterOutL    { 0 };
        std::atomic<int>  masterOutR    { 1 };
        std::atomic<bool> masterStereo  { true };

        // Punch mode flags. punchEnabled gates the whole feature;
        // per-track armed bits are kept as a separate vector keyed by
        // physical track index. UI mutates from the message thread,
        // MainComponent's timer reads and drives startRecording.
        std::atomic<bool>           punchEnabled { false };
        std::vector<std::atomic<bool>> punchArmed;

        ClickEngine        click;
        MidiClockOut       midiClockOut;
        NDIBridge          ndi;
        std::array<VcaBus, kNumVcas> vcas;
    public:
        ClickEngine& getClickEngine() noexcept { return click; }
    private:
        std::unique_ptr<OscRemote> osc;
        std::unique_ptr<CompanionServer> companion;
        std::unique_ptr<juce::PropertiesFile> appProps;

        // Live performance telemetry -- written from the audio thread
        // (audioLoadPct) and the writer threads (diskMBPerSec /
        // ringFillPct), polled from the UI to drive the header dashboard.
        // 0-100 % units.
        std::atomic<double> deviceSampleRate { 0.0 };
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
        std::vector<int>    autoArmStreaks;
    public:
        int   getEstimatedMinutesRemaining() const noexcept
        {
            return diskMinutesRemaining.load (std::memory_order_relaxed);
        }
        // Force a fresh free-space probe. Cheap (one syscall per
        // volume); UI calls this every few seconds while recording so
        // the displayed remaining time tracks the live drain rate.
        void  refreshDiskMinutesRemaining()
        {
            const auto sessionDir = recorder.getActiveSessionDir();
            if (! sessionDir.isDirectory()) { diskMinutesRemaining.store (0); return; }
            diskMinutesRemaining.store (
                recorder.estimateMinutesRemaining (sessionDir,
                                                    recorder.getBackupDirectory()),
                std::memory_order_relaxed);
        }
    private:
        std::atomic<float>  currentTempoBpm     { 120.0f };
        std::atomic<int>    timeSigNumerator    { 4 };
        std::atomic<int>    timeSigDenominator  { 4 };
        // Pro Tools-style edit cursor. -1 = unset (drop-marker /
        // split / paste use the playhead as the fallback).
        std::atomic<juce::int64> editCursorSample { -1 };
        std::vector<TempoChange> tempoMap;   // sorted by samplePos; UI/message-thread only
        // Active snap grid. Read by snapSampleToGrid + by the
        // splitTrackAtPlayhead path. Default Off.
        std::atomic<int> snapMode { 0 };

        // Pooled list of detected onsets across every Track_NN.wav in
        // the active session (sorted). Built lazily on first Tab-to-
        // transient query. Invalidated on session load + new record.
        std::vector<juce::int64> transientCache;
        // Per-track onsets keyed by 1-based track index (matching
        // Track_NN.wav). Populated alongside `transientCache` so
        // track-restricted Tab can search the relevant lane only.
        std::vector<std::vector<juce::int64>> transientPerTrack;
        bool                     transientCacheValid { false };

        // Per-track, per-parameter automation. UI-thread only for now
        // (audio thread doesn't yet consume these -- the engineer reads
        // them visually and they ride along in cue snapshots).
        struct TrackAutomation
        {
            std::vector<AutomationPoint> volume, pan, mute;
            // Trim offsets (Pro Tools-style). Added on top of the
            // automation value at read time. Volume in dB, pan in
            // linear -1..+1, mute ignored. Atomics so the audio
            // thread can read without locking.
            std::atomic<float> volumeTrim { 0.0f };
            std::atomic<float> panTrim    { 0.0f };

            // Per-track Automation Safe lock. Engine refuses writes
            // to this track when true. Read path is unaffected.
            std::atomic<bool>  safe { false };

            // Last write sample per param -- used by writeAutomation-
            // PointThinned to throttle WRITE-mode point drops.
            // Index: 0 volume / 1 pan / 2 mute. -1 = no prior write.
            std::atomic<juce::int64> lastWriteSample[3] { {-1}, {-1}, {-1} };

            TrackAutomation() = default;
            TrackAutomation (TrackAutomation&& o) noexcept
                : volume (std::move (o.volume)),
                  pan    (std::move (o.pan)),
                  mute   (std::move (o.mute)),
                  volumeTrim (o.volumeTrim.load()),
                  panTrim    (o.panTrim.load()),
                  safe (o.safe.load())
            {
                for (int i = 0; i < 3; ++i) lastWriteSample[i].store (o.lastWriteSample[i].load());
            }
            TrackAutomation& operator= (TrackAutomation&& o) noexcept
            {
                volume = std::move (o.volume);
                pan    = std::move (o.pan);
                mute   = std::move (o.mute);
                volumeTrim.store (o.volumeTrim.load());
                panTrim   .store (o.panTrim   .load());
                safe      .store (o.safe      .load());
                for (int i = 0; i < 3; ++i) lastWriteSample[i].store (o.lastWriteSample[i].load());
                return *this;
            }
            TrackAutomation (const TrackAutomation&) = delete;
            TrackAutomation& operator= (const TrackAutomation&) = delete;
        };
        std::vector<TrackAutomation> automationData;
        mutable juce::CriticalSection automationLock;
        std::atomic<int>              automationWriteMode { 0 };   // AutomationWriteMode::Off
        std::atomic<bool>             automationTrimMode  { false };
        // SUSPEND: temporarily ignore every lane's stored points at
        // read time so the engineer can audition raw fader / pan /
        // mute values. PUNCH: when on, WRITE is gated to a single
        // [punchInSample, punchOutSample) range; outside that range
        // the engine reads existing automation but does not record.
        std::atomic<bool>             automationReadSuspended { false };
        std::atomic<bool>             automationPunchEnabled  { false };
        std::atomic<juce::int64>      automationPunchIn       { -1 };
        std::atomic<juce::int64>      automationPunchOut      { -1 };
        // Per-track clip lists. Empty/missing entry → 'play the whole
        // Track_NN.wav' (the current behaviour). Once the engineer
        // splits or trims, the entry has one or more Clips covering
        // the audible regions.
        std::vector<std::vector<Clip>> trackClips;
        // Per-track Playlist storing every Take (named clip list). The
        // active take's clips mirror trackClips above so the player
        // path doesn't need to know about takes. setActiveTake swaps
        // the active take's contents into trackClips + republishes
        // to the player.
        std::vector<Playlist> trackPlaylists;
        std::vector<AutomationPoint> emptyAutomation;   // returned by ref when none

        std::vector<AutomationPoint>* findLane (int track, AutomationParam);
        const std::vector<AutomationPoint>* findLane (int track, AutomationParam) const;
        std::atomic<float>  diskMBPerSec     { 0.0f };
        std::atomic<float>  ringFillPct      { 0.0f };
    public:
        float  getAudioLoadPct() const noexcept { return audioLoadPct.load (std::memory_order_relaxed); }
        float  getDiskMBPerSec() const noexcept { return recorder.getDiskBytesPerSec() / (1024.0f * 1024.0f); }
        float  getRingFillPct()  const noexcept { return recorder.getRingFillPct(); }
    private:

        void applyPersistedStripState();

        // Audio-thread scratch for routed VSC playback: track i fills
        // channel i, then engine copies into the real device output that
        // strip i is routed to.
        juce::AudioBuffer<float> playerScratch { 32, 4096 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
    };
}
