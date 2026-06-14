#include <juce_gui_extra/juce_gui_extra.h>
#include <csignal>

#include "UI/MainComponent.h"
#include "Theme/BrandColors.h"
#include "Theme/BrandTokens.h"
#include "Audio/StripNames.h"
#include "Audio/StripGains.h"

// Branded launch splash: a borderless rounded card shown on top for a few
// seconds before the main window opens. Paints the brand (forge-Z over a
// red scope wave, matching the app icon) + product name + author.
class SplashWindow final : public juce::Component
{
public:
    SplashWindow (juce::String titleText, juce::String subtitleText)
        : title (std::move (titleText)), subtitle (std::move (subtitleText))
    {
        setOpaque (false);
        setSize (580, 340);
        setAlwaysOnTop (true);
        addToDesktop (juce::ComponentPeer::windowIsTemporary
                      | juce::ComponentPeer::windowHasDropShadow);
        if (auto* disp = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            setBounds (getLocalBounds().withCentre (disp->userArea.getCentre()));
        setVisible (true);
        toFront (true);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = 20.0f;

        g.setGradientFill (juce::ColourGradient (
            zynforge::brand::bgPanel.brighter (0.10f), r.getCentreX(), r.getY(),
            zynforge::brand::bgDeep,                   r.getCentreX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, rad);
        g.setColour (zynforge::brand::gloss (0.12f));
        g.drawRoundedRectangle (r, rad, 1.0f);

        // 'Z' mark with a faint red scope wave behind it (icon motif).
        auto zArea = juce::Rectangle<float> (0, 0, 92.0f, 92.0f)
                         .withCentre ({ r.getCentreX(), r.getY() + 96.0f });
        juce::Path wave;
        const float wx0 = r.getX() + 70.0f, wx1 = r.getRight() - 70.0f, wamp = 20.0f;
        const float wy  = zArea.getCentreY();
        for (int i = 0; i <= 160; ++i)
        {
            const float fx  = (float) i / 160.0f;
            const float x   = wx0 + (wx1 - wx0) * fx;
            const float env = std::sin (fx * juce::MathConstants<float>::pi);
            const float y   = wy - std::sin (fx * juce::MathConstants<float>::pi * 5.0f) * wamp * env;
            if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
        }
        g.setColour (zynforge::brand::accentRecord.withAlpha (zynforge::brand::alpha::muted));
        g.strokePath (wave, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        drawZ (g, zArea);

        g.setColour (zynforge::brand::brandOrange);
        g.setFont (zynforge::brand::type::ui (30.0f, true));
        g.drawText (title, r.withTrimmedTop (160.0f).removeFromTop (40.0f),
                    juce::Justification::centred, false);

        g.setColour (zynforge::brand::textMuted);
        g.setFont (zynforge::brand::type::ui (16.0f, false));
        g.drawText (subtitle, r.withTrimmedTop (206.0f).removeFromTop (28.0f),
                    juce::Justification::centred, false);
    }

    // New ZynForge forge-mark: orange hexagon + rising double-chevron + dot,
    // matching the app icon / dialog badge (replaces the old forged-Z).
    static void drawZ (juce::Graphics& g, juce::Rectangle<float> b)
    {
        const auto xf = juce::AffineTransform::scale (b.getWidth(), b.getHeight())
                                               .translated (b.getX(), b.getY());
        juce::Path hex;
        hex.startNewSubPath (0.50f, 0.05f); hex.lineTo (0.92f, 0.28f); hex.lineTo (0.92f, 0.72f);
        hex.lineTo (0.50f, 0.95f); hex.lineTo (0.08f, 0.72f); hex.lineTo (0.08f, 0.28f); hex.closeSubPath();
        juce::Path chv;
        chv.startNewSubPath (0.28f, 0.64f); chv.lineTo (0.50f, 0.41f); chv.lineTo (0.72f, 0.64f);
        chv.startNewSubPath (0.36f, 0.73f); chv.lineTo (0.50f, 0.58f); chv.lineTo (0.64f, 0.73f);
        hex.applyTransform (xf); chv.applyTransform (xf);

        g.setColour (zynforge::brand::brandOrange.withAlpha (zynforge::brand::alpha::subtle));
        g.fillPath (hex);
        g.setColour (zynforge::brand::brandOrange);
        g.strokePath (hex, juce::PathStrokeType (juce::jmax (1.5f, b.getWidth() * 0.035f)));
        g.strokePath (chv, juce::PathStrokeType (juce::jmax (2.0f, b.getWidth() * 0.075f),
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
        // Spark dot above the chevron.
        const float s = b.getWidth() * 0.05f;
        g.fillEllipse (b.getCentreX() - s, b.getY() + b.getHeight() * 0.27f - s, s * 2.0f, s * 2.0f);
    }

private:
    juce::String title, subtitle;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SplashWindow)
};

class ZynforgeRecordingApp final : public juce::JUCEApplication
{
public:
    ZynforgeRecordingApp() = default;

    const juce::String getApplicationName()    override { return "Zynforge Recording"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed()          override { return false; }

    void initialise (const juce::String& commandLine) override
    {
        // Ignore SIGPIPE process-wide. The companion HTTP/stream server writes
        // to client sockets; when a browser closes the page or aborts the
        // /stream.wav audio element, the in-flight write hits a broken pipe and
        // the default SIGPIPE action would TERMINATE the whole app (seen as the
        // app vanishing with signal 13, no crash report). Ignoring it makes the
        // write fail with EPIPE instead, which the socket code handles.
        std::signal (SIGPIPE, SIG_IGN);

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

            // Isolate persistent state so the tests never write to the
            // engineer's real per-channel names / gains / pans.
            zynforge::StripNames::setTestMode (true);
            zynforge::StripGains::setTestMode (true);

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
        // Branded launch splash for 7 s, then the main window opens.
        splash = std::make_unique<SplashWindow> (getApplicationName().toUpperCase(),
                                                 "created by Jean-Pierre Boutros");
        juce::Timer::callAfterDelay (7000, [this]
        {
            splash.reset();
            mainWindow = std::make_unique<MainWindow> (getApplicationName());
        });
    }

    void shutdown() override
    {
        // Tear the UI down first (no more paints), THEN release every
        // cached juce::Font while CoreText is still alive. Both the
        // BrandTokens font cache and the LookAndFeel's bundled typefaces
        // would otherwise destruct at __cxa_finalize -- after CoreText is
        // gone -- and terminate() the process on quit.
        mainWindow.reset();
        zynforge::brand::type::clearFontCache();
    }

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
    std::unique_ptr<SplashWindow> splash;
};

START_JUCE_APPLICATION (ZynforgeRecordingApp)
