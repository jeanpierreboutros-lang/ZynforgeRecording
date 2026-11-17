// Design pass 2026-11-17, alarms. At a real show the banner named a dead
// channel, but with 56 channels and a dozen strips in view the strip itself
// looked normal. Now the faulted strip marks itself in MIXER, EDIT and the
// meterbridge (A1), the banner holds one alarm per line and a click on a
// channel line brings the strip into view (A2), and the BACKUP chip shows its
// state with a dot (A3).

#include "../UI/MainComponent.h"
#include "../UI/BackupChip.h"
#include "../UI/StripAlarm.h"
#include "../Audio/ChannelAlarm.h"
#include "SilentAudioDevice.h"

#include <CoreFoundation/CoreFoundation.h>
#include <functional>

namespace zynforge
{
    class Design20261117AlarmsTests final : public juce::UnitTest
    {
    public:
        Design20261117AlarmsTests() : juce::UnitTest ("Design 2026-11-17 alarms", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restore { [] { AudioEngine::setTestModeSkipAudioInit (false); } };
            stripLevels();
            monitorToStrips();
            revealTargetsFollowTheLines();
            bannerLines();
            backupChip();
            windowMarksAndReveals();
        }

    private:
        using Alarm = channelalarm::Alarm;
        using Level = stripalarm::Level;
        using BannerLevel = AlarmBanner::Level;
        static constexpr double kRate = 48000.0;
        static constexpr int kBlock = 256;

        static Alarm alarmOf (const juce::String& kind, int strip, std::vector<int> strips = {},
                              const juce::String& name = "CH")
        {
            Alarm a;
            a.kind = kind;
            a.strip = strip;
            a.name = name;
            a.strips = std::move (strips);
            if (kind == channelalarm::kStream) a.channels = juce::jmax (8, (int) a.strips.size());
            return a;
        }

        static std::vector<Level> levelsFor (const std::vector<Alarm>& live, int tracks, int pairLeft = -1)
        {
            std::vector<Level> out;
            stripalarm::mapTracks (live, tracks, [pairLeft] (int t) { return t == pairLeft; }, out);
            return out;
        }

        static juce::int64 frames (double seconds) { return (juce::int64) std::llround (seconds * kRate); }

        static bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 4000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        static juce::MouseEvent clickAt (juce::Component& c, juce::Point<int> p)
        {
            const auto at = p.toFloat();
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, {}, 0.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, &c, &c, now, at, now, 1, false);
        }

        // ── A1: which strips a set of live alarms marks ──────────────────────
        void stripLevels()
        {
            beginTest ("A channel fault marks its strip and nothing else");
            {
                const auto levels = levelsFor ({ alarmOf (channelalarm::kChannel, 3) }, 8);
                expectEquals ((int) levels.size(), 8);
                for (int t = 0; t < 8; ++t)
                    expect (levels[(std::size_t) t] == (t == 3 ? Level::Lost : Level::None), "track " + juce::String (t));
            }

            beginTest ("A stereo pair is one strip: both halves carry its level");
            {
                const auto levels = levelsFor ({ alarmOf (channelalarm::kChannel, 4) }, 8, 4);
                expect (levels[4] == Level::Lost && levels[5] == Level::Lost);
                expect (levels[3] == Level::None && levels[6] == Level::None);
                const auto notice = levelsFor ({ alarmOf (channelalarm::kNoSignal, 4) }, 8, 4);
                expect (notice[4] == Level::NoSignal && notice[5] == Level::NoSignal);
            }

            beginTest ("A lost stream marks every strip it took out");
            {
                const auto levels = levelsFor ({ alarmOf (channelalarm::kStream, -1, { 0, 2, 4 }) }, 8, 4);
                for (int t : { 0, 2, 4, 5 }) expect (levels[(std::size_t) t] == Level::Lost, "track " + juce::String (t));
                for (int t : { 1, 3, 6, 7 }) expect (levels[(std::size_t) t] == Level::None, "track " + juce::String (t));
            }

            beginTest ("No signal yet is a notice; a fault on the same strip outranks it");
            {
                auto levels = levelsFor ({ alarmOf (channelalarm::kNoSignal, 6) }, 8);
                expect (levels[6] == Level::NoSignal);
                levels = levelsFor ({ alarmOf (channelalarm::kNoSignal, 6), alarmOf (channelalarm::kChannel, 6) }, 8);
                expect (levels[6] == Level::Lost);
                levels = levelsFor ({ alarmOf (channelalarm::kStream, -1, { 6 }), alarmOf (channelalarm::kNoSignal, 6) }, 8);
                expect (levels[6] == Level::Lost);
            }

            beginTest ("A recovered alarm, an ended take and a stray index mark nothing");
            {
                auto ended = alarmOf (channelalarm::kChannel, 2);
                ended.endSample = 1000;
                ended.recovered = true;
                for (auto l : levelsFor ({ ended }, 8)) expect (l == Level::None);
                for (auto l : levelsFor ({}, 8)) expect (l == Level::None);
                for (auto l : levelsFor ({ alarmOf (channelalarm::kChannel, -1), alarmOf (channelalarm::kChannel, 9),
                                           alarmOf (channelalarm::kStream, -1, { -3, 12 }) }, 8))
                    expect (l == Level::None);
                expect (levelsFor ({ alarmOf (channelalarm::kChannel, 7) }, 8, 7)[7] == Level::Lost,
                        "a pair flag on the last track reached past the end");
            }

            beginTest ("A steady tick reuses its buffer");
            {
                std::vector<Level> out;
                const std::vector<Alarm> live { alarmOf (channelalarm::kChannel, 1) };
                const auto never = [] (int) { return false; };
                stripalarm::mapTracks (live, 24, never, out);
                const auto* data = out.data();
                for (int i = 0; i < 10; ++i) stripalarm::mapTracks (live, 24, never, out);
                expect (out.data() == data, "the per-strip levels reallocated on a steady tick");
            }
        }

