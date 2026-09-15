#include "ClickTrackRenderer.h"

#include <algorithm>
#include <cmath>

namespace zynforge::clickrender
{
    namespace
    {
        bool isCancelled (const std::atomic<bool>* cancel) noexcept
        {
            return cancel != nullptr && cancel->load (std::memory_order_relaxed);
        }
    }

    Result render (const juce::File& destination,
                   const Settings& settings,
                   const std::atomic<bool>* cancel)
    {
        if (isCancelled (cancel)) return Result::cancelled;
        if (settings.sampleRate < 8000.0 || settings.totalSamples <= 0
            || ! destination.getParentDirectory().isDirectory())
            return Result::failed;

        const auto temporary = destination.getNonexistentSibling (false);
        bool installed = false;
        const juce::ScopeGuard removeTemporary { [&]
        {
            if (! installed) temporary.deleteFile();
        } };

        std::unique_ptr<juce::FileOutputStream> output (temporary.createOutputStream());
        if (output == nullptr) return Result::failed;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (output.get(), settings.sampleRate, 1, 24, {}, 0));
        if (writer == nullptr) return Result::failed;
        output.release();

        constexpr int chunkSize = 32768;
        juce::AudioBuffer<float> buffer (1, chunkSize);

        struct Burst
        {
            double phase { 0.0 }, seconds { 0.0 }, frequency { 1000.0 }, decay { 60.0 };
            float gain { 0.0f };
            bool active { false };
        };
        Burst voice1, voice2;
        double until1 = 0.0, until2 = 0.0;
        int beat1 = 0, beat2 = 0;

        auto tempoMap = settings.tempoMap;
        std::sort (tempoMap.begin(), tempoMap.end(), [] (const auto& a, const auto& b)
        {
            return a.samplePos < b.samplePos;
        });
        size_t nextTempo = 0;
        float bpm = juce::jlimit (20.0f, 999.0f, settings.initialBpm);
        while (nextTempo < tempoMap.size() && tempoMap[nextTempo].samplePos <= 0)
            bpm = juce::jlimit (20.0f, 999.0f, tempoMap[nextTempo++].bpm);

        const auto factor1 = ClickEngine::subFactor (settings.subdivision1);
        const auto factor2 = ClickEngine::subFactor (settings.subdivision2);
        double samplesPerClick1 = 0.0, samplesPerClick2 = 0.0;
        auto updateIntervals = [&]
        {
            const auto quarter = 60.0 * settings.sampleRate / (double) bpm;
            samplesPerClick1 = factor1 > 0.0 ? quarter / factor1 : 0.0;
            samplesPerClick2 = factor2 > 0.0 ? quarter / factor2 : 0.0;
        };
        updateIntervals();

        auto renderBurst = [&] (Burst& burst)
        {
            if (! burst.active) return 0.0f;
            const auto envelope = std::exp (-burst.seconds * burst.decay);
            const auto sample = (float) (std::sin (burst.phase) * envelope) * burst.gain;
            burst.phase += juce::MathConstants<double>::twoPi * burst.frequency / settings.sampleRate;
            burst.seconds += 1.0 / settings.sampleRate;
            if (envelope < 0.001) burst.active = false;
            return sample;
        };

        const int beatsPerBar = juce::jlimit (1, 32, settings.beatsPerBar);
        juce::int64 written = 0;
        while (written < settings.totalSamples)
        {
            if (isCancelled (cancel)) return Result::cancelled;
            const auto count = (int) juce::jmin ((juce::int64) chunkSize,
                                                 settings.totalSamples - written);
            buffer.clear();
            auto* samples = buffer.getWritePointer (0);

            int offset = 0;
            while (offset < count)
            {
                const auto absolute = written + offset;
                while (nextTempo < tempoMap.size()
                       && tempoMap[nextTempo].samplePos <= absolute)
                {
                    bpm = juce::jlimit (20.0f, 999.0f, tempoMap[nextTempo++].bpm);
                    updateIntervals();
                }

                const auto nextBoundary = nextTempo < tempoMap.size()
                    ? juce::jlimit (absolute, written + count, tempoMap[nextTempo].samplePos)
                    : written + count;
                const int segmentEnd = (int) (nextBoundary - written);

                for (; offset < segmentEnd; ++offset)
                {
                    if (samplesPerClick1 > 0.0 && --until1 <= 0.0)
                    {
                        until1 += samplesPerClick1;
                        if ((beat1 % beatsPerBar) == 0)
                            voice1 = { 0.0, 0.0, settings.voice1.freq,
                                       settings.voice1.decay, settings.gain1, true };
                        ++beat1;
                    }
                    if (samplesPerClick2 > 0.0 && --until2 <= 0.0)
                    {
                        until2 += samplesPerClick2;
                        const bool duplicatesDownbeat =
                            settings.subdivision2 == ClickEngine::Subdivision::Quarter
                            && (beat2 % beatsPerBar) == 0;
                        if (! duplicatesDownbeat)
                            voice2 = { 0.0, 0.0, settings.voice2.freq,
                                       settings.voice2.decay, settings.gain2, true };
                        ++beat2;
                    }
                    samples[offset] = renderBurst (voice1) + renderBurst (voice2);
                }
            }

            if (! writer->writeFromFloatArrays (buffer.getArrayOfReadPointers(), 1, count))
                return Result::failed;
            written += count;
        }

        writer.reset(); // close and flush before replacing a known-good file
        if (isCancelled (cancel)) return Result::cancelled;
        if (! temporary.replaceFileIn (destination)) return Result::failed;
        installed = true;
        return Result::succeeded;
    }
}
