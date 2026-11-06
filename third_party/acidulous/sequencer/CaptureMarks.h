#pragma once
#include <atomic>
#include <cstdint>

#include <engine/core/Constants.h>

// Where the song was during each part of a recording.
//
// Capture writes the input to a file without knowing where the song is, and
// the scheduler doesn't know a recording is happening. The audio thread joins
// them by stamping a mark at each cell boundary with the frames written so far
// and the cell just entered.
//
// That's all splitting a take needs. Consecutive marks pair into segments,
// each one cell's region of the file, so Kotlin doesn't need to work out scene
// lengths, repeats or tempo overrides. An end mark (cycleTicks 0) says the song
// stopped there, and ends the segment before it.
//
// Limits:
//
//   - a boundary is only accurate to the block (64 frames, 1.3 ms);
//   - if the capture ring overflowed, frame indexes after the drop are wrong,
//     so the marks are poisoned and the take can be kept but not split.
//
// Header-only so tools/ can use it without the engine library.
namespace acidulous::seq {

/** One boundary: where in the file, and where in the song. */
struct CaptureMark {
    int64_t frame = 0;       // frames written to the capture file at this moment
    int64_t sceneId = 0;     // the cell being entered
    int32_t tick = 0;        // how far into that cell's cycle it starts
    int32_t cycleTicks = 0;  // the cycle length, bars x repeat
    float bpm = 120.0f;      // the tempo it was recorded at
};

class CaptureMarks {
  public:
    /**
     * How many boundaries one take may cross. Five minutes of two-second
     * cells is 150, so this is generous. Going over poisons the split rather
     * than silently losing the end.
     */
    static constexpr int32_t kMax = 512;

    /** Mount thread, when a lane is armed and when a capture starts, before it runs. */
    void reset() {
        n.store(0, std::memory_order_relaxed);
        bad.store(false, std::memory_order_relaxed);
        lastScene = kNoScene;
        lastTick = -1;
    }

    /**
     * Audio thread, once a block while a recording is armed and the song is
     * playing.
     *
     * Writes a mark at the start, when the cell changes, and when the cycle
     * starts again (the tick going backwards).
     *
     * [tickFrame] is how many frames into this block the tick [cycleTick] was
     * reached (negative when it was in the block before), and [framesPerTick]
     * the tick's length, so the mark pairs a tick with the exact frame it fell
     * on. The scheduler notices a new cycle up to a tick late (the launcher
     * applies a cycle that ends on a block's last tick in the next block), so
     * a cell whose top was in this block or the one before is backed up to
     * its top, tick 0. Every pass then starts at tick 0 in both modes.
     */
    void observe(int64_t frame, int64_t sceneId, int64_t cycleTick, int32_t cycleTicks, float bpm,
                 double tickFrame = 0.0, double framesPerTick = 0.0) {
        // No cycle means no cell. In clip mode a rack with nothing launched
        // isn't in any cell, so audio recorded before a clip is tapped isn't
        // marked. A clip that stops ends the take there.
        if (cycleTicks <= 0) {
            halt(frame);
            return;
        }
        const bool fresh = lastScene == kNoScene;
        const bool moved = sceneId != lastScene;
        const bool wrapped = cycleTick < lastTick;
        lastScene = sceneId;
        lastTick = cycleTick;
        if (!fresh && !moved && !wrapped) return;

        double at = static_cast<double>(frame) + tickFrame;
        int64_t tick = cycleTick;
        if (framesPerTick > 0.0) {
            const double top = at - static_cast<double>(cycleTick) * framesPerTick;
            // A tick of slack for rounding in the clock's remainder.
            const double reach = tickFrame + kBlockFrames + framesPerTick;
            if (cycleTick > 0 && static_cast<double>(cycleTick) * framesPerTick <= reach && top >= 0.0) {
                at = top;
                tick = 0;
            }
            // The first block of a file can't reach back before frame 0.
            if (at < 0.0) {
                at += framesPerTick;
                tick = (tick + 1) % cycleTicks;
            }
        }
        add(static_cast<int64_t>(at + 0.5), sceneId, tick, cycleTicks, bpm);
    }

    /**
     * Audio thread: the song stopped (or the armed rack's clip did) while
     * recording. Writes an end mark, cycleTicks 0, so the take ends here
     * rather than wherever the capture is stopped, and the next play starts
     * a fresh cell.
     */
    void halt(int64_t frame) {
        if (lastScene == kNoScene) return;
        lastScene = kNoScene;
        lastTick = -1;
        add(frame, 0, 0, 0, 0.0f);
    }

    /** The ring dropped frames, so every frame index after it is wrong. */
    void poison() { bad.store(true, std::memory_order_relaxed); }

    bool poisoned() const { return bad.load(std::memory_order_relaxed); }
    int32_t count() const { return n.load(std::memory_order_acquire); }
    const CaptureMark &at(int32_t i) const { return marks[i]; }

  private:
    static constexpr int64_t kNoScene = INT64_MIN;

    void add(int64_t frame, int64_t sceneId, int64_t tick, int32_t cycleTicks, float bpm) {
        const int32_t at = n.load(std::memory_order_relaxed);
        if (at >= kMax) {
            bad.store(true, std::memory_order_relaxed);
            return;
        }
        marks[at].frame = frame;
        marks[at].sceneId = sceneId;
        marks[at].tick = static_cast<int32_t>(tick);
        marks[at].cycleTicks = cycleTicks;
        marks[at].bpm = bpm;
        n.store(at + 1, std::memory_order_release);
    }

    CaptureMark marks[kMax];
    std::atomic<int32_t> n{0};
    std::atomic<bool> bad{false};
    // Audio thread only.
    int64_t lastScene = kNoScene;
    int64_t lastTick = -1;
};

} // namespace acidulous::seq
