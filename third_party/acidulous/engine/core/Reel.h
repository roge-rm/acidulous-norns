#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <engine/core/Mapping.h>
#include <memory>
#include <vector>

// An audio track's recordings, and where in the song each one plays.
//
// Works like FrozenSet: keyed by scene, per rack, mounted whole and swapped
// at a block boundary. The differences:
//
//   - A region is a window into a file. A take sung across four scenes is one
//     file and four regions at different offsets, so splitting at scene lines
//     loses no audio.
//   - Four lanes play at once, like a four-track, and are summed.
//
// Built on a worker and never changed. The audio thread only reads it, and a
// new one is swapped in.
namespace acidulous::audio {

/**
 * The longest take that's held in memory: two minutes. Shorter takes are
 * decoded into vectors.
 */
constexpr int32_t kResidentSeconds = 120;

/**
 * The longest take allowed: half an hour. Takes longer than
 * [kResidentSeconds] are converted once to the engine's flat format and
 * memory-mapped, so only the part being played uses RAM. See `audio::Mapping`.
 */
constexpr int32_t kMaxReelSeconds = 1800;

/** How many lanes Bias has, like a four-track. */
constexpr int32_t kReelLanes = 4;

/**
 * Float to int16, clamped and rounded.
 *
 * A take can go above full scale (the channel strip brings it down), so this
 * clamps instead of wrapping, which would click.
 */
inline int16_t toI16(float v) {
    const float x = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<int16_t>(x * 32767.0f + (x >= 0.0f ? 0.5f : -0.5f));
}

struct Reel {
    /**
     * One decoded file, shared by every region that reads it.
     *
     * Stored as int16, and mono stays mono, so a five-minute take is 29 MB
     * instead of 115 MB as stereo float. A phone mic's noise floor is well
     * above 16 bits, so nothing audible is lost.
     *
     * Shared with `shared_ptr` so a long take split into many regions is
     * only decoded once.
     */
    struct Source {
        /**
         * Always read through these pointers, not the storage below. A source
         * is either decoded into `own` or mapped from a cache file, and `lp`
         * and `rp` point into whichever one is used.
         *
         * Planar, with mono kept as one channel. The mapped file uses the
         * same layout.
         */
        const int16_t *lp = nullptr;
        const int16_t *rp = nullptr;
        int32_t frames = 0;
        bool stereo = false;

        std::vector<int16_t> own;   // resident: the samples themselves
        std::shared_ptr<Mapping> map; // mapped: the file they live in

        /** Resident, from planar channels already in hand. */
        void hold(std::vector<int16_t> &&planes, int32_t n, bool isStereo) {
            own = std::move(planes);
            frames = n;
            stereo = isStereo;
            lp = own.data();
            rp = isStereo ? own.data() + n : own.data();
        }

        /** Mapped, from a converted cache file: left plane then right. */
        bool point(std::shared_ptr<Mapping> m, int32_t n, bool isStereo) {
            const size_t want = static_cast<size_t>(n) * (isStereo ? 2 : 1) * sizeof(int16_t);
            if (!m || !m->valid() || m->size() < want) return false;
            map = std::move(m);
            frames = n;
            stereo = isStereo;
            lp = reinterpret_cast<const int16_t *>(map->data());
            rp = isStereo ? lp + n : lp;
            return true;
        }

        /** Hints to the kernel which frames a cell is about to play. */
        void willNeed(int32_t from, int32_t count) const {
            if (!map) return;
            const size_t unit = sizeof(int16_t);
            map->willNeed(static_cast<size_t>(from) * unit, static_cast<size_t>(count) * unit);
            if (stereo) {
                map->willNeed(static_cast<size_t>(frames + from) * unit,
                              static_cast<size_t>(count) * unit);
            }
        }
    };

    /** One lane of one cell: which file, which part of it, and when. */
    struct Region {
        std::shared_ptr<const Source> source;
        int32_t offset = 0;    // frames into the source where this cell starts
        int32_t frames = 0;    // how many of them are this cell's
        int32_t startTick = 0; // where in the cycle it begins, non-zero for a punch-in
        int32_t ticks = 0;     // the cycle it was recorded against
        float bpm = 120.0f;    // the tempo it was recorded at
        bool loop = false;     // wrap within the region instead of going silent
        /**
         * Fade in and out lengths, in frames.
         *
         * Equal-power, so two overlapping takes crossfade at a steady level.
         * A linear fade would dip 3 dB in the middle.
         */
        int32_t fadeIn = 0;
        int32_t fadeOut = 0;

        /** The envelope at [at] frames into the region: 0..1. */
        float fadeAt(int64_t at) const {
            float g = 1.0f;
            if (fadeIn > 0 && at < fadeIn) g = static_cast<float>(at) / static_cast<float>(fadeIn);
            if (fadeOut > 0 && at > frames - fadeOut) {
                const float k = static_cast<float>(frames - at) / static_cast<float>(fadeOut);
                g = g < k ? g : k;
            }
            if (g <= 0.0f) return 0.0f;
            if (g >= 1.0f) return 1.0f;
            // Equal power: the squares of this and its mirror sum to one.
            return std::sin(g * 1.5707963f);
        }
    };

    /** One cell: the lanes that have anything on them. */
    struct Cell {
        int64_t sceneId = 0;
        Region lanes[kReelLanes];
        bool has(int32_t lane) const {
            return lane >= 0 && lane < kReelLanes && lanes[lane].source != nullptr &&
                   lanes[lane].frames > 0;
        }
    };

    std::vector<Cell> cells;

    /** Audio thread. Only a handful of cells, so a linear scan is fine. */
    const Cell *find(int64_t sceneId) const {
        if (sceneId == 0) return nullptr;
        for (const Cell &c : cells) {
            if (c.sceneId == sceneId) return &c;
        }
        return nullptr;
    }
};

/**
 * A read position that runs free between blocks and only resyncs when it has
 * drifted more than 256 frames. Recomputing it from the tick every block
 * would step it by a sample or two each time, which clicks. Same idea as
 * `Rack::syncFrozen`.
 */
struct FrameCursor {
    int64_t at = -1;

    void invalidate() { at = -1; }

    void anchor(int64_t target) {
        if (at < 0 || std::llabs(target - at) > 256) at = target;
    }
};

} // namespace acidulous::audio
