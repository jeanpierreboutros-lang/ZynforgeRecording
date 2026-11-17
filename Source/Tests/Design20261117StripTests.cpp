// The MIXER strip design pass of 2026-11-17: no moving light on a rolling
// strip, meters that change colour at -12 / -6 / -2 dBFS, one labelled scale
// per strip, routing boxes that stay readable on a narrow window, a
// meterbridge whose numbers and names never collide, and strip text that
// reads at 11 pt on M and L strips.

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/DisplayText.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../UI/ChannelStrip.h"
#include "../UI/LedMeter.h"
#include "../UI/MasterStrip.h"
#include "../UI/Meterbridge.h"
#include "../UI/RoutingCombo.h"

#include <algorithm>
#include <functional>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class Design20261117StripTests final : public juce::UnitTest
    {
    public:
        Design20261117StripTests() : juce::UnitTest ("Design strip 2026-11-17", "zynforge") {}

        void runTest() override
        {
            meterColours();
            drawnMeterColours();
            shortRoutingLabels();
            narrowRoutingBoxes();
            spineStaysStill();
            oneLabelledScale();
            oneMinusInfinity();
            stripTextSizes();
            meterbridgeColumns();
        }

    private:
        static void pump (int ms)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + ms;
            while (juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
        }

        static bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 3000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        static juce::String nameOf (juce::Colour c)
        {
            if (c == brand::meterGreen)    return "green";
            if (c == brand::meterEmber)    return "ember";
            if (c == brand::meterHot)      return "hot";
            if (c == brand::meterWhiteHot) return "white-hot";
            if (c == brand::meterIdle)     return "idle";
            return c.toDisplayString (true);
        }

        template <typename T>
        static T* findTitled (juce::Component& root, const juce::String& title)
        {
            if (auto* t = dynamic_cast<T*> (&root); t != nullptr && root.getTitle() == title) return t;
            for (auto* child : root.getChildren())
                if (auto* found = findTitled<T> (*child, title)) return found;
            return nullptr;
        }

        // ── S2: meter colour thresholds ─────────────────────────────────
        void meterColours()
        {
            beginTest ("Meters are green to -12 dBFS, ember to -6, hot orange to -2, white-hot above");
            {
                const std::pair<float, juce::Colour> pins[] = {
                    { -13.0f, brand::meterGreen }, { -11.0f, brand::meterEmber },
                    {  -7.0f, brand::meterEmber }, {  -5.0f, brand::meterHot },
                    {  -3.0f, brand::meterHot },   {  -1.0f, brand::meterWhiteHot } };
                for (const auto& [dB, want] : pins)
                {
                    expectEquals (nameOf (brand::meterHeatAtDb (dB)), nameOf (want),
                                  juce::String (dB) + " dBFS by level");
                    const float frac = (dB + 60.0f) / 60.0f;
                    expectEquals (nameOf (brand::meterHeatAt (frac)), nameOf (want),
                                  juce::String (dB) + " dBFS as a height on the meter");
                }

                // The edges belong to the cooler colour.
                expectEquals (nameOf (brand::meterHeatAtDb (-12.0f)), juce::String ("green"));
                expectEquals (nameOf (brand::meterHeatAtDb (-6.0f)),  juce::String ("ember"));
                expectEquals (nameOf (brand::meterHeatAtDb (-2.0f)),  juce::String ("hot"));
                expectEquals (nameOf (brand::meterHeatAtDb (-40.0f)), juce::String ("green"));
                expectEquals (nameOf (brand::meterHeatAtDb (0.0f)),   juce::String ("white-hot"));

                // The 20-segment ladder: each segment takes the colour at its top.
                // 16 tops out at -12, 18 at -6, 19 at -3, 20 at 0.
                expectEquals (nameOf (brand::meterHeatAt (16.0f / 20.0f)), juce::String ("green"), "segment 16 (-12)");
                expectEquals (nameOf (brand::meterHeatAt (17.0f / 20.0f)), juce::String ("ember"), "segment 17 (-9)");
                expectEquals (nameOf (brand::meterHeatAt (18.0f / 20.0f)), juce::String ("ember"), "segment 18 (-6)");
                expectEquals (nameOf (brand::meterHeatAt (19.0f / 20.0f)), juce::String ("hot"),   "segment 19 (-3)");
                expectEquals (nameOf (brand::meterHeatAt (20.0f / 20.0f)), juce::String ("white-hot"), "segment 20 (0)");
            }
        }

        // The LedMeter every strip, the master, the EDIT rows and the
        // meterbridge use draws those colours.
        void drawnMeterColours()
        {
            beginTest ("The LED meter draws the ladder in those colours");
            {
                TrackState ts;
                LedMeter meter (ts);
                meter.setShowDbLabels (false);
                meter.setBounds (0, 0, 30, 214);   // bar 200 px tall: 20 segments of 10 px
                meter.setVisible (true);
                const float level = juce::Decibels::decibelsToGain (-7.0f);
                ts.peak.store (level);
                ts.rms .store (level);
                pump (400);

                const auto image = meter.createComponentSnapshot (meter.getLocalBounds(), true, 1.0f);
                auto segmentColour = [&] (int segment)   // 1 = bottom
                {
                    const int bottom = 214 - 7;
                    return image.getPixelAt (15, bottom - 10 * segment + 5);
                };
                expectEquals (nameOf (segmentColour (10)), juce::String ("green"), "-30 dBFS segment");
                expectEquals (nameOf (segmentColour (16)), juce::String ("green"), "segment up to -12");
                expectEquals (nameOf (segmentColour (17)), juce::String ("ember"), "segment up to -9, lit at -7");
                expectEquals (nameOf (segmentColour (18)), juce::String ("idle"),  "segment up to -6, above -7");
                meter.detach();
            }
        }

        // ── S4: narrow routing text ─────────────────────────────────────
        void shortRoutingLabels()
        {
            beginTest ("A routing entry's short form drops the arrow and the In / Out word");
            {
                const auto arrow = juce::String::fromUTF8 ("\xe2\x86\x92 ");
                expectEquals (shortRoutingLabel (arrow + "Master"), juce::String ("Mst"));
                expectEquals (shortRoutingLabel ("-> Master"),      juce::String ("Mst"));
                expectEquals (shortRoutingLabel ("In 9-10"),        juce::String ("9-10"));
                expectEquals (shortRoutingLabel ("In 1"),           juce::String ("1"));
                expectEquals (shortRoutingLabel ("In 13"),          juce::String ("13"));
                expectEquals (shortRoutingLabel ("Out 3-4"),        juce::String ("3-4"));
                expectEquals (shortRoutingLabel ("Out 7"),          juce::String ("7"));
                expectEquals (shortRoutingLabel ("In 33 (off)"),    juce::String ("33 off"));
                expectEquals (shortRoutingLabel ("Out 17-18 (off)"), juce::String ("17-18 off"));
                expectEquals (shortRoutingLabel ("(unrouted)"),     juce::String ("none"));
                expectEquals (shortRoutingLabel ("Off (no output)"), juce::String ("Off (no output)"), "unknown text is kept");
                expectEquals (shortRoutingLabel (""),               juce::String());
            }
        }

        void narrowRoutingBoxes()
        {
            beginTest ("On a narrow strip the routing boxes draw the short form; the entries stay whole");
            {
                TrackState left, right;
                left.isStereo.store (true);
                ChannelStrip pair (8, left, {}, {}, {}, {}, {}, {}, &right, [] (float) {});
                pair.setAvailableInputs (32);
                pair.setAvailableOutputs (32);
                TrackState mono;
                ChannelStrip strip (12, mono);
                strip.setAvailableInputs (32);
                strip.setAvailableOutputs (32);

                for (auto* s : { &pair, &strip }) s->setBounds (0, 0, 86, 700);   // M at a 1210 px window
                expectEquals (strip.outputCombo.shownText(), juce::String ("Mst"));
                expect (strip.outputCombo.getText().endsWith ("Master"), "the entry itself keeps its whole text");
                expectEquals (strip.outputCombo.getSelectedId(), 1, "the short form must not unselect the entry");
                expectEquals (pair.inputCombo.shownText(), juce::String ("9-10"));
                expectEquals (pair.inputCombo.getText(), juce::String ("In 9-10"));
                expectEquals (pair.inputCombo.getSelectedId(), 8 + 2);

                // Whatever the size, the box shows the whole entry when it fits
                // and otherwise its short form -- never a clipped "-> ...".
                for (int w : { 50, 64, 86, 110, 200 })
                {
                    for (auto* s : { &pair, &strip })
                    {
                        s->setBounds (0, 0, w, 700);
                        for (auto* box : { &s->inputCombo, &s->outputCombo })
                        {
                            const auto shown = box->shownText();
                            expect (shown == box->getText() || shown == shortRoutingLabel (box->getText()),
                                    "width " + juce::String (w) + " shows " + shown);
                        }
                    }
                }
                strip.setBounds (0, 0, 280, 700);   // an L strip has room for all of it
                expectEquals (strip.outputCombo.shownText(), strip.outputCombo.getText());
                expectEquals (strip.inputCombo.shownText(), juce::String ("In 13"));
            }
        }

        // ── S1: no moving light on a rolling strip ──────────────────────
        void spineStaysStill()
        {
            beginTest ("A rolling take leaves the strip body as it was: no light on the spine, no motion");
            {
                TrackState ts;
                ts.armed.store (true);
                ChannelStrip strip (3, ts);
                strip.setBounds (0, 0, 110, 700);
                strip.setVisible (true);
                auto snap = [&strip] { return strip.createComponentSnapshot (strip.getLocalBounds(), true, 1.0f); };
                pumpUntil ([&] { return strip.dbLabel.getText().isNotEmpty(); });   // readouts settled

                const auto idle = snap();
                strip.setTakeRolling (true);
                const auto rolling = snap();
                pump (450);   // half a cycle of the old 1 Hz pulse
                const auto later = snap();

                // The R lifts while the take rolls; nothing else may change.
                const auto arm = strip.armButton.getBounds().expanded (1);
                int changed = 0, moved = 0;
                for (int y = 0; y < idle.getHeight(); ++y)
                    for (int x = 0; x < idle.getWidth(); ++x)
                    {
                        if (arm.contains (x, y)) continue;
                        if (idle.getPixelAt (x, y) != rolling.getPixelAt (x, y)) ++changed;
                        if (rolling.getPixelAt (x, y) != later.getPixelAt (x, y)) ++moved;
                    }
                expectEquals (changed, 0, "pixels outside the R changed when the take rolled");
                expectEquals (moved, 0, "pixels moved while the take rolled");
                expect (strip.armButton.findColour (juce::ToggleButton::tickColourId) == brand::armRollFill,
                        "the armed R still lifts while the take rolls");

                // The spine is the channel colour.
                const auto spine = rolling.getPixelAt (3, 350);
                expect (spine == strip.getResolvedColour(), "spine is " + spine.toDisplayString (true));
                strip.setTakeRolling (false);
                expect (strip.armButton.findColour (juce::ToggleButton::tickColourId) == brand::armFill);
            }
        }

        // ── S3: one labelled scale ──────────────────────────────────────
        void oneLabelledScale()
        {
            beginTest ("The meter's dBFS scale is signed; the fader's ruler labels only 0");
            {
                juce::StringArray marks;
                for (float dB : LedMeter::kScaleMarksDb) marks.add (LedMeter::scaleLabel (dB));
                expectEquals (marks.joinIntoString (" "), juce::String ("0 -6 -12 -18 -30 -60"));

                juce::StringArray labelled;
                for (int dB : ChannelStrip::kFaderMarksDb)
                    if (ChannelStrip::faderMarkLabel (dB).isNotEmpty())
                        labelled.add (ChannelStrip::faderMarkLabel (dB));
                expectEquals (labelled.joinIntoString (" "), juce::String ("0"), "the fader's ruler labels only unity");
            }

            beginTest ("The meter scale gets the room first; the ruler only what the fader can spare");
            {
                TrackState ts;
                ChannelStrip strip (0, ts);
                const int scale11 = LedMeter::scaleColumnWidth (brand::type::caption());
                strip.setBounds (0, 0, 110, 700);   // M at full width
                expectEquals (strip.meter.getWidth(), 16 + scale11, "the M strip's meter carries its scale");
                expect (strip.faderRuler()->isVisible(), "the M strip shows the fader's ruler");
                strip.setBounds (0, 0, 86, 700);    // M at a 1210 px window
                expectEquals (strip.meter.getWidth(), 16 + scale11, "the narrow M strip keeps the labelled scale");
                expect (strip.gainFader.getWidth() >= ChannelStrip::kMinFaderW, "the fader stays movable");
                expect (! strip.meter.getBounds().intersects (strip.gainFader.getBounds()));
                if (strip.faderRuler()->isVisible())
                    expect (! strip.faderRuler()->getBounds().intersects (strip.gainFader.getBounds()));
            }
        }

        // ── S3: one way to write minus infinity ─────────────────────────
        void oneMinusInfinity()
        {
            beginTest ("Minus infinity reads one way on every strip readout");
            {
                const auto inf = juce::String::fromUTF8 ("-\xe2\x88\x9e");
                expectEquals (zynforge::minusInfinity(), inf);
                expectEquals (levelText (-12.34f, -60.0f), juce::String ("-12.3 dB"));
                expectEquals (levelText (-60.0f, -60.0f), inf + " dB");

                TrackState ts;
                ChannelStrip strip (0, ts);
                strip.setBounds (0, 0, 110, 700);
                strip.gainFader.setValue (-60.0, juce::sendNotificationSync);
                expectEquals (strip.faderValue.getText(), inf + " dB", "a fader at its OFF floor");
                strip.gainFader.setValue (-6.0, juce::sendNotificationSync);
                expectEquals (strip.faderValue.getText(), juce::String ("-6.0 dB"));
                expectEquals (strip.gainFader.getValueFromText (inf + " dB"), -60.0, "what it shows reads back as OFF");

                ts.peak.store (0.0f);
                pumpUntil ([&] { return strip.dbLabel.getText().isNotEmpty(); });
                expectEquals (strip.dbLabel.getText(), inf + " dB", "a silent strip's peak");

                // Too narrow for the unit (XS on a small window): the numbers
                // stay whole and the unit goes, rather than "-34.5...".
                strip.setBounds (0, 0, 50, 700);
                expectEquals (strip.faderValue.getText(), juce::String ("-6.0"));
                pumpUntil ([&] { return strip.dbLabel.getText() == inf; });
                expectEquals (strip.dbLabel.getText(), inf, "a silent XS strip's peak");
                ts.peak.store (juce::Decibels::decibelsToGain (-34.5f));
                pumpUntil ([&] { return strip.dbLabel.getText() == "-34.5"; });
                expectEquals (strip.dbLabel.getText(), juce::String ("-34.5"));
                expect (juce::GlyphArrangement::getStringWidth (strip.dbLabel.getFont(), "-88.8")
                            <= (float) strip.dbLabel.getWidth(), "the XS readout clips its number");
                strip.setBounds (0, 0, 110, 700);
                expectEquals (strip.faderValue.getText(), juce::String ("-6.0 dB"), "the unit comes back with the room");

                AudioEngine engine;
                engine.setStripCount (1);
                MasterStrip master (engine);
                master.setBounds (0, 0, 140, 700);
                auto* fader = findTitled<juce::Slider> (master, "Master gain");
                expect (fader != nullptr);
                if (fader != nullptr)
                    expectEquals (fader->getTextFromValue (-60.0), inf + " dB", "the master at its OFF floor");
            }
        }

        // ── S6: strip text size ─────────────────────────────────────────
        void stripTextSizes()
        {
            beginTest ("Strip text is 11 pt on M and L strips, and never below 10");
            {
                TrackState left, right;
                left.isStereo.store (true);
                ChannelStrip pair (4, left, {}, {}, {}, {}, {}, {}, &right, [] (float) {});
                TrackState ts;
                ChannelStrip strip (21, ts);
                ts.clipCount.store (3);
                const std::pair<int, const char*> widths[] = {
                    { 48, "XS" }, { 62, "S" }, { 70, "M" }, { 86, "M narrow" }, { 120, "M" }, { 150, "L" }, { 280, "L" } };
                for (const auto& [w, size] : widths)
                {
                    for (auto* s : { &strip, &pair })
                    {
                        s->setBounds (0, 0, w, 700);
                        const float least = w >= ChannelStrip::kReadableW ? 11.0f : 10.0f;
                        const auto where = juce::String (size) + " (" + juce::String (w) + " px)";
                        const std::pair<const char*, float> fonts[] = {
                            { "number",       s->readoutFont (true).getHeight() },
                            { "meter scale",  s->scaleFont().getHeight() },
                            { "ruler 0",      s->faderRulerFont().getHeight() },
                            { "clip count",   s->clipLabel.getFont().getHeight() },
                            { "peak readout", s->dbLabel.getFont().getHeight() },
                            { "fader value",  s->faderValue.getFont().getHeight() },
                            { "pan readout",  s->panLabel.getFont().getHeight() } };
                        for (const auto& [what, h] : fonts)
                        {
                            expect (h >= least, juce::String (what) + " is " + juce::String (h) + " pt at " + where);
                            const int onScale[] = { 10, 11, 13, 14, 18, 22, 28, 48 };
                            expect (std::find (std::begin (onScale), std::end (onScale), juce::roundToInt (h)) != std::end (onScale),
                                    juce::String (what) + " is off the type scale at " + where);
                        }
                        // The number fits its box at every size.
                        expect (juce::GlyphArrangement::getStringWidth (s->readoutFont (true), "22")
                                    <= (float) s->numberBounds().getWidth(),
                                "the number is clipped at " + where);
                    }
                }
            }
        }

        // ── S5: meterbridge labels ──────────────────────────────────────
        void meterbridgeColumns()
        {
            beginTest ("A meterbridge column's number and name never overlap, at any window width");
            {
                const auto numberFont = brand::type::monoStamp();
                for (int columns : { 1, 4, 11, 19, 32 })
                    for (int width = 240; width <= 2600; width += 37)
                    {
                        const auto inner = juce::Rectangle<int> (12, 12, width - 24, 356);
                        for (int i = 0; i < columns; ++i)
                        {
                            const auto c = Meterbridge::columnAt (inner, columns, i);
                            const auto where = juce::String (columns) + " columns at " + juce::String (width)
                                             + " px, column " + juce::String (i);
                            expect (! c.number.intersects (c.name), "number over name: " + where);
                            expect (c.number.getBottom() <= c.name.getY(), "number below its name: " + where);
                            expect (c.name.getBottom() <= c.meter.getY(), "name over the meter: " + where);
                            expect (juce::GlyphArrangement::getStringWidth (numberFont, "32") <= (float) c.number.getWidth(),
                                    "number clipped: " + where);
                            if (i + 1 < columns)
                            {
                                const auto next = Meterbridge::columnAt (inner, columns, i + 1);
                                expect (c.name.getRight() <= next.name.getX(), "names run together: " + where);
                                expect (c.number.getRight() <= next.number.getX(), "numbers run together: " + where);
                            }
                        }
                    }
            }
        }
    };

    static Design20261117StripTests design20261117StripTests;
}
