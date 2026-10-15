#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// A clip rendered to audio, played back by a rack instead of running its
// machine.
//
// A frozen clip costs a memory read and the channel strip, much cheaper than
// running a heavy machine. It's rendered off the audio thread by the offline
// path in EngineHost and handed over as an object.
namespace acidulous {

struct FrozenClip {
    std::vector<float> left, right;
    /** Exactly the clip's length. Playback loops on this. */
    int32_t frames = 0;
    /**
     * The tempo it was rendered at. At any other tempo the rack plays the
     * machine live instead (it only stretches through a scene's tempo ramp).
     */
    float bpm = 120.0f;
    /** The clip length in ticks, turned into frames using the tempo above. */
    int32_t ticks = 0;
    /**
     * Frames of ring-out stored after the clip, not part of the loop.
     *
     * The rack plays this with a second cursor at every loop point, over the
     * start of the next pass, and again when the clip stops.
     *
     * 0 means an older freeze with the tail already mixed into the start, so
     * `frames` is the whole file.
     */
    int32_t tail = 0;
};

/**
 * One rack's frozen clips, by scene. Keyed by the scene's stable id, not its
 * index, so moving or deleting scenes doesn't attach a freeze to the wrong one.
 */
struct FrozenSet {
    struct Entry {
        int64_t sceneId = 0;
        std::shared_ptr<const FrozenClip> clip;
    };
    std::vector<Entry> entries;

    // Audio thread: a handful of entries, so a scan beats anything cleverer.
    const FrozenClip *find(int64_t sceneId) const {
        for (const Entry &e : entries) {
            if (e.sceneId == sceneId) return e.clip.get();
        }
        return nullptr;
    }
};

} // namespace acidulous
