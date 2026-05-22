#include "StripColourPicker.h"
#include "../Theme/BrandColors.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace zynforge
{
    StripColourPicker::StripColourPicker (juce::Colour currentColour, Callback cb)
        : current (currentColour), callback (std::move (cb))
    {
        // Eight ZynForge personality colours + two neutrals.
        presets = {
            brand::personality[0], brand::personality[1], brand::personality[2], brand::personality[3], brand::personality[4],
            brand::personality[5], brand::personality[6], brand::personality[7],
            brand::swatchSlate,
            brand::swatchGraphite,
        };

        customButton.onClick = [this] { openCustomSelector(); };
        addAndMakeVisible (customButton);

        resetButton.onClick = [this]
        {
            if (callback) callback (juce::Colour ((juce::uint32) 0));   // alpha = 0 → revert
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        };
        addAndMakeVisible (resetButton);

        const int width  = kMargin * 2 + kCols * kSize + (kCols - 1) * kGap;
        const int height = kMargin * 2 + kRows * kSize + (kRows - 1) * kGap + 36;
        setSize (width, height);
    }

    void StripColourPicker::resized()
    {
        auto r = getLocalBounds().reduced (kMargin);
        r.removeFromTop (kRows * kSize + (kRows - 1) * kGap);
        r.removeFromTop (kGap);
        const int w = (r.getWidth() - kGap) / 2;
        resetButton .setBounds (r.removeFromLeft (w));
        r.removeFromLeft (kGap);
        customButton.setBounds (r);
    }

    void StripColourPicker::paint (juce::Graphics& g)
    {
        g.fillAll (brand::bgPanel);
        g.setColour (brand::edge);
        g.drawRect (getLocalBounds(), 1);

        for (int i = 0; i < (int) presets.size(); ++i)
        {
            const int col = i % kCols;
            const int row = i / kCols;
            const int x = kMargin + col * (kSize + kGap);
            const int y = kMargin + row * (kSize + kGap);
            juce::Rectangle<float> r ((float) x, (float) y, (float) kSize, (float) kSize);

            g.setColour (presets[(std::size_t) i]);
            g.fillRoundedRectangle (r, 4.0f);

            const bool isCurrent = (presets[(std::size_t) i].withAlpha (1.0f).getARGB()
                                    == current.withAlpha (1.0f).getARGB());
            g.setColour (isCurrent ? brand::accentStatus : brand::edge);
            g.drawRoundedRectangle (r, 4.0f, isCurrent ? 2.0f : 1.0f);
        }
    }

    void StripColourPicker::mouseDown (const juce::MouseEvent& e)
    {
        for (int i = 0; i < (int) presets.size(); ++i)
        {
            const int col = i % kCols;
            const int row = i / kCols;
            const int x = kMargin + col * (kSize + kGap);
            const int y = kMargin + row * (kSize + kGap);
            juce::Rectangle<int> r (x, y, kSize, kSize);

            if (r.contains (e.getPosition()))
            {
                if (callback) callback (presets[(std::size_t) i]);
                if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                    box->dismiss();
                return;
            }
        }
    }

    void StripColourPicker::openCustomSelector()
    {
        auto selector = std::make_unique<juce::ColourSelector> (
            juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders,
            4, 0);
        selector->setSize (300, 280);
        selector->setCurrentColour (current);

        class Listener final : public juce::ChangeListener
        {
        public:
            Listener (juce::ColourSelector& sel, Callback cb) : selector (sel), callback (std::move (cb)) {}
            void changeListenerCallback (juce::ChangeBroadcaster*) override
            {
                if (callback) callback (selector.getCurrentColour());
            }
            juce::ColourSelector& selector;
            Callback callback;
        };

        auto listener = std::make_shared<Listener> (*selector, callback);
        selector->addChangeListener (listener.get());

        // Keep listener + selector alive for the lifetime of the CallOutBox.
        struct Holder final : public juce::Component
        {
            std::unique_ptr<juce::ColourSelector> sel;
            std::shared_ptr<Listener>             lst;
            Holder (std::unique_ptr<juce::ColourSelector> s, std::shared_ptr<Listener> l)
                : sel (std::move (s)), lst (std::move (l))
            {
                addAndMakeVisible (*sel);
                setSize (sel->getWidth(), sel->getHeight());
            }
            void resized() override { sel->setBounds (getLocalBounds()); }
        };

        auto holder = std::make_unique<Holder> (std::move (selector), std::move (listener));

        auto screenBounds = getScreenBounds();
        juce::CallOutBox::launchAsynchronously (std::move (holder), screenBounds,
                                                nullptr);

        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }
}