        // ── A1 through the real monitor: fault, pair, stream, recovery, take end
        void monitorToStrips()
        {
            using Input = channelalarm::Monitor::Input;
            // Ten inputs; a stereo pair sits on tracks 4 and 5 (one strip, index 4).
            const std::vector<Input> inputs { { 0, "A" }, { 1, "B" }, { 2, "C" }, { 3, "D" }, { 4, "OH" },
                                              { 4, "OH" }, { 6, "E" }, { 7, "F" }, { 8, "G" }, { 9, "H" } };
            const auto pairLeft = [] (int t) { return t == 4; };
            std::vector<Level> levels;

            beginTest ("The monitor's channel fault marks the strip until it recovers");
            {
                channelalarm::Monitor monitor;
                monitor.begin (inputs, kRate);
                const auto now = frames (10.0);
                std::vector<juce::int64> signalEnd (inputs.size(), now);
                signalEnd[2] = frames (5.0);
                monitor.update (signalEnd, now);
                auto live = monitor.live (0);
                stripalarm::mapTracks (live, 10, pairLeft, levels);
                expect (levels[2] == Level::Lost);
                expect (levels[1] == Level::None && levels[3] == Level::None);

                signalEnd[5] = frames (5.0);   // the pair's R half goes to zero
                monitor.update (signalEnd, now);
                live = monitor.live (0);
                stripalarm::mapTracks (live, 10, pairLeft, levels);
                expect (levels[4] == Level::Lost && levels[5] == Level::Lost, "a pair half lost marks the whole strip");

                signalEnd[2] = frames (10.2);   // signal again
                monitor.update (signalEnd, frames (10.2));
                stripalarm::mapTracks (monitor.live (0), 10, pairLeft, levels);
                expect (levels[2] == Level::None, "a recovered strip stayed marked");
                expect (levels[4] == Level::Lost);

                monitor.finish (frames (11.0));
                stripalarm::mapTracks (monitor.live (0), 10, pairLeft, levels);
                for (auto l : levels) expect (l == Level::None, "a mark outlived the take");
            }

            beginTest ("A lost stream names its strips, and every one of them is marked");
            {
                channelalarm::Monitor monitor;
                monitor.begin (inputs, kRate);
                const auto now = frames (20.0);
                std::vector<juce::int64> signalEnd (inputs.size(), now);
                for (std::size_t i = 0; i < 8; ++i) signalEnd[i] = frames (15.0);
                monitor.update (signalEnd, now);
                const auto live = monitor.live (0);
                expect (! live.empty() && live.front().kind == channelalarm::kStream);
                expect (live.front().strips == std::vector<int> ({ 0, 1, 2, 3, 4, 6, 7 }));
                stripalarm::mapTracks (live, 10, pairLeft, levels);
                for (int t = 0; t < 8; ++t) expect (levels[(std::size_t) t] == Level::Lost, "track " + juce::String (t));
                expect (levels[8] == Level::None && levels[9] == Level::None);
                expectEquals (stripalarm::revealTargets (live).stream, 0);

                const auto back = Alarm::fromJson (live.front().toJson());
                expect (back.strips == live.front().strips, "the strips did not survive the daemon's status");
                expect (! Alarm::fromJson (alarmOf (channelalarm::kChannel, 2).toJson()).toJson().getDynamicObject()
                             ->hasProperty ("strips"), "a channel alarm grew a strips list");
            }

            beginTest ("An armed input silent a minute in is an amber notice on its strip");
            {
                channelalarm::Monitor monitor;
                monitor.begin (inputs, kRate);
                const auto now = frames (61.0);
                std::vector<juce::int64> signalEnd (inputs.size(), now);
                signalEnd[9] = -1;
                monitor.update (signalEnd, now);
                stripalarm::mapTracks (monitor.live (0), 10, pairLeft, levels);
                expect (levels[9] == Level::NoSignal);
                expectEquals (stripalarm::revealTargets (monitor.live (0)).noSignal, 9);
            }
        }

