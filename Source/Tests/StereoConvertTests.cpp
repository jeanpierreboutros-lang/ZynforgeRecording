// Right-click > Convert to Stereo (2026-11-22): a mono strip becomes a stereo
// strip in place. A right half is inserted straight after it, so the next
// strip keeps its channel (Link to next channel takes it), and the strips after
// keep their takes and the input they hear. A strip with audio stays mono.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"

#include <functional>

namespace zynforge
{
class StereoConvertTests final : public juce::UnitTest
{
public:
    StereoConvertTests() : juce::UnitTest ("Convert to stereo", "zynforge") {}

    static constexpr double kRate = 48000.0;
    using Row = EditPage::TrackRow;

    struct Session
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("zf-stereoconvert-" + juce::Uuid().toString());
        Session()  { dir.getChildFile ("Audio Files").createDirectory(); }
        ~Session() { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); }
        juce::File take (int n) const
        { return dir.getChildFile ("Audio Files/" + juce::String::formatted ("Track_%02d.wav", n)); }
    };

    static bool writeTake (const juce::File& file, float value, int samples = 4800)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
            file.createOutputStream().release(), kRate, 1, 24, {}, 0));
        if (writer == nullptr) return false;
        juce::AudioBuffer<float> audio (1, samples);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), value, samples);
        return writer->writeFromAudioSampleBuffer (audio, 0, samples);
    }

    static float firstSample (const juce::File& file)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));
        if (reader == nullptr) return -1.0f;
        juce::AudioBuffer<float> one (1, 1);
        reader->read (&one, 0, 1, 0, true, false);
        return one.getSample (0, 0);
    }

    static void openStrips (AudioEngine& e, const juce::StringArray& names)
    {
        e.prepareForTests (kRate, 256);
        e.clearAllStripOverrides();
        e.setStripCount (names.size());
        for (int i = 0; i < names.size(); ++i) e.setTrackName (i, names[i]);
    }

    static void closeEngine (AudioEngine& e)
    {
        e.clearSessionState();
        e.setStripCount (0);
        e.clearAllStripOverrides();
    }

    static TrackState& track (AudioEngine& e, int i) { return e.getRecorder().getTrack (i); }
    static juce::String nameOf (AudioEngine& e, int i) { return track (e, i).getNameThreadSafe(); }
    static int heardInput (AudioEngine& e, int i)
    {
        return listenedInput (i, track (e, i).inputRouting.load(), e.getCurrentDeviceInputCount());
    }

    static juce::StringArray itemsOf (const juce::PopupMenu& menu)
    {
        juce::StringArray items;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            items.add (it.getItem().text);
        return items;
    }

    static std::vector<Row*> rowsOf (EditPage& page)
    {
        std::vector<Row*> rows;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            for (auto* child : c.getChildren())
            {
                if (auto* row = dynamic_cast<Row*> (child)) rows.push_back (row);
                walk (*child);
            }
        };
        walk (page);
        std::sort (rows.begin(), rows.end(), [] (auto* a, auto* b) { return a->getY() < b->getY(); });
        return rows;
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        lastStripConvertsInPlace();
        middleStripKeepsTheOthers();
        refusalsChangeNothing();
        editRowOffersIt();
    }

