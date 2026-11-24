// Fixes to the skipped-cycle handling of 2026-11-24 (11c3145), from the review
// the same day:
// - CAP-01: StereoMix.wav gets the same silence as the tracks for a skipped
//   cycle, and gaps that come before the mix writer has taken the last one
//   all reach the file.
// - CAP-02 / ENG-03: a skipped cycle is filled only where the block is
//   captured on a free-running take timeline. In a punch that follows
//   playback it is counted and never makes the splice see a hole; outside a
//   punch's captured window it is neither filled nor counted.
// - ENG-02: a buffer JUCE cut into pieces that all carry the same host time is
//   never taken for a skipped cycle.
#include <CoreFoundation/CoreFoundation.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/CycleGapDetector.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/StereoMixWriter.h"

#include <cmath>
#include <functional>
#include <limits>
#include <vector>

namespace zynforge
{
namespace fix20261124capture
{
    constexpr double kRate  = 48000.0;
    constexpr int    kBlock = 256;

    inline std::uint64_t nsFor (juce::int64 frames)
    {
        return (std::uint64_t) std::llround ((double) frames * 1.0e9 / kRate);
    }

    struct Folder
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("zf-fix20261124-capture-" + juce::Uuid().toString());
        Folder()  { dir.createDirectory(); }
        ~Folder() { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); }
        juce::File track (int n = 1) const
        { return dir.getChildFile ("Audio Files/Track_" + juce::String::formatted ("%02d.wav", n)); }
        juce::File mix() const { return dir.getChildFile ("Export Files/StereoMix.wav"); }
        juce::var report() const { return juce::JSON::parse (dir.getChildFile ("session.report.json")); }
    };

    inline bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 10000)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
        return done();
    }

    // The whole take (every part), or the file, or an empty buffer.
    inline juce::AudioBuffer<float> readAll (const juce::File& file)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (ConcatReader::create (fm, findTakeParts (file)));
        if (reader == nullptr) reader.reset (fm.createReaderFor (file));
        if (reader == nullptr) return {};
        juce::AudioBuffer<float> data ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&data, 0, (int) reader->lengthInSamples, 0, true, true);
        return data;
    }

    // One stretch of a file: `length` frames at `level`. 0 is digital silence;
    // NaN is audio at any level, but never silence.
    struct Stretch { juce::int64 length; float level; };

    // The file is exactly these stretches on every channel, sample for sample.
    inline void expectStretches (juce::UnitTest& test, const juce::File& file,
                                 const std::vector<Stretch>& stretches, const juce::String& what)
    {
        const auto data = readAll (file);
        juce::int64 total = 0;
        for (const auto& s : stretches) total += s.length;
        test.expectEquals ((juce::int64) data.getNumSamples(), total, what + ": the file is not as long as expected");
        if (data.getNumSamples() != total || data.getNumChannels() == 0) return;
        for (int c = 0; c < data.getNumChannels(); ++c)
        {
            juce::int64 at = 0;
            for (size_t i = 0; i < stretches.size(); ++i)
            {
                const auto& s = stretches[i];
                const auto* x = data.getReadPointer (c, (int) at);
                int wrong = 0;
                for (juce::int64 n = 0; n < s.length; ++n)
                {
                    const float v = x[n];
                    const bool ok = std::isnan (s.level) ? std::abs (v) > 0.01f
                                  : s.level == 0.0f      ? std::abs (v) < 0.00001f
                                                         : std::abs (v - s.level) < 0.01f;
                    if (! ok) ++wrong;
                }
                test.expectEquals (wrong, 0, what + ": channel " + juce::String (c + 1) + ", stretch "
                                                 + juce::String ((int) i + 1) + " from frame " + juce::String (at)
                                                 + " is not what it should be");
                at += s.length;
            }
        }
    }

    // Feeds the callback as CoreAudio does: every block stamped with its host
    // time, read off a frame count so the stamps never drift. A skipped cycle
    // moves the clock on and delivers nothing.
    struct Clock
    {
        AudioEngine& engine;
        int inputs { 2 };
        juce::int64 frames { 0 };
        juce::AudioBuffer<float> in { 8, kBlock }, out { 2, kBlock };
        static constexpr std::uint64_t kBaseNs = 7'000'000'000ull;

        void blocks (int count, float value)
        {
            for (int c = 0; c < in.getNumChannels(); ++c)
                juce::FloatVectorOperations::fill (in.getWritePointer (c), value, kBlock);
            for (int b = 0; b < count; ++b)
            {
                out.clear();
                const std::uint64_t stamp = kBaseNs + nsFor (frames);
                const juce::AudioIODeviceCallbackContext context { &stamp };
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), inputs,
                                                         out.getArrayOfWritePointers(), 2, kBlock, context);
                frames += kBlock;
            }
        }
        void skip (int cycles = 1) { frames += (juce::int64) cycles * kBlock; }
    };

    // `count` armed strips on inputs 1.., no pre-roll, WAV 24-bit.
    inline void readyStrips (AudioEngine& engine, int count, bool toTheMix = false)
    {
        engine.prepareForTests (kRate, kBlock);
        engine.clearAllStripOverrides();
        engine.setStripCount (count);
        auto& recorder = engine.getRecorder();
        recorder.setPreRollSeconds (0);
        recorder.setCaptureFormat (CaptureFormat::Wav24);
        for (int i = 0; i < count; ++i)
        {
            engine.setTrackInputRouting (i, i);
            auto& t = recorder.getTrack (i);
            t.armed.store (true);
            t.muted.store (false);
            t.soloed.store (false);
            t.gainDb.store (0.0f);
            t.pan.store (0.0f);
            t.isStereo.store (false);
            t.streamSend.store (toTheMix);
        }
    }

    inline void stop (AudioEngine& engine)
    {
        engine.stopRecording();
        pumpUntil ([&] { return ! engine.isCaptureFinalizing(); });
    }

    // A one-strip take of 0.5, `blocks` blocks long: the take a punch goes into.
    inline void recordBase (const juce::File& dir, int blocks)
    {
        AudioEngine engine;
        readyStrips (engine, 1);
        if (! engine.startRecording (dir)) return;
        Clock clock { engine, 1 };
        clock.blocks (blocks, 0.5f);
        stop (engine);
    }

    // The engine on the base take with strip 1 armed and the playhead at `at`.
    inline int readyPunch (AudioEngine& engine, const juce::File& dir, juce::int64 at)
    {
        readyStrips (engine, 1);
        const int loaded = engine.loadSession (dir);
        engine.setTrackInputRouting (0, 0);
        engine.getRecorder().getTrack (0).armed.store (true);
        engine.getPlayer().setPositionSamples (at);
        return loaded;
    }
}

