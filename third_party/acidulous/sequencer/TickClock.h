#pragma once
#include <atomic>
#include <cstdint>
#include <engine/core/Constants.h>

// The sequencer's time base. The position is an integer tick count; only the
// sub-tick remainder is fractional, and it never accumulates error across ticks.
//
// Audio thread only, except requestSongTempo(), which any thread may call.

namespace acidulous::seq {


class TickClock {
  public:
    void setSampleRate(int32_t sr) {
        sampleRate = sr;
        recompute();
    }

    // Any thread: the song's tempo as set from the UI. The scheduler decides
    // whether it applies (a scene with its own tempo overrides it).
    void requestSongTempo(float bpm) { songTempo.store(clamp(bpm), std::memory_order_relaxed); }
    float songTempoRequested() const { return songTempo.load(std::memory_order_relaxed); }

    /**
     * Audio thread: follow an external clock.
     *
     * The tick length is set directly from the incoming clock and the bpm
     * is worked out from it for the display.
     */
    void setExternalFramesPerTick(double framesPerTick) {
        if (framesPerTick < 1.0) {
            return;
        }
        ramping = false;
        samplesPerTick = framesPerTick;
        currentBpm = clamp(static_cast<float>(static_cast<double>(sampleRate) * 60.0 /
                                              (framesPerTick * static_cast<double>(kPPQN))));
    }

    /** Shift the phase without moving the tick count, to pull into line. */
    void nudge(double frames) { sampleRemainder += frames; }

    // Audio thread: set the effective tempo now, cancelling any ramp.
    void setTempo(float bpm) {
        ramping = false;
        currentBpm = clamp(bpm);
        recompute();
    }

    // Audio thread: glide from the current tempo to `toBpm` over `overTicks`
    // ticks starting now. Used for smooth scene transitions.
    void rampTempo(float toBpm, int64_t overTicks) {
        if (overTicks <= 0) {
            setTempo(toBpm);
            return;
        }
        rampFrom = currentBpm;
        rampTo = clamp(toBpm);
        rampStart = tick;
        rampLength = overTicks;
        ramping = true;
    }
    bool isRamping() const { return ramping; }

    void reset() {
        tick = 0;
        sampleRemainder = 0.0;
        start = end = 0;
        ramping = false;
    }

    // Advance by one engine block. Afterwards [blockStart, blockEnd) is the
    // tick range this block covers, which every player fires events in.
    void advance(int32_t frames) {
        if (ramping) {
            const double progress = static_cast<double>(tick - rampStart) / static_cast<double>(rampLength);
            if (progress >= 1.0) {
                currentBpm = rampTo;
                ramping = false;
            } else {
                currentBpm = static_cast<float>(rampFrom + (rampTo - rampFrom) * progress);
            }
            recompute();
        }
        start = tick;
        sampleRemainder += static_cast<double>(frames);
        while (sampleRemainder >= samplesPerTick) {
            sampleRemainder -= samplesPerTick;
            ++tick;
        }
        end = tick;
    }

    double samplesPerTickNow() const { return samplesPerTick; }

    /** How long a tick would be at [bpm], without setting anything. */
    double framesPerTickAt(double bpm) const {
        if (bpm < 1.0) return samplesPerTick;
        return static_cast<double>(sampleRate) * 60.0 / (bpm * static_cast<double>(kPPQN));
    }
    int32_t rate() const { return sampleRate; }

    /**
     * How many frames into the block just advanced the tick boundary [t]
     * falls. Exact, because `sampleRemainder` is the sub-tick phase at the
     * end of the block and the tempo doesn't change within a block.
     *
     * Blocks almost never start on a tick boundary, so without this events
     * like metronome clicks land up to a tick late.
     */
    double frameOffsetOfTick(int64_t t, int32_t frames) const {
        return static_cast<double>(frames) - sampleRemainder -
               static_cast<double>(end - t) * samplesPerTick;
    }

    int64_t blockStart() const { return start; }
    int64_t blockEnd() const { return end; }
    int64_t position() const { return tick; }
    float bpm() const { return currentBpm; }

  private:
    static float clamp(float bpm) {
        if (bpm < 20.0f) return 20.0f;
        if (bpm > 999.0f) return 999.0f;
        return bpm;
    }

    void recompute() {
        // At 48 kHz and 120 bpm this is exactly 100 samples per tick.
        samplesPerTick = static_cast<double>(sampleRate) * 60.0 /
                         (static_cast<double>(currentBpm) * static_cast<double>(kPPQN));
    }

    std::atomic<float> songTempo{120.0f};
    float currentBpm = 120.0f;
    int32_t sampleRate = kSampleRate;
    double samplesPerTick = 100.0;
    double sampleRemainder = 0.0;
    int64_t tick = 0;
    int64_t start = 0;
    int64_t end = 0;

    bool ramping = false;
    float rampFrom = 120.0f;
    float rampTo = 120.0f;
    int64_t rampStart = 0;
    int64_t rampLength = 0;
};

} // namespace acidulous::seq
