// Design-render harness: renders the real main window (MIXER, EDIT, armed,
// recording, a dead channel, a narrow window) and its dialogs to PNG, from a
// synthetic 22-strip show, so a look change can be judged before and after.
// Opt-in: it does nothing unless $ZF_RENDER_DIR names the output folder, e.g.
//   open -W -n --env ZF_RENDER_DIR=/tmp/renders "Zynforge Recording Tests.app" \
//        --args "--test-filter=Design render"
// The fed audio runs faster than real time, so the disk-trouble alarm, true
// peak and loudness figures in these renders are artefacts, not findings.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../UI/MainComponent.h"
#include "SilentAudioDevice.h"

#include <cmath>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class DesignRenderHarness final : public juce::UnitTest
    {
    public:
        DesignRenderHarness() : juce::UnitTest ("Design render", "zynforge") {}

        static constexpr double kRate = 48000.0;
        static constexpr int kBlock = 512;

        juce::File out;

        struct StripDef { const char* name; int kind; bool stereo; float level; };
        // kind: 0 kick 1 snare 2 hat 3 tom 4 overhead 5 bass 6 gtr 7 keys 8 vox 9 silent 10 mix
        static std::vector<StripDef> show()
        {
            return {
                { "KICK IN", 0, false, 0.80f }, { "KICK OUT", 0, false, 0.60f },
                { "SNARE TOP", 1, false, 0.70f }, { "SNARE BOT", 1, false, 0.35f },
                { "HI HAT", 2, false, 0.30f }, { "TOM 1", 3, false, 0.50f }, { "TOM 2", 3, false, 0.45f },
                { "FLOOR TOM", 3, false, 0.55f }, { "OH", 4, true, 0.40f }, { "OH R", 4, false, 0.40f },
                { "BASS DI", 5, false, 0.70f }, { "BASS MIC", 5, false, 0.45f },
                { "GTR JP", 6, false, 0.50f }, { "GTR 2", 6, false, 0.40f },
                { "KEYS", 7, true, 0.40f }, { "KEYS R", 7, false, 0.40f },
                { "VOX LEAD", 8, false, 0.60f }, { "BV 1", 8, false, 0.35f }, { "BV 2", 8, false, 0.30f },
                { "TALKBACK", 9, false, 0.0f }, { "MIX LR", 10, true, 0.50f }, { "MIX LR R", 10, false, 0.50f },
            };
        }

        static float sampleFor (int kind, float level, juce::int64 n, juce::Random& rnd)
        {
            const double t = (double) n / kRate;
            const double beat = std::fmod (t, 0.5);
            switch (kind)
            {
                case 0: return level * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 55.0 * beat) * std::exp (-beat * 18.0));
                case 1: { const double b = std::fmod (t + 0.25, 0.5); return level * (rnd.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-b * 25.0); }
                case 2: { const double b = std::fmod (t, 0.125); return level * (rnd.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-b * 80.0); }
                case 3: { const double b = std::fmod (t, 2.0); return b < 0.6 ? level * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * b) * std::exp (-b * 6.0)) : 0.0f; }
                case 4: return level * 0.6f * (rnd.nextFloat() * 2.0f - 1.0f) * (float) (0.4 + 0.6 * std::exp (-std::fmod (t, 0.5) * 8.0));
                case 5: return level * (float) std::sin (2.0 * juce::MathConstants<double>::pi * (std::fmod (t, 2.0) < 1.0 ? 55.0 : 73.4) * t);
                case 6: { const double ph = std::fmod (t * 196.0, 1.0); return level * (float) (2.0 * ph - 1.0) * (float) (0.5 + 0.5 * std::sin (t * 1.3)); }
                case 7: return level * 0.5f * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 261.6 * t) + std::sin (2.0 * juce::MathConstants<double>::pi * 329.6 * t));
                case 8: { const bool on = std::fmod (t, 4.0) < 2.6; return on ? level * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t) * (0.6 + 0.4 * std::sin (t * 9.0))) : 0.0f; }
                case 10: return level * 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * t) + level * 0.3f * (rnd.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-std::fmod (t, 0.5) * 12.0);
                default: return 0.0f;
            }
        }

        bool writeTake (const juce::File& file, int kind, float level, int channels, int seconds)
        {
            file.getParentDirectory().createDirectory();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (file.createOutputStream().release());
            if (os == nullptr) return false;
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (kRate)
                                                .withNumChannels (channels).withBitsPerSample (24));
            if (w == nullptr) return false;
            juce::Random rnd (kind * 31 + 7);
            const int total = (int) kRate * seconds;
            juce::AudioBuffer<float> buf (channels, 4096);
            for (int pos = 0; pos < total; pos += 4096)
            {
                const int n = juce::jmin (4096, total - pos);
                for (int i = 0; i < n; ++i)
                {
                    const float v = sampleFor (kind, level, pos + i, rnd);
                    for (int c = 0; c < channels; ++c) buf.setSample (c, i, c == 0 ? v : v * 0.9f);
                }
                if (! w->writeFromAudioSampleBuffer (buf, 0, n)) return false;
            }
            return true;
        }

        void pump (int ms)
        {
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
            while (juce::Time::getMillisecondCounter() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, true);
        }

        void save (juce::Component& c, const juce::String& name, float scale = 2.0f)
        {
            if (c.getWidth() <= 0 || c.getHeight() <= 0) { logMessage ("skip " + name + " (no size)"); return; }
            const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, scale);
            const auto file = out.getChildFile (name + ".png");
            file.deleteFile();
            juce::FileOutputStream stream (file);
            juce::PNGImageFormat png;
            png.writeImageToStream (image, stream);
            logMessage ("rendered " + file.getFullPathName() + " " + juce::String (c.getWidth()) + "x" + juce::String (c.getHeight()));
        }

        void saveTopLevels (const juce::String& prefix)
        {
            for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
                if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i))
                    if (w->isVisible())
                        save (*w, prefix + "-" + juce::String (i) + "-" + juce::File::createLegalFileName (w->getName()).replaceCharacter (' ', '_'));
        }

        void closeTopLevels()
        {
            for (int k = 0; k < 6; ++k)
            {
                for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
                    if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i))
                    {
                        if (auto* dw = dynamic_cast<juce::DialogWindow*> (w)) dw->exitModalState (0);
                        w->setVisible (false);
                    }
                pump (60);
                for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
                    if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i)) delete w;
                pump (60);
            }
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
            pump (100);
        }

        void tick (MainComponent& main, int n)
        {
            for (int i = 0; i < n; ++i) { main.timerCallback(); pump (30); }
        }

        void feed (MainComponent& main, int blocks, bool silenceStrip0, juce::int64& pos)
        {
            const auto defs = show();
            const int nIn = 32;
            juce::AudioBuffer<float> in (nIn, kBlock), outBuf (2, kBlock);
            juce::Random rnd (99);
            for (int b = 0; b < blocks; ++b)
            {
                in.clear();
                for (int s = 0; s < (int) defs.size() && s < nIn; ++s)
                    for (int i = 0; i < kBlock; ++i)
                        in.setSample (s, i, (silenceStrip0 && s == 0) ? 0.0f : sampleFor (defs[(size_t) s].kind, defs[(size_t) s].level, pos + i, rnd));
                main.engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), nIn,
                                                             outBuf.getArrayOfWritePointers(), 2, kBlock, {});
                pos += kBlock;
                if (b % 9 == 0) { main.timerCallback(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.001, true); }
            }
        }

        void runTest() override
        {
            beginTest ("render");
            const auto dir = juce::SystemStats::getEnvironmentVariable ("ZF_RENDER_DIR", {});
            if (dir.isEmpty())
            {
                logMessage ("Design render: skipped (set ZF_RENDER_DIR to render)");
                expect (true);
                return;
            }
            out = juce::File (dir);
            out.createDirectory();

            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restore { [] { MainComponent::s_testConstruct = false; AudioEngine::setTestModeSkipAudioInit (false); } };

            const auto session = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("zf-design-show-" + juce::Uuid().toString().substring (0, 6)).getChildFile ("Saturday Show");
            session.deleteRecursively();
            const auto audio = session.getChildFile ("Audio Files");
            const auto defs = show();
            for (int i = 0; i < (int) defs.size(); ++i)
            {
                const auto& d = defs[(size_t) i];
                if (i > 0 && defs[(size_t) i - 1].stereo) continue;   // R half of a pair
                const auto stem = juce::String::formatted ("%02d ", i + 1) + juce::String (d.name);
                expect (writeTake (audio.getChildFile (stem + ".wav"), d.kind, d.level, d.stereo ? 2 : 1, 24));
            }

            MainComponent main;
            testaudio::useSilentDevice (main.engine, kRate, 32);
            main.setSize (1600, 1000);
            expect (main.openSessionFolder (session, true) > 0);
            main.engine.prepareForTests (kRate, kBlock);
            for (int i = 0; i < (int) defs.size(); ++i)
            {
                main.engine.setTrackName (i, defs[(size_t) i].name);
                if (defs[(size_t) i].stereo) main.engine.setTrackStereo (i, true);
            }
            main.rebuildStrips();
            main.resized();
            tick (main, 5);
            pump (500);

            // 1. MIXER, idle
            main.switchView (MainComponent::View::Mix);
            tick (main, 4);
            save (main, "01-mixer-idle");

            // 2. EDIT, idle, waveforms scanned
            main.switchView (MainComponent::View::Edit);
            for (int i = 0; i < 40; ++i) { tick (main, 1); pump (150); }
            save (main, "02-edit-idle");

            // 3. Take rolling: arm and record with signal
            main.switchView (MainComponent::View::Mix);
            for (int i = 0; i < (int) defs.size(); ++i)
                if (i < 19 || i == 20) main.engine.setTrackArmed (i, true);
            tick (main, 2);
            save (main, "03-mixer-armed");
            main.onRecordClicked();
            tick (main, 2);
            pump (300);
            juce::int64 pos = 0;
            feed (main, (int) (kRate * 6 / kBlock), false, pos);
            tick (main, 3);
            save (main, "04-mixer-recording");
            main.switchView (MainComponent::View::Edit);
            feed (main, (int) (kRate * 3 / kBlock), false, pos);
            tick (main, 3);
            save (main, "05-edit-recording");

            // 4. A dead channel during the take (strip 1 goes to digital zero)
            main.switchView (MainComponent::View::Mix);
            feed (main, (int) (kRate * 5 / kBlock), true, pos);
            tick (main, 4);
            save (main, "06-mixer-dead-channel-alarm");

            // 5. Narrow window during the take
            main.setSize (1210, 760);
            main.resized();
            tick (main, 3);
            save (main, "07-mixer-recording-narrow");
            main.setSize (1600, 1000);
            main.resized();

            // stop the take (two taps)
            main.onStopClicked();
            main.onStopClicked();
            for (int i = 0; i < 40 && main.engine.isRecording(); ++i) pump (100);
            for (int i = 0; i < 30; ++i) { tick (main, 1); pump (100); }
            save (main, "08-mixer-after-stop");

            // 6. Dialogs
            closeTopLevels();
            main.openPatchWindow();               pump (400); saveTopLevels ("10-patch");         closeTopLevels();
            main.onDeviceClicked();               pump (400); saveTopLevels ("11-device");        closeTopLevels();
            main.onBackupClicked();               pump (400); saveTopLevels ("12-backup");        closeTopLevels();
            main.showPreflightChecklist();        pump (1500); saveTopLevels ("13-preflight");    closeTopLevels();
            main.launchNewSessionDialog();        pump (400); saveTopLevels ("14-new-session");   closeTopLevels();
            main.showKeyboardShortcuts();         pump (400); saveTopLevels ("15-shortcuts");     closeTopLevels();
            main.showSessionInfo();               pump (400); saveTopLevels ("16-session-info");  closeTopLevels();
            main.openMetersWindow();              pump (400); saveTopLevels ("17-meters");        closeTopLevels();
            main.showStartupWelcome();            pump (600); saveTopLevels ("18-welcome");       closeTopLevels();

            session.getParentDirectory().deleteRecursively();
        }
    };

    static DesignRenderHarness designRenderHarness;
}
