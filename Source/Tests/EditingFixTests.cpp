// Regression guards for the 2026-10-29 clip-editing / takes / offline-render
// bug hunt. Each test reproduces one finding headlessly through the engine.

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/NoiseAnalyzer.h"

namespace zynforge
{
class EditingFixTests final : public juce::UnitTest
{
public:
    EditingFixTests() : UnitTest ("Editing fixes 2026-10-29", "zynforge") {}

    struct Folder
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-editing-" + juce::Uuid().toString());
        Folder() { dir.createDirectory(); }
        ~Folder() { dir.deleteRecursively(); }
        juce::File audio (const juce::String& name) const
        { return dir.getChildFile ("Audio Files").getChildFile (name); }
        juce::File track (int n = 1) const
        { return audio ("Track_" + juce::String::formatted ("%02d.wav", n)); }
    };

    // Mono 24-bit WAV holding a constant 0.25.
    static bool write (const juce::File& file, int samples = 48000, float value = 0.25f)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
            file.createOutputStream().release(), 48000.0, 1, 24, {}, 0));
        if (! writer) return false;
        juce::AudioBuffer<float> audio (1, samples);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), value, samples);
        return writer->writeFromAudioSampleBuffer (audio, 0, samples);
    }

    static void setup (AudioEngine& e, int tracks = 1)
    {
        e.clearAllStripOverrides(); e.setStripCount (tracks); e.prepareForTests (48000, 256);
    }

    // Render the edited arrangement and read one sample of it.
    float renderedAt (AudioEngine& e, int track, juce::int64 total, int sample)
    {
        juce::AudioBuffer<float> out;
        expect (e.renderTrackArrangement (track, out, total), "render failed");
        return out.getNumSamples() > sample ? out.getSample (0, sample) : -1.0f;
    }

    // Arrangement for the range tests: the default clip A = [0, 48000) plus a
    // pasted clip B = [12000, 36000) overlapping it (same file, file [0, 24000)).
    static void overlapArrangement (AudioEngine& e, const Folder& f)
    {
        setup (e); e.loadSession (f.dir);
        e.pasteClip (0, 12000, 0, 24000, 0, 0, 0.0f, "B", juce::File(), -1);
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);

        beginTest ("Clear range cuts every overlapping clip, not just the first");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; overlapArrangement (e, f);
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 20000), 0.5f, 0.001f, "A + B before");
            expect (e.clearTrackRange (0, 18000, 24000));
            for (const auto& c : e.clipsFor (0))
                expect (c.timelineStartSamples >= 24000
                        || c.timelineStartSamples + c.fileLengthSamples <= 18000,
                        "a clip still crosses the cleared range: " + c.name);
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 20000), 0.0f, 0.001f,
                                       "the overlapping clip still plays inside the cleared range");
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 30000), 0.5f, 0.001f);
        }

        beginTest ("Ripple delete shifts the overlapping clip's tail with everything else");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; overlapArrangement (e, f);
            expect (e.rippleDeleteRange (0, 18000, 24000));
            // A's tail now covers [18000, 42000); B's tail [18000, 30000).
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 20000), 0.5f, 0.001f);
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 32000), 0.25f, 0.001f,
                                       "B was left unshifted on top of the clips that slid left");
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 43000), 0.0f, 0.001f);
        }

        beginTest ("Consolidate over overlapping clips plays the range once, not doubled");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; overlapArrangement (e, f);
            expect (e.consolidateRange (0, 18000, 24000));
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 20000), 0.5f, 0.001f,
                                       "B's surviving piece doubles the consolidated render");
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 30000), 0.5f, 0.001f);
        }

        beginTest ("Comp from a take replaces every overlapping clip in the range");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            expectEquals (e.newTakeFromCurrent (0, "Take 2"), 1);
            e.pasteClip (0, 12000, 0, 24000, 0, 0, 0.0f, "B", juce::File(), -1);
            expect (e.compRangeFromTake (0, 0, 18000, 24000));
            expectWithinAbsoluteError (renderedAt (e, 0, 48000, 20000), 0.25f, 0.001f,
                                       "the comped range must hold Take 1's audio only");
        }

        beginTest ("Undo of a paste onto a never-recorded strip removes the pasted clip");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e, 4); e.loadSession (f.dir);
            const auto before = e.playlistsToJson();       // strip 4 has no Track_04 and no playlist
            expect (e.pasteClip (3, 0, 0, 4800, 0, 0, 0.0f, "copied", f.track(), 0) >= 0);
            const auto after = e.playlistsToJson();
            expectEquals (e.getTakeCount (3), 1);

            e.loadPlaylistsFromJson (before);                // ClipSnapshotAction::undo
            expect (e.tryClipsFor (3) == nullptr, "the pasted clip survived the undo");
            expectEquals (e.getTakeCount (3), 0);
            expectWithinAbsoluteError (renderedAt (e, 3, 4800, 1000), 0.0f, 0.00001f,
                                       "the undone paste still renders");

            e.loadPlaylistsFromJson (after);                 // redo
            expect (e.tryClipsFor (3) != nullptr && e.tryClipsFor (3)->size() == 1);
            expectWithinAbsoluteError (renderedAt (e, 3, 4800, 1000), 0.25f, 0.001f);

            // Same with the strip present in the snapshot as an empty playlist.
            e.loadPlaylistsFromJson (before);
            const auto emptyEntry = e.playlistsToJson();
            e.pasteClip (3, 0, 0, 4800, 0, 0, 0.0f, "copied", f.track(), 0);
            e.loadPlaylistsFromJson (emptyEntry);
            expect (e.tryClipsFor (3) == nullptr, "an empty restored playlist kept the clip");
        }

        beginTest ("Restoring an older clip snapshot keeps a take recorded since");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            const auto before = e.playlistsToJson();       // knows only Track_01
            expect (write (f.track (2)));
            e.loadSession (f.dir, true, true);               // the take stop reload
            expectEquals ((int) e.clipsFor (1).size(), 1);
            e.loadPlaylistsFromJson (before);
            expectEquals ((int) e.clipsFor (1).size(), 1, "undo wiped a newer recording");
            expectWithinAbsoluteError (renderedAt (e, 1, 48000, 1000), 0.25f, 0.001f);
        }

        beginTest ("Clips on strips above the highest Track_NN keep playing after a reload");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e, 8); e.loadSession (f.dir);
            expect (e.pasteClip (5, 1000, 0, 4000, 0, 0, 0.0f, "copied", f.track(), 0) >= 0);
            e.loadSession (f.dir, true, true);               // stop-record / daemon STOP reload
            expectEquals ((int) e.clipsFor (5).size(), 1);
            expect (e.getPlayer().getNumTracks() >= 6,
                    "the player no longer reaches strip 6 after the reload");
            expect (playerPeak (e, 8, 5, 2000) > 0.1f, "strip 6 is silent in playback");
        }

        beginTest ("Recording onto a strip with no prior media joins its edited arrangement");
        {
            Folder f; expect (write (f.track()));
            AudioEngine e; setup (e, 3); e.loadSession (f.dir);
            expect (e.pasteClip (2, 0, 0, 4800, 0, 0, 0.0f, "copied", f.track(), 0) >= 0);
            expect (write (f.track (3), 9600, 0.5f));        // the new take's media
            e.loadSession (f.dir, true, true);
            bool ownAudio = false;
            for (const auto& c : e.clipsFor (2))
                ownAudio |= c.audioFile == juce::File() && c.fileLengthSamples == 9600;
            expect (ownAudio, "the recorded audio is missing from the arrangement");
            expectWithinAbsoluteError (renderedAt (e, 2, 9600, 6000), 0.5f, 0.001f);
        }

        beginTest ("Trimming a clip shorter than its fades shrinks the fades to fit");
        {
            Folder f; expect (write (f.track(), 4800));
            for (const auto mode : { AudioEngine::ClipEdit::TrimRight, AudioEngine::ClipEdit::TrimLeft })
            {
                AudioEngine e; setup (e); e.loadSession (f.dir);
                expect (e.setClipFades (0, 0, 2000, 2000));
                const auto delta = mode == AudioEngine::ClipEdit::TrimRight ? -3000 : 3000;
                expect (e.editClip (0, 0, mode, delta));
                const auto& c = e.clipsFor (0)[0];
                expectEquals (c.fileLengthSamples, (juce::int64) 1800);
                expect (c.fadeInSamples + c.fadeOutSamples <= c.fileLengthSamples,
                        "fades are longer than the trimmed clip");
                expect (c.fadeInSamples > 0 && c.fadeOutSamples > 0, "a fade was dropped");
                // The fade handles must still work.
                expect (e.setClipFades (0, 0, c.fadeInSamples, c.fadeOutSamples));
            }
        }

        beginTest ("Strip Silence leaves muted clips in place");
        {
            Folder f; expect (write (f.track(), 9600));
            AudioEngine e; setup (e); e.loadSession (f.dir);
            expect (e.splitTrackAtSample (0, 4800));
            e.setClipMuted (0, 1, true);
            e.stripSilence (0, -40.0f, 480, 480, 0);
            int muted = 0;
            for (const auto& c : e.clipsFor (0))
                if (c.muted) { ++muted; expectEquals (c.timelineStartSamples, (juce::int64) 4800); }
            expectEquals (muted, 1, "Strip Silence deleted the muted clip");
        }

        beginTest ("Analysis scans ignore consolidate renders and punch sidecars");
        {
            Folder f; expect (write (f.track()));
            expect (write (f.audio ("Track_01_consolidated_1.wav"), 4800));
            expect (write (f.audio ("Track_01.punchbase.wav"), 4800));
            const auto findings = NoiseAnalyzer::analyseSession (f.dir, nullptr);
            expectEquals ((int) findings.size(), 1, "a sidecar became a phantom noise-report row");

            // Track 2 was never recorded; consolidating a clip pasted onto it
            // leaves only Track_02_consolidated_1.wav, whose clicks must not
            // become track 2's transients.
            expect (writeClicks (f.audio ("Track_02_consolidated_1.wav")));
            AudioEngine e; setup (e, 2); e.loadSession (f.dir);
            expect (e.getTransientsForTrack (2).empty(),
                    "a consolidate render's onsets were filed under track 2");
        }

        beginTest ("Tab lands on transients where the edited arrangement plays them");
        {
            Folder f; expect (writeClicks (f.track()));     // bursts at file 24000 / 48000 / 72000
            AudioEngine e; setup (e); e.loadSession (f.dir);
            expect (e.rippleDeleteRange (0, 0, 12000));     // the take slides 12000 earlier
            for (const int restrict : { 1, -1 })
            {
                const auto next = e.nextTransientSample (0, restrict);
                expect (std::abs (next - 12000) < 600,
                        "Tab used the file position, not the timeline: " + juce::String (next));
            }
            expect (e.clearTrackRange (0, 30000, 40000));   // deletes the burst now at 36000
            const auto afterGap = e.nextTransientSample (13000, 1);
            expect (std::abs (afterGap - 60000) < 600,
                    "Tab stopped on audio that was deleted: " + juce::String (afterGap));
            const auto back = e.prevTransientSample (59000, 1);
            expect (std::abs (back - 12000) < 600, "Shift+Tab: " + juce::String (back));
        }
    }

    // Peak of one player output channel at `pos`, polling while the
    // background readers fill.
    static float playerPeak (AudioEngine& e, int channels, int channel, juce::int64 pos)
    {
        auto& player = e.getPlayer();
        juce::AudioBuffer<float> out (channels, 512);
        for (int attempt = 0; attempt < 120; ++attempt)
        {
            out.clear();
            player.setPositionSamples (pos);
            player.start();
            player.processBlock (out.getArrayOfWritePointers(), channels, 512);
            player.stop();
            const auto peak = out.getMagnitude (channel, 0, 512);
            if (peak > 0.1f) return peak;
            juce::Thread::sleep (5);
        }
        return 0.0f;
    }

    // Silence with three loud bursts, 48 kHz mono.
    static bool writeClicks (const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
            file.createOutputStream().release(), 48000.0, 1, 24, {}, 0));
        if (! writer) return false;
        juce::AudioBuffer<float> audio (1, 96000);
        audio.clear();
        for (const int at : { 24000, 48000, 72000 })
            for (int i = 0; i < 2400; ++i)
                audio.setSample (0, at + i, (i % 2 == 0) ? 0.6f : -0.6f);
        return writer->writeFromAudioSampleBuffer (audio, 0, 96000);
    }
};

static EditingFixTests editingFixTests;
}