        // ── A2: which strip a click on each banner line brings into view ─────
        void revealTargetsFollowTheLines()
        {
            beginTest ("Each channel line leads to the first strip it names");
            {
                std::vector<Alarm> live { alarmOf (channelalarm::kStream, -1, { 12, 5, 9 }, "Dante"),
                                          alarmOf (channelalarm::kChannel, 7, {}, "KICK IN"),
                                          alarmOf (channelalarm::kChannel, 3, {}, "SNARE"),
                                          alarmOf (channelalarm::kNoSignal, 20, {}, "TALKBACK") };
                const auto targets = stripalarm::revealTargets (live);
                expectEquals (targets.stream, 5);
                expectEquals (targets.channel, 3);
                expectEquals (targets.noSignal, 20);

                // The heads are what channelalarm::bannerText writes.
                const auto lines = AlarmBanner::splitAlarms (channelalarm::bannerText (live));
                expectEquals (lines.size(), 3);
                expectEquals (stripalarm::revealTrackFor (lines[0], targets), 5);
                expectEquals (stripalarm::revealTrackFor (lines[1], targets), 3);
                expectEquals (stripalarm::revealTrackFor (lines[2], targets), 20);
                expectEquals (stripalarm::revealTrackFor ("! DISK STRUGGLING -- missed samples imminent", targets), -1);
                expectEquals (stripalarm::revealTrackFor ("! BACKUP WRITE FAILED", stripalarm::RevealTargets {}), -1);
            }
        }

        // ── A2: one alarm per line, overflow, ellipsis, clicks ───────────────
        void bannerLines()
        {
            const std::vector<AlarmBanner::Alarm> three {
                { "No signal yet this take: TALKBACK", BannerLevel::Notice, 19 },
                { "! CHANNEL FAULT -- input lost, digital zero: KICK IN", BannerLevel::Danger, 0 },
                { "! DISK STRUGGLING -- missed samples imminent", BannerLevel::Danger, -1 } };

            beginTest ("Lines that fit, and the rest counted");
            {
                expectEquals (AlarmBanner::linesFor (0, 36), 0);
                expectEquals (AlarmBanner::linesFor (1, 36), 1);
                expectEquals (AlarmBanner::linesFor (2, 36), 2);
                expectEquals (AlarmBanner::linesFor (7, 36), 2);
                expectEquals (AlarmBanner::linesFor (7, 70), 4);
                expectEquals (AlarmBanner::linesFor (3, 0), 1, "an unlaid banner still shows one line");
                expect (AlarmBanner::visibleIndices (5, 2, -1) == std::vector<int> ({ 0, 1 }));
                expect (AlarmBanner::visibleIndices (5, 2, 4) == std::vector<int> ({ 0, 4 }), "a held alarm takes the last line");
                expect (AlarmBanner::visibleIndices (5, 2, 1) == std::vector<int> ({ 0, 1 }));
                expect (AlarmBanner::visibleIndices (1, 2, -1) == std::vector<int> ({ 0 }));
                expect (AlarmBanner::visibleIndices (0, 2, 3).empty());
            }

            beginTest ("Most serious first, one per line, and +N more for the rest");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                banner.show (three);
                expectEquals (banner.getVisibleLineCount(), 2);
                expectEquals (banner.getOverflowCount(), 1);
                expect (banner.getShownText().startsWith ("! CHANNEL FAULT"), banner.getShownText());
                expect (banner.getShownText().contains ("DISK STRUGGLING"), banner.getShownText());
                expect (! banner.getShownText().contains ("TALKBACK"), "a notice took a danger's line");
                expect (banner.getText().contains ("TALKBACK"), "the overflow left the list");
                expectEquals (AlarmBanner::moreText (banner.getOverflowCount()), juce::String ("+1 more"));
                const auto more = banner.moreBounds();
                expect (! more.isEmpty() && more.getY() >= banner.getHeight() / 2, "+N more is not on the last line");
            }

