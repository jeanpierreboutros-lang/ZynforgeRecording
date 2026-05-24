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
        // Mirrors AudioEngine::AutomationWriteMode -- order matters.
        enum class WriteMode : int { Off = 0, Touch = 1, Latch = 2, Write = 3 };

        AutomationToolbar();

        Tool  getTool()  const noexcept { return tool;  }
        Param getParam() const noexcept { return param; }

        // Force the toolbar's param combo to the given value without
        // firing onParamChanged. Used by EditPage to keep the toolbar
        // in lockstep with the per-row VIEW lane mode picker.
        void  setParamSilently (Param p);

        // Sync the WRITE-mode combo without re-emitting the callback.
        // Host calls this after restoring a project to keep UI + engine
        // in lockstep.
        void  setWriteModeSilently   (WriteMode);
        void  setSuspendSilently     (bool on);
        void  setPunchSilently       (bool on);
        void  setTrimSilently        (bool on);

        std::function<void (Tool)>  onToolChanged;
        std::function<void (Param)> onParamChanged;
        std::function<void()>       onClearAll;
        // Fired when the engineer changes the WRITE-mode combo. Host
        // wires this to engine.setAutomationWriteMode. WriteMode::Off
        // disables the engine's write path; the other three enable it
        // with progressively more aggressive semantics (today the
        // engine treats Touch/Latch/Write identically and just looks
        // at on/off -- finer-grained behaviour is layered on top).
        std::function<void (WriteMode)>        onWriteModeChanged;
        // Fired when TRIM is toggled. WRITE and TRIM are mutually
        // exclusive on the toolbar (turning TRIM on forces WRITE to
        // Off and vice versa), so the host only ever sees one as
        // active at a time.
        std::function<void (bool /*trimOn*/)>  onTrimModeChanged;
        // Global SUSPEND -- engine reads ignore stored automation,
        // letting the engineer audition raw fader / pan / mute.
        std::function<void (bool /*suspendOn*/)> onSuspendChanged;
        // PUNCH -- automation writes only fire while the playhead is
        // inside the engine's punch range.
        std::function<void (bool /*punchOn*/)>   onPunchChanged;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void styleToolButton (juce::TextButton&, juce::Colour activeColour);
        void selectTool (Tool);
        void styleWriteCombo();

        Tool  tool  { Tool::Select };
        Param param { Param::Volume };

        juce::Label title;
        juce::TextButton selectButton  { "Select" };
        juce::TextButton addButton     { "+ Point" };
        juce::TextButton deleteButton  { "Delete" };

        juce::Label paramLabel;
        juce::ComboBox paramCombo;

        juce::TextButton clearButton   { "Clear all" };
        juce::Label      writeLabel;
        juce::ComboBox   writeCombo;
        juce::TextButton trimButton    { "Trim" };
        juce::TextButton suspendButton { "Suspend" };
        juce::TextButton punchButton   { "Punch" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationToolbar)
    };
}
