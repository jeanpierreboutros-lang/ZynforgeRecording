#include "../Audio/AudioEngine.h"

#include <limits>

namespace zynforge
{
class PlaylistBoundaryTests final : public juce::UnitTest
{
public:
    PlaylistBoundaryTests() : UnitTest ("Playlist input boundaries", "zynforge") {}

    static juce::var playlist (const juce::var& timeline, const juce::var& fileStart,
                               const juce::var& length, const juce::var& fadeIn = 0,
                               const juce::var& fadeOut = 0)
    {
        auto* clip = new juce::DynamicObject();
        clip->setProperty ("name", "boundary fixture");
        clip->setProperty ("tlStart", timeline);
        clip->setProperty ("fileStart", fileStart);
        clip->setProperty ("fileLen", length);
        clip->setProperty ("fadeIn", fadeIn);
        clip->setProperty ("fadeOut", fadeOut);
        auto* take = new juce::DynamicObject();
        take->setProperty ("name", "Take 1");
        take->setProperty ("clips", juce::Array<juce::var> { juce::var (clip) });
        auto* track = new juce::DynamicObject();
        track->setProperty ("track", 0);
        track->setProperty ("activeTake", 0);
        track->setProperty ("takes", juce::Array<juce::var> { juce::var (take) });
        // Exercise the actual project JSON number representations, rather
        // than assuming in-memory var integers model the persisted input.
        return juce::JSON::parse (juce::JSON::toString (
            juce::var (juce::Array<juce::var> { juce::var (track) })));
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);

        beginTest ("Valid playlist int64 coordinates above 2^53 retain exact samples");
        {
            AudioEngine engine;
            const juce::int64 coordinate = ((juce::int64) 1 << 53) + 1;
            engine.loadPlaylistsFromJson (playlist (coordinate, coordinate, 17, 3, 5));
            const auto* clips = engine.tryClipsFor (0);
            expect (clips != nullptr && clips->size() == 1);
            if (clips != nullptr && clips->size() == 1)
            {
                expectEquals (clips->front().timelineStartSamples, coordinate);
                expectEquals (clips->front().fileStartSamples, coordinate);
                expectEquals (clips->front().fileLengthSamples, (juce::int64) 17);
                const auto saved = engine.playlistsToJson();
                engine.loadPlaylistsFromJson (saved);
                expectEquals (engine.clipsFor (0).front().timelineStartSamples, coordinate);
            }
        }

        const juce::int64 halfRange = (juce::int64) 1 << 62;
        struct InvalidInput { const char* reason; juce::var document; };
        const std::vector<InvalidInput> invalid {
            { "negative timeline", playlist (-1, 0, 16) },
            { "negative source offset", playlist (0, -1, 16) },
            { "negative length", playlist (0, 0, -1) },
            { "negative fade", playlist (0, 0, 16, -1, 0) },
            { "source endpoint overflow", playlist (0, halfRange, halfRange) },
            { "timeline endpoint overflow", playlist (halfRange, 0, halfRange) },
            { "out of range floating sample", playlist (1.0e100, 0, 16) },
            { "int64 maximum plus length", playlist (std::numeric_limits<juce::int64>::max(), 0, 1) }
        };
        for (const auto& input : invalid)
        {
            beginTest ("Invalid playlist clip is rejected with an initialized empty take: "
                       + juce::String (input.reason));
            AudioEngine engine;
            engine.loadPlaylistsFromJson (input.document);
            expect (engine.isTrackArrangementEmpty (0), "invalid clip was published to the player");
            expectEquals (engine.getTakeCount (0), 1, "invalid clip erased the initialized take");
            const auto serialized = engine.playlistsToJson();
            const auto* tracks = serialized.getArray();
            expect (tracks != nullptr && tracks->size() == 1);
            if (tracks != nullptr && tracks->size() == 1)
            {
                const auto* takes = (*tracks)[0]["takes"].getArray();
                expect (takes != nullptr && takes->size() == 1);
                if (takes != nullptr && takes->size() == 1)
                {
                    const auto* savedClips = (*takes)[0]["clips"].getArray();
                    expect (savedClips != nullptr && savedClips->isEmpty(),
                            "rejected clip reappeared in the saved playlist");
                }
            }
        }
    }
};
static PlaylistBoundaryTests playlistBoundaryTests;
}
