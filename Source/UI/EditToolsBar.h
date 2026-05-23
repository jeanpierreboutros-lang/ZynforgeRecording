#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <array>
#include <functional>

namespace zynforge
{
    // Pro Tools-style edit-mode toolbar. One-of-N selection biases the
    // EDIT view's left-click hit-test:
    //  - Smart     → top of clip = fade handles, edges = trim, body = move (default)
    //  - Selector  → click sets playhead, drag sets the loop range
    //  - Trim      → body clicks trim from the nearer edge
    //  - Grabber   → body clicks always move
    //  - Fade      → body clicks open the fade preset menu
    //  - Scrubber  → drag sets the playhead per-pixel
    //
    // Header-only on purpose — no extra .cpp in CMakeLists.
    class EditToolsBar final : public juce::Component
    {
    public:
        enum class Tool : int { Smart = 0, Selector, Trim, Grabber, Fade, Scrubber };

        EditToolsBar()
        {
            const std::array<std::pair<Tool, const char*>, 6> items {{
                { Tool::Smart,    "Smart"    },
                { Tool::Selector, "Selector" },
                { Tool::Trim,     "Trim"     },
                { Tool::Grabber,  "Grabber"  },
                { Tool::Fade,     "Fade"     },
                { Tool::Scrubber, "Scrubber" }
            }};

            for (auto& [t, name] : items)
            {
                auto b = std::make_unique<ToolButton> (t, name);
                b->onClick = [this, tool = t]() { selectTool (tool); };
                addAndMakeVisible (*b);
                buttons.push_back (std::move (b));
            }
            applySelection();
        }

        Tool getTool() const noexcept { return tool; }

        void setTool (Tool t)
        {
            if (t == tool) return;
            tool = t;
            applySelection();
            if (onToolChanged) onToolChanged (tool);
            repaint();
        }

        std::function<void (Tool)> onToolChanged;

        void paint (juce::Graphics& g) override
        {
            g.setColour (brand::bgPanel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
            g.setColour (brand::edge);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (brand::space::sm, brand::space::xs);
            const int btnW = 32;
            const int gap  = 4;
            for (auto& b : buttons)
            {
                b->setBounds (r.removeFromLeft (btnW));
                r.removeFromLeft (gap);
            }
        }

    private:
        class ToolButton final : public juce::Component,
                                  public juce::SettableTooltipClient
        {
        public:
            ToolButton (Tool t, juce::String n) : tool (t), name (std::move (n)) {}

            bool active { false };
            std::function<void()> onClick;

            void mouseUp (const juce::MouseEvent& e) override
            {
                if (e.mouseWasClicked() && onClick) onClick();
            }

            void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
            void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }

            void paint (juce::Graphics& g) override
            {
                auto rect = getLocalBounds().toFloat().reduced (1.0f);

                const auto accent = brand::accentVS;
                if (active)
                {
                    g.setGradientFill (juce::ColourGradient (
                        accent.brighter (0.18f), rect.getCentreX(), rect.getY(),
                        accent.darker  (0.20f), rect.getCentreX(), rect.getBottom(), false));
                    g.fillRoundedRectangle (rect, 4.0f);
                    g.setColour (accent.brighter (0.30f));
                    g.drawRoundedRectangle (rect.reduced (0.5f), 4.0f, 1.0f);
                }
                else
                {
                    g.setColour (hover ? brand::controlBgHover : brand::bgPanel);
                    g.fillRoundedRectangle (rect, 4.0f);
                    g.setColour (brand::edge);
                    g.drawRoundedRectangle (rect.reduced (0.5f), 4.0f, 1.0f);
                }

                g.setColour (active ? juce::Colours::white : brand::textSecondary);
                paintGlyph (g, rect, tool);
            }

            void paintGlyph (juce::Graphics& g, juce::Rectangle<float> r, Tool t)
            {
                const float cx = r.getCentreX();
                const float cy = r.getCentreY();
                juce::Path p;
                switch (t)
                {
                    case Tool::Smart:
                        // Pointer + waveform whisp (auto-detects)
                        p.startNewSubPath (cx - 5, cy - 5);
                        p.lineTo            (cx + 4, cy);
                        p.lineTo            (cx - 1, cy + 1);
                        p.lineTo            (cx + 1, cy + 5);
                        p.lineTo            (cx - 1, cy + 5);
                        p.lineTo            (cx - 3, cy + 1);
                        p.lineTo            (cx - 5, cy + 4);
                        p.closeSubPath();
                        g.fillPath (p);
                        break;

                    case Tool::Selector:
                        // I-beam
                        g.fillRect (juce::Rectangle<float> (cx - 0.7f, cy - 6, 1.4f, 12));
                        g.fillRect (juce::Rectangle<float> (cx - 3,    cy - 6, 6,    1.4f));
                        g.fillRect (juce::Rectangle<float> (cx - 3,    cy + 5, 6,    1.4f));
                        break;

                    case Tool::Trim:
                        // [→ ←]
                        g.fillRect (juce::Rectangle<float> (cx - 6, cy - 4, 1.4f, 8));
                        g.fillRect (juce::Rectangle<float> (cx + 5, cy - 4, 1.4f, 8));
                        p.startNewSubPath (cx - 3, cy - 3);
                        p.lineTo            (cx,     cy);
                        p.lineTo            (cx - 3, cy + 3);
                        p.startNewSubPath (cx + 3, cy - 3);
                        p.lineTo            (cx,     cy);
                        p.lineTo            (cx + 3, cy + 3);
                        g.strokePath (p, juce::PathStrokeType (1.3f));
                        break;

                    case Tool::Grabber:
                        // Open hand simplified — 4 fingers + palm
                        g.fillRect (juce::Rectangle<float> (cx - 5, cy - 2, 10, 6));
                        g.fillRect (juce::Rectangle<float> (cx - 5, cy - 6, 2,  6));
                        g.fillRect (juce::Rectangle<float> (cx - 2, cy - 7, 2,  7));
                        g.fillRect (juce::Rectangle<float> (cx + 1, cy - 6, 2,  6));
                        g.fillRect (juce::Rectangle<float> (cx + 4, cy - 5, 2,  5));
                        break;

                    case Tool::Fade:
                        // Triangle fade-in shape
                        p.startNewSubPath (cx - 6, cy + 5);
                        p.lineTo            (cx + 6, cy - 5);
                        p.lineTo            (cx + 6, cy + 5);
                        p.closeSubPath();
                        g.fillPath (p);
                        break;

                    case Tool::Scrubber:
                        // Speaker + diagonal motion lines
                        p.startNewSubPath (cx - 5, cy - 3);
                        p.lineTo            (cx - 2, cy - 3);
                        p.lineTo            (cx + 1, cy - 6);
                        p.lineTo            (cx + 1, cy + 6);
                        p.lineTo            (cx - 2, cy + 3);
                        p.lineTo            (cx - 5, cy + 3);
                        p.closeSubPath();
                        g.fillPath (p);
                        // sound arcs
                        g.drawEllipse (cx + 2, cy - 4, 6, 8, 1.2f);
                        break;
                }
            }

            Tool tool;
            juce::String name;
            bool hover { false };
        };

        void selectTool (Tool t) { setTool (t); }

        void applySelection()
        {
            for (auto& b : buttons)
            {
                const bool on = (b->tool == tool);
                b->active = on;
                b->setTooltip (b->name + (on ? juce::String (" (active)") : juce::String()));
                b->repaint();
            }
        }

        Tool tool { Tool::Smart };
        std::vector<std::unique_ptr<ToolButton>> buttons;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditToolsBar)
    };
}
