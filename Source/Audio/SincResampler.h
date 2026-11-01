#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <Accelerate/Accelerate.h>
#include <cmath>
#include <vector>

namespace zynforge
{
    // Windowed-sinc sample-rate conversion for exports and imports. The output
    // starts on the source's first sample, so converted stems stay aligned. A
    // Kaiser-windowed kernel 96 zero crossings long rejects aliases by about
    // 100 dB; going down, the cutoff drops to the new Nyquist and the kernel
    // stretches to match. Nothing here is real-time, but a long multichannel
    // export runs every file through it, so the kernel is worked out once for
    // 1024 positions between samples and each output blends the nearest two.
    //
    // Same calls as juce::ResamplingAudioSource: the ratio is source rate over
    // destination rate.
    class SincResampler final : public juce::AudioSource
    {
    public:
        SincResampler (juce::AudioSource* input, int numChannels)
            : source (input), channels (numChannels) {}

        void setResamplingRatio (double sourcePerDestination) { ratio = sourcePerDestination; }

        void prepareToPlay (int samplesPerBlock, double sampleRate) override
        {
            source->prepareToPlay (samplesPerBlock, sampleRate);
            cutoff = juce::jmin (1.0, 1.0 / ratio) * kPassband;
            halfWidth = (int) std::ceil (kZeroCrossings / juce::jmin (1.0, 1.0 / ratio));
            pullSize = juce::jmax (samplesPerBlock, 4096);
            history.setSize (channels, 2 * (2 * halfWidth + pullSize));
            history.clear();
            pulled.setSize (channels, pullSize);
            coefficients.resize ((size_t) (2 * halfWidth));
            buildPhaseTable();
            // The kernel reads silence before the first source sample.
            fill = halfWidth + 1;
            firstInBuffer = -(juce::int64) fill;
            historyPosition = 0.0;
        }

        void releaseResources() override { source->releaseResources(); }

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            const int taps = 2 * halfWidth;
            for (int out = 0; out < info.numSamples; ++out)
            {
                const auto centre = (juce::int64) std::floor (historyPosition);
                const auto lastNeeded = centre + halfWidth;
                while (firstInBuffer + fill <= lastNeeded) pull();

                const double phasePosition = (historyPosition - (double) centre) * kPhases;
                const auto phase = (size_t) phasePosition;
                const auto blend = (float) (phasePosition - (double) phase);
                const float* lower = phaseTable.data() + phase * (size_t) taps;
                const float* upper = lower + taps;
                for (int k = 0; k < taps; ++k)
                    coefficients[(size_t) k] = lower[k] + blend * (upper[k] - lower[k]);

                const auto offset = (int) (centre - halfWidth + 1 - firstInBuffer);
                for (int ch = 0; ch < channels && ch < info.buffer->getNumChannels(); ++ch)
                {
                    float value = 0.0f;
                    vDSP_dotpr (coefficients.data(), 1, history.getReadPointer (ch, offset), 1,
                                &value, (vDSP_Length) taps);
                    info.buffer->setSample (ch, info.startSample + out, value);
                }
                historyPosition += ratio;
            }
        }

    private:
        static constexpr int    kZeroCrossings = 48;     // each side of the centre
        static constexpr double kPassband      = 0.94;   // of Nyquist, where the filter's -6 dB falls
        static constexpr double kKaiserBeta    = 11.0;
        static constexpr int    kPhases        = 1024;   // kernel positions between two samples

        static double besselI0 (double x)
        {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 40; ++k)
            {
                term *= (x * 0.5 / k) * (x * 0.5 / k);
                sum += term;
            }
            return sum;
        }

        double kernelAt (double distance) const
        {
            const double width = (double) halfWidth;
            if (distance >= width) return 0.0;
            const double r = distance / width;
            const double window = besselI0 (kKaiserBeta * std::sqrt (1.0 - r * r)) / besselI0 (kKaiserBeta);
            const double px = juce::MathConstants<double>::pi * distance * cutoff;
            return cutoff * (distance == 0.0 ? 1.0 : std::sin (px) / px) * window;
        }

        // Row p holds the kernel for a read position p / kPhases past a sample,
        // normalised so a constant signal keeps its level. The extra last row is
        // the one the blend reaches from just below the next sample.
        void buildPhaseTable()
        {
            const int taps = 2 * halfWidth;
            phaseTable.resize ((size_t) ((kPhases + 1) * taps));
            std::vector<double> row ((size_t) taps);
            for (int p = 0; p <= kPhases; ++p)
            {
                const double fraction = (double) p / kPhases;
                double sum = 0.0;
                for (int k = 0; k < taps; ++k)
                {
                    row[(size_t) k] = kernelAt (std::abs ((double) (k - halfWidth + 1) - fraction));
                    sum += row[(size_t) k];
                }
                for (int k = 0; k < taps; ++k)
                    phaseTable[(size_t) (p * taps + k)] = (float) (row[(size_t) k] / sum);
            }
        }

        // Appends one block from the source. A source past its end reads as
        // silence, which is what the kernel's tail should see.
        void pull()
        {
            if (fill + pullSize > history.getNumSamples())
            {
                const int keep = 2 * halfWidth + 2;
                const int drop = fill - keep;
                for (int ch = 0; ch < channels; ++ch)
                    history.copyFrom (ch, 0, history, ch, drop, keep);
                firstInBuffer += drop;
                fill = keep;
            }
            pulled.clear();
            juce::AudioSourceChannelInfo info (&pulled, 0, pullSize);
            source->getNextAudioBlock (info);
            for (int ch = 0; ch < channels; ++ch)
                history.copyFrom (ch, fill, pulled, ch, 0, pullSize);
            fill += pullSize;
        }

        juce::AudioSource* source;
        int channels;
        double ratio { 1.0 }, cutoff { 1.0 };
        int halfWidth { kZeroCrossings }, pullSize { 4096 };
        juce::AudioBuffer<float> history, pulled;
        std::vector<float> phaseTable, coefficients;
        int fill { 0 };                  // valid samples in history
        juce::int64 firstInBuffer { 0 }; // source position of history[0]
        double historyPosition { 0.0 };  // source position of the next output sample
    };
}
