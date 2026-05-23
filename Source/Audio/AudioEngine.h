#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "Markers.h"
#include "ClickEngine.h"
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
        void startPlayback()                               { player.start(); }
        void stopPlayback()                                { player.stop(); }
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

        // Routing. -1 = unrouted; values clamped to current device's range.
        void  setTrackInputRouting  (int channelIndex, int deviceCh);
        void  setTrackOutputRouting (int channelIndex, int deviceCh);

        // Sets both input AND output for the strip to the same hardware
        // channel index — the standard VSC workflow where strip N is
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

        // Persisted mono/stereo state for a strip. The L track holds
        // the flag; R partner is implicit at trackIndex + 1.
        void setTrackStereo (int channelIndex, bool isStereoPair);

        // Swap two physical tracks completely: state fields (name,
        // colour, gain, pan, routing, mute/solo/mon/arm, stereo flag),
        // persisted overrides, and the underlying Track_NN.wav files in
        // the active session's Audio Files/ folder. UI keeps holding the
        // same TrackState references — only their contents swap — so
        // strips don't dangle. Returns true on success.
        bool swapTracks (int a, int b);

        // Punch in/out — the player auto-arms enabled punch tracks when
        // the playhead enters the loop region and disarms again when it
        // leaves. The actual record state is driven on the message
        // thread by MainComponent's timer polling getPunch* and
        // toggling startRecording / stopRecording / track.armed.
        bool isPunchModeOn()      const noexcept { return punchEnabled.load (std::memory_order_relaxed); }
        void setPunchModeOn (bool en) noexcept   { punchEnabled.store (en, std::memory_order_relaxed); }
        bool isTrackPunchArmed (int channel) const noexcept;
        void setTrackPunchArmed (int channel, bool armed);
        // The loop region from SessionPlayer doubles as the punch window
        // — there's only ever one 'do this between A and B' selection.

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

        // Pin the active session folder explicitly (used by New Session…
        // and Open Session…). Without this, the engine reports an active
        // session only while recording or while a player has loaded one
        // — which left Save / Save As greyed out right after creating an
        // empty session. Cleared by passing an empty / non-directory file.
        void setActiveSessionDir (const juce::File& dir);

        // Session tempo. setSessionTempoBpm() is the canonical setter —
        // it updates currentTempoBpm + persists to appProps. The tempo
        // map is a sorted list of (samplePos, bpm) change points; the
        // engine doesn't consume it on the audio thread yet (no MIDI
        // click out), but TempoBar / EDIT view / cue snapshot all read
        // and write it through these getters.
        struct TempoChange { juce::int64 samplePos; float bpm; };
        float  getSessionTempoBpm() const noexcept { return currentTempoBpm.load (std::memory_order_relaxed); }
        void   setSessionTempoBpm (float bpm);

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
        struct AutomationPoint { juce::int64 samplePos; float value; };

        // Returns the points for (track, parameter). Empty when none.
        const std::vector<AutomationPoint>& getAutomation (int track, AutomationParam) const;

        // Drop a new point. Replaces an existing one if it lands within
        // a tolerance window (so dragging never produces fan-out clouds).
        void addAutomationPoint (int track, AutomationParam, juce::int64 samplePos, float value);

        // Remove the nearest point inside the given sample tolerance.
        void removeAutomationPointNear (int track, AutomationParam,
                                        juce::int64 samplePos, juce::int64 tolerance);

        void clearAutomation (AutomationParam);
        void clearAutomationForTrack (int track, AutomationParam);

        // Per-track clip list. Lazy: a track stays in 'whole file' mode
        // (no entry in trackClips, or one full-range entry) until the
        // engineer splits or trims it. The EDIT view + future playback
        // path read from here when present; falls back to the underlying
        // Track_NN.wav otherwise.
        std::vector<Clip>& clipsFor (int track);
        const std::vector<Clip>* tryClipsFor (int track) const;
        // Split the named track at the current playhead — creates two
        // clips that reference the same audio file with adjacent regions.
        bool splitTrackAtPlayhead (int track);

        // Recent sessions — maintained when loadSession / startRecording
        // succeed. Persisted in appProps as 'recentSession_<i>' (i = 0
        // most recent). Capped at kMaxRecent entries.
        static constexpr int kMaxRecent = 10;
        void rememberRecentSession (const juce::File& dir);
        juce::Array<juce::File> getRecentSessions() const;
        void clearRecentSessions();

        // Wipe every per-strip persisted override so all strips read
        // their defaults (name = '1', '2', '3' …, no colour override,
        // 0 dB gain, centre pan, master routing). Used by the welcome
        // dialog's 'Create New Session' so the engineer starts with a
        // truly clean board.
        void resetAllStripState();

        // Forwards to MultitrackRecorder.
        void setBackupDirectory (const juce::File& dir) { recorder.setBackupDirectory (dir); }
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
    public:
        ClickEngine& getClickEngine() noexcept { return click; }
    private:
        std::unique_ptr<OscRemote> osc;
        std::unique_ptr<CompanionServer> companion;
        std::unique_ptr<juce::PropertiesFile> appProps;

        // Live performance telemetry — written from the audio thread
        // (audioLoadPct) and the writer threads (diskMBPerSec /
        // ringFillPct), polled from the UI to drive the header dashboard.
        // 0–100 % units.
        std::atomic<double> deviceSampleRate { 0.0 };
        std::atomic<float>  audioLoadPct     { 0.0f };
        std::atomic<float>  currentTempoBpm  { 120.0f };
        std::vector<TempoChange> tempoMap;   // sorted by samplePos; UI/message-thread only

        // Per-track, per-parameter automation. UI-thread only for now
        // (audio thread doesn't yet consume these — the engineer reads
        // them visually and they ride along in cue snapshots).
        struct TrackAutomation
        {
            std::vector<AutomationPoint> volume, pan, mute;
        };
        std::vector<TrackAutomation> automationData;
        // Per-track clip lists. Empty/missing entry → 'play the whole
        // Track_NN.wav' (the current behaviour). Once the engineer
        // splits or trims, the entry has one or more Clips covering
        // the audible regions.
        std::vector<std::vector<Clip>> trackClips;
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
