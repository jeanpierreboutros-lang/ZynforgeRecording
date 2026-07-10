#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace zynforge
{
    // Post-record analysis pass. Reads each Track_NN.wav (or .flac /
    // .aif) in a session, runs three heuristics, and writes a JSON
    // summary to <session>/noise_report.json that the engineer can
    // open between songs.
    //
    // Heuristics:
    //   - Hum:        peaky FFT bins at 50/60 Hz + first 2 harmonics,
    //                 each > -40 dBFS AND > 6 dB above the surrounding
    //                 noise floor. Reports the worst dB above floor.
    //   - Mic bumps:  large LOW-frequency transients (< 80 Hz energy
    //                 spike) with sudden onset and minimal high-freq
    //                 content. Counts events per track.
    //   - Const noise: high steady RMS + low crest factor → suggests
    //                 a fan / preamp hiss baseline.
    //
    // Implementation: 1-second analysis frames, hop = 0.5s. FFT size
    // 4096. Doesn't need to be RT-safe -- runs on a background thread
    // from the message thread after stopRecording or on demand.
    struct NoiseFinding
    {
        int    track            { -1 };
        juce::String trackName;
        // Hum analysis
        float  humDb            { -120.0f };   // worst harmonic level (dBFS)
        float  humDbAboveFloor  { 0.0f };
        int    humFundamentalHz { 0 };         // 0 if not detected, else 50 or 60
        // Mic bumps
        int    bumpCount        { 0 };
        // Constant noise floor
        float  noiseFloorDbFS   { -90.0f };
        float  crestFactor      { 0.0f };      // peak / rms ratio (dB)
    };

    class NoiseAnalyzer
    {
    public:
        static NoiseFinding analyseFile (const juce::File& wavFile,
                                          int trackIdx,
                                          const juce::String& name)
        {
            NoiseFinding nf;
            nf.track     = trackIdx;
            nf.trackName = name;

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            fm.registerFormat (new juce::FlacAudioFormat(), false);   // FLAC takes are analysable too
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wavFile));
            if (reader == nullptr || reader->numChannels < 1) return nf;   // guard OOB read on malformed file

            const double sr     = reader->sampleRate;
            const auto   total  = (juce::int64) reader->lengthInSamples;
            if (sr <= 0.0 || total < (int) sr) return nf;

            constexpr int kFftOrder = 12;          // 4096-point
            constexpr int kFftSize  = 1 << kFftOrder;
            const int hopSamples    = (int) (sr * 0.5);

            juce::dsp::FFT fft (kFftOrder);
            std::vector<float> window (kFftSize);
            for (int i = 0; i < kFftSize; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                              * (float) i / (kFftSize - 1));

            juce::AudioBuffer<float> chunk (1, kFftSize);
            std::vector<float> fftBuf ((size_t) kFftSize * 2, 0.0f);

            // Running aggregates.
            float maxPeak   = 0.0f;
            double sumSq    = 0.0;
            double sumCount = 0.0;
            int    bumps    = 0;
            float  prevLowEnergy = 0.0f;

            float bestHumDb       = -120.0f;
            float bestHumAboveFloor = 0.0f;
            int   bestHumFund     = 0;

            auto binFor = [&] (double hz) -> int
            {
                return juce::jlimit (0, kFftSize / 2 - 1, (int) std::round (hz * kFftSize / sr));
            };

            for (juce::int64 pos = 0; pos + kFftSize <= total; pos += hopSamples)
            {
                chunk.clear();
                reader->read (&chunk, 0, kFftSize, pos, true, false);
                const float* in = chunk.getReadPointer (0);

                // Window into fftBuf (real-only).
                std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
                for (int i = 0; i < kFftSize; ++i)
                {
                    const float s = in[i] * window[(size_t) i];
                    fftBuf[(size_t) i] = s;
                    if (std::abs (in[i]) > maxPeak) maxPeak = std::abs (in[i]);
                    sumSq += (double) in[i] * (double) in[i];
                    ++sumCount;
                }

                fft.performFrequencyOnlyForwardTransform (fftBuf.data());

                // Spectrum is now magnitudes in fftBuf[0..kFftSize/2].
                // Compute noise floor as the median of the 100..800 Hz
                // band -- bypasses both the hum harmonics and the HF
                // content from typical music.
                const int floorLo = binFor (100.0);
                const int floorHi = binFor (800.0);
                std::vector<float> floorBins (fftBuf.begin() + floorLo,
                                              fftBuf.begin() + floorHi + 1);
                std::nth_element (floorBins.begin(),
                                  floorBins.begin() + floorBins.size() / 2,
                                  floorBins.end());
                const float floorMag = floorBins[floorBins.size() / 2];

                auto magDb = [] (float m) -> float
                {
                    return juce::Decibels::gainToDecibels (juce::jmax (1e-7f, m), -120.0f);
                };
                const float floorDb = magDb (floorMag);

                // Hum: check 50 / 60 Hz fundamentals + first 2 harmonics.
                for (int fundamental : { 50, 60 })
                {
                    int hits = 0;
                    float worstAbove = 0.0f;
                    for (int h = 1; h <= 3; ++h)
                    {
                        const auto m = fftBuf[(size_t) binFor (fundamental * h)];
                        const float above = magDb (m) - floorDb;
                        if (above > 6.0f && magDb (m) > -40.0f)
                        {
                            ++hits;
                            worstAbove = juce::jmax (worstAbove, above);
                        }
                    }
                    if (hits >= 2 && worstAbove > bestHumAboveFloor)
                    {
                        bestHumAboveFloor = worstAbove;
                        bestHumDb         = magDb (fftBuf[(size_t) binFor (fundamental)]);
                        bestHumFund       = fundamental;
                    }
                }

                // Mic bump: low-band energy spike (< 80 Hz) > 18 dB
                // larger than the prior frame's, with low high-freq
                // content (HF / LF magnitude ratio under 0.25).
                float lowSum = 0.0f, hiSum = 0.0f;
                for (int b = binFor (20.0); b <= binFor (80.0); ++b)
                    lowSum += fftBuf[(size_t) b];
                for (int b = binFor (1500.0); b <= binFor (5000.0); ++b)
                    hiSum  += fftBuf[(size_t) b];
                if (prevLowEnergy > 0.0f && lowSum > prevLowEnergy * 6.0f
                    && (lowSum > 0.0f && hiSum / lowSum < 0.25f))
                {
                    ++bumps;
                }
                prevLowEnergy = lowSum;
            }

            const double rms = sumCount > 0 ? std::sqrt (sumSq / sumCount) : 0.0;
            nf.noiseFloorDbFS = juce::Decibels::gainToDecibels (juce::jmax (1e-7, rms), -120.0);
            nf.crestFactor    = juce::Decibels::gainToDecibels (juce::jmax (1e-7f, maxPeak), -120.0f)
                                - nf.noiseFloorDbFS;
            nf.humDb           = bestHumDb;
            nf.humDbAboveFloor = bestHumAboveFloor;
            nf.humFundamentalHz= bestHumFund;
            nf.bumpCount       = bumps;
            return nf;
        }

        // Scan the session, write noise_report.json, return findings
        // for the UI to also display.
        static std::vector<NoiseFinding> analyseSession (const juce::File& sessionDir,
                                                          std::function<juce::String (int)> trackNameOf)
        {
            std::vector<NoiseFinding> findings;
            if (! sessionDir.isDirectory()) return findings;

            const auto audioFiles = sessionDir.getChildFile ("Audio Files");
            const auto base = audioFiles.isDirectory() ? audioFiles : sessionDir;
            for (const auto& f : base.findChildFiles (juce::File::findFiles, false, "Track_*"))
            {
                // Audio only -- exclude .wav.punchbase and other Track_* sidecars
                // that produced phantom "clean" rows in the report.
                const auto ext = f.getFileExtension().toLowerCase();
                if (ext != ".wav" && ext != ".flac" && ext != ".aif" && ext != ".aiff")
                    continue;
                const auto stem = f.getFileNameWithoutExtension();   // "Track_04" or "Track_04_part02"
                if (stem.containsIgnoreCase ("_part"))
                    continue;   // continuation parts belong to their main file, not a separate row
                // Index from the Track_NN PREFIX. The old last-"_" parse turned
                // "Track_04_part02" into "part02" -> 0 -> idx -1 and silently
                // dropped continuation-recorded takes from the report.
                const int idx = stem.fromFirstOccurrenceOf ("Track_", false, false).getIntValue() - 1;
                if (idx < 0) continue;
                const auto nm = trackNameOf ? trackNameOf (idx) : stem;
                findings.push_back (analyseFile (f, idx, nm));
            }

            // Emit JSON to noise_report.json.
            juce::Array<juce::var> arr;
            for (const auto& nf : findings)
            {
                juce::DynamicObject::Ptr o (new juce::DynamicObject());
                o->setProperty ("track",            nf.track);
                o->setProperty ("name",             nf.trackName);
                o->setProperty ("humDb",            (double) nf.humDb);
                o->setProperty ("humAboveFloorDb",  (double) nf.humDbAboveFloor);
                o->setProperty ("humFundamentalHz", nf.humFundamentalHz);
                o->setProperty ("bumpCount",        nf.bumpCount);
                o->setProperty ("noiseFloorDbFS",   (double) nf.noiseFloorDbFS);
                o->setProperty ("crestFactorDb",    (double) nf.crestFactor);
                arr.add (juce::var (o.get()));
            }
            juce::DynamicObject::Ptr root (new juce::DynamicObject());
            root->setProperty ("generatedAt", juce::Time::getCurrentTime().toISO8601 (true));
            root->setProperty ("tracks", juce::var (arr));
            sessionDir.getChildFile ("noise_report.json")
                      .replaceWithText (juce::JSON::toString (juce::var (root.get())));

            return findings;
        }

        // Build a human-readable summary line per track.
        static juce::String summaryLine (const NoiseFinding& nf)
        {
            juce::StringArray parts;
            if (nf.humFundamentalHz > 0)
                parts.add (juce::String (nf.humFundamentalHz) + " Hz hum "
                            + juce::String (nf.humDbAboveFloor, 1) + " dB above floor");
            if (nf.bumpCount > 0)
                parts.add (juce::String (nf.bumpCount) + " mic bump"
                            + (nf.bumpCount > 1 ? juce::String ("s") : juce::String()));
            if (nf.noiseFloorDbFS > -50.0f)
                parts.add ("high noise floor (" + juce::String (nf.noiseFloorDbFS, 1) + " dBFS)");
            if (parts.isEmpty()) parts.add ("clean");
            return nf.trackName + ": " + parts.joinIntoString ("; ");
        }
    };
}