private:
    void lastStripConvertsInPlace()
    {
        beginTest ("The last strip becomes a stereo strip in place: same name, a right half after it");
        AudioEngine e;
        openStrips (e, { "Kick", "Snare", "Keys" });
        e.setTrackColour (2, juce::Colour (0xff3366aa));
        e.setTrackGainDb (2, -6.0f);
        track (e, 2).armed.store (true);
        const auto id = track (e, 2).stripId;
        expect (e.stereoConversionRefusal (2).isEmpty(), e.stereoConversionRefusal (2));

        juce::String why;
        expect (e.convertToStereo (2, &why), why);
        expectEquals (e.getRecorder().getNumTracks(), 4);
        expect (track (e, 2).isStereo.load(), "the strip is not stereo");
        expect (! track (e, 3).isStereo.load(), "the right half is marked as a left half");
        expectEquals (nameOf (e, 2), juce::String ("Keys"));
        expectEquals (nameOf (e, 3), juce::String ("Keys R"));
        expectEquals (track (e, 2).stripId, id, "the strip lost its identity, so its cues");
        expectEquals ((juce::int64) track (e, 3).colourARGB.load(), (juce::int64) track (e, 2).colourARGB.load(),
                      "the right half has another colour");
        expectWithinAbsoluteError (track (e, 3).gainDb.load(), -6.0f, 0.001f, "the right half's fader is not the strip's");
        expect (track (e, 3).armed.load(), "the right half is not armed with its left half");
        expectWithinAbsoluteError (track (e, 2).pan.load(), -1.0f, 0.001f);
        expectWithinAbsoluteError (track (e, 3).pan.load(),  1.0f, 0.001f);
        expectEquals (heardInput (e, 3), heardInput (e, 2) + 1, "the right half does not hear the input after its left half");
        expectEquals (nameOf (e, 0), juce::String ("Kick"));
        expectEquals (nameOf (e, 1), juce::String ("Snare"));
        closeEngine (e);
    }

    void middleStripKeepsTheOthers()
    {
        beginTest ("A middle strip converts without taking the next strip, its take or its input");
        Session s;
        AudioEngine e;
        openStrips (e, { "Kick", "Keys", "Bass", "Gtr" });
        e.setTrackInputRouting (3, 9);   // Gtr patched by hand to In 10
        e.setActiveSessionDir (s.dir);
        e.claimMixerFor (s.dir);
        e.saveSessionMixTo (s.dir);
        expect (writeTake (s.take (1), 0.25f), "fixture: Kick's take");
        expect (writeTake (s.take (3), 0.5f),  "fixture: Bass's take");
        const int bassHeard = heardInput (e, 2);
        const auto bassId = track (e, 2).stripId;

        juce::String why;
        expect (e.convertToStereo (1, &why), why);
        expectEquals (e.getRecorder().getNumTracks(), 5);
        expect (track (e, 1).isStereo.load());
        expectEquals (nameOf (e, 1), juce::String ("Keys"));
        expectEquals (nameOf (e, 2), juce::String ("Keys R"));
        expectEquals (nameOf (e, 3), juce::String ("Bass"), "the next strip was taken or lost");
        expectEquals (nameOf (e, 4), juce::String ("Gtr"));
        expectEquals (track (e, 3).stripId, bassId);
        expect (! track (e, 3).isStereo.load(), "the next strip became part of the pair");

        // Bass's take moved with it; Kick's stayed.
        expectWithinAbsoluteError (firstSample (e.getTrackAudioFile (3)), 0.5f, 0.001f, "Bass is not reading its own take");
        expectWithinAbsoluteError (firstSample (e.getTrackAudioFile (0)), 0.25f, 0.001f, "Kick lost its take");
        expect (! e.getTrackAudioFile (1).existsAsFile(), "the converted strip found someone's take");

        // Nobody else was repatched: the default patch follows the position, so
        // Bass is pinned to what it heard, and Gtr keeps its hand patch.
        expectEquals (heardInput (e, 3), bassHeard, "making Keys stereo moved Bass to another input");
        expectEquals (heardInput (e, 4), 9, "Gtr lost its patch");
        expectEquals (heardInput (e, 2), heardInput (e, 1) + 1);
        closeEngine (e);
    }

    void refusalsChangeNothing()
    {
        beginTest ("A strip with audio, a stereo strip and a right half are refused, with nothing changed");
        {
            Session s;
            AudioEngine e;
            openStrips (e, { "Kick", "Keys" });
            e.setActiveSessionDir (s.dir);
            e.claimMixerFor (s.dir);
            e.saveSessionMixTo (s.dir);
            expect (writeTake (s.take (2), 0.5f), "fixture: Keys's take");

            juce::String why;
            expect (! e.convertToStereo (1, &why), "a recorded mono strip turned stereo; RECORD would refuse it");
            expect (why.containsIgnoreCase ("audio"), "no reason: " + why);
            expectEquals (e.getRecorder().getNumTracks(), 2);
            expect (! track (e, 1).isStereo.load());
            closeEngine (e);
        }
        {
            AudioEngine e;
            openStrips (e, { "Pad", "Pad R", "Vox" });
            expect (e.linkStereoPair (0, true));
            juce::String why;
            expect (! e.convertToStereo (0, &why), "a stereo strip was converted again");
            expect (why.isNotEmpty());
            expect (! e.convertToStereo (1, &why), "a right half was converted");
            expect (e.stereoConversionRefusal (5).isNotEmpty(), "a strip that does not exist was accepted");
            expectEquals (e.getRecorder().getNumTracks(), 3);
            expect (e.convertToStereo (2, &why), why);   // the mono strip after the pair still can
            expectEquals (e.getRecorder().getNumTracks(), 4);
            expect (track (e, 0).isStereo.load() && track (e, 2).isStereo.load());
            closeEngine (e);
        }
    }

    void editRowOffersIt()
    {
        beginTest ("EDIT's row header offers Convert to Stereo on a mono row only, and hands the host the strip");
        AudioEngine e;
        openStrips (e, { "Kick", "Pad", "Pad R" });
        expect (e.linkStereoPair (1, true));
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        juce::AudioThumbnailCache cache { 8 };

        Row mono (0, false, e, formats, cache);
        Row pair (1, true, e, formats, cache);
        expect (itemsOf (mono.rowMenu (true)).contains ("Convert to Stereo"), "the mono header lacks it");
        expect (! itemsOf (mono.rowMenu (false)).contains ("Convert to Stereo"), "a stray lane click offers it");
        expect (! itemsOf (pair.rowMenu (true)).contains ("Convert to Stereo"), "a stereo row offers it");

        EditPage page (e);
        page.setBounds (0, 0, 1400, 500);
        page.refresh();
        int asked = -1;
        page.onRowConvertToStereo = [&asked] (int physTrack) { asked = physTrack; };
        const auto rows = rowsOf (page);
        expect (! rows.empty(), "setup: EDIT built no rows");
        if (! rows.empty())
        {
            rows.front()->applyRowMenuChoice (Row::kMenuConvertToStereo);
            expectEquals (asked, 0, "the host was not asked to convert the row's strip");
        }
        closeEngine (e);
    }
};

static StereoConvertTests stereoConvertTests;
}