            beginTest ("Each line has its own colour and mark");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                banner.show ({ three[1], three[0] });   // a danger over a notice
                juce::Image image (juce::Image::ARGB, 446, 36, true);
                {
                    juce::Graphics g (image);
                    banner.paint (g);
                }
                expect (image.getPixelAt (3, 9) == brand::accentRecord, "the danger line is not red");
                expect (image.getPixelAt (3, 27) == brand::alertAmber, "the notice line is not amber");
                expect (AlarmBanner::displayText ("!! INPUT STREAM LOST -- 8") == "INPUT STREAM LOST -- 8",
                        "the drawn mark should stand in for the '!'");
                expectEquals (AlarmBanner::displayText ("No signal yet this take: A"), juce::String ("No signal yet this take: A"));
            }

            beginTest ("A lone alarm too wide for the large size steps down before it is cut");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                const juce::String backup ("! BACKUP WRITE FAILED -- primary recording continues");
                banner.show ({ { backup, BannerLevel::Danger } });
                expect (juce::GlyphArrangement::getStringWidth (brand::type::headline(), typeset (AlarmBanner::displayText (backup)))
                            > 446.0f - 2 * brand::space::lg - 16 - brand::space::sm, "this one should not fit large");
                expect (banner.isFullyShown (backup), "a common alarm was cut at the narrowest banner");
            }

            beginTest ("A long alarm ends in an ellipsis instead of wrapping");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                const juce::String longOne ("! COPY /Volumes/Backup Drive 2/Sessions/2026 Tour/Night 3 fell a whole ring behind "
                                            "and lost audio while the primary kept recording");
                banner.show ({ { longOne, BannerLevel::Danger } });
                expectEquals (banner.getVisibleLineCount(), 1);
                expect (! banner.isFullyShown (longOne));
                banner.show ({ { "! BACKUP WRITE FAILED", BannerLevel::Danger } });
                expect (banner.isFullyShown ("BACKUP WRITE FAILED"));
            }

            beginTest ("A click on the channel line reveals its strip; elsewhere opens the list");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                banner.show (three);
                int revealed = -1;
                banner.onReveal = [&revealed] (int track) { revealed = track; };
                const juce::Point<int> firstLine (60, 8), secondLine (60, 27);
                expectEquals (banner.revealTrackAt (firstLine), 0);
                expectEquals (banner.revealTrackAt (secondLine), -1, "DISK STRUGGLING names no strip");
                expectEquals (banner.revealTrackAt (banner.moreBounds().getCentre()), -1, "+N more must open the list");
                banner.mouseUp (clickAt (banner, firstLine));
                expectEquals (revealed, 0);
            }

            beginTest ("A refused press takes the last line, and the count stays honest");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                auto list = three;
                list.push_back ({ "! BACKUP WRITE FAILED -- primary recording continues", BannerLevel::Danger, -1 });
                list.push_back ({ "Recording refused -- arm at least one track", BannerLevel::Notice, -1 });
                banner.show (list);
                expect (! banner.getShownText().contains ("refused"));
                banner.bringIntoView ("Recording refused");
                expect (banner.getShownText().contains ("refused"), banner.getShownText());
                expect (banner.getShownText().startsWith ("! CHANNEL FAULT"), "the worst lost its line");
                expectEquals (banner.getOverflowCount(), 3);
                list.pop_back();
                banner.show (list);
                expect (! banner.getShownText().contains ("refused"), "a cleared refusal held its line");
            }

            beginTest ("The list opens, and closes when the alarms clear");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                banner.show (three);
                banner.showList();
                expect (banner.isListOpen());
                banner.show ({});
                expect (! banner.isVisible());
                expect (pumpUntil ([&banner] { return ! banner.isListOpen(); }), "the list outlived the alarms");
            }
        }

        // ── A3: the BACKUP chip ──────────────────────────────────────────────
        void backupChip()
        {
            using backupchip::State;
            const auto stateOf = [] (bool configured, bool recording, bool active, bool failed, bool answering)
            {
                backupchip::Facts f;
                f.configured = configured;
                f.recording = recording;
                f.backupActive = active;
                f.backupFailed = failed;
                f.driveAnswering = answering;
                return backupchip::stateFor (f);
            };

            beginTest ("The chip's state follows the backup");
            {
                expect (stateOf (false, false, false, false, false) == State::Off);
                expect (stateOf (false, true,  false, true,  false) == State::Off, "no folder is OFF whatever a take did");
                expect (stateOf (true,  false, false, false, true)  == State::Idle);
                expect (stateOf (true,  false, false, false, false) == State::Trouble, "a still drive read as ready");
                expect (stateOf (true,  true,  true,  false, false) == State::Writing);
                expect (stateOf (true,  true,  false, false, true)  == State::Trouble, "a take not writing its backup read as fine");
                expect (stateOf (true,  true,  true,  true,  true)  == State::Failed, "a failed write read as writing");
                expect (stateOf (true,  false, false, true,  true)  == State::Failed, "a failed take's backup read fine after STOP");
            }

            beginTest ("Each state has its dot and word");
            {
                expect (backupchip::dotFor (State::Off) == brand::alertAmber);
                expect (backupchip::dotFor (State::Idle) == brand::debossFace);
                expect (backupchip::dotFor (State::Writing) == brand::accentStatus);
                expect (backupchip::dotFor (State::Trouble) == brand::alertAmber);
                expect (backupchip::dotFor (State::Failed) == brand::accentRecord);
                expectEquals (backupchip::labelFor (State::Off), juce::String ("BACKUP OFF"));
                expectEquals (backupchip::labelFor (State::Idle), juce::String ("BACKUP"));
                expectEquals (backupchip::labelFor (State::Writing), juce::String ("BACKUP"));
                expectEquals (backupchip::labelFor (State::Failed), juce::String ("BACKUP !"));
                expect (backupchip::describe (State::Idle) != backupchip::describe (State::Writing));
            }
        }

        // ── The window: strips, EDIT rows, the banner click, the chip ────────
        static juce::File tempDir()
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-design-alarms-" + juce::Uuid().toString());
        }

        static void recordBaseTake (const juce::File& dir, int tracks, int blocks)
        {
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, tracks);
            std::vector<std::vector<float>> audio;
            std::vector<const float*> inputs;
            for (int t = 0; t < tracks; ++t)
            {
                rec.getTrack (t).armed.store (true);
                audio.emplace_back ((size_t) kBlock, 0.1f);
                inputs.push_back (audio.back().data());
            }
            rec.startRecording (dir);
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
            rec.stopRecording();
        }

        static ChannelStrip* stripFor (MainComponent& main, int track)
        {
            for (auto& s : main.strips)
                if (s != nullptr && s->getStripIndex() == track) return s.get();
            return nullptr;
        }

        void windowMarksAndReveals()
        {
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restore { [] { MainComponent::s_testConstruct = false; } };
            MainComponent main;
            testaudio::useSilentDevice (main.engine);
            main.setSize (1320, 820);
            const auto session = tempDir();
            recordBaseTake (session, 30, 20);
            expect (main.openSessionFolder (session, true) > 0);
            main.engine.prepareForTests (kRate, kBlock);
            main.engine.setTrackStereo (4, true);
            main.rebuildStrips();
            main.resized();
            main.editPage->refresh();

            beginTest ("The faulted strips mark themselves in MIXER and EDIT, and clear with the take");
            {
                EngineStatus status;
                status.recording = true;
                status.channelAlarms = { alarmOf (channelalarm::kChannel, 2, {}, "C"),
                                         alarmOf (channelalarm::kChannel, 4, {}, "OH"),
                                         alarmOf (channelalarm::kNoSignal, 7, {}, "TB") };
                main.updateCaptureWarnings (status);
                expect (stripFor (main, 2) != nullptr && stripFor (main, 2)->getInputAlarm() == Level::Lost);
                expect (stripFor (main, 4) != nullptr && stripFor (main, 4)->isStereo()
                        && stripFor (main, 4)->getInputAlarm() == Level::Lost, "the pair's strip");
                expect (stripFor (main, 7)->getInputAlarm() == Level::NoSignal);
                expect (stripFor (main, 3)->getInputAlarm() == Level::None);
                expect (main.editPage->inputAlarmForTests (2) == Level::Lost);
                expect (main.editPage->inputAlarmForTests (5) == Level::Lost, "the pair's EDIT row");
                expect (main.editPage->inputAlarmForTests (7) == Level::NoSignal);
                expect (main.editPage->inputAlarmForTests (3) == Level::None);

                bool sawFault = false, sawNotice = false;
                for (const auto& line : main.alarmBanner.getAlarms())
                {
                    if (line.text.startsWith ("! CHANNEL FAULT")) { sawFault = true; expectEquals (line.revealTrack, 2); }
                    if (line.text.startsWith ("No signal yet"))  { sawNotice = true; expectEquals (line.revealTrack, 7); }
                }
                expect (sawFault && sawNotice, main.alarmBanner.getText());

                main.updateCaptureWarnings (EngineStatus());
                expect (stripFor (main, 2)->getInputAlarm() == Level::None, "a mark outlived the take");
                expect (main.editPage->inputAlarmForTests (2) == Level::None);
                expect (! main.alarmBanner.isVisible());
            }

            beginTest ("A click on the fault line brings an off-screen strip into view");
            {
                main.switchView (MainComponent::View::Mix);
                main.stripsViewport.setViewPosition (0, 0);
                auto* far = stripFor (main, 28);
                expect (far != nullptr);
                expect (far->getRight() > main.stripsViewport.getViewArea().getRight(), "strip 29 should start off screen");
                EngineStatus status;
                status.recording = true;
                status.channelAlarms = { alarmOf (channelalarm::kChannel, 28, {}, "LAST") };
                main.updateCaptureWarnings (status);
                auto& banner = main.alarmBanner;
                banner.mouseUp (clickAt (banner, { 80, banner.getHeight() / 2 }));
                const auto view = main.stripsViewport.getViewArea();
                const auto b = far->getBounds();
                expect (view.getX() <= b.getX() && b.getRight() <= view.getRight() && view.getY() <= b.getY(),
                        "the strip, header first, stayed off screen: view " + view.toString() + " strip " + b.toString());

                main.switchView (MainComponent::View::Edit);
                main.editPage->refresh();
                main.editPage->resized();
                expect (! main.editPage->rowFullyInViewForTests (28), "row 29 should start off screen");
                banner.mouseUp (clickAt (banner, { 80, banner.getHeight() / 2 }));
                expect (main.editPage->rowFullyInViewForTests (28), "the EDIT row stayed off screen");
                main.switchView (MainComponent::View::Mix);
                main.updateCaptureWarnings (EngineStatus());
            }

            beginTest ("With no backup folder the chip reads BACKUP OFF with an amber dot, and it fits");
            {
                const auto before = main.engine.getRecorder().getBackupDirectory();
                main.engine.setBackupDirectory ({});
                main.updateBackupButton (EngineStatus());
                expectEquals (main.backupButton.getButtonText(), juce::String ("BACKUP OFF"));
                expect (main.backupButton.getStatusDot() == std::optional<juce::Colour> (brand::alertAmber));
                // Icon column, label and dot inside the chip (IconButton::paintButton).
                const float room = (float) main.backupButton.getWidth() - (16.0f + 10.0f) - 4.0f
                                 - (IconButton::kDotSize + (float) brand::space::xs);
                expect (juce::GlyphArrangement::getStringWidth (brand::type::uiLabel(), "BACKUP OFF") <= room,
                        "BACKUP OFF does not fit its chip");
                main.engine.setBackupDirectory (before);
            }

            main.condemnAllStrips();
            main.engine.setStripCount (0);
            session.deleteRecursively();
        }
    };

    static Design20261117AlarmsTests design20261117AlarmsTests;
}
