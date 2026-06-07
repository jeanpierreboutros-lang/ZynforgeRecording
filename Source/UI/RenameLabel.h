#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace zynforge
{
    // A Label whose inline editor treats Tab / Shift+Tab as "commit and
    // jump to the next / previous channel" rather than ordinary focus
    // traversal. The owner wires onTabKey to open the sibling field's
    // editor. Used by the MIXER strip + EDIT row name fields so a whole
    // input list can be renamed keyboard-only: type → Tab → type → Tab.
    //
    // We commit + hand off asynchronously: deleting the live editor from
    // inside its own key dispatch would be a use-after-free, so the Tab
    // handler returns immediately (consuming the key so JUCE doesn't run
    // its default focus traversal) and defers the commit + next-open to
    // the message loop.
    class RenameLabel final : public juce::Label,
                              private juce::KeyListener
    {
    public:
        // shiftDown == true means "go to the previous channel".
        std::function<void (bool /*shiftDown*/)> onTabKey;

    protected:
        void editorShown (juce::TextEditor* ed) override
        {
            juce::Label::editorShown (ed);
            ed->addKeyListener (this);
        }

    private:
        bool keyPressed (const juce::KeyPress& key, juce::Component* /*origin*/) override
        {
            if (key.getKeyCode() != juce::KeyPress::tabKey)
                return false;

            if (onTabKey != nullptr)
            {
                juce::Component::SafePointer<RenameLabel> self (this);
                const bool shift = key.getModifiers().isShiftDown();
                juce::MessageManager::callAsync ([self, shift]
                {
                    if (self == nullptr) return;
                    // NB: Label::hideEditor's param is *discardCurrentEditorContents*
                    // -- pass false so the typed name is committed (fires onTextChange).
                    self->hideEditor (false);                // commit current edit
                    if (self->onTabKey) self->onTabKey (shift);   // open next field
                });
            }
            return true;   // consume Tab -- no default focus traversal
        }
    };
}
