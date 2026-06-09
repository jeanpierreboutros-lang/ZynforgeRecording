// Headless tests for launch-time crash telemetry
// (Source/Audio/CrashReportScan.h): prefix + mtime filtering, newest-first
// ordering, and the .ips header summary extraction.

#include <juce_core/juce_core.h>

#include "../Audio/CrashReportScan.h"

namespace zynforge
{
    class CrashScanTests final : public juce::UnitTest
    {
    public:
        CrashScanTests() : juce::UnitTest ("Crash report scan", "zynforge") {}

        void runTest() override
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-crash-" + juce::Uuid().toString());
            dir.createDirectory();

            const auto now = juce::Time::getCurrentTime();
            auto makeReport = [&dir] (const juce::String& name, juce::Time mtime,
                                      const juce::String& content = "{}")
            {
                auto f = dir.getChildFile (name);
                f.replaceWithText (content);
                f.setLastModificationTime (mtime);
                return f;
            };

            beginTest ("findNewReports filters by prefix and modification time");
            {
                makeReport ("Zynforge Recording-old.ips",  now - juce::RelativeTime::days (3));
                auto fresh = makeReport ("Zynforge Recording-new.ips",
                                         now - juce::RelativeTime::minutes (5));
                makeReport ("OtherApp-new.ips", now);                 // wrong prefix
                makeReport ("Zynforge-note.txt", now);                // wrong extension

                const auto found = crashscan::findNewReports (
                    dir, now - juce::RelativeTime::days (1));
                expectEquals (found.size(), 1);
                if (! found.isEmpty())
                    expectEquals (found.getFirst().getFileName(), fresh.getFileName());
            }

            beginTest ("findNewReports orders newest first");
            {
                makeReport ("Zynforge Recording-a.ips", now - juce::RelativeTime::minutes (30));
                makeReport ("Zynforge Recording-b.ips", now - juce::RelativeTime::minutes (1));
                const auto found = crashscan::findNewReports (
                    dir, now - juce::RelativeTime::hours (1));
                expect (found.size() >= 3);
                for (int i = 1; i < found.size(); ++i)
                    expect (found[i - 1].getLastModificationTime()
                              >= found[i].getLastModificationTime(),
                            "reports not sorted newest-first");
            }

            beginTest ("summarize extracts timestamp / signal / type from the .ips header");
            {
                const juce::String ips =
                    "{\"app_name\":\"Zynforge Recording\",\"timestamp\":\"2026-06-09 13:04:00.00 +0300\"}\n"
                    "{\"exception\":{\"type\":\"EXC_BAD_ACCESS\",\"signal\":\"SIGSEGV\"},"
                    "\"terminationReason\":\"Namespace SIGNAL, Code 11\"}";
                auto f = makeReport ("Zynforge Recording-sum.ips", now, ips);
                const auto s = crashscan::summarize (f);
                expect (s.contains ("2026-06-09 13:04:00"));
                expect (s.contains ("EXC_BAD_ACCESS"));
                expect (s.contains ("SIGSEGV"));
                expect (s.contains ("Namespace SIGNAL"));
            }

            beginTest ("missing directory yields no reports");
            {
                dir.deleteRecursively();
                expect (crashscan::findNewReports (dir, juce::Time (0)).isEmpty());
            }
        }
    };

    static CrashScanTests crashScanTests;
}
