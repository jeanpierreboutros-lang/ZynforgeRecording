#include "../Audio/AudioEngine.h"
#include "../Audio/SettingsFile.h"

namespace zynforge
{
    class SettingsBatchAuditTests final : public juce::UnitTest
    {
    public:
        SettingsBatchAuditTests() : UnitTest ("Settings batch reset audit", "zynforge") {}

        static juce::PropertiesFile::Options options()
        {
            juce::PropertiesFile::Options result;
            result.storageFormat = juce::PropertiesFile::storeAsXML;
            return result;
        }

        static juce::String seededKey (int index)
        {
            // Every supported slot is seeded; rotate domains to keep the red
            // fixture small enough that 1,536 old XML reloads finish promptly.
            static const char* prefixes[] { "strip_gain_", "strip_pan_", "strip_color_",
                                           "strip_name_", "strip_in_", "strip_out_" };
            return juce::String (prefixes[index % 6]) + juce::String (index);
        }

        void seed (AudioEngine& engine)
        {
            auto* properties = engine.getAppProps();
            expect (properties != nullptr);
            if (properties == nullptr) return;
            for (int i = 0; i < 256; ++i) properties->setValue (seededKey (i), 17);
            properties->setValue ("stripCount", 256);
            properties->setValue ("strip_gain_256", -13);
            properties->setValue ("strip_future_override_5", "preserve unknown domain");
            properties->setValue ("audit_global_unknown", "older cached value");
            properties->setValue ("audit_global_deleted", "stale cached value");
            expect (properties->saveIfNeeded());
            // A sibling writer changes a global value and deletes another
            // AFTER the engine cache was loaded. Clearing must replace, not
            // merge or blindly save that stale cache over the newer file.
            juce::PropertiesFile sibling (properties->getFile(), options());
            sibling.setValue ("audit_global_unknown", "new shared value / Ω");
            sibling.removeValue ("audit_global_deleted");
            expect (sibling.saveIfNeeded());
        }

        void expectGlobalsPreserved (const juce::PropertiesFile& saved)
        {
            expectEquals (saved.getValue ("audit_global_unknown"), juce::String ("new shared value / Ω"));
            expect (! saved.containsKey ("audit_global_deleted"), "sibling-deleted global was resurrected");
            expectEquals (saved.getValue ("strip_future_override_5"), juce::String ("preserve unknown domain"));
            expectEquals (saved.getIntValue ("strip_gain_256"), -13);
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("All 256 strip overrides reset with at most five shared-file reloads");
            {
                AudioEngine engine;
                engine.getRecorder().setTrackCount (2);
                seed (engine);
                auto& live = engine.getRecorder().getTrack (0);
                live.gainDb.store (-6.0f); live.pan.store (0.7f);
                live.muted.store (true); live.soloed.store (true); live.armed.store (true);
                const auto before = settingsReloadCountForTests.load();
                engine.clearAllStripOverrides();
                const auto reloads = settingsReloadCountForTests.load() - before;
                logMessage ("Full reset shared-file reloads: " + juce::String (reloads));
                expect (reloads <= 5, "reset still reloads XML per field/per strip: " + juce::String (reloads));
                juce::PropertiesFile saved (engine.getAppProps()->getFile(), options());
                for (int i = 0; i < 256; ++i)
                    expect (! saved.containsKey (seededKey (i)), "override survived: " + seededKey (i));
                expectEquals (saved.getIntValue ("stripCount", -1), 0);
                expectGlobalsPreserved (saved);
                expectWithinAbsoluteError (live.gainDb.load(), 0.0f, 0.0001f);
                expectWithinAbsoluteError (live.pan.load(), 0.0f, 0.0001f);
                expect (! live.muted.load() && ! live.soloed.load() && ! live.armed.load());
            }

            beginTest ("Partial strip reset uses at most five reloads and preserves unrelated slots");
            {
                AudioEngine engine;
                seed (engine);
                const auto before = settingsReloadCountForTests.load();
                engine.clearStripOverridesRange (10, 20);
                const auto reloads = settingsReloadCountForTests.load() - before;
                logMessage ("Range reset shared-file reloads: " + juce::String (reloads));
                expect (reloads <= 5, "range reset still reloads XML per field/per strip: " + juce::String (reloads));
                // An ordinary sibling update must not restore keys that a
                // different module just removed from the shared preferences.
                engine.stripNames.setName (0, "fresh sibling name");
                juce::PropertiesFile saved (engine.getAppProps()->getFile(), options());
                for (int i = 0; i < 256; ++i)
                    expect (saved.containsKey (seededKey (i)) == (i < 10 || i >= 20),
                            "range changed the wrong slot: " + seededKey (i));
                expectGlobalsPreserved (saved);
            }

            beginTest ("Full reset removes current and legacy persisted aux-send fields");
            {
                AudioEngine engine;
                auto* properties = engine.getAppProps();
                for (int track : { 0, 17, 255 })
                    for (int send = 0; send < TrackState::kNumSends; ++send)
                    {
                        const auto key = "strip_send_" + juce::String (track) + "_" + juce::String (send);
                        properties->setValue (key + "_bus", 7);
                        properties->setValue (key + "_dB", -9.0);
                        properties->setValue (key + "_post", false);
                        properties->setValue ("strip_send_" + juce::String (send) + "_" + juce::String (track), 7);
                    }
                properties->setValue ("audit_aux_global", "preserve");
                expect (properties->saveIfNeeded());
                engine.clearAllStripOverrides();
                juce::PropertiesFile saved (properties->getFile(), options());
                for (int track : { 0, 17, 255 })
                    for (int send = 0; send < TrackState::kNumSends; ++send)
                    {
                        const auto key = "strip_send_" + juce::String (track) + "_" + juce::String (send);
                        for (auto* suffix : { "_bus", "_dB", "_post" })
                            expect (! saved.containsKey (key + suffix), "current send field survived: " + key + suffix);
                        expect (! saved.containsKey ("strip_send_" + juce::String (send) + "_" + juce::String (track)),
                                "legacy send field survived");
                    }
                expectEquals (saved.getValue ("audit_aux_global"), juce::String ("preserve"));
            }

            beginTest ("Public strip growth preserves old slots and reports its remaining UUID reload cost");
            {
                AudioEngine engine;
                engine.setStripCount (2);
                seed (engine);
                const auto before = settingsReloadCountForTests.load();
                engine.setStripCount (6);
                const auto reloads = settingsReloadCountForTests.load() - before;
                logMessage ("Public growth 2 -> 6 shared-file reloads (including UUID publication): " + juce::String (reloads));
                expectEquals (engine.getRecorder().getNumTracks(), 6);
                juce::PropertiesFile saved (engine.getAppProps()->getFile(), options());
                for (int i = 0; i < 256; ++i)
                    expect (saved.containsKey (seededKey (i)) == (i < 2 || i >= 6),
                            "growth changed the wrong override: " + seededKey (i));
                expectEquals (saved.getIntValue ("stripCount"), 6);
                expectGlobalsPreserved (saved);
                for (int i = 2; i < 6; ++i)
                {
                    const auto& track = engine.getRecorder().getTrack (i);
                    expectWithinAbsoluteError (track.gainDb.load(), 0.0f, 0.0001f);
                    expectWithinAbsoluteError (track.pan.load(), 0.0f, 0.0001f);
                }
            }
        }
    };
    static SettingsBatchAuditTests settingsBatchAuditTests;
}