class Fix20261124CaptureTests final : public juce::UnitTest
{
public:
    Fix20261124CaptureTests() : UnitTest ("Fixes 2026-11-24 capture", "zynforge") {}

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        mixWriterQueuesGaps();            // CAP-01
        skippedCycleReachesTheMix();      // CAP-01
        splitBuffersAreNotSkips();        // ENG-02
        splitBuffersInTheEngine();        // ENG-02
        windowedPunchKeepsItsSkip();      // CAP-02
        rollingPunchKeepsItsSkip();       // CAP-02
        skipsOutsideThePunchWindow();     // ENG-03
    }

private:
    using Stretch = fix20261124capture::Stretch;
    static constexpr int kBlock = fix20261124capture::kBlock;
    static constexpr float kAny = std::numeric_limits<float>::quiet_NaN();

    // ── CAP-01 ─────────────────────────────────────────────────────────
    static std::unique_ptr<juce::AudioFormatWriter> wavWriter (const juce::File& file)
    {
        juce::WavAudioFormat wav;
        return std::unique_ptr<juce::AudioFormatWriter> (
            wav.createWriterFor (file.createOutputStream().release(), fix20261124capture::kRate, 2, 24, {}, 0));
    }

    void mixWriterQueuesGaps()
    {
        beginTest ("CAP-01: the mix writer takes every gap queued before it drains, each at its place");
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("zf-fix20261124-mix-" + juce::Uuid().toString() + ".wav");
        std::atomic<bool> failed { false };
        {
            // A thread that never runs: nothing is written until the writer
            // goes, so each gap below waits behind the ones before it.
            juce::TimeSliceThread idle ("mix writer test");
            auto inner = wavWriter (file);
            expect (inner != nullptr);
            if (inner == nullptr) return;
            StereoMixWriter writer (std::move (inner), idle, 4096, failed, {}, 0);
            juce::AudioBuffer<float> block (2, kBlock);
            const auto put = [&] (float value)
            {
                for (int c = 0; c < 2; ++c) juce::FloatVectorOperations::fill (block.getWritePointer (c), value, kBlock);
                const float* chans[2] = { block.getReadPointer (0), block.getReadPointer (1) };
                expect (writer.write (chans, kBlock));
            };
            put (0.25f);
            writer.addGap (100);   // a skipped cycle...
            writer.addGap (50);    // ...and a second gap at the same place before the writer took the first
            put (0.5f);
            writer.addGap (30);
            put (0.75f);
        }
        expect (! failed.load(), "a second gap before the writer drained marked the mix failed");
        fix20261124capture::expectStretches (*this, file,
            { { kBlock, 0.25f }, { 150, 0.0f }, { kBlock, 0.5f }, { 30, 0.0f }, { kBlock, 0.75f } },
            "queued gaps");
        file.deleteFile();

        beginTest ("CAP-01: the mix writer holds many gaps, and says the mix failed only when it has no room");
        std::atomic<bool> full { false };
        {
            juce::TimeSliceThread idle ("mix writer test");
            auto inner = wavWriter (file);
            if (inner == nullptr) return;
            StereoMixWriter writer (std::move (inner), idle, 4096, full, {}, 0);
            int held = 0;
            while (! full.load() && held < 100000)
            {
                writer.addGap (1);
                if (! full.load()) ++held;
            }
            expectGreaterOrEqual (held, 32, "the mix writer holds too few gaps before it drains");
            expect (full.load(), "a gap the queue had no room for was lost without a word");
        }
        file.deleteFile();
    }

    void skippedCycleReachesTheMix()
    {
        beginTest ("CAP-01: a skipped cycle in a live take is the same silence in StereoMix.wav as in every track");
        fix20261124capture::Folder session;
        AudioEngine engine;
        fix20261124capture::readyStrips (engine, 2, true);
        engine.setRecordStereoMix (true);
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        fix20261124capture::Clock clock { engine };
        clock.blocks (20, 0.25f);
        clock.skip();                 // one cycle lost
        clock.blocks (1, 0.5f);
        clock.skip();                 // and another one block later, before the mix writer runs
        clock.blocks (19, 0.5f);
        expectEquals (engine.captureStatus().missedSamples, (juce::int64) (2 * 2 * kBlock),
                      "both skips should be missed on both strips");
        fix20261124capture::stop (engine);
        expect (! engine.hasStereoMixWriteFailed(), "two skips close together marked the mix failed");
        engine.setRecordStereoMix (false);

        const std::vector<Stretch> track { { 20 * kBlock, 0.25f }, { kBlock, 0.0f }, { kBlock, 0.5f },
                                           { kBlock, 0.0f }, { 19 * kBlock, 0.5f } };
        fix20261124capture::expectStretches (*this, session.track (1), track, "Track 1");
        fix20261124capture::expectStretches (*this, session.track (2), track, "Track 2");
        fix20261124capture::expectStretches (*this, session.mix(),
            { { 20 * kBlock, kAny }, { kBlock, 0.0f }, { kBlock, kAny }, { kBlock, 0.0f }, { 19 * kBlock, kAny } },
            "StereoMix.wav");
    }

    // ── ENG-02 ─────────────────────────────────────────────────────────
    // CallbackMaxSizeEnforcer (juce_AudioDeviceManager.cpp) gives every piece
    // of a buffer bigger than the size asked for the same context, so the
    // same host time, when the driver rounds its buffer up.
    static juce::int64 feedSplit (CycleGapDetector& detector, int deviceFrames, int pieceFrames, int cycles,
                                  int skipBefore = -1)
    {
        juce::int64 lost = 0, frames = 0;
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            if (cycle == skipBefore) frames += deviceFrames;
            const std::uint64_t stamp = 3'000'000'000ull + fix20261124capture::nsFor (frames);
            for (int at = 0; at < deviceFrames; at += pieceFrames)
                lost += detector.onBlock (&stamp, juce::jmin (pieceFrames, deviceFrames - at),
                                          fix20261124capture::kRate);
            frames += deviceFrames;
        }
        return lost;
    }

    void splitBuffersAreNotSkips()
    {
        beginTest ("ENG-02: pieces of one buffer that share a host time are never a skipped cycle");
        {
            CycleGapDetector detector;
            expectEquals (feedSplit (detector, 1024, 256, 200), (juce::int64) 0, "1024 cut in four read as lost");
        }
        {
            CycleGapDetector detector;
            expectEquals (feedSplit (detector, 1000, 256, 200), (juce::int64) 0,
                          "1000 cut as 256+256+256+232 read as lost");
        }
        beginTest ("ENG-02: a cycle skipped between split buffers is still counted, once");
        {
            CycleGapDetector detector;
            expectEquals (feedSplit (detector, 1024, 256, 50, 20), (juce::int64) 1024);
        }
    }

    void splitBuffersInTheEngine()
    {
        beginTest ("ENG-02: a take on a driver that rounds its buffer up adds no silence and misses nothing");
        fix20261124capture::Folder session;
        AudioEngine engine;
        fix20261124capture::readyStrips (engine, 2);
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        juce::AudioBuffer<float> in (2, kBlock), out (2, kBlock);
        for (int c = 0; c < 2; ++c) juce::FloatVectorOperations::fill (in.getWritePointer (c), 0.25f, kBlock);
        juce::int64 frames = 0;
        for (int cycle = 0; cycle < 40; ++cycle)
        {
            const std::uint64_t stamp = 9'000'000'000ull + fix20261124capture::nsFor (frames);
            for (int piece = 0; piece < 4; ++piece)
            {
                out.clear();
                const juce::AudioIODeviceCallbackContext context { &stamp };
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2,
                                                         out.getArrayOfWritePointers(), 2, kBlock, context);
            }
            frames += 4 * kBlock;
        }
        expectEquals (engine.captureStatus().missedSamples, (juce::int64) 0, "split buffers were counted as missed");
        fix20261124capture::stop (engine);
        fix20261124capture::expectStretches (*this, session.track (1), { { 160 * kBlock, 0.25f } }, "Track 1");
        expectEquals ((int) session.report().getProperty ("inputCyclesSkipped", -1), 0);
    }

    // ── CAP-02 ─────────────────────────────────────────────────────────
    // The base take is 64 blocks (16384 frames) of 0.5. The selection punch
    // plays from 1024 and captures [4096, 8192): blocks 12 to 27 of the pass.
    static constexpr juce::int64 kFrom = 1024, kIn = 4096, kOut = 8192, kBase = 64 * kBlock;

    void windowedPunchKeepsItsSkip()
    {
        beginTest ("CAP-02: a cycle skipped inside a selection punch is counted, and the punch still lands");
        fix20261124capture::Folder session;
        fix20261124capture::recordBase (session.dir, 64);
        {
            AudioEngine engine;
            expect (fix20261124capture::readyPunch (engine, session.dir, kFrom) > 0, "the base take did not load");
            engine.setCaptureWindow (kIn, kOut);
            engine.armPunchIn (kIn);
            expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
            expect (engine.startPlayback(), "the punch's transport did not roll");
            fix20261124capture::Clock clock { engine, 1 };
            clock.blocks (16, -0.3f);   // the pre-roll, then into the window
            clock.skip();               // the player loses this cycle too
            clock.blocks (20, -0.3f);   // the rest of the window and the post-roll
            expectEquals (engine.getRecorder().getInputCyclesSkipped(), (juce::int64) 1,
                          "a skip inside the captured window must be reported");
            fix20261124capture::stop (engine);
            engine.stopPlayback();
            const auto problems = engine.getRecorder().getCopyProblems().joinIntoString (" | ");
            expect (! engine.getRecorder().hasPunchSpliceFailed(), "the punch was discarded: " + problems);
        }
        fix20261124capture::expectStretches (*this, session.track (1),
            { { kIn, 0.5f }, { kOut - kIn, -0.3f }, { kBase - kOut, 0.5f } }, "punched Track 1");
        expectEquals ((int) session.report().getProperty ("inputCyclesSkipped", -1), 1);
    }

    void rollingPunchKeepsItsSkip()
    {
        beginTest ("CAP-02: a cycle skipped in a punch made with the transport rolling never discards it");
        fix20261124capture::Folder session;
        fix20261124capture::recordBase (session.dir, 64);
        {
            AudioEngine engine;
            expect (fix20261124capture::readyPunch (engine, session.dir, kFrom) > 0, "the base take did not load");
            expect (engine.startPlayback(), "the transport did not roll");
            engine.armPunchIn (kFrom);
            expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
            fix20261124capture::Clock clock { engine, 1 };
            clock.blocks (10, -0.3f);
            clock.skip();
            clock.blocks (22, -0.3f);
            fix20261124capture::stop (engine);
            engine.stopPlayback();
            const auto problems = engine.getRecorder().getCopyProblems().joinIntoString (" | ");
            expect (! engine.getRecorder().hasPunchSpliceFailed(), "the punch was discarded: " + problems);
        }
        fix20261124capture::expectStretches (*this, session.track (1),
            { { kFrom, 0.5f }, { 32 * kBlock, -0.3f }, { kBase - kFrom - 32 * kBlock, 0.5f } }, "punched Track 1");
        expectEquals ((int) session.report().getProperty ("inputCyclesSkipped", -1), 1);
    }

    // ── ENG-03 ─────────────────────────────────────────────────────────
    void skipsOutsideThePunchWindow()
    {
        beginTest ("ENG-03: cycles skipped before the transport rolls, in the pre-roll and after the out point "
                   "are not the punch's: no silence, nothing counted");
        fix20261124capture::Folder session;
        fix20261124capture::recordBase (session.dir, 64);
        {
            AudioEngine engine;
            expect (fix20261124capture::readyPunch (engine, session.dir, kFrom) > 0, "the base take did not load");
            engine.setCaptureWindow (kIn, kOut);
            engine.armPunchIn (kIn);
            expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
            fix20261124capture::Clock clock { engine, 1 };
            clock.blocks (2, -0.3f);    // the take is open, the transport not rolling yet
            clock.skip();
            clock.blocks (2, -0.3f);
            expect (engine.startPlayback(), "the punch's transport did not roll");
            clock.blocks (4, -0.3f);    // pre-roll
            clock.skip();
            clock.blocks (26, -0.3f);   // the rest of the pre-roll, the window, into the post-roll
            clock.skip();               // after the out point
            clock.blocks (4, -0.3f);
            expectEquals (engine.captureStatus().missedSamples, (juce::int64) 0,
                          "a skip outside the punch was counted against it");
            expectEquals (engine.getRecorder().getInputCyclesSkipped(), (juce::int64) 0);
            fix20261124capture::stop (engine);
            engine.stopPlayback();
            const auto problems = engine.getRecorder().getCopyProblems().joinIntoString (" | ");
            expect (! engine.getRecorder().hasPunchSpliceFailed(), "the punch was discarded: " + problems);
        }
        fix20261124capture::expectStretches (*this, session.track (1),
            { { kIn, 0.5f }, { kOut - kIn, -0.3f }, { kBase - kOut, 0.5f } }, "punched Track 1");
        expectEquals ((int) session.report().getProperty ("inputCyclesSkipped", -1), 0);
    }
};

static Fix20261124CaptureTests fix20261124CaptureTests;
}
