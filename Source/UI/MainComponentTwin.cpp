// The Twin Recorder link's view of this app: what it reads, how a follower's
// take starts and stops, and the header element that shows the link's state.
// The rules live in Source/Network/TwinLink.h; these bodies only translate.

#include "MainComponent.h"
#include "TwinRecorderDialog.h"

using namespace zynforge;

twin::MacStatus MainComponent::twinStatus()
{
    // captureStatus and isRecording both cover a daemon take.
    const auto status = engine.captureStatus();
    const auto session = engine.getActiveSessionDir();

    twin::MacStatus s;
    s.sessionName     = session.getFileName();
    s.sessionDir      = session;
    s.recording       = engine.isRecording();
    s.finalizing      = captureMetadataPending || engine.isCaptureFinalizing();
    s.elapsedSamples  = status.elapsedSamples;
    s.sampleRate      = status.sampleRate;
    s.armedTracks     = status.armedTracks;
    s.missedSamples   = status.missedSamples;
    s.writeFailures   = (status.primaryFailed ? 1 : 0) + (status.backupFailed ? 1 : 0)
                      + (status.mirrorFailed ? 1 : 0) + status.mirrorsSkipped;
    s.diskMinutesLeft = status.minutesRemaining;
    s.backupHealth    = status.backupFailed || status.mirrorFailed ? "failed"
                      : status.backupActive ? "ok" : "off";
    return s;
}

bool MainComponent::twinStartRecord (juce::String& refusal)
{
    // During a punch's pre-roll a RECORD press would end the punch instead.
    if (punchSessionActive)
    {
        refusal = "a punch is in progress on this Mac";
        return false;
    }
    const juce::ScopedValueSetter<bool> inFlight (twinStartInFlight, true);
    juce::String error;
    if (engine.performRemoteTransport (AudioEngine::RemoteTransportAction::StartRecord, error))
        return true;
    // A pre-flight refusal comes back only as "check the host", and that host
    // is another Mac for the operator reading this. The refusal here has the why.
    const bool preflight = ! sessionLocked && ! (useCaptureDaemon && ! captureSupervisor.isAttached());
    refusal = ! preflight ? error
            : recordRefusal.isNotEmpty() ? recordRefusal : statusLabel.getText();
    return false;
}

void MainComponent::twinStopRecord (std::function<void (bool, juce::String)> done)
{
    // LOCK refuses every remote STOP. The take keeps rolling and the leader
    // is told why.
    if (sessionLocked)
    {
        done (false, "session is locked; unlock on the host");
        return;
    }
    // This is the far end of a STOP already confirmed twice on the leader, so
    // it is not one more first tap. A tap armed here must not outlive the take.
    stopArmedAtMs = 0;
    juce::Component::SafePointer<MainComponent> self (this);
    stopActiveCaptureAsync (false, [self, done] (bool ok)
    {
        if (self == nullptr) return;
        const auto& error = self->lastCaptureFinalizationError;
        done (ok, ok ? juce::String()
                     : error.isNotEmpty() ? error : juce::String ("capture stop failed; check this Mac"));
    });
}

void MainComponent::twinDropMarker (const juce::String& name)
{
    engine.dropMarkerAtCurrentPosition (name);
}

void MainComponent::twinNotify (const juce::String& text)
{
    showStatus (text);
}

void MainComponent::noteTwinStopIfEnded()
{
    // Only a take that has really ended counts: a first tap, or a daemon that
    // did not confirm, leaves it rolling and the follower with it.
    if (! engine.isRecording()) twinLink.noteLocalStopAccepted();
}

void MainComponent::showTwinRecorderDialog()
{
    if (auto* window = twinDialog.getComponent())
    {
        window->setVisible (true);
        window->toFront (true);
        return;
    }
    twinDialog = TwinRecorderDialog::launch (twinLink, engine);
}

void MainComponent::updateTwinStatusLabel()
{
    // Its own close box only hides a floating panel; reap it as the tabs do.
    if (auto* window = twinDialog.getComponent())
        if (! window->isVisible() && ! window->isMinimised())
        {
            delete window;
            twinDialog = nullptr;
        }

    const auto link = twinLink.snapshot();
    const bool shown = link.headline.isNotEmpty();
    if (twinStatusLabel.getText() != link.headline || twinStatusLabel.isVisible() != shown)
    {
        twinStatusLabel.setText (link.headline, juce::dontSendNotification);
        twinStatusLabel.setVisible (shown);
        resized();
    }

    const auto fill = link.level == 2 ? brand::accentRecord : juce::Colours::transparentBlack;
    const auto text = link.level == 2 ? brand::onSignal (brand::accentRecord)
                    : link.level == 1 ? brand::alertAmber : brand::featureEngaged;
    twinStatusLabel.setColour (juce::Label::backgroundColourId, fill);
    twinStatusLabel.setColour (juce::Label::textColourId, text);

    juce::StringArray tooltip;
    if (link.detail.isNotEmpty()) tooltip.add (link.detail);
    tooltip.addArray (link.warnings);
    twinStatusLabel.setTooltip (tooltip.joinIntoString ("\n"));
}
