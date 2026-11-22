// Right-click > Convert to Stereo (2026-11-22): a mono strip becomes a stereo
// strip in place. A right half is inserted straight after it, so the next
// strip keeps its channel (Link to next channel takes it) and its takes; the
// later strips on the inputs and outputs after it move one on to make room, up
// to the first gap. A strip with audio stays mono.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"
#include "../UI/StereoConvertCues.h"

#include <functional>
#include <set>

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

    static bool writeTake (const juce::File& file, float value, int samples = 4800, int channels = 1)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
            file.createOutputStream().release(), kRate, (unsigned int) channels, 24, {}, 0));
        if (writer == nullptr) return false;
        juce::AudioBuffer<float> audio (channels, samples);
        for (int c = 0; c < channels; ++c)
            juce::FloatVectorOperations::fill (audio.getWritePointer (c), value, samples);
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
        laterStripsMakeRoom();
        everyStripMakesRoom();
        stereoTakeOnTheLastStrip();
        strayTakeRefused();
        cuesFollowTheConversion();
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

        // Keys R took In 3, so Bass, on the input straight after, moved one on.
        // Gtr's hand patch (In 10) is past the gap and stays.
        expectEquals (heardInput (e, 2), heardInput (e, 1) + 1);
        expectEquals (heardInput (e, 3), bassHeard + 1, "Bass was left on the input Keys R now takes");
        expectEquals (heardInput (e, 4), 9, "Gtr, past the gap, was repatched");
        closeEngine (e);
    }

    void laterStripsMakeRoom()
    {
        beginTest ("The strips on the inputs and outputs after it move one on, up to the first gap");
        AudioEngine e;
        // A..D patched In/Out 1..4, E by hand on In/Out 11, F on its default (In 6, master).
        openStrips (e, { "A", "B", "C", "D", "E", "F" });
        for (int i = 0; i < 4; ++i) e.setTrackLinkedRouting (i, i);
        e.setTrackLinkedRouting (4, 10);
        // In soundcheck each strip's show output is saved: B's on Out 2, C's on Out 3.
        track (e, 1).outputBeforeVsc.store (1);
        track (e, 2).outputBeforeVsc.store (2);
        AudioEngine::StereoConversion moved;
        juce::String why;
        expect (e.convertToStereo (1, &why, &moved), why);
        expectEquals (e.getRecorder().getNumTracks(), 7);
        // A, B (L In 2), B R (In 3), C, D, E, F
        expectEquals (nameOf (e, 3), juce::String ("C"));
        expectEquals (heardInput (e, 1), 1);
        expectEquals (heardInput (e, 2), 2, "the right half is not on the input after its left half");
        expectEquals (track (e, 2).outputRouting.load(), 2, "the right half is not on the output after its left half");
        expectEquals (heardInput (e, 3), 3, "C stayed on the input B R now takes");
        expectEquals (heardInput (e, 4), 4, "D stayed on the input C now takes");
        expectEquals (track (e, 3).outputRouting.load(), 3, "C's output did not move on");
        expectEquals (track (e, 4).outputRouting.load(), 4, "D's output did not move on");
        expectEquals (track (e, 3).outputBeforeVsc.load(), 3, "C's saved show output did not move with it");
        expectEquals (heardInput (e, 5), 10, "E, past the gap, was repatched");
        expectEquals (track (e, 5).outputRouting.load(), 10);
        expectEquals (heardInput (e, 6), 5, "F, past the gap, followed its new position to another input");
        expect (track (e, 6).outputRouting.load() < 0, "F left the master");
        expectEquals (track (e, 0).outputRouting.load(), 0, "an earlier strip moved");
        expectEquals (moved.inputsMoved, 2);
        expectEquals (moved.outputsMoved, 2);
        expectEquals (moved.bothMoved, 2);
        expectEquals (moved.inputsFrom, 2);
        expectEquals (moved.inputsTo, 3);
        closeEngine (e);
    }

    // Audit 2026-11-22 H4: the run searched only later strips, so a strip
    // dragged to the front kept a channel a moved strip then landed on.
    void everyStripMakesRoom()
    {
        beginTest ("Strips anywhere in the order make room, so no two share a channel after it");
        {
            AudioEngine e;
            // Vox dragged to the front on In 4; A In 1, T1 In 2, T2 In 3, OH In 5.
            openStrips (e, { "Vox", "A", "T1", "T2", "OH" });
            const int patch[] = { 3, 0, 1, 2, 4 };
            for (int i = 0; i < 5; ++i) e.setTrackLinkedRouting (i, patch[i]);
            juce::String why;
            expect (e.convertToStereo (2, &why), why);
            // Vox, A, T1 (In 2), T1 R (In 3), T2, OH
            expectEquals (heardInput (e, 3), 2);
            expectEquals (heardInput (e, 4), 3, "T2 did not move off the input T1 R took");
            expectEquals (heardInput (e, 0), 4, "Vox, earlier in the order, kept the input T2 now takes");
            expectEquals (heardInput (e, 5), 5, "OH stayed on the input Vox now takes");
            std::set<int> inputs, outputs;
            for (int i = 0; i < e.getRecorder().getNumTracks(); ++i)
            {
                inputs.insert (heardInput (e, i));
                outputs.insert (track (e, i).outputRouting.load());
            }
            expectEquals ((int) inputs.size(), 6, "two strips share an input after the conversion");
            expectEquals ((int) outputs.size(), 6, "two strips share an output after the conversion");
            closeEngine (e);
        }
        {
            // The last strip: an earlier strip on the channel after it moves.
            AudioEngine e;
            openStrips (e, { "A", "B" });
            e.setTrackLinkedRouting (0, 1);
            e.setTrackLinkedRouting (1, 0);
            juce::String why;
            expect (e.convertToStereo (1, &why), why);
            expectEquals (heardInput (e, 2), 1, "the right half is not on the input after its left half");
            expectEquals (heardInput (e, 0), 2, "A kept the input the right half took");
            closeEngine (e);
        }
    }

    // Audit 2026-11-22 L4: with a stereo take on the last strip, the right
    // half appended for a moment after it read as that take's right half, and
    // the move was refused with a reason naming neither strip.
    void stereoTakeOnTheLastStrip()
    {
        beginTest ("A stereo take on the last strip does not stop a middle strip converting");
        Session s;
        AudioEngine e;
        openStrips (e, { "Keys", "Bass", "Pad" });
        e.setActiveSessionDir (s.dir);
        e.claimMixerFor (s.dir);
        e.saveSessionMixTo (s.dir);
        expect (writeTake (s.take (3), 0.5f, 4800, 2), "fixture: Pad's stereo take");
        juce::String why;
        expect (e.convertToStereo (0, &why), why);
        expectEquals (e.getRecorder().getNumTracks(), 4);
        expectEquals (nameOf (e, 3), juce::String ("Pad"));
        int channel = -1;
        const auto padTake = e.getTrackAudioFile (3, &channel);
        expectWithinAbsoluteError (firstSample (padTake), 0.5f, 0.001f, "Pad lost its take");
        expectEquals (channel, 0);
        closeEngine (e);
    }

    void strayTakeRefused()
    {
        beginTest ("A stray take numbered one past the last strip is refused, not adopted");
        Session s;
        AudioEngine e;
        openStrips (e, { "Keys", "Bass" });
        e.setActiveSessionDir (s.dir);
        e.claimMixerFor (s.dir);
        e.saveSessionMixTo (s.dir);
        expect (writeTake (s.take (3), 0.5f), "fixture: a stray Track_03");
        juce::String why;
        expect (! e.convertToStereo (0, &why), "the right half would have played a stray take");
        expect (why.containsIgnoreCase ("stray"), "no reason: " + why);
        expectEquals (e.getRecorder().getNumTracks(), 2);
        closeEngine (e);
    }

    // Audit 2026-11-22 H5: cues saved before a conversion move the same way.
    void cuesFollowTheConversion()
    {
        beginTest ("Cues carry the room the conversion made, and the pair's image");
        AudioEngine::StereoConversion m;
        m.inputsFrom = 2;  m.inputsTo = 3;
        m.outputsFrom = 2; m.outputsTo = 2;
        m.leftId = "L";    m.rightId = "R";
        SetlistBar::Cue cue;
        const auto snap = [] (juce::String id, int in, int out)
        {
            SetlistBar::StripSnapshot s;
            s.stripId = id; s.inputRouting = in; s.outputRouting = out;
            return s;
        };
        cue.strips = { snap ("L", 1, 1), snap ("X", 2, 2), snap ("Y", 3, 3), snap ("Z", 9, -1) };
        std::vector<SetlistBar::Cue> cues { cue };
        stereoconvert::carryCues (cues, m);
        const auto& st = cues[0].strips;
        expectEquals ((int) st.size(), 5, "the right half was not added to the cue");
        if (st.size() == 5)
        {
            expectEquals (st[0].stripId, juce::String ("L"));
            expectWithinAbsoluteError (st[0].pan, -1.0f, 0.001f);
            expectEquals (st[1].stripId, juce::String ("R"));
            expectWithinAbsoluteError (st[1].pan, 1.0f, 0.001f);
            expectEquals (st[2].inputRouting, 3, "X's input did not move with it");
            expectEquals (st[2].outputRouting, 3);
            expectEquals (st[3].inputRouting, 4, "Y's input did not move with it");
            expectEquals (st[3].outputRouting, 3, "Y's output was outside the output run");
            expectEquals (st[4].inputRouting, 9, "a strip past the gap moved");
            expectEquals (st[4].outputRouting, -1);
        }
        // During soundcheck a cue holds the saved show output.
        AudioEngine::StereoConversion vsc;
        vsc.showOutputsSaved = true;
        vsc.outputsFrom = 2; vsc.outputsTo = 9;   // live outputs sit on the inputs
        vsc.savedFrom = 5;  vsc.savedTo = 5;
        std::vector<SetlistBar::Cue> show { SetlistBar::Cue {} };
        show[0].strips = { snap ("X", -1, 5), snap ("Y", -1, 3) };
        stereoconvert::carryCues (show, vsc);
        expectEquals (show[0].strips[0].outputRouting, 6, "the show output did not move");
        expectEquals (show[0].strips[1].outputRouting, 3, "a live soundcheck output moved a show output");
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
