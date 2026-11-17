// Design pass 2026-11-17, panels: the clock plate's label-over-value groups and
// the order they give way, EDIT LOCKED on the plate, EDIT's zoom buttons in the
// ruler's header column, the perf panel's type floor, the EDIT row buttons, the
// shortcuts sheet's wrapping rows, the two dialogs that drew a second title bar,
// Backup's secondary Add button and empty hint, Session Info's file type, and
// PATCH's column-header names.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "../UI/BackupMirrorDialog.h"
#include "../UI/BigClockPanel.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"
#include "../UI/FloatingLaunch.h"
#include "../UI/LedMeter.h"
#include "../UI/MainComponent.h"
#include "../UI/PatchPage.h"
#include "../UI/PerfDashboard.h"
#include "../UI/SessionInfoDialog.h"
#include "../UI/ShortcutSheet.h"
#include "../Theme/BrandTokens.h"

#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class Design20261117PanelsTests final : public juce::UnitTest
    {
    public:
        Design20261117PanelsTests() : juce::UnitTest ("Design panels 2026-11-17", "zynforge") {}

        using G = BigClockPanel::Group;

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
            return predicate();
        }

        struct Fixture
        {
            juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zf-design-panels-" + juce::Uuid().toString());
            MainComponent main;
            AudioEngine&  engine { main.engine };

            explicit Fixture (int strips)
            {
                engine.clearAllStripOverrides();
                engine.prepareForTests (48000.0, 512);
                engine.setStripCount (strips);
                dir.getChildFile ("Audio Files").createDirectory();
                engine.setActiveSessionDir (dir);
                engine.claimMixerFor (dir);
                engine.loadSession (dir);
                engine.setStripCount (strips);
            }
            ~Fixture()
            {
                engine.clearAllStripOverrides();
                dir.deleteRecursively();
            }
        };

        // Whichever groups are on the plate, every one later in the give-way
        // order is too: a group never goes while one meant to go first stays.
        void expectGivesWayInOrder (const BigClockPanel::GroupMask& shown, const juce::String& where)
        {
            const auto order = BigClockPanel::giveWayOrder();
            for (size_t i = 0; i < order.size(); ++i)
                if (shown[(size_t) order[i]])
                    for (size_t j = i + 1; j < order.size(); ++j)
                        expect (shown[(size_t) order[j]], where + ": a group gave way before one meant to go first");
        }

        static BigClockPanel::GroupMask maskOf (const BigClockPanel& plate)
        {
            BigClockPanel::GroupMask m {};
            const auto b = plate.getGroupBounds();
            for (int i = 0; i < BigClockPanel::kNumGroups; ++i) m[(size_t) i] = ! b[(size_t) i].isEmpty();
            return m;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;

            testClockPlate();
            testPerfPanel();
            testPatchHeaders();
            testShortcutsSheet();
            testBackupDialog();
            testWindowsWithMainComponent();
        }

        void testClockPlate()
        {
            beginTest ("The plate's small groups give way in order; TIME LEFT never does");
            {
                const BigClockPanel::GroupWidths w { 40.0f, 60.0f, 50.0f, 60.0f, 80.0f };
                const float gap = 16.0f;
                auto all = BigClockPanel::fitGroups (w, 1000.0f, gap);
                for (bool b : all) expect (b, "a group gave way with room to spare");
                expectWithinAbsoluteError (BigClockPanel::groupsWidth (w, all, gap), 290.0f + 4.0f * gap, 0.01f);

                // Exactly the full width still fits; a pixel less loses MARKERS first.
                expect (BigClockPanel::fitGroups (w, 354.0f, gap)[(size_t) G::Markers]);
                auto m = BigClockPanel::fitGroups (w, 353.0f, gap);
                expect (! m[(size_t) G::Markers] && m[(size_t) G::LastWrite] && m[(size_t) G::Free] && m[(size_t) G::Missed]);
                m = BigClockPanel::fitGroups (w, 230.0f, gap);    // TL+FREE+MISSED = 180 + 32
                expect (! m[(size_t) G::Markers] && ! m[(size_t) G::LastWrite] && m[(size_t) G::Free] && m[(size_t) G::Missed]);
                m = BigClockPanel::fitGroups (w, 140.0f, gap);    // TL+MISSED = 120 + 16
                expect (! m[(size_t) G::Free] && m[(size_t) G::Missed]);
                m = BigClockPanel::fitGroups (w, 10.0f, gap);
                expect (m[(size_t) G::TimeLeft], "TIME LEFT gave way");
                for (auto g : BigClockPanel::giveWayOrder()) expect (! m[(size_t) g]);

                // A group that is not on the plate stays off however much room.
                auto noFree = w;
                noFree[(size_t) G::Free] = 0.0f;
                expect (! BigClockPanel::fitGroups (noFree, 1000.0f, gap)[(size_t) G::Free]);
                for (float room = 0.0f; room < 400.0f; room += 7.0f)
                    expectGivesWayInOrder (BigClockPanel::fitGroups (w, room, gap), "room " + juce::String (room));
            }

            beginTest ("The plate's groups read a small label over their value");
            {
                expectEquals (BigClockPanel::missedValueText (0), juce::String ("0"));
                expectEquals (BigClockPanel::missedValueText (4800), juce::String ("4800!"),
                              "a miss must not rest on colour alone");
                expectEquals (BigClockPanel::timeLeftLabelText (false, false), juce::String ("TIME LEFT"));
                expectEquals (BigClockPanel::timeLeftLabelText (false, true),  juce::String ("PRIMARY TIME LEFT"));
                expectEquals (BigClockPanel::timeLeftLabelText (true,  false), juce::String ("DISK FREE"));

                BigClockPanel plate;
                plate.setSize (1242, 64);    // MIXER at 1600 px
                plate.setMode (BigClockPanel::Mode::Recording);
                plate.setElapsed (48000 * 29, 48000.0);
                plate.setMarkers (3);
                plate.setDiskInfo (32.7, 0, 0, 3.0 * 3600.0, BigClockPanel::DiskReading::Fresh);
                const auto b = plate.getGroupBounds();
                for (int i = 0; i < BigClockPanel::kNumGroups; ++i)
                    expect (! b[(size_t) i].isEmpty(), "a group gave way on a wide plate: " + juce::String (i));
                expectEquals (plate.getGroupLabel (G::Missed),    juce::String ("MISSED"));
                expectEquals (plate.getGroupValue (G::Missed),    juce::String ("0"));
                expectEquals (plate.getGroupLabel (G::LastWrite), juce::String ("LAST WRITE"));
                expectEquals (plate.getGroupValue (G::LastWrite), juce::String ("0 ms"));
                expectEquals (plate.getGroupLabel (G::Markers),   juce::String ("MARKERS"));
                expectEquals (plate.getGroupValue (G::Markers),   juce::String ("3"));
                expectEquals (plate.getGroupLabel (G::Free),      juce::String ("FREE"));
                expectEquals (plate.getGroupValue (G::Free),      juce::String ("32.7 GB"));
                expectEquals (plate.getGroupLabel (G::TimeLeft),  juce::String ("TIME LEFT"));
                expectEquals (plate.getGroupValue (G::TimeLeft),  juce::String ("3h00m"));
                // Left to right, no overlap, inside the plate, TIME LEFT at the right edge.
                for (int i = 1; i < BigClockPanel::kNumGroups; ++i)
                    expect (b[(size_t) i].getX() >= b[(size_t) i - 1].getRight(), "groups overlap");
                for (auto& r : b) expect (plate.getLocalBounds().toFloat().contains (r), "a group left the plate");
                expect (b[(size_t) G::TimeLeft].getRight() > (float) plate.getWidth() - 40.0f, "TIME LEFT is not at the right edge");
                // The timer keeps the middle: the groups sit right of centre.
                expect (b[(size_t) G::Missed].getX() > (float) plate.getWidth() * 0.5f, "the groups crowd the timer");
            }

            beginTest ("Before a take's estimate the large group is DISK FREE and FREE is not repeated");
            {
                BigClockPanel plate;
                plate.setSize (900, 48);     // EDIT at 1600 px
                plate.setDiskInfo (32.7, 0, 0, 0.0, BigClockPanel::DiskReading::Fresh);
                const auto shown = maskOf (plate);
                expect (! shown[(size_t) G::Free], "free space shown twice");
                expect (shown[(size_t) G::TimeLeft]);
                expectEquals (plate.getGroupLabel (G::TimeLeft), juce::String ("DISK FREE"));
                expectEquals (plate.getGroupValue (G::TimeLeft), juce::String ("32.7 GB"));
            }

            beginTest ("STALE and NO DRIVE keep their places on the plate");
            {
                BigClockPanel plate;
                plate.setSize (1242, 64);
                plate.setDiskInfo (32.7, 0, 0, 3600.0, BigClockPanel::DiskReading::PrimaryStale);
                expectEquals (plate.getGroupValue (G::TimeLeft), juce::String ("STALE"));
                expectEquals (plate.getGroupValue (G::Free),     juce::String ("STALE"));
                plate.setDiskInfo (32.7, 0, 0, 3600.0, BigClockPanel::DiskReading::CopyStale);
                expectEquals (plate.getGroupLabel (G::TimeLeft), juce::String ("PRIMARY TIME LEFT"));
                plate.setDiskInfo (0.0, 0, 0, 0.0, BigClockPanel::DiskReading::DriveMissing);
                expectEquals (plate.getGroupLabel (G::TimeLeft), juce::String ("DISK FREE"));
                expectEquals (plate.getGroupValue (G::TimeLeft), juce::String ("NO DRIVE"));
            }

            beginTest ("On EDIT's narrow plate the small groups give way first and the timer keeps its room");
            {
                BigClockPanel plate;
                plate.setMode (BigClockPanel::Mode::Recording);
                plate.setElapsed (48000 * 37, 48000.0);
                plate.setMarkers (12);
                plate.setDiskInfo (32.7, 4, 0, 3.0 * 3600.0, BigClockPanel::DiskReading::Fresh);
                for (int w = 300; w <= 1300; w += 25)
                {
                    plate.setSize (w, 48);
                    const auto shown = maskOf (plate);
                    expect (shown[(size_t) G::TimeLeft], "TIME LEFT gave way at " + juce::String (w));
                    expectGivesWayInOrder (shown, "width " + juce::String (w));
                    // Whatever is drawn sits right of the state block and the timer.
                    const auto timerFont = brand::type::mono (juce::jmin (36.0f * 0.95f, brand::type::h_hero), true);
                    const float timerW = juce::GlyphArrangement::getStringWidth (timerFont, "00:00:37");
                    const auto b = plate.getGroupBounds();
                    for (int i = 0; i < BigClockPanel::kNumGroups; ++i)
                        if (! b[(size_t) i].isEmpty() && i != (int) G::TimeLeft)
                            expect (b[(size_t) i].getX() >= 10.0f + 112.0f + timerW + 2.0f * (float) brand::space::xl,
                                    "a small group ran into the timer at " + juce::String (w));
                }
                plate.setSize (516, 48);    // EDIT at the minimum window
                const auto narrow = maskOf (plate);
                expect (! narrow[(size_t) G::Markers], "MARKERS stayed on the narrowest plate");
            }

            beginTest ("EDIT LOCKED shows on the plate under the state, only while set");
            {
                BigClockPanel plate;
                plate.setSize (900, 48);
                expect (! plate.isEditLockShown());
                expect (plate.getTooltip().isEmpty());
                plate.setMode (BigClockPanel::Mode::Recording);
                plate.setEditLocked (true);
                expect (plate.isEditLockShown());
                plate.setEditLocked (false);
                expect (! plate.isEditLockShown());
            }
        }

        void testPerfPanel()
        {
            beginTest ("The perf panel's loudness line fits in both views at the minimum window");
            {
                PerfDashboard perf;
                for (int h : { 48, 64 })
                {
                    perf.setSize (332, h);
                    expect (perf.loudnessFits(), "the readout does not fit at height " + juce::String (h));
                }
                // The widest format line at the panel's smallest size (caption) fits its column.
                expect (juce::GlyphArrangement::getStringWidth (brand::type::mono (brand::type::h_caption, true), "192.0 kHz") <= 60.0f);
                for (auto label : { std::pair<const char*, float> { "AUDIO", 34.0f }, { "DISK", 28.0f }, { "BUF", 22.0f } })
                    expect (juce::GlyphArrangement::getStringWidth (brand::type::mono (brand::type::h_caption, true), label.first) <= label.second,
                            juce::String (label.first) + " overflows its column at caption size");
            }
        }

        void testPatchHeaders()
        {
            beginTest ("PATCH headers show a long name on two lines, else cut with an ellipsis");
            {
                const auto font = brand::type::uiLabel();
                const auto ellipsis = juce::String::charToString ((juce::juce_wchar) 0x2026);
                const float w = 50.0f;    // a 60 px column
                auto fitsAll = [&] (const juce::StringArray& lines)
                {
                    for (auto& l : lines) if (juce::GlyphArrangement::getStringWidth (font, l) > w + 0.01f) return false;
                    return true;
                };

                auto lines = PatchPage::headerNameLines ("KICK IN", font, w);
                expectEquals (lines.size(), 1);
                expectEquals (lines[0], juce::String ("KICK IN"));

                lines = PatchPage::headerNameLines ("SNARE TOP", font, w);
                expectEquals (lines.joinIntoString ("|"), juce::String ("SNARE|TOP"), "a two-word name was not split");
                lines = PatchPage::headerNameLines ("FLOOR TOM", font, w);
                expectEquals (lines.joinIntoString ("|"), juce::String ("FLOOR|TOM"));

                lines = PatchPage::headerNameLines ("TALKBACK", font, w);
                expectEquals (lines.size(), 1);
                expect (lines[0].endsWith (ellipsis), "a cut name carries no ellipsis: " + lines[0]);
                expect (fitsAll (lines));

                lines = PatchPage::headerNameLines ("LEAD VOCAL SPARE WIRELESS", font, w);
                expect (lines.size() == 2 && lines[1].endsWith (ellipsis), lines.joinIntoString ("|"));
                expect (fitsAll (lines));
                // Nothing cut short without an ellipsis, at any width.
                for (float width = 20.0f; width < 120.0f; width += 3.0f)
                    for (auto name : { "SNARE TOP", "TALKBACK", "OH", "BASS DI AMP", "ACOUSTIC GUITAR LEFT" })
                    {
                        const auto l = PatchPage::headerNameLines (name, font, width);
                        const auto joined = l.joinIntoString (" ");
                        expect (joined == juce::String (name) || joined.endsWith (ellipsis),
                                juce::String (name) + " read as " + joined + " at " + juce::String (width));
                    }

                expect (PatchPage::cornerChannelLabel().contains (juce::String::charToString ((juce::juce_wchar) 0x2192)));
                expect (! PatchPage::cornerChannelLabel().contains ("->"));
            }
        }

        void testShortcutsSheet()
        {
            beginTest ("Keyboard shortcut rows wrap and grow; nothing is cut with an ellipsis");
            {
                using namespace shortcuts::table;
                // The dialog's table: 700 px less the body padding and the scrollbar.
                const int contentW = 640;
                bool sawSpace = false;
                int total = kTopPad;
                for (const auto& s : shortcuts::sheet())
                {
                    total += kHeaderH + kSectionGap;
                    for (const auto& r : s.rows)
                    {
                        const auto desc = layOut (r.description, descriptionFont(), {}, descriptionWidth (contentW));
                        int covered = 0;
                        for (int i = 0; i < desc.getNumLines(); ++i)
                            covered = juce::jmax (covered, desc.getLine (i).stringRange.getEnd());
                        expectEquals (covered, r.description.length(), "a row lost text: " + r.keys);
                        const int h = rowHeight (r, contentW);
                        expect (h >= kRowH);
                        expect ((float) h >= desc.getHeight(), "a row is shorter than its text: " + r.keys);
                        if (desc.getNumLines() > 1) expect (h > kRowH, "a wrapped row kept one line's height: " + r.keys);
                        total += h;
                        if (r.keys == "Space")
                        {
                            sawSpace = true;
                            expect (desc.getNumLines() >= 2, "the Space row did not wrap");
                        }
                    }
                }
                expect (sawSpace, "setup: the Space row");
                expectEquals (contentHeight (contentW), total + kTopPad);
            }
        }

        void testBackupDialog()
        {
            beginTest ("Backup: Add mirror is a secondary button, and an empty list says what a mirror is");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine engine;
                // The dialog edits its own copy; nothing here reaches the engine
                // (no Apply), so the saved mirrors stay as they are.
                backupdlg::Content dialog (engine, {});
                dialog.setSize (760, 540);
                expect (! dialog.addButtonIsPrimary(), "Add mirror competes with Apply");
                while (! dialog.working.empty()) dialog.removeRow (0);
                expect (dialog.isEmptyHintShown(), "no hint on an empty mirror list");
                expect (juce::String (backupdlg::Content::kNoMirrorsHint).startsWith ("No mirror drives."));
                expect (dialog.emptyHint.getBounds().intersects (dialog.rowView.getBounds()), "the hint is not in the list's place");
                dialog.addBlankRow();
                expect (! dialog.isEmptyHintShown(), "the hint stayed over a row");
                dialog.removeRow (0);
                expect (dialog.isEmptyHintShown(), "the hint did not come back");
            }
        }

        static juce::DialogWindow* findDialog (const juce::String& title)
        {
            for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
                if (auto* d = dynamic_cast<juce::DialogWindow*> (juce::TopLevelWindow::getTopLevelWindow (i)))
                    if (d->getName() == title) return d;
            return nullptr;
        }

        void testWindowsWithMainComponent()
        {
            Fixture f (2);

            beginTest ("Backup and Keyboard shortcuts use the system title bar and still close for a take");
            {
                // A modal another suite left open would be closed with ours.
                closeModalsForTake();
                pumpUntil ([] { return false; }, 100);
                for (auto launch : { std::pair<juce::String, std::function<void()>> { "Keyboard shortcuts", [&f] { f.main.showKeyboardShortcuts(); } },
                                     { "Backup & Mirror Drives", [&f] { BackupMirrorDialog::launch (f.engine, {}); } } })
                {
                    launch.second();
                    juce::Component::SafePointer<juce::DialogWindow> d (findDialog (launch.first));
                    expect (d != nullptr, "setup: " + launch.first);
                    if (d == nullptr) continue;
                    expect (d->isUsingNativeTitleBar(), launch.first + " draws its own title bar over the FORGE header");
                    expect ((bool) d->getProperties()[kClosesForTake], launch.first + " no longer closes for a take");
                    expect (closeModalsForTake() >= 1, launch.first + " was not among the modals a take closes");
                    pumpUntil ([&d] { return d == nullptr || ! d->isCurrentlyModal(); }, 2000);
                    expect (d == nullptr || ! d->isCurrentlyModal(), launch.first + " stayed open over a take");
                    pumpUntil ([] { return false; }, 50);
                }
            }

            beginTest ("Session Info names the file type once: the format, with the bit depth in its own field");
            {
                SessionInfoDialog::Fields shown;
                SessionInfoDialog::launchForTests = [&shown] (SessionInfoDialog::Fields x) { shown = x; };
                const juce::ScopeGuard reset { [] { SessionInfoDialog::launchForTests = nullptr; } };
                f.main.showSessionInfo();
                expect (shown.captureFormat == "WAV" || shown.captureFormat == "AIFF" || shown.captureFormat == "FLAC",
                        "file type reads " + shown.captureFormat);
                expect (! shown.captureFormat.containsIgnoreCase ("bit"), "the bit depth is said twice");
                expect (shown.bitDepth.contains ("-bit"), "bit depth reads " + shown.bitDepth);
                if (f.engine.getRecorder().getCaptureFormat() == CaptureFormat::Wav24)
                {
                    expectEquals (shown.captureFormat, juce::String ("WAV"));
                    expectEquals (shown.bitDepth, juce::String ("24-bit"));
                }
            }

            beginTest ("EDIT's zoom buttons sit in the ruler's header column, never over the lanes");
            {
                auto& page = *f.main.editPage;
                page.setBounds (0, 0, 1400, 600);
                page.refresh();
                page.resized();
                juce::Rectangle<int> lanes;
                for (auto* child : page.getChildren())
                    if (auto* v = dynamic_cast<juce::Viewport*> (child)) lanes = v->getBounds();
                expect (! lanes.isEmpty(), "setup: the lanes");
                int found = 0;
                for (auto* child : page.getChildren())
                    if (auto* b = dynamic_cast<juce::TextButton*> (child))
                    {
                        const auto t = b->getButtonText();
                        if (t != "V+" && t != "V-" && t != "H+" && t != "H-" && t != "FOLLOW") continue;
                        ++found;
                        expect (! b->getBounds().intersects (lanes), t + " covers the lanes");
                        expect (b->getBottom() <= lanes.getY(), t + " is below the ruler");
                        expect (b->getRight() <= brand::space::editHeaderW, t + " is over the time scale");
                        expect (b->getX() >= 80, t + " covers the ruler's labels");
                        expect (b->getWidth() >= 44 && b->getHeight() >= 26, t + " shrank");
                    }
                expectEquals (found, 5, "the five zoom controls");
            }

            beginTest ("EDIT row I / R / S / M buttons reach 26 x 18 at the default row and keep clear of the meter and combos");
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                juce::AudioThumbnailCache cache (4);
                for (int height : { 80, 110, 50, 28 })
                {
                    EditPage::TrackRow row (0, false, f.engine, formats, cache);
                    row.setSize (row.getHeaderWidth() + 600, height);
                    row.resized();
                    std::vector<juce::Rectangle<int>> buttons, others;
                    for (auto* child : row.getChildren())
                    {
                        if (! child->isVisible()) continue;
                        if (auto* b = dynamic_cast<juce::Button*> (child))
                        {
                            const auto t = b->getButtonText();
                            if (t == "I" || t == "R" || t == "S" || t == "M") { buttons.push_back (b->getBounds()); continue; }
                        }
                        if (dynamic_cast<juce::ComboBox*> (child) != nullptr || dynamic_cast<LedMeter*> (child) != nullptr)
                            others.push_back (child->getBounds());
                    }
                    expectEquals ((int) buttons.size(), 4, "setup: four row buttons");
                    for (auto& b : buttons)
                    {
                        if (height >= 80)
                            expect (b.getWidth() >= 26 && b.getHeight() >= 18 + 4,   // + the look-and-feel's inset
                                    "a row button is " + juce::String (b.getWidth()) + " x " + juce::String (b.getHeight())
                                        + " at row height " + juce::String (height));
                        expect (b.getBottom() <= height, "a row button left the row");
                        for (auto& o : others)
                            expect (! b.intersects (o), "a row button overlaps the meter or a combo at height " + juce::String (height));
                        for (auto& c : buttons)
                            expect (&c == &b || ! b.intersects (c), "row buttons overlap each other");
                    }
                }
            }
        }
    };

    static Design20261117PanelsTests design20261117PanelsTests;
}
