#include "../Audio/FloatAiffWriter.h"
#include "../Audio/TrackExporter.h"
#include "../Audio/PunchSplice.h"

namespace zynforge
{
class ReviewFloatFinalizeTests final : public juce::UnitTest
{
public:
    ReviewFloatFinalizeTests() : UnitTest ("Review float finalization", "zynforge") {}

    static bool writeFixture (const juce::File& file, bool aiff)
    {
        auto output = file.createOutputStream();
        if (output == nullptr) return false;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (aiff
            ? FloatAiffWriter::create (output.get(), 48000.0, 1)
            : wav.createWriterFor (output.get(), 48000.0, 1, 32, {}, 0));
        if (writer == nullptr) return false;
        output.release();
        std::array<float, 256> samples;
        samples.fill (1.25f);
        const float* input[] { samples.data() };
        return writer->writeFromFloatArrays (input, 1, (int) samples.size()) && writer->flush();
    }

    void runTest() override
    {
        beginTest ("Finalization fixtures use disposable files");
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-review-float-finalize-" + juce::Uuid().toString());
        expect (root.createDirectory().wasOk());
        const auto cleanup = juce::ScopeGuard { [&] {
            FloatAiffWriter::onCreateForTests = {};
            root.deleteRecursively();
        } };
        const auto source = root.getChildFile ("source.wav");
        const auto base = root.getChildFile ("base.aif");
        expect (writeFixture (source, false));
        expect (writeFixture (base, true));

        for (int mode = 0; mode < 3; ++mode)
        {
            beginTest (mode == 0 ? "AIFF export refuses final flush failure and preserves destination"
                : mode == 1 ? "Resampled AIFF export refuses final flush failure and preserves destination"
                            : "Stereo AIFF export refuses final flush failure and preserves destination");
            const auto stem = root.getChildFile ("export-" + juce::String (mode));
            const auto destination = stem.withFileExtension (".aif");
            const juce::String previous = "previous destination must remain byte-for-byte intact";
            expect (destination.replaceWithText (previous));
            int creations = 0;
            FloatAiffWriter::onCreateForTests = [&] (FloatAiffWriter& writer) {
                ++creations;
                writer.failFlushForTests = true;
            };
            const auto clearHook = juce::ScopeGuard { [] { FloatAiffWriter::onCreateForTests = {}; } };
            ExportOptions options;
            options.format = ExportFormat::Aiff24;
            options.bitsPerSample = 32;
            options.sampleRate = mode == 1 ? 44100.0 : 48000.0;
            TrackExporter exporter;
            juce::String error;
            const bool ok = mode == 2
                ? exporter.exportStereoPair (source, source, stem, options, error)
                : exporter.exportTrack (source, stem, options, error);
            expectEquals (creations, 1, "Fault must reach the actual float AIFF writer");
            expect (! ok, "A final header/flush failure must not publish success");
            expect (error.isNotEmpty(), "A failed finalization must explain the refusal");
            expectEquals (destination.loadFileAsString(), previous);
        }

        beginTest ("AIFF punch refuses final flush failure and removes unpublished output");
        juce::MemoryBlock originalBase, originalInsert;
        expect (base.loadFileAsData (originalBase));
        expect (source.loadFileAsData (originalInsert));
        int creations = 0;
        FloatAiffWriter::onCreateForTests = [&] (FloatAiffWriter& writer) {
            ++creations;
            writer.failFlushForTests = true;
        };
        const auto clearHook = juce::ScopeGuard { [] { FloatAiffWriter::onCreateForTests = {}; } };
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const auto punched = root.getChildFile ("punched.aif");
        expect (! splicePunchFile (formats, base, source, 64, punched),
                "A failed final header cannot be reported as a successful punch");
        expectEquals (creations, 1, "Fault must reach the punch writer");
        expect (! punched.existsAsFile(), "Partial punch output must be removed");
        juce::MemoryBlock afterBase, afterInsert;
        expect (base.loadFileAsData (afterBase));
        expect (source.loadFileAsData (afterInsert));
        expect (afterBase == originalBase, "Original take must remain intact");
        expect (afterInsert == originalInsert, "Punch insert must remain intact");
    }
};
static ReviewFloatFinalizeTests reviewFloatFinalizeTests;
}
