#include "TransportBar.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    class TransportBar::IconButton final : public juce::Button
    {
    public:
        enum class Icon { GotoStart, GotoEnd, Play, Stop, Record, Loop };

        IconButton (const juce::String& name, Icon i)
            : juce::Button (name), icon (i) {}

        void setActive (bool a) { active = a; repaint(); }
        void setBaseColour (juce::Colour c) { baseColour = c; repaint(); }

        // Record-button-only states. recording == rolling (stable solid
        // red); armed == primed but not yet rolling (blinks on the
        // blinkPhase the transport bar drives). Idle is neither.
        void setRecording (bool v) { if (recording  != v) { recording  = v; repaint(); } }
        void setArmed     (bool v) { if (armed      != v) { armed      = v; repaint(); } }
        void setBlinkPhase (bool v) { if (blinkPhase != v) { blinkPhase = v; if (armed && ! recording) repaint(); } }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            // Square button body -- gradient fill, 1px border. Matches the
            // ZynForge Live transport button finish.
            auto r = getLocalBounds().toFloat().reduced (2.0f);

            const bool isRec   = (icon == Icon::Record);
            // Solid red while rolling; a lit red frame on the "on" half
            // of the arm blink. Idle (and the blink "off" half) read as
            // the normal dark control body so the eye catches the flash.
            const bool recOn   = isRec && recording;
            const bool blinkOn = isRec && armed && ! recording && blinkPhase;

            // Play / Loop "lit" state -- when active (playing / looping)
            // the button glows in its own accent (green for PLAY) so the
            // engineer can see at a glance that transport is running, even
            // when it was started from the space bar rather than a click.
            const bool litActive = active && ! isRec;

            auto base = (down ? brand::controlBgDown
                              : over ? brand::controlBgHover
                                     : brand::controlBg);
            if      (recOn)     base = brand::accentRecord;                // ON AIR -- stable
            else if (blinkOn)   base = brand::accentRecord.darker (0.30f); // armed -- blink lit
            else if (litActive) base = baseColour;                         // PLAY lit -- green
            g.setGradientFill (brand::verticalGradient (base, r, 0.10f, 0.18f));
            g.fillRoundedRectangle (r, brand::radius::md);

            // Record gets a permanent brand-red 2 px stroke even when
            // idle -- engineer at the console reading at distance and
            // under stage glare cannot tell circle / triangle / square
            // apart by colour alone. The persistent stroke makes the
            // call-to-action shape-distinct from PLAY (green) and STOP
            // (amber) regardless of light conditions. The stroke goes
            // fully opaque while rolling or on a blink-lit frame.
            if (isRec)
            {
                const float a = (recOn || blinkOn) ? 1.0f : brand::alpha::prominent;
                g.setColour (brand::accentRecord.withAlpha (a));
                g.drawRoundedRectangle (r.reduced (1.0f), brand::radius::md, 2.0f);
            }
            else if (litActive)
            {
                // Bright accent ring around the lit (running) button.
                g.setColour (baseColour.brighter (0.20f));
                g.drawRoundedRectangle (r.reduced (1.0f), brand::radius::md, 2.0f);
            }
            else
            {
                g.setColour (brand::controlBorder);
                g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
            }

            // Icon (centred inside the inner area).
            auto ic = r.reduced (8.0f, 6.0f);
            const float cx = ic.getCentreX();
            const float cy = ic.getCentreY();

            // On a coloured body (rolling / blink-lit / lit-active) the
            // glyph needs a legible foreground; otherwise the per-button
            // accent.
            g.setColour ((recOn || blinkOn) ? brand::onSignal (brand::accentRecord)
                         : litActive        ? brand::onSignal (baseColour)
                                            : baseColour);

            switch (icon)
            {
                case Icon::GotoStart:
                {
                    // Vertical bar on left + leftward triangle.
                    g.fillRect (juce::Rectangle<float> (ic.getX(), ic.getY(),
                                                       2.5f, ic.getHeight()));
                    juce::Path p;
                    p.addTriangle (ic.getX() + 4.0f, cy,
                                   ic.getRight(),    ic.getY(),
                                   ic.getRight(),    ic.getBottom());
                    g.fillPath (p);
                    break;
                }
                case Icon::GotoEnd:
                {
                    juce::Path p;
                    p.addTriangle (ic.getRight() - 4.0f, cy,
                                   ic.getX(),            ic.getY(),
                                   ic.getX(),            ic.getBottom());
                    g.fillPath (p);
                    g.fillRect (juce::Rectangle<float> (ic.getRight() - 2.5f, ic.getY(),
                                                       2.5f, ic.getHeight()));
                    break;
                }
                case Icon::Play:
                {
                    juce::Path p;
                    p.addTriangle (ic.getX(),     ic.getY(),
                                   ic.getX(),     ic.getBottom(),
                                   ic.getRight(), cy);
                    g.fillPath (p);
                    break;
                }
                case Icon::Stop:
                {
                    g.fillRoundedRectangle (ic.reduced (1.0f), 1.5f);
                    break;
                }
                case Icon::Record:
                {
                    // Concentric-ring glyph: outer ring + filled inner
                    // disc. The ring + dot reads as 'target' from
                    // distance, distinct from PLAY's solid triangle
                    // and STOP's solid square even when only the
                    // silhouette is legible.
                    const float radius = juce::jmin (ic.getWidth(), ic.getHeight()) * 0.5f;
                    g.drawEllipse (cx - radius, cy - radius,
                                   radius * 2.0f, radius * 2.0f, 1.6f);
                    const float innerR = radius * 0.55f;
                    g.fillEllipse (cx - innerR, cy - innerR,
                                   innerR * 2.0f, innerR * 2.0f);
                    break;
                }
                case Icon::Loop:
                {
                    // Circular arrow.
                    juce::Path p;
                    const float r2 = juce::jmin (ic.getWidth(), ic.getHeight()) * 0.4f;
                    p.addCentredArc (cx, cy, r2, r2,
                                     0.0f,
                                     juce::MathConstants<float>::pi * 0.25f,
                                     juce::MathConstants<float>::twoPi - 0.4f,
                                     true);
                    g.strokePath (p, juce::PathStrokeType (2.2f));
                    // Arrowhead at the open side.
                    juce::Path head;
                    const float ax = cx + r2 * std::cos (-0.4f);
                    const float ay = cy + r2 * std::sin (-0.4f);
                    head.addTriangle (ax,        ay - 5.0f,
                                      ax + 6.0f, ay,
                                      ax,        ay + 5.0f);
                    g.fillPath (head);
                    break;
                }
            }

            // ─── Action flash. A brief baseColour wash over the whole
            // button when an action fires (e.g. STOP), so the engineer
            // gets visual confirmation even when the trigger was the
            // space bar rather than a click. The transport bar's 10 Hz
            // timer decays flashLevel back to 0.
            if (flashLevel > 0.0f)
            {
                g.setColour (baseColour.withAlpha (juce::jlimit (0.0f, 0.85f, flashLevel)));
                g.fillRoundedRectangle (r, brand::radius::md);
            }
        }

        // Kick off a one-shot flash (full intensity); the bar's timer
        // fades it. tickFlash() returns true while still fading so the
        // caller knows to keep ticking.
        void triggerFlash() { flashLevel = 1.0f; repaint(); }
        bool tickFlash()
        {
            if (flashLevel <= 0.0f) return false;
            flashLevel = (flashLevel > 0.08f) ? flashLevel * 0.55f : 0.0f;
            repaint();
            return flashLevel > 0.0f;
        }

    private:
        Icon         icon;
        bool         active     { false };
        bool         recording  { false };
        bool         armed      { false };
        bool         blinkPhase { false };
        float        flashLevel { 0.0f };
        juce::Colour baseColour { brand::textPrimary };
    };

    TransportBar::TransportBar (AudioEngine& eng) : engine (eng)
    {
        auto make = [this] (const juce::String& name, IconButton::Icon i, juce::Colour c)
        {
            auto b = std::make_unique<IconButton> (name, i);
            b->setBaseColour (c);
            addAndMakeVisible (*b);
            return b;
        };

        gotoStart = make ("Go to start",  IconButton::Icon::GotoStart, brand::textPrimary);
        gotoEnd   = make ("Go to end",    IconButton::Icon::GotoEnd,   brand::textPrimary);
        play      = make ("Play",         IconButton::Icon::Play,      brand::accentPlay);
        stop      = make ("Stop",         IconButton::Icon::Stop,      brand::accentVS);
        record    = make ("Record",       IconButton::Icon::Record,    brand::accentRecord);
        loop      = make ("Loop region",  IconButton::Icon::Loop,      brand::textMuted);

        gotoStart->setTooltip ("Go to session start (rewind playback to 0).");
        gotoEnd  ->setTooltip ("Go to session end.");
        play     ->setTooltip ("Play / pause VSC playback.");
        stop     ->setTooltip ("Stop everything (recording + playback) and rewind.");
        record   ->setTooltip ("Toggle recording.");
        loop     ->setTooltip ("Loop play: repeat the selected range (or the whole take) while playing.");

        // Accessibility: icon-only buttons need an explicit spoken name --
        // VoiceOver has no glyph to read. Title = the action; help text
        // reuses the tooltip. The bar itself is a labelled focus-container
        // group so the six buttons are navigable as "Transport".
        setTitle ("Transport");
        setDescription ("Playback and recording transport");
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);
        for (IconButton* b : { gotoStart.get(), gotoEnd.get(), play.get(),
                               stop.get(), record.get(), loop.get() })
        {
            b->setTitle (b->getName());        // "Go to start", "Play", ...
            b->setHelpText (b->getTooltip());
            b->setWantsKeyboardFocus (true);   // reachable via Tab / VO
        }

        gotoStart->onClick = [this] { engine.getPlayer().setPositionSamples (0); };
        gotoEnd  ->onClick = [this]
        {
            auto& p = engine.getPlayer();
            p.setPositionSamples (p.getTotalLengthSamples());
        };
        // Delegate transport actions to the host (MainComponent) when
        // a hook is wired -- that path runs the pre-flight checks
        // (armed tracks, valid routing, status messages, ...). Fall
        // back to a direct engine call when no hook is set.
        play     ->onClick = [this]
        {
            if (onRequestPlay) { onRequestPlay(); return; }
            auto& p = engine.getPlayer();
            if (! p.isLoaded()) return;
            if (p.isPlaying()) engine.stopPlayback(); else engine.startPlayback();
        };
        stop     ->onClick = [this]
        {
            if (onRequestStop) { onRequestStop(); return; }
            if (engine.isRecording()) engine.stopRecording();
            engine.stopPlayback();
            engine.getPlayer().rewind();
        };
        record   ->onClick = [this]
        {
            if (onRequestRecord) { onRequestRecord(); return; }
            if (engine.isRecording())
            {
                engine.stopRecording();
                return;
            }
            const auto root = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                  .getChildFile ("Zynforge Sessions");
            const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
            engine.startRecording (root.getChildFile ("Session_" + stamp));
        };
        loop     ->onClick = [this]
        {
            // Loop-PLAY toggle: when on, playback wraps within the selected
            // range (the loop region) forever. Turning it on with no range
            // selected loops the whole take. Does NOT clear the region (that's
            // the edit selection) -- toggling off just stops the wrap.
            auto& p = engine.getPlayer();
            const bool nowOn = ! p.isLoopEnabled();
            if (nowOn && ! p.hasLoopRegion())
            {
                const auto total = p.getTotalLengthSamples();
                if (total > 0) p.setLoopRegion (0, total);
            }
            p.setLoopEnabled (nowOn);
            refreshStates();
        };

        refreshStates();
        startTimerHz (10);
    }

    TransportBar::~TransportBar() = default;

    void TransportBar::resized()
    {
        auto r = getLocalBounds();
        const int n = 6;
        const int gap = 4;

        // Square buttons sized to the bar's height -- never wider than tall,
        // never taller than wide. Matches the reference mock-up's shape.
        const int side = juce::jmin (r.getHeight(),
                                     (r.getWidth() - (n - 1) * gap) / n);

        // Centre the row horizontally if it ends up narrower than the bar.
        const int totalW = n * side + (n - 1) * gap;
        if (totalW < r.getWidth())
            r.removeFromLeft ((r.getWidth() - totalW) / 2);

        for (auto* b : { gotoStart.get(), gotoEnd.get(), play.get(),
                         stop.get(), record.get(), loop.get() })
        {
            // Vertically centre each square inside the bar's full height.
            const int yOffset = (r.getHeight() - side) / 2;
            auto cell = r.removeFromLeft (side);
            b->setBounds (cell.withY (cell.getY() + yOffset).withHeight (side));
            r.removeFromLeft (gap);
        }
    }

    void TransportBar::timerCallback()
    {
        refreshStates();
        if (stop != nullptr) stop->tickFlash();   // fade the STOP flash
    }

    void TransportBar::refreshStates()
    {
        const bool rec  = engine.isRecording();
        const bool pl   = engine.isPlaying();
        const bool lp   = engine.getPlayer().isLoopEnabled();   // ⟲ lit when loop-PLAY is on

        // A transport stop -- playback or recording just went from on to
        // off, whatever triggered it (space bar, button, stop-all). Flash
        // the STOP button so the engineer sees the action registered.
        const bool stopped = (lastPlaying && ! pl) || (lastRecording && ! rec);

        // "Record armed" == at least one track armed, transport primed
        // but not yet rolling. Mirrors the BigClock's armed-ready signal
        // so both call-to-action surfaces tell the same story.
        bool anyArmed = false;
        auto& recr = engine.getRecorder();
        for (int i = 0, n = recr.getNumTracks(); i < n; ++i)
            if (recr.getTrack (i).armed.load (std::memory_order_relaxed)) { anyArmed = true; break; }
        const bool recordArmed = anyArmed && ! rec && ! pl;

        if (rec         != lastRecording) { record->setRecording (rec);  lastRecording = rec; }
        if (recordArmed != lastArmed)     { record->setArmed (recordArmed); lastArmed = recordArmed; }
        if (pl          != lastPlaying)   { play  ->setActive (pl);       lastPlaying   = pl;  }
        if (lp          != lastLooping)   { loop  ->setActive (lp);       lastLooping   = lp;  }

        if (stopped && stop != nullptr) stop->triggerFlash();

        // Drive the arm blink at ~1 Hz (brand::motion::pulseHz) off the
        // 10 Hz timer: flip the phase every 5 ticks → 0.5 s on / 0.5 s off.
        if (recordArmed)
        {
            if (++blinkCounter >= 5)
            {
                blinkCounter = 0;
                blinkPhase   = ! blinkPhase;
                record->setBlinkPhase (blinkPhase);
            }
        }
        else if (blinkPhase || blinkCounter != 0)
        {
            blinkCounter = 0;
            blinkPhase   = false;
            record->setBlinkPhase (false);
        }
    }
}
