#include <juce_gui_extra/juce_gui_extra.h>

#include "UI/MainComponent.h"
#include "Theme/BrandColors.h"

class ZynforgeRecordingApp final : public juce::JUCEApplication
{
public:
    ZynforgeRecordingApp() = default;

    const juce::String getApplicationName()    override { return "Zynforge Recording"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed()          override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        // ZYNFORGE_RUN_TESTS=1 (or --run-tests on the cmd line) runs
        // every juce::UnitTest registered in the binary, prints the
        // results to stderr, and quits with a non-zero exit when any
        // test failed. Lets CI / scripts catch regressions without
        // standing up a separate test target.
        const bool envRun = juce::SystemStats::getEnvironmentVariable ("ZYNFORGE_RUN_TESTS", "0") != "0";
        if (envRun || commandLine.contains ("--run-tests"))
        {
            // GUI-app bundles don't get a controlling TTY when
            // launched outside Terminal -- stdout / stderr go
            // nowhere. Write the test report to a deterministic
            // file (~/Library/Logs/Zynforge/test-report.log) AND
            // mirror to stderr so terminal launches still see it.
            const auto reportDir = juce::File::getSpecialLocation (
                                       juce::File::userApplicationDataDirectory)
                                       .getChildFile ("Logs/Zynforge");
            reportDir.createDirectory();
            const auto reportFile = reportDir.getChildFile ("test-report.log");
            reportFile.deleteFile();

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

            juce::UnitTestRunner runner;
            runner.setAssertOnFailure (false);
            runner.runAllTests();
            int failed = 0;
            for (int i = 0; i < runner.getNumResults(); ++i)
                failed += runner.getResult (i)->failures;
            const auto summary = juce::String ("[zynforge tests] ")
                               + juce::String (runner.getNumResults())
                               + " test groups, " + juce::String (failed)
                               + " failure(s)";
            std::cerr << summary << "\n";
            reportFile.appendText (summary + "\n");

            juce::Logger::setCurrentLogger (nullptr);
            setApplicationReturnValue (failed == 0 ? 0 : 1);
            quit();
            return;
        }
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override { mainWindow.reset(); }

    void systemRequestedQuit() override
    {
        // Route through MainComponent::confirmAndQuit so an unsaved
        // session / running recording surfaces the appropriate prompt
        // before we shut down. confirmAndQuit calls quit() on user OK.
        if (mainWindow != nullptr)
            if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            {
                mc->confirmAndQuit();
                return;
            }
        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              zynforge::brand::brandDeep,
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, true);
            setContentOwned (new MainComponent(), true);
            centreWithSize (1320, 820);
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (ZynforgeRecordingApp)
