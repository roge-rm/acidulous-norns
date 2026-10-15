#pragma once
#include <atomic>
#include <cstdint>

// Where the song was during each part of a recording.
//
// Capture writes the input to a file without knowing where the song is, and
// the scheduler doesn't know a recording is happening. The audio thread joins
// them by stamping a mark at each cell boundary with the frames written so far
// and the cell just entered.
//
// That's all splitting a take needs. Consecutive marks pair into segments,
// each one cell's region of the file, so Kotlin doesn't need to work out scene
// lengths, repeats or tempo overrides.
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

    /** Mount thread, when a recording is armed. */
    void reset() {
        n.store(0, std::memory_order_relaxed);
        bad.store(false, std::memory_order_relaxed);
        lastScene = kNoScene;
        lastTick = -1;
    }

    /**
     * Audio thread, once a block while a recording is armed.
     *
     * Writes a mark at the start, when the cell changes, and when the cycle
     * starts again (the tick going backwards).
     */
    void observe(int64_t frame, int64_t sceneId, int64_t cycleTick, int32_t cycleTicks, float bpm) {
        // No cycle means no cell. In clip mode a rack with nothing launched
        // isn't in any cell, so audio recorded before a clip is tapped isn't
        // marked. The same applies before the transport starts.
        if (cycleTicks <= 0) return;
        const bool fresh = lastScene == kNoScene;
        const bool moved = sceneId != lastScene;
        const bool wrapped = cycleTick < lastTick;
        lastScene = sceneId;
        lastTick = cycleTick;
        if (!fresh && !moved && !wrapped) return;

        const int32_t at = n.load(std::memory_order_relaxed);
        if (at >= kMax) {
            bad.store(true, std::memory_order_relaxed);
            return;
        }
        marks[at].frame = frame;
        marks[at].sceneId = sceneId;
        marks[at].tick = static_cast<int32_t>(cycleTick);
        marks[at].cycleTicks = cycleTicks;
        marks[at].bpm = bpm;
        n.store(at + 1, std::memory_order_release);
    }

    /** The ring dropped frames, so every frame index after it is wrong. */
    void poison() { bad.store(true, std::memory_order_relaxed); }

    bool poisoned() const { return bad.load(std::memory_order_relaxed); }
    int32_t count() const { return n.load(std::memory_order_acquire); }
    const CaptureMark &at(int32_t i) const { return marks[i]; }

  private:
    static constexpr int64_t kNoScene = INT64_MIN;

    CaptureMark marks[kMax];
    std::atomic<int32_t> n{0};
    std::atomic<bool> bad{false};
    // Audio thread only.
    int64_t lastScene = kNoScene;
    int64_t lastTick = -1;
};

} // namespace acidulous::seq
