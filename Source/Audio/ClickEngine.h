#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace zynforge
{
    // Real-time metronome that mixes into the engine's master output.
    // Tempo / voice / subdivision are all atomic so the message thread
    // can change them on the fly — the audio thread reads them every
    // block, so a BPM tweak takes effect on the next beat without ever
    // stopping the click.
    //
    // CLICK 1 fires on every downbeat of a 4-beat bar; CLICK 2 fires
    // on the remaining beats. Both voices have their own subdivision
    // multiplier (whole/half/quarter/8th/16th/triplet/silent) so the
    // engineer can build patterns like "cowbell on 1, 8th-note clicks
    // elsewhere".
    class ClickEngine
    {
    public:
        enum class Voice : int { Sine = 0, Beep, Cowbell, WoodBlock, Click };
        enum class Subdivision : int { Whole = 0, Half, Quarter, Eighth, Sixteenth, Triplet, Silent };

        void prepare (double sampleRate);
        void processBlock (float* outL, float* outR, int numSamples);

        // Setters — safe from any thread.
        void setEnabled    (bool en) noexcept       { enabled     .store (en,  std::memory_order_relaxed); }
        void setTempoBpm   (float bpm) noexcept     { tempoBpm    .store (juce::jlimit (20.0f, 999.0f, bpm),
                                                                          std::memory_order_relaxed); }
        void setVolume1Db  (float dB) noexcept      { vol1Db      .store (dB,  std::memory_order_relaxed); }
        void setVolume2Db  (float dB) noexcept      { vol2Db      .store (dB,  std::memory_order_relaxed); }
        void setVoice1     (Voice v) noexcept       { voice1      .store ((int) v, std::memory_order_relaxed); }
        void setVoice2     (Voice v) noexcept       { voice2      .store ((int) v, std::memory_order_relaxed); }
        void setSub1       (Subdivision s) noexcept { sub1        .store ((int) s, std::memory_order_relaxed); }
        void setSub2       (Subdivision s) noexcept { sub2        .store ((int) s, std::memory_order_relaxed); }

        bool  isEnabled()  const noexcept           { return enabled  .load (std::memory_order_relaxed); }
        float getTempoBpm() const noexcept          { return tempoBpm .load (std::memory_order_relaxed); }
        Voice getVoice1()  const noexcept           { return (Voice) voice1.load (std::memory_order_relaxed); }
        Voice getVoice2()  const noexcept           { return (Voice) voice2.load (std::memory_order_relaxed); }
        float getVol1Db()  const noexcept           { return vol1Db  .load (std::memory_order_relaxed); }
        float getVol2Db()  const noexcept           { return vol2Db  .load (std::memory_order_relaxed); }
        Subdivision getSub1() const noexcept        { return (Subdivision) sub1.load (std::memory_order_relaxed); }
        Subdivision getSub2() const noexcept        { return (Subdivision) sub2.load (std::memory_order_relaxed); }

        // Public so offline render can use the same beat-schedule math
        // and per-voice tone presets as the real-time path.
        static double subFactor (Subdivision) noexcept;
        struct VoicePreset { double freq; double decay; };
        static VoicePreset getVoicePreset (Voice) noexcept;

    private:
        std::atomic<bool>  enabled  { false };
        std::atomic<float> tempoBpm { 120.0f };
        std::atomic<float> vol1Db   { 0.0f };
        std::atomic<float> vol2Db   { -3.0f };
        std::atomic<int>   voice1   { (int) Voice::Click };
        std::atomic<int>   voice2   { (int) Voice::Click };
        std::atomic<int>   sub1     { (int) Subdivision::Quarter };
        std::atomic<int>   sub2     { (int) Subdivision::Quarter };

        // Audio-thread state.
        double sampleRate { 48000.0 };
        double samplesUntilNextBeat1 { 0.0 };
        double samplesUntilNextBeat2 { 0.0 };
        int    beat1Counter { 0 };    // 0..3, downbeat is 0
        int    beat2Counter { 0 };

        // One active voice burst per channel. The burst is a damped
        // sine, parameterised by frequency + envelope decay rate.
        struct Burst
        {
            double phase   { 0.0 };
            double freq    { 1000.0 };
            double decay   { 60.0 };
            double gainLin { 0.0 };
            double tSec    { 0.0 };
            bool   active  { false };
        };
        Burst voice1Burst;
        Burst voice2Burst;

        static void   triggerBurst (Burst&, Voice, double gainLin) noexcept;
        static float  renderBurst (Burst&, double sampleRate) noexcept;
    };
}
