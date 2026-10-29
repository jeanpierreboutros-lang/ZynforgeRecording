// TrackState lifetime and mixer-undo change detection.
//
// UI surfaces (ChannelStrip, LedMeter, MiniSpectrum) cache a TrackState& and
// sample it on their own timers. Every recorder shrink -- and a device restart
// that reshapes the track set inside MultitrackRecorder::prepare() -- freed
// those objects while the surfaces still read them until the next rebuild
// tick. Audits re-fixed that one call site at a time with condemnAllStrips().
// The recorder now owns TrackStates through shared_ptr and each surface takes
// a keep-alive from the reference it is given, so a removed track's surface
// reads a detached-but-valid object until it is rebuilt.
//
// Mixer undo detects changes with an allocation-free fingerprint built from
// the same field table as the snapshot, instead of stringifying the whole
// mixer every 100 ms.

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../UI/LedMeter.h"
#include "../UI/MiniSpectrum.h"
#include "../UI/MixerSnapshot.h"

#include <set>

namespace zynforge
{
    class StripLifetimeTests final : public juce::UnitTest
    {
    public:
        StripLifetimeTests() : juce::UnitTest ("Strip lifetime and mixer fingerprint", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("A meter keeps its TrackState alive after the recorder drops it");
            {
                AudioEngine eng;
                eng.clearAllStripOverrides();
                eng.prepareForTests (48000.0, 512);
                eng.setStripCount (3);
                auto& doomed = eng.getRecorder().getTrack (2);
                doomed.peak.store (0.5f);
                auto meter    = std::make_unique<LedMeter> (doomed);
                auto spectrum = std::make_unique<MiniSpectrum> (doomed);

                eng.setStripCount (1);   // the recorder releases track 2
                expectEquals (eng.getRecorder().getNumTracks(), 1);
                // Without the keep-alive this read is a use-after-free (ASan
                // reports it); with it, the detached object is still valid.
                expectEquals (doomed.peak.load(), 0.5f);
                meter.reset();
                spectrum.reset();
                eng.clearAllStripOverrides();
            }

            beginTest ("A TrackState not owned by the recorder needs no keep-alive");
            {
                TrackState standalone;   // tests and temporaries build these on the stack
                LedMeter meter (standalone);
                expect (standalone.weak_from_this().expired());
            }

            beginTest ("The fingerprint covers every field the undo snapshot records");
            {
                AudioEngine eng;
                eng.clearAllStripOverrides();
                eng.prepareForTests (48000.0, 512);
                eng.setStripCount (2);

                std::set<juce::String> snapshotKeys;
                const auto snapshot = mixersnapshot::snapshotStrip (eng, 0);
                if (auto* obj = snapshot.getDynamicObject())
                    for (const auto& p : obj->getProperties()) snapshotKeys.insert (p.name.toString());
                std::set<juce::String> fieldKeys;
                for (const auto& f : mixersnapshot::fields()) fieldKeys.insert (f.key);
                auto join = [] (const std::set<juce::String>& s) { juce::StringArray a; for (auto& k : s) a.add (k); return a.joinIntoString (","); };
                expect (snapshotKeys == fieldKeys, "snapshot and fingerprint must share one field table: snapshot ["
                        + join (snapshotKeys) + "] vs table [" + join (fieldKeys) + "]");

                // Every field, mutated alone, must move the fingerprint.
                auto& t = eng.getRecorder().getTrack (1);
                const std::vector<std::pair<juce::String, std::function<void()>>> edits {
                    { "name",    [&] { eng.setTrackName (1, "Kick In"); } },
                    { "colour",  [&] { t.colourARGB.store ((int) 0xff336699); } },
                    { "gainDb",  [&] { t.gainDb.store (-3.5f); } },
                    { "pan",     [&] { t.pan.store (0.25f); } },
                    { "in",      [&] { t.inputRouting.store (5); } },
                    { "out",     [&] { t.outputRouting.store (6); } },
                    { "outMute", [&] { t.outputMuted.store (! t.outputMuted.load()); } },
                    { "stream",  [&] { t.streamSend.store (! t.streamSend.load()); } },
                    { "mute",    [&] { t.muted.store (! t.muted.load()); } },
                    { "solo",    [&] { t.soloed.store (! t.soloed.load()); } },
                    { "mon",     [&] { t.monitor.store (! t.monitor.load()); } },
                    { "rec",     [&] { t.armed.store (! t.armed.load()); } },
                };
                expectEquals ((int) edits.size(), (int) fieldKeys.size(), "add an edit for every new field");
                for (const auto& [key, edit] : edits)
                {
                    expect (fieldKeys.count (key) == 1, "unknown field " + key);
                    const auto before = mixersnapshot::fingerprint (eng);
                    edit();
                    expect (mixersnapshot::fingerprint (eng) != before, "fingerprint missed a change to " + key);
                }
                const auto stable = mixersnapshot::fingerprint (eng);
                expect (mixersnapshot::fingerprint (eng) == stable, "an unchanged mixer must fingerprint the same");
                eng.clearAllStripOverrides();
            }

            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static StripLifetimeTests stripLifetimeTests;
}
