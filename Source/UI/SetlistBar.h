#pragma once

#include "IconButton.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Setlist + cue bar: [SETLIST | ◂ | <dropdown> | ▸ | + Cue | Update].
    //
    // A cue is just a named transport-position bookmark. The engineer
    // builds an ordered list of cues (one per song, intro, scene...) and
    // then nudges with ◂/▸ during the show to jump between them.
    //
    // The bar is purely UI. Cue storage + transport jumps live in
    // MainComponent, which wires the callbacks below.
    class SetlistBar final : public juce::Component
    {
    public:
        // Per-strip snapshot captured at +Cue / Update time. Restored
        // when the cue is recalled (◂ / ▸ or dropdown pick).
        struct StripSnapshot
        {
            // Stable strip UUID -- set on capture, looked up on recall.
            // Empty string means 'pre-v2 snapshot' -- recall falls back
            // to array index (the position in the cue.strips vector).
            juce::String stripId;

            float gainDb       { 0.0f };
            float pan          { 0.0f };
            int   inputRouting { -1 };
            int   outputRouting{ -1 };
            bool  muted        { false };
            bool  soloed       { false };
            bool  monitor      { false };
            bool  armed        { false };
        };

        struct Cue
        {
            juce::String name;
            juce::int64  samplePos { 0 };
            std::vector<StripSnapshot> strips;   // length = recorder.getNumTracks() at capture time

        };

        SetlistBar();

        // Repopulate the dropdown from the current cue list. selectedIndex
        // is the cue currently active (highlighted); pass -1 if none.
        void setCues (const std::vector<Cue>& cues, int selectedIndex);

        // Triggered by the engineer:
        //   onPick(int)          -- chose a cue from the dropdown
        //   onPrev() / onNext()  -- clicked ◂ or ▸
        //   onAddCue()           -- clicked '+ Cue' (drop at current pos)
        //   onUpdateCue()        -- clicked 'Update' (overwrite current pos)
        std::function<void (int)> onPick;
        std::function<void()>     onPrev;
        std::function<void()>     onNext;
        std::function<void()>     onAddCue;
        std::function<void()>     onUpdateCue;
        std::function<void()>     onRecallCue;   // 'Recall' -- re-apply the selected cue
        // Triggered by the right-click menu on the bar or combo. The
        // engineer picks the cue first (combo selection or step), then
        // right-clicks for Rename / Delete.
        std::function<void()>     onRenameCue;
        std::function<void()>     onDeleteCue;
        // Reorder the active cue: -1 = move up (earlier in list),
        // +1 = move down (later). Host applies the swap + persists.
        std::function<void (int /*direction*/)> onMoveCue;

        // An empty setlist needs no row of its own, only a way to start one:
        // collapsed, the bar is just its + Cue button, as a neutral header chip.
        void setCollapsed (bool shouldCollapse);
        bool isCollapsed() const noexcept { return collapsed; }

        void mouseDown (const juce::MouseEvent&) override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        // Path-drawn arrow button -- replaces the previous unicode-glyph
        // TextButtons so cue navigation matches the TransportBar's
        // vector-icon vocabulary. Forward/backward triangles drawn in
        // a juce::Path; theming follows the standard ZynForge button
        // chrome via the host LookAndFeel.
        class ArrowButton final : public juce::Button
        {
        public:
            ArrowButton (const juce::String& n, bool forward)
                : juce::Button (n), pointsRight (forward) {}

            void paintButton (juce::Graphics& g, bool over, bool down) override
            {
                auto r = getLocalBounds().toFloat().reduced (2.0f);
                const auto base = down ? brand::controlBgDown
                                       : over ? brand::controlBgHover
                                              : brand::bgElevated;
                g.setColour (base);   // FLAT: solid fill (no gradient)
                g.fillRoundedRectangle (r, brand::radius::md);
                g.setColour (brand::edge);
                g.drawRoundedRectangle (r, brand::radius::md, 1.0f);

                const float cx = r.getCentreX();
                const float cy = r.getCentreY();
                const float w  = juce::jmin (r.getWidth(), r.getHeight()) * 0.40f;
                juce::Path p;
                if (pointsRight)
                    p.addTriangle (cx - w * 0.5f, cy - w, cx - w * 0.5f, cy + w, cx + w * 0.7f, cy);
                else
                    p.addTriangle (cx + w * 0.5f, cy - w, cx + w * 0.5f, cy + w, cx - w * 0.7f, cy);
                g.setColour (brand::textPrimary);
                g.fillPath (p);
            }

        private:
            bool pointsRight;
        };

        juce::Label        titleLabel;
        ArrowButton        prevButton  { "Previous cue", false };
        juce::ComboBox     cueCombo;
        ArrowButton        nextButton  { "Next cue",     true };
        IconButton         addCueButton { icons::Glyph::Plus, "CUE" };
        juce::TextButton   updateButton;
        juce::TextButton   recallButton;

        bool suppressComboCallback { false };
        bool collapsed { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetlistBar)
    };
}
