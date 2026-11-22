// Fixes from the 2026-11-22 review, export and readout side: a song export at
// another rate reports the overs its conversion makes, a clock wait for one
// device never hands back another device's reading, the Mackie time display
// in its 3-2-2-3 grouping, timecode labels that truncate to the frame a
// position is in, and a pre-flight speed test that stops once pre-flight has
// stopped waiting for it.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/ClockSource.h"
#include "../Audio/McuProtocol.h"
#include "../Audio/PreflightProbes.h"
#include "../Audio/SongExport.h"
#include "../Audio/TimelineExport.h"
#include "../Audio/TrackExporter.h"

#include <array>
#include <atomic>
#include <cmath>
#include <thread>

namespace zynforge
{
class Fix20261122ExportTests final : public juce::UnitTest
{
public:
    Fix20261122ExportTests() : juce::UnitTest ("Fixes 2026-11-22 export", "zynforge") {}

    static constexpr double kRate = 48000.0;

    struct Folder
    {
        juce::File dir { juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("zf-fix1122-" + juce::Uuid().toString()).getChildFile ("Show") };
        Folder()  { audio().createDirectory(); exports().createDirectory(); }
        ~Folder() { dir.getParentDirectory().deleteRecursively(); }
        juce::File audio()   const { return dir.getChildFile ("Audio Files"); }
        juce::File exports() const { return dir.getChildFile ("Export Files"); }
    };

    // A 12 kHz tone at 48 kHz, phased so every sample lands at 0.707 of the
    // true peak: 1.3 peaks read +/-0.919, under full scale, until a rate
    // conversion puts samples near the crest and they go over.
    static float overSample (int n)
    {
        return 1.3f * (float) std::sin (juce::MathConstants<double>::halfPi * (double) n
                                         + juce::MathConstants<double>::pi * 0.25);
    }

    static juce::AudioBuffer<float> overTone (int samples)
    {
        juce::AudioBuffer<float> tone (1, samples);
        for (int i = 0; i < samples; ++i) tone.setSample (0, i, overSample (i));
        return tone;
    }

    static bool writeFloatTake (const juce::File& file, const juce::AudioBuffer<float>& audio)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (file.createOutputStream().release());
        if (os == nullptr) return false;
        auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (kRate)
                                                   .withNumChannels (1)
                                                   .withBitsPerSample (32)
                                                   .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr) return false;
        return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    static float filePeak (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr) return -1.0f;
        const int n = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> audio ((int) reader->numChannels, n);
        reader->read (&audio, 0, n, 0, true, true);
        return audio.getMagnitude (0, n);
    }

    static juce::Array<juce::File> filesIn (const juce::File& dir, const juce::String& pattern = "*")
    {
        juce::Array<juce::File> out;
        if (dir.isDirectory())
            dir.findChildFiles (out, juce::File::findFiles, true, pattern);
        return out;
    }

    static void setup (AudioEngine& e, int strips)
    {
        e.clearAllStripOverrides();
        e.setStripCount (strips);
        e.prepareForTests (kRate, 256);
        e.getMasterState().muted.store (false);
        e.getMasterState().gainDb.store (0.0f);
        for (int i = 0; i < strips; ++i)
        {
            auto& t = e.getRecorder().getTrack (i);
            t.gainDb.store (0.0f); t.pan.store (0.0f); t.muted.store (false); t.soloed.store (false);
            t.isStereo.store (false); t.setNameThreadSafe ("Strip " + juce::String (i + 1));
        }
    }

    // What samplesToTimecode must print for frame `frame`, from integers.
    static juce::String label (juce::int64 frame, int fps)
    {
        const auto seconds = frame / fps;
        return juce::String::formatted ("%02d:%02d:%02d:%02d", (int) (seconds / 3600), (int) ((seconds / 60) % 60),
                                        (int) (seconds % 60), (int) (frame % fps));
    }

    void runTest() override
    {
        // Before the first engine exists: without it the engine opens the
        // real audio device.
        AudioEngine::setTestModeSkipAudioInit (true);
        songExportClipping();
        clockWaitForTheRightDevice();
        mackieTimeDisplay();
        timecodeLabelsTruncate();
        speedProbeCancels();
    }

