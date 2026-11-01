#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <vector>

namespace zynforge
{
    // Windowed-sinc sample-rate conversion for exports and imports. The output
    // starts on the source's first sample, so converted stems stay aligned. A
    // Kaiser-windowed kernel 96 zero crossings long rejects aliases by about
    // 100 dB; going down, the cutoff drops to the new Nyquist and the kernel
    // stretches to match. Nothing here is real-time: it costs about a hundred
    // multiplies per output sample per channel.
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
            // The kernel reads silence before the first source sample.
            fill = halfWidth + 1;
            firstInBuffer = -(juce::int64) fill;
            historyPosition = 0.0;
            buildKernel();
        }

        void releaseResources() override { source->releaseResources(); }

        void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
        {
            for (int out = 0; out < info.numSamples; ++out)
            {
                const auto centre = (juce::int64) std::floor (historyPosition);
                const auto lastNeeded = centre + halfWidth;
                while (firstInBuffer + fill <= lastNeeded) pull();

                const double fraction = historyPosition - (double) centre;
                double sum = 0.0;
                for (int k = 0; k < 2 * halfWidth; ++k)
                {
                    const double distance = (double) (k - halfWidth + 1) - fraction;
                    const double c = kernelAt (std::abs (distance));
                    coefficients[(size_t) k] = c;
                    sum += c;
                }
                const auto offset = (int) (centre - halfWidth + 1 - firstInBuffer);
                for (int ch = 0; ch < channels && ch < info.buffer->getNumChannels(); ++ch)
                {
                    const auto* in = history.getReadPointer (ch, offset);
                    double value = 0.0;
                    for (int k = 0; k < 2 * halfWidth; ++k)
                        value += coefficients[(size_t) k] * (double) in[k];
                    info.buffer->setSample (ch, info.startSample + out, (float) (value / sum));
                }
                historyPosition += ratio;
            }
        }

    private:
        static constexpr int    kZeroCrossings = 48;     // each side of the centre
        static constexpr double kPassband      = 0.94;   // of Nyquist, where the filter's -6 dB falls
        static constexpr double kKaiserBeta    = 11.0;
        static constexpr int    kKernelSteps   = 2048;   // table steps per zero crossing

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

        void buildKernel()
        {
            const int steps = kKernelSteps * (int) std::ceil (kZeroCrossings / cutoff) + 2;
            kernel.resize ((size_t) steps);
            const double width = (double) halfWidth;
            const double norm = besselI0 (kKaiserBeta);
            for (int i = 0; i < steps; ++i)
            {
                const double x = (double) i / kKernelSteps;
                const double r = juce::jmin (1.0, x / width);
                const double window = besselI0 (kKaiserBeta * std::sqrt (1.0 - r * r)) / norm;
                const double px = juce::MathConstants<double>::pi * x * cutoff;
                kernel[(size_t) i] = cutoff * (i == 0 ? 1.0 : std::sin (px) / px) * window;
            }
        }

        double kernelAt (double distance) const
        {
            if (distance >= (double) halfWidth) return 0.0;
            const double position = distance * kKernelSteps;
            const auto index = (size_t) position;
            const double t = position - (double) index;
            return kernel[index] + t * (kernel[index + 1] - kernel[index]);
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
        std::vector<double> kernel, coefficients;
        int fill { 0 };                  // valid samples in history
        juce::int64 firstInBuffer { 0 }; // source position of history[0]
        double historyPosition { 0.0 };  // source position of the next output sample
    };
}
