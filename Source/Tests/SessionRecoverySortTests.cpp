// Regression: SessionRecoveryDialog sorts its orphan-session list with a
// comparator that MUST satisfy strict weak ordering, or libc++'s hardened
// std::sort calls abort(). The descending branch used `! lt`, which returns
// true for EQUAL elements (and for cmp(a,a)) -- an SWO violation that aborted
// the app on relaunch after an unclean shutdown (the exact moment the recovery
// dialog opens, i.e. when recovery matters most). Fixed to compare with the
// operands swapped.
//
// This builds orphan sessions whose sort keys are all IDENTICAL (empty dirs ->
// sizeBytes 0, trackCount 0) and constructs the dialog, which runs the
// descending sort in its constructor -- so a regression aborts right here.

#include <juce_gui_basics/juce_gui_basics.h>

#include "../UI/SessionRecoveryDialog.h"

namespace zynforge
{
    class SessionRecoverySortTests final : public juce::UnitTest
    {
    public:
        SessionRecoverySortTests() : juce::UnitTest ("Session recovery sort", "zynforge") {}

        void runTest() override
        {
            beginTest ("equal-keyed orphan list sorts without aborting (strict weak ordering)");
            {
                auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("zf-recov-" + juce::Uuid().toString());
                root.deleteRecursively();

                // 24 empty session dirs -> identical sizeBytes (0) + trackCount (0).
                // Equal keys are the worst case for a `! lt` descending comparator:
                // every pair compares as "both less", which is what aborts.
                juce::Array<juce::File> orphans;
                for (int i = 0; i < 24; ++i)
                {
                    auto d = root.getChildFile ("Session_" + juce::String (i));
                    d.createDirectory();
                    orphans.add (d);
                }

                int opened = 0;
                // Constructor runs sortIndicesBy(3, false) (descending by size).
                // Pre-fix: aborts under hardened std::sort. Post-fix: returns.
                SessionRecoveryDialog dlg (orphans, [&] (const juce::File&) { ++opened; });
                expect (true, "constructed + descending-sorted equal keys without aborting");
                expectEquals (opened, 0, "no open callback should fire on construction");

                root.deleteRecursively();
            }
        }
    };

    static SessionRecoverySortTests sessionRecoverySortTests;
}