private:
    void songExportClipping()
    {
        namespace sx = songexport;

        beginTest ("A file stream at another rate counts the overs its conversion makes");
        {
            Folder f;
            TrackExporter exporter;
            ExportOptions options;
            options.format = ExportFormat::Wav24;
            options.bitsPerSample = 24;
            options.sampleRate = 44100.0;
            const auto tone = overTone (48000);
            expect (tone.getMagnitude (0, tone.getNumSamples()) < 1.0f, "the fixture must stay under full scale before conversion");

            ExportFileStream out (exporter, f.exports().getChildFile ("Converted"), options, kRate, 1);
            expect (out.isOpen());
            expect (out.write (tone, tone.getNumSamples()));
            expect (! out.hasClipped(), "nothing was over full scale before the conversion");
            juce::String error;
            expect (out.commit (error), error);
            expect (out.hasClipped(), "the converted file clipped and the stream said it was clean");
            expectEquals (exporter.getClippedFiles(), 1);
            expect (filePeak (f.exports().getChildFile ("Converted.wav")) > 0.999f,
                    "the fixture must actually flat-top once converted");
        }

        beginTest ("A song export at another rate reports the overs its conversion makes");
        {
            Folder f;
            expect (writeFloatTake (f.audio().getChildFile ("Track_01.wav"), overTone (48000)));
            AudioEngine e; setup (e, 1);
            e.loadSession (f.dir);
            const std::vector<sx::Range> songs { { 0, 48000, "Song" } };
            std::atomic<bool> cancel { false };

            const auto runWith = [&] (int bits, double rate)
            {
                sx::Choice choice;
                choice.songs = songs;
                choice.numbers = { 1 };
                choice.mix = false;
                choice.stems = true;
                choice.format.format = ExportFormat::Wav24;
                choice.format.bitsPerSample = bits;
                choice.format.sampleRate = rate;
                return sx::run (e, sx::buildPlan (e, choice, f.exports()), cancel, nullptr);
            };

            const auto converted = runWith (24, 44100.0);
            expect (converted.complete(), converted.firstError);
            expectEquals (converted.clippedFiles, 1, "the 44.1 kHz stem clipped and the export called it clean");
            const auto line = sx::statusLine (converted, "Songs");
            expect (line.contains ("CLIPPED"), "the status line must warn: " + line);

            const auto floatFile = runWith (32, 44100.0);
            expect (floatFile.complete(), floatFile.firstError);
            expectEquals (floatFile.clippedFiles, 0, "a float file holds the overs");

            const auto sameRate = runWith (24, kRate);
            expect (sameRate.complete(), sameRate.firstError);
            expectEquals (sameRate.clippedFiles, 0, "at the session rate nothing went over");
            e.clearAllStripOverrides();
        }
    }

    void clockWaitForTheRightDevice()
    {
        using namespace clocksource;

        // Interface A's read holds in the driver until released.
        const auto heldProbe = [] (std::shared_ptr<juce::WaitableEvent> releaseA,
                                   std::shared_ptr<std::atomic<int>> reads)
        {
            return Probe ([releaseA, reads] (const juce::String& device) -> Reading
            {
                ++*reads;
                if (device == "Interface A") releaseA->wait (5000);
                return { Kind::external, device };
            });
        };

        beginTest ("A clock wait for B after A's read never returns A's reading");
        {
            auto releaseA = std::make_shared<juce::WaitableEvent> (true);
            auto reads    = std::make_shared<std::atomic<int>> (0);
            auto probe    = heldProbe (releaseA, reads);

            probe.request ("Interface A");
            const auto start = juce::Time::getMillisecondCounterHiRes();
            expect (probe.wait ("Interface B", 60).kind == Kind::unavailable,
                    "B was answered while A's read held the thread");
            expect (juce::Time::getMillisecondCounterHiRes() - start < 1000.0, "the wait was not bounded");

            std::thread releaser ([releaseA] { juce::Thread::sleep (50); releaseA->signal(); });
            const auto answer = probe.wait ("Interface B", 5000);
            releaser.join();
            expectEquals (answer.name, juce::String ("Interface B"), "wait (B) handed back another device's clock");
            expect (answer.kind == Kind::external);
            expectEquals (probe.latest().name, juce::String ("Interface B"));
            expectEquals (reads->load(), 2, "B is read once, after A");
        }

        beginTest ("A read for a device nobody asks about any more never becomes the latest");
        {
            auto releaseA = std::make_shared<juce::WaitableEvent> (true);
            auto reads    = std::make_shared<std::atomic<int>> (0);
            auto probe    = heldProbe (releaseA, reads);

            probe.request ("Interface A");
            expect (probe.wait ("Interface B", 30).kind == Kind::unavailable);
            releaseA->signal();
            for (int i = 0; i < 300 && probe.latest().kind == Kind::unavailable; ++i)
                juce::Thread::sleep (10);
            expectEquals (probe.latest().name, juce::String ("Interface B"),
                          "a report would carry the clock of the interface asked about before");
            expect (probe.wait ("Interface B", 2000).kind == Kind::external);
        }
    }

    void mackieTimeDisplay()
    {
        // Rightmost digit (CC 0x40) first. '0'..'9' are 0x30..0x39, a blank
        // is 0x20, and 0x40 lights the dot.
        const auto bytesFor = [this] (double seconds, const juce::String& what)
        {
            std::vector<juce::MidiMessage> digits;
            mcu::timeDisplayMessages (seconds, 30, digits);
            expectEquals ((int) digits.size(), 10, what);
            std::array<int, 10> values;
            values.fill (-1);
            for (const auto& d : digits)
            {
                expectEquals (d.getRawDataSize(), 3, what);
                const auto* raw = d.getRawData();
                expectEquals ((int) raw[0], 0xB0, what + ": a controller on channel 1");
                const int digit = (int) raw[1] - 0x40;
                expect (digit >= 0 && digit < 10, what + ": CC outside the display");
                if (digit < 0 || digit >= 10) continue;
                expectEquals (values[(size_t) digit], -1, what + ": a digit sent twice");
                values[(size_t) digit] = (int) raw[2];
            }
            return values;
        };
        const auto expectDigits = [this] (const std::array<int, 10>& got, const std::array<int, 10>& want,
                                          const juce::String& what)
        {
            for (size_t i = 0; i < want.size(); ++i)
                expectEquals (got[i], want[i], what + ": CC 0x" + juce::String::toHexString (0x40 + (int) i));
        };

        beginTest ("The Mackie time display groups hours 3, minutes 2, seconds 2, frames 3");
        {
            // 01:01:01:15 -> [ 01.][01.][01.][ 15], hours and frames right-aligned.
            expectDigits (bytesFor (3661.5, "01:01:01:15"),
                          { 0x35, 0x31, 0x20, 0x71, 0x30, 0x71, 0x30, 0x71, 0x30, 0x20 }, "01:01:01:15");

            // Late in frame 29 of 10:59:59: the readout stays in that frame.
            expectDigits (bytesFor ((1187999.0 + 0.9) / 30.0, "10:59:59:29"),
                          { 0x39, 0x32, 0x20, 0x79, 0x35, 0x79, 0x35, 0x70, 0x31, 0x20 }, "10:59:59:29");
        }

        beginTest ("The Mackie time string names the frame the position is in");
        {
            expectEquals (mcu::timeString (0.999, 30), juce::String ("00000029"), "frame 29 read as the next second");
            expectEquals (mcu::timeString (1.0, 30), juce::String ("00000100"));
            expectEquals (mcu::timeString (1599.0 / kRate, 30), juce::String ("00000000"));
            expectEquals (mcu::timeString (1600.0 / kRate, 30), juce::String ("00000001"), "a boundary sample is its frame");
            int wrong = 0;
            for (juce::int64 frame = 1; frame < 5000; ++frame)
                if (mcu::timeString ((double) (frame * 1600) / kRate, 30).substring (6).getIntValue() != (int) (frame % 30))
                    ++wrong;
            expectEquals (wrong, 0, "a frame boundary read as the frame before it");
        }
    }

    void timecodeLabelsTruncate()
    {
        using timelineexport::samplesToTimecode;

        beginTest ("A timecode label truncates to the frame the sample is in");
        {
            expectEquals (samplesToTimecode (47999, kRate, 30), juce::String ("00:00:00:29"), "frame 29 read as the next second");
            expectEquals (samplesToTimecode (48000, kRate, 30), juce::String ("00:00:01:00"));
            expectEquals (samplesToTimecode (1599, kRate, 30), juce::String ("00:00:00:00"));
            expectEquals (samplesToTimecode (1600, kRate, 30), juce::String ("00:00:00:01"));
            // 44.1 kHz at 24 fps: a frame is 1837.5 samples.
            expectEquals (samplesToTimecode (1837, 44100.0, 24), juce::String ("00:00:00:00"));
            expectEquals (samplesToTimecode (1838, 44100.0, 24), juce::String ("00:00:00:01"));
            expectEquals (samplesToTimecode (-5, kRate, 30), juce::String ("00:00:00:00"), "before the start reads as the start");
            // Still the old answers on exact seconds.
            const juce::int64 s = (juce::int64) ((1 * 3600 + 2 * 60 + 3) * 48000);
            expectEquals (samplesToTimecode (s, kRate, 30), juce::String ("01:02:03:00"));
            expectEquals (samplesToTimecode (s - 1, kRate, 30), juce::String ("01:02:02:29"));
        }

        beginTest ("Every frame boundary starts its frame, at every common rate");
        {
            const std::array<std::pair<juce::int64, int>, 8> rates {{ { 48000, 30 }, { 48000, 25 }, { 48000, 24 },
                                                                      { 44100, 30 }, { 44100, 25 }, { 44100, 24 },
                                                                      { 96000, 30 }, { 88200, 24 } }};
            for (const auto& [rate, fps] : rates)
            {
                int wrong = 0;
                const auto check = [&, r = rate, f = fps] (juce::int64 frame)
                {
                    const auto boundary = (frame * r + f - 1) / f;   // the first sample of `frame`
                    if (samplesToTimecode (boundary, (double) r, f) != label (frame, f)) ++wrong;
                    if (samplesToTimecode (boundary - 1, (double) r, f) != label (frame - 1, f)) ++wrong;
                };
                for (juce::int64 frame = 1; frame < 5000; ++frame) check (frame);
                const juce::int64 tenHours = (juce::int64) fps * 3600 * 10;
                for (juce::int64 frame = tenHours - 2000; frame < tenHours + 2000; ++frame) check (frame);
                expectEquals (wrong, 0, juce::String (rate) + " Hz at " + juce::String (fps) + " fps");
            }
        }
    }

    void speedProbeCancels()
    {
        using namespace preflight;

        beginTest ("A probe pre-flight stopped waiting for is told so; one that answers is not");
        {
            expect (! probeCancelled(), "a thread that is not a probe reads as cancelled");
            expect (! withinTime (2000, [] { return probeCancelled(); }).value_or (true), "an answer in time was cancelled");

            auto sawCancel = std::make_shared<std::atomic<bool>> (false);
            auto finished  = std::make_shared<juce::WaitableEvent> (true);
            const auto answer = withinTime (20, [sawCancel, finished]
            {
                const auto end = juce::Time::getMillisecondCounterHiRes() + 10000.0;
                while (! probeCancelled() && juce::Time::getMillisecondCounterHiRes() < end)
                    juce::Thread::sleep (1);
                sawCancel->store (probeCancelled());
                finished->signal();
                return true;
            });
            expect (! answer.has_value());
            expect (finished->wait (5000), "the probe never stopped");
            expect (sawCancel->load(), "the probe ran on after the wait gave up");
        }

        beginTest ("A speed test that timed out stops writing and leaves no file");
        {
            Folder f;
            const auto dir = f.exports();
            struct State { juce::WaitableEvent done { true }; std::atomic<double> mbps { 0.0 }; };
            auto state = std::make_shared<State>();

            // 512 MB cannot finish in 20 ms: the wait gives up mid-stream.
            const auto measured = withinTime (20, [dir, state]
            {
                state->mbps.store (measureWriteSpeedMBps (dir, 512));
                state->done.signal();
                return state->mbps.load();
            });
            expect (! measured.has_value(), "512 MB finished inside 20 ms");
            expect (state->done.wait (10000), "the cancelled speed test never stopped");
            expect (state->mbps.load() <= 0.0, "a cancelled speed test wrote all of it and reported a speed");
            expectEquals (filesIn (dir, ".zf-preflight-speed*").size(), 0, "the cancelled speed test left its file");

            std::atomic<bool> cancel { true };
            expect (measureWriteSpeedMBps (dir, 4, &cancel) <= 0.0, "a cancelled speed test reported a speed");
            expectEquals (filesIn (dir).size(), 0, "a speed test cancelled before it began wrote a file");

            cancel.store (false);
            expect (measureWriteSpeedMBps (dir, 4, &cancel) > 0.0, "an uncancelled speed test must still measure");
            expectEquals (filesIn (dir).size(), 0);
        }
    }
};

static Fix20261122ExportTests fix20261122ExportTests;
}
