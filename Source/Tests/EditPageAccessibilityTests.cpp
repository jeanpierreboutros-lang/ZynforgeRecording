// Headless accessibility test for the EDIT page's zoom controls. The four
// zoom buttons are labelled "V+/V-/H+/H-" on screen, which a screen reader
// would speak as glyphs; each gets a descriptive title. Verify the spoken
// names resolve. EditPage needs an AudioEngine, built in test mode (no
// device); its 24 Hz timer never ticks without a running message loop.

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <set>

#include "../UI/EditPage.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class EditPageAccessibilityTests final : public juce::UnitTest
    {
    public:
        EditPageAccessibilityTests() : UnitTest ("EditPage accessibility", "zynforge") {}

        static void walk (juce::Component& c, const std::function<void (juce::Component&)>& fn)
        {
            for (int i = 0; i < c.getNumChildComponents(); ++i)
            {
                auto* ch = c.getChildComponent (i);
                fn (*ch);
                walk (*ch, fn);
            }
        }

        void runTest() override
        {
            beginTest ("Zoom buttons expose spoken accessible names");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                {
                    AudioEngine engine;
                    EditPage page (engine);
                    page.setBounds (0, 0, 900, 500);

                    std::set<juce::String> names;
                    walk (page, [&] (juce::Component& c)
                    {
                        if (auto* b = dynamic_cast<juce::Button*> (&c))
                            names.insert (b->getTitle().isNotEmpty() ? b->getTitle() : b->getButtonText());
                    });

                    for (auto* expected : { "Vertical zoom in", "Vertical zoom out",
                                            "Horizontal zoom in", "Horizontal zoom out",
                                            "Follow playhead" })
                        expect (names.count (expected) > 0,
                                juce::String (expected) + " has no spoken accessible name");
                }
                AudioEngine::setTestModeSkipAudioInit (false);
            }

            beginTest ("Recording locks row edits while zoom stays clickable");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                {
                    AudioEngine engine;
                    EditPage page (engine);
                    page.setBounds (0, 0, 900, 500);

                    juce::Viewport* viewport = nullptr;
                    juce::Button* horizontalIn = nullptr;
                    juce::Button* verticalIn = nullptr;
                    walk (page, [&] (juce::Component& c)
                    {
                        if (auto* v = dynamic_cast<juce::Viewport*> (&c)) viewport = v;
                        if (auto* b = dynamic_cast<juce::Button*> (&c))
                        {
                            if (b->getTitle() == "Horizontal zoom in") horizontalIn = b;
                            if (b->getTitle() == "Vertical zoom in") verticalIn = b;
                        }
                    });
                    expect (viewport != nullptr && horizontalIn != nullptr && verticalIn != nullptr);
                    if (viewport != nullptr && horizontalIn != nullptr && verticalIn != nullptr)
                    {
                        page.setReadOnlyWhileRecording (true);
                        bool selfClicks = true, childClicks = true;
                        viewport->getViewedComponent()->getInterceptsMouseClicks (selfClicks, childClicks);
                        expect (! selfClicks && ! childClicks, "Rows must reject edits during capture");
                        expect (page.isEnabled() && horizontalIn->isEnabled() && verticalIn->isEnabled());
                        expect (horizontalIn->getWidth() >= 44 && verticalIn->getWidth() >= 44);
                        horizontalIn->onClick();
                        verticalIn->onClick();
                        expect (page.getZoom() > 1.0f && page.getVerticalZoom() > 1.0f);

                        page.setReadOnlyWhileRecording (false);
                        viewport->getViewedComponent()->getInterceptsMouseClicks (selfClicks, childClicks);
                        expect (selfClicks && childClicks, "Row edits must return after capture");
                    }
                }
                AudioEngine::setTestModeSkipAudioInit (false);
            }
        }
    };

    static EditPageAccessibilityTests editPageAccessibilityTestsInstance;
}
