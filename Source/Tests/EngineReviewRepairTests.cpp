// Regressions for the real-time engine and playback review (2026-10-29):
// timecode chase wiring, input routing on a smaller device, persistence of
// pans set by cues and control surfaces, stereo-link state, MIDI clock
// transport messages and the session player's locate / loop behaviour.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class EngineReviewRepairTests final : public juce::UnitTest
    {
    public:
        EngineReviewRepairTests() : juce::UnitTest ("Engine review repairs", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Chase liveness follows the selected source, not whichever one spoke last");
            {
                AudioEngine e;
                e.setChaseMode (AudioEngine::ChaseMode::Ltc);
                // An MTC port left open while LTC is the master.
                e.handleIncomingMidiMessage (nullptr, juce::MidiMessage::fullFrame (
                    1, 2, 3, 4, juce::MidiMessage::fps25));
                expect (! e.isChaseLive(), "MTC made an LTC chase read as live");
                e.setChaseMode (AudioEngine::ChaseMode::Mtc);
                e.handleIncomingMidiMessage (nullptr, juce::MidiMessage::fullFrame (
                    1, 2, 3, 4, juce::MidiMessage::fps25));
                expect (e.isChaseLive(), "MTC chase did not go live on a full frame");
                e.setChaseMode (AudioEngine::ChaseMode::Off);
            }

            beginTest ("The LTC source strip follows a reorder; deleting it clears it");
            {
                AudioEngine e;
                e.setStripCount (3);
                e.setLtcSourceStrip (2);          // 1-based: strip index 1
                expect (e.swapTracks (0, 1));
                expectEquals (e.getLtcSourceStrip(), 1, "LTC source stayed on the old index");
                expect (e.removeStripAt (0));
                expectEquals (e.getLtcSourceStrip(), 0, "a deleted LTC strip left a stale source");
                e.setStripCount (0);
            }
        }
    };

    static EngineReviewRepairTests engineReviewRepairTests;
}
