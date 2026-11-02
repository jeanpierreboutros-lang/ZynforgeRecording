// Regression guards for the 2026-08-12 EDIT-view audit.
//
// The EDIT row is a private nested class inside EditPage, so the fixes that
// live in its mouse handlers (meter condemn, routing-combo fallback,
// wave-cache ordering) can't be driven headlessly.
// What IS testable is the shared logic those fixes were extracted onto --
// EditTimeline.h.
// The rest is smoke-test territory; see CHANGELOG.

#include <juce_core/juce_core.h>

#include "../Audio/AudioEngine.h"
#include "../UI/EditTimeline.h"
#include "../UI/WaveCacheFreshness.h"
#include "../UI/WaveformOverview.h"

namespace zynforge
{
    class EditViewFixTests final : public juce::UnitTest
    {
    public:
        EditViewFixTests() : juce::UnitTest ("EDIT view fixes 2026-08-12", "zynforge") {}

        static Clip clipAt (juce::int64 start, juce::int64 len)
        {
            Clip c;
            c.timelineStartSamples = start;
            c.fileStartSamples     = 0;
            c.fileLengthSamples    = len;
            return c;
        }

        void runTest() override
        {
            beginTest ("Zoomed-out waveform keeps an isolated transient without making it a solid block");
            {
                const auto overview = buildWaveformOverview (4, 0.0, 4.0,
                    [] (double a, double)
                    { return a < 1.0 / 16.0 ? 0.95f : 0.20f; });
                expectEquals ((int) overview.body.size(), 4);
                expectWithinAbsoluteError (overview.body[0], 0.20f, 0.001f);
                expectWithinAbsoluteError (overview.peaks[0], 0.95f, 0.001f);
                expectWithinAbsoluteError (overview.body[1], 0.20f, 0.001f);

                const auto sustained = buildWaveformOverview (2, 0.0, 2.0,
                    [] (double, double) { return 0.70f; });
                expectWithinAbsoluteError (sustained.body[0], 0.70f, 0.001f);
                expectWithinAbsoluteError (sustained.peaks[0], 0.70f, 0.001f);
            }

            beginTest ("A finalized take invalidates a waveform cache saved before its audio");
            {
                const auto session = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getNonexistentChildFile ("zynforge-wave-cache-test", "", true);
                expect (session.createDirectory().wasOk());
                const auto audioDir = session.getChildFile ("Audio Files");
                expect (audioDir.createDirectory().wasOk());
                const auto cache = session.getChildFile ("WaveCache.wfm");
                const auto take = audioDir.getChildFile ("Track_01.wav");
                expect (cache.replaceWithText ("old thumbnail"));
                expect (take.replaceWithText ("new take"));

                const auto now = juce::Time::getCurrentTime().toMilliseconds();
                expect (cache.setLastModificationTime (juce::Time (now - 60000)));
                expect (take.setLastModificationTime (juce::Time (now)));
                expect (waveCachePredatesAudio (cache, session));

                expect (cache.setLastModificationTime (juce::Time (now + 60000)));
                expect (! waveCachePredatesAudio (cache, session));
                session.deleteRecursively();
            }

            beginTest ("Horizontal zoom steps return exactly to fit-to-take");
            {
                const float in = steppedTimelineZoom (1.0f, true);
                expect (in > 1.0f);
                expectEquals (steppedTimelineZoom (in, false), 1.0f);
                expectEquals (steppedTimelineZoom (1.0f, false), 1.0f);
                expect (steppedTimelineZoom (16.0f, true) > 16.0f,
                        "a long finalized take must zoom beyond the old 16x ceiling");
                expectEquals (steppedTimelineZoom (kMaxTimelineZoom, true),
                              kMaxTimelineZoom);
            }

            beginTest ("Deep zoom draws only grid ticks near the viewport");
            {
                const auto ticks = visibleTimelineTicks (0.2, 10.0 * 3600.0,
                                                         5.0 * 3600.0,
                                                         5.0 * 3600.0 + 30.0);
                expect (ticks.first > 0);
                expect (ticks.last - ticks.first < 160,
                        "a 30-second view must not iterate the whole 10-hour grid");
                const auto empty = visibleTimelineTicks (0.0, 3600.0, 0.0, 30.0);
                expect (empty.first > empty.last);
            }

            beginTest ("Manual timeline navigation yields to a rolling take, but vertical scroll does not");
            {
                TimelineFollowState follow;
                expect (follow.isFollowing());
                follow.transportChanged (true);
                follow.horizontalScroll (0, 0, false);  // vertical-only scroll
                expect (follow.isFollowing());
                follow.horizontalScroll (0, 100, true); // automatic page follow
                expect (follow.isFollowing());
                follow.horizontalScroll (100, 40, false); // manual trackpad/minimap pan
                expect (! follow.isFollowing());
                follow.setFollowing (true);              // FOLLOW button
                expect (follow.isFollowing());
                follow.userZoom();                       // H+ / Cmd-wheel
                expect (! follow.isFollowing());
                follow.transportChanged (false);
                follow.transportChanged (true);          // next pass follows by default
                expect (follow.isFollowing());
            }

            beginTest ("A three-hour recording extends the scrollable timeline");
            {
                constexpr juce::int64 sr = 48000;
                const auto base = recordingTimelineSpanSamples (0, 0, (double) sr);
                expectEquals (base, 5LL * 60 * sr);
                const int viewW = 1000;
                expectEquals (recordingContentWidth (viewW, 1.0f, base, base), viewW);
                const auto threeHours = 3LL * 3600 * sr;
                const auto span = recordingTimelineSpanSamples (threeHours, 0, (double) sr);
                const int contentW = recordingContentWidth (viewW, 1.0f, span, base);
                expectEquals (contentW, viewW * 36);
                expect (contentW - viewW > viewW,
                        "the viewport needs room to follow a long take");
                expectEquals (recordingTimelineSpanSamples (sr * 60, 0, (double) sr), base);
            }

            // ── The empty-session lane span is ONE number ────────────────────
            // The ruler drew 300 s while laneTimelineSamples() returned 60 s, so on
            // a session with no audio anything placed under the ruler's 2:30 mark
            // was stored at 0:30. Both now route through this helper.
            beginTest ("Notional empty-lane span is 5 minutes at the DEVICE rate");
            {
                expectEquals (kNotionalEmptyLaneSec, 300.0);
                expectEquals (notionalEmptyLaneSamples (48000.0), (juce::int64) (48000 * 300));
                expectEquals (notionalEmptyLaneSamples (96000.0), (juce::int64) (96000 * 300));
                expectEquals (notionalEmptyLaneSamples (44100.0), (juce::int64) (44100 * 300));
                // A zero / bogus device rate must not collapse the lane to 0
                // samples (which would make every hit-test map to sample 0).
                expect (notionalEmptyLaneSamples (0.0) > 0, "must not collapse to a zero-length lane");
            }

            // ── Edit-group broadcasts resolve the PEER's own clip ────────────
            // Clip indices are per-track. Broadcasting this row's index edited
            // whichever clip sat at that position on the peer.
            beginTest ("clipIndexAtMidpoint maps by timeline overlap, not index");
            {
                // Peer split into three; source is one clip covering [1000,2000).
                const std::vector<Clip> peer {
                    clipAt (0,    1000),
                    clipAt (1000, 1000),
                    clipAt (2000, 1000),
                };
                // Source clip 0 spans [1000,2000) -> peer clip 1, NOT peer clip 0.
                expectEquals (clipIndexAtMidpoint (peer, 1000, 1000), 1,
                              "must follow the timeline, not reuse the source index");
                expectEquals (clipIndexAtMidpoint (peer, 0,    1000), 0);
                expectEquals (clipIndexAtMidpoint (peer, 2000, 1000), 2);

                // Identical arrangements: index mapping is the identity, so the
                // normal grouped-pair case is unchanged.
                const std::vector<Clip> same { clipAt (0, 500), clipAt (500, 500) };
                expectEquals (clipIndexAtMidpoint (same, 0,   500), 0);
                expectEquals (clipIndexAtMidpoint (same, 500, 500), 1);

                // Peer has nothing under the source clip -> -1 so the caller
                // SKIPS that peer instead of editing an arbitrary clip.
                const std::vector<Clip> gap { clipAt (0, 100) };
                expectEquals (clipIndexAtMidpoint (gap, 5000, 1000), -1);
                expectEquals (clipIndexAtMidpoint ({},   0,    1000), -1);
                // Degenerate source clip.
                expectEquals (clipIndexAtMidpoint (gap, 0, 0), -1);
            }

        }
    };

    static EditViewFixTests editViewFixTests;
}
