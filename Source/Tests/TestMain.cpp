// Entry point of the ZynforgeRecordingTests app: runs every registered
// juce::UnitTest (or those matching --test-filter=<substring>) against the
// shared ZynforgeCore library, writes the report and exits non-zero on any
// failure. The tests used to be compiled into the shipping recorder and run
// via its --run-tests switch; they now live only in this separate bundle.
//
//   "ZynforgeRecordingTests.app/Contents/MacOS/Zynforge Recording Tests" \
//       [--test-filter=<substring>] [--test-report=/absolute/path.log]
//
// The bundle embeds the ZynforgeCapture helper exactly like the recorder, so
// the daemon tests exercise the same discovery and launch path.

#include <juce_gui_extra/juce_gui_extra.h>
#include <csignal>
#include <iostream>

#include "../Audio/SettingsFile.h"
#include "../Audio/StripNames.h"
#include "../Audio/StripGains.h"
#include "../Theme/BrandTokens.h"

class ZynforgeRecordingTestsApp final : public juce::JUCEApplication
{
public:
    // JUCE keys its single-instance lock by application name: a fixed test
    // identity never forwards arguments to a running recorder, while still
    // excluding overlapping test processes.
    const juce::String getApplicationName() override    { return "Zynforge Recording Tests"; }
    const juce::String getApplicationVersion() override
    {
       #ifdef JUCE_APPLICATION_VERSION_STRING
        return JUCE_APPLICATION_VERSION_STRING;
       #else
        return "0.0.0";
       #endif
    }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String&) override
    {
        // Same process setup as the recorder: companion/stream tests write to
        // sockets a client may close.
        std::signal (SIGPIPE, SIG_IGN);
        // Never touch the engineer's real preferences, before anything runs.
        zynforge::isolateSettings();

        juce::String testFilter, reportPath;
        bool hasTestFilter = false;
        for (const auto& parameter : getCommandLineParameterArray())
        {
            const auto argument = parameter.unquoted();
            if (argument.startsWith ("--test-filter="))
            {
                hasTestFilter = true;
                testFilter = argument.substring (juce::String ("--test-filter=").length());
            }
            else if (argument.startsWith ("--test-report="))
            {
                reportPath = argument.substring (juce::String ("--test-report=").length());
                if (! juce::File::isAbsolutePath (reportPath))
                {
                    std::cerr << "[zynforge tests] ERROR: --test-report requires an absolute file path\n";
                    setApplicationReturnValue (2);
                    quit();
                    return;
                }
            }
        }

        // GUI-app bundles don't get a controlling TTY when
        // launched outside Terminal -- stdout / stderr go
        // nowhere. Keep the normal report path unless the audit runner
        // explicitly requests its own --test-report=/absolute/path.
        const auto defaultReportDir = juce::File::getSpecialLocation (
                                   juce::File::userApplicationDataDirectory)
                                   .getChildFile ("Logs/Zynforge");
        const auto reportFile = reportPath.isEmpty()
            ? defaultReportDir.getChildFile ("test-report.log") : juce::File (reportPath);
        if (reportFile.getParentDirectory().createDirectory().failed()
            || (reportFile.exists() && ! reportFile.deleteFile())
            || reportFile.create().failed())
        {
            std::cerr << "[zynforge tests] ERROR: cannot create test report: "
                      << reportFile.getFullPathName().toRawUTF8() << "\n";
            setApplicationReturnValue (2);
            quit();
            return;
        }

        struct DualLogger : public juce::Logger
        {
            juce::File f;
            explicit DualLogger (const juce::File& file) : f (file) {}
            void logMessage (const juce::String& m) override
            {
                std::cerr << m.toRawUTF8() << "\n";
                f.appendText (m + "\n");
            }
        };
        DualLogger dual (reportFile);
        juce::Logger::setCurrentLogger (&dual);

        // Isolate persistent state so the tests never write to the
        // engineer's real per-channel names / gains / pans.
        zynforge::StripNames::setTestMode (true);
        zynforge::StripGains::setTestMode (true);

        juce::UnitTestRunner runner;
        runner.setAssertOnFailure (false);
        int failed = 0;
        bool invalidSelection = false;
        if (hasTestFilter)
        {
            juce::Array<juce::UnitTest*> selected;
            if (testFilter.isNotEmpty())
                for (auto* test : juce::UnitTest::getAllTests())
                    if (test->getName().containsIgnoreCase (testFilter))
                        selected.add (test);
            if (selected.isEmpty())
            {
                invalidSelection = true;
                failed = 1;
                dual.logMessage ("[zynforge tests] ERROR: no registered test names match --test-filter="
                                 + testFilter);
            }
            else
            {
                dual.logMessage ("[zynforge tests] Selected " + juce::String (selected.size())
                                 + " registered test suite(s) matching: " + testFilter);
                runner.runTests (selected);
            }
        }
        else
        {
            runner.runAllTests();
        }
        for (int i = 0; i < runner.getNumResults(); ++i)
            failed += runner.getResult (i)->failures;
        const auto summary = juce::String ("[zynforge tests] ")
                           + juce::String (runner.getNumResults())
                           + " test groups, " + juce::String (failed)
                           + " failure(s)";
        std::cerr << summary << "\n";
        reportFile.appendText (summary + "\n");

        juce::Logger::setCurrentLogger (nullptr);
        setApplicationReturnValue (invalidSelection ? 2 : failed == 0 ? 0 : 1);
        quit();
        return;
    }

    void shutdown() override
    {
        // Release cached fonts while CoreText is alive; at __cxa_finalize it
        // is gone and the destructors terminate() the process (see Main.cpp).
        zynforge::brand::type::clearFontCache();
    }

    void systemRequestedQuit() override { quit(); }
};

START_JUCE_APPLICATION (ZynforgeRecordingTestsApp)
