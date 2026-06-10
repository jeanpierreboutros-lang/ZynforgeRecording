// Headless tests for the Phase-0 status boundary (Source/Audio/EngineStatus.h):
// the serialisable snapshot must round-trip through JSON intact and keep the
// JSON shape the companion web client already consumes.

#include <juce_core/juce_core.h>

#include "../Audio/EngineStatus.h"

namespace zynforge
{
    class EngineStatusTests final : public juce::UnitTest
    {
    public:
        EngineStatusTests() : juce::UnitTest ("Engine status boundary", "zynforge") {}

        void runTest() override
        {
            beginTest ("EngineStatus round-trips through JSON");
            {
                EngineStatus s;
                s.recording = true;  s.playing = false;
                s.positionSamples = 123456; s.elapsedSamples = 654321;
                s.sampleRate = 48000.0; s.blockSize = 256;
                s.audioLoadPct = 17.5f; s.diskMBPerSec = 12.3; s.minutesRemaining = 88;
                s.missedSamples = 0; s.numTracks = 2; s.armedTracks = 1;
                s.backupActive = true; s.captureFormat = 3;

                TrackStatus a; a.name = "Kick"; a.peak = 0.8f; a.rms = 0.3f;
                a.armed = true; a.muted = false; a.soloed = true; a.monitor = true;
                a.stereoLeft = false; a.colourARGB = 0xff112233u;
                TrackStatus b; b.name = "OH L"; b.stereoLeft = true; b.colourARGB = 0xffaabbccu;
                s.tracks = { a, b };

                const auto back = EngineStatus::fromJson (s.toJson());
                expect (back.recording == s.recording);
                expect (back.playing   == s.playing);
                expectEquals (back.positionSamples, s.positionSamples);
                expectEquals (back.elapsedSamples,  s.elapsedSamples);
                expectWithinAbsoluteError (back.sampleRate, s.sampleRate, 1.0e-9);
                expectEquals (back.blockSize, s.blockSize);
                expectWithinAbsoluteError (back.audioLoadPct, s.audioLoadPct, 1.0e-4f);
                expectEquals (back.minutesRemaining, s.minutesRemaining);
                expectEquals (back.numTracks, s.numTracks);
                expectEquals (back.armedTracks, s.armedTracks);
                expect (back.backupActive == s.backupActive);
                expectEquals (back.captureFormat, s.captureFormat);
                expectEquals ((int) back.tracks.size(), 2);
                if (back.tracks.size() == 2)
                {
                    expectEquals (back.tracks[0].name, juce::String ("Kick"));
                    expectWithinAbsoluteError (back.tracks[0].peak, 0.8f, 1.0e-4f);
                    expect (back.tracks[0].soloed);
                    expect (back.tracks[1].stereoLeft);
                    // Colour survives (full ARGB, alpha forced opaque).
                    expectEquals ((int) back.tracks[0].colourARGB, (int) 0xff112233u);
                    expectEquals ((int) back.tracks[1].colourARGB, (int) 0xffaabbccu);
                }
            }

            beginTest ("toJson keeps the companion web-client shape");
            {
                EngineStatus s;
                s.recording = true; s.playing = true;
                TrackStatus t; t.name = "Vox"; t.armed = true; t.muted = false;
                t.soloed = false; t.peak = 0.5f; t.colourARGB = 0xff204060u;
                s.tracks = { t };

                const auto v = s.toJson();
                expect ((bool) v.getProperty ("recording", false));
                expect ((bool) v.getProperty ("playing", false));
                auto* arr = v.getProperty ("tracks", {}).getArray();
                expect (arr != nullptr && arr->size() == 1);
                if (arr != nullptr && arr->size() == 1)
                {
                    const auto& tv = (*arr)[0];
                    expectEquals (tv.getProperty ("name", "").toString(), juce::String ("Vox"));
                    expect ((bool) tv.getProperty ("armed", false));
                    expectWithinAbsoluteError ((float) (double) tv.getProperty ("peak", 0.0), 0.5f, 1.0e-4f);
                    // colour is "#RRGGBB".
                    const auto col = tv.getProperty ("colour", "").toString();
                    expect (col.startsWith ("#") && col.length() == 7, "colour not #RRGGBB: " + col);
                    expectEquals (col, juce::String ("#204060"));
                }
            }
        }
    };

    static EngineStatusTests engineStatusTests;
}
