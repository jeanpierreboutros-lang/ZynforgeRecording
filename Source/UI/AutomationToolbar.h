#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace zynforge
{
    // Toolbar shown only in EDIT view. Sets the active automation tool
    // mode + the parameter being edited (volume / pan / mute), and
    // exposes Add / Delete / Clear actions the EDIT rows act on.
    class AutomationToolbar final : public juce::Component
    {
    public:
        enum class Tool : int { Select = 0, AddPoint, DeletePoint };
        enum class Param : int { Volume = 0, Pan, Mute, Click, Tempo };

        AutomationToolbar();

        Tool  getTool()  const noexcept { return tool;  }
        Param getParam() const noexcept { return param; }

        // Force the toolbar's param combo to the given value without
        // firing onParamChanged. Used by EditPage to keep the toolbar
        // in lockstep with the per-row VIEW lane mode picker.
        void  setParamSilently (Param p);

        std::function<void (Tool)>  onToolChanged;
        std::function<void (Param)> onParamChanged;
        std::function<void()>       onClearAll;
        // Fired when the engineer toggles WRITE on/off. Host wires
        // this to engine.setAutomationWriteMode.
        std::function<void (bool /*writeOn*/)> onWriteModeChanged;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void styleToolButton (juce::TextButton&, juce::Colour activeColour);
        void selectTool (Tool);

        Tool  tool  { Tool::Select };
        Param param { Param::Volume };

        juce::Label title;
        juce::TextButton selectButton  { "Select" };
        juce::TextButton addButton     { "+ Point" };
        juce::TextButton deleteButton  { "Delete" };

        juce::Label paramLabel;
        juce::ComboBox paramCombo;

        juce::TextButton clearButton   { "Clear all" };
        juce::TextButton writeButton   { "Write" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationToolbar)
    };
}
