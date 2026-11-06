#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include <engine/dsp/RoomCapture.h>

// Plays a level-check tone or a room sweep out of the master in place of
// the song, and records the input in the same blocks, so the passes line up
// with the sweep by frame count. See dsp/RoomCapture.h for what's done with
// the recording.
//
// The buffers are made by prepare() on another thread while the audio thread
// leaves them alone (idle or done); process() never allocates. `busy` and
// `state` work as Audition's do: the audio thread sets busy before reading
// the state, and stop() waits for busy to clear before buffers can change.
namespace acidulous {

class RoomSweep {
  public:
    enum State : int32_t { Idle = 0, Tone = 1, Sweeping = 2, Done = 3 };

    /** Not the audio thread. Builds the sweep and the recording buffer. False if it's busy. */
    bool prepare(const dsp::room::SweepSpec &s, float level) {
        if (state.load() == Sweeping || state.load() == Tone) return false;
        state.store(Idle);
        waitIdle();
        spec = s;
        sweep = dsp::room::makeSweep(s.sweepFrames, s.rate, level);
        recorded.assign(static_cast<size_t>(s.totalFrames()) * 2, 0.0f);
        at = 0;
        posted.store(0, std::memory_order_relaxed);
        peak.store(0.0f, std::memory_order_relaxed);
        deaf.store(false, std::memory_order_relaxed);
        return true;
    }
    /** Starts the sweep prepared, from the next block. */
    void start() {
        if (recorded.empty()) return;
        at = 0;
        posted.store(0, std::memory_order_relaxed);
        state.store(Sweeping);
    }
    /** A steady 1 kHz tone at [level], to set the input level by. */
    void startTone(float level) {
        if (state.load() == Sweeping) return;
        toneLevel.store(level, std::memory_order_relaxed);
        state.store(Tone);
    }
    /** Stops the tone or the sweep, and waits for the audio thread to let go. */
    void stop() {
        const int32_t was = state.load();
        if (was == Tone || was == Sweeping) state.store(Idle);
        waitIdle();
    }

    /** Stops the tone or the sweep without waiting. Audio thread, for a panic. */
    void cancel() {
        int32_t was = Tone;
        if (!state.compare_exchange_strong(was, Idle) && was == Sweeping) state.compare_exchange_strong(was, Idle);
    }
    /**
     * Not the audio thread. Frees the sweep and the recording, unless a tone
     * or sweep has started since (the next window's).
     */
    void release() {
        int32_t was = state.load();
        if (was == Tone || was == Sweeping) return;
        if (was == Done && !state.compare_exchange_strong(was, Idle)) return;
        waitIdle();
        std::vector<float>().swap(sweep);
        std::vector<float>().swap(recorded);
        at = 0;
        posted.store(0, std::memory_order_relaxed);
    }

    State current() const { return static_cast<State>(state.load(std::memory_order_acquire)); }
    /** How far through the sweep, 0..1. */
    float progress() const {
        const int64_t total = spec.totalFrames();
        if (total <= 0) return 0.0f;
        if (current() == Done) return 1.0f;
        return static_cast<float>(static_cast<double>(posted.load(std::memory_order_relaxed)) / static_cast<double>(total));
    }
    /** The loudest input sample recorded. */
    float recordedPeak() const { return peak.load(std::memory_order_relaxed); }
    /** True if blocks came with no input. */
    bool wasDeaf() const { return deaf.load(std::memory_order_relaxed); }
    /** Once Done: interleaved stereo, spec().totalFrames() frames. */
    const std::vector<float> &recording() const { return recorded; }
    const dsp::room::SweepSpec &sweepSpec() const { return spec; }

    /**
     * Audio thread. [in] may be null when there is no input. Writes the
     * tone or the sweep over [out]. True when it did, so the caller leaves
     * the auditions and the monitor out of this block.
     */
    bool process(const float *in, float *out, int32_t frames) {
        if (state.load(std::memory_order_relaxed) == Idle) return false;
        busy.store(true);
        const int32_t s = state.load();
        bool took = false;
        if (s == Tone) {
            const float level = toneLevel.load(std::memory_order_relaxed);
            const double step = 2.0 * M_PI * 1000.0 / std::max(1, spec.rate > 0 ? spec.rate : 48000);
            for (int32_t i = 0; i < frames; ++i) {
                const auto v = static_cast<float>(std::sin(tonePhase)) * level;
                tonePhase += step;
                if (tonePhase > 2.0 * M_PI) tonePhase -= 2.0 * M_PI;
                out[i * 2] = out[i * 2 + 1] = v;
            }
            took = true;
        } else if (s == Sweeping) {
            const int64_t total = spec.totalFrames();
            const int32_t pass = spec.passFrames();
            float loud = peak.load(std::memory_order_relaxed);
            if (in == nullptr) deaf.store(true, std::memory_order_relaxed);
            for (int32_t i = 0; i < frames; ++i) {
                float v = 0.0f;
                if (at < total) {
                    const auto within = static_cast<int32_t>(at % pass);
                    if (within < spec.sweepFrames) v = sweep[static_cast<size_t>(within)];
                    const float l = in != nullptr ? in[i * 2] : 0.0f;
                    const float r = in != nullptr ? in[i * 2 + 1] : 0.0f;
                    recorded[static_cast<size_t>(at) * 2] = l;
                    recorded[static_cast<size_t>(at) * 2 + 1] = r;
                    loud = std::max(loud, std::max(std::fabs(l), std::fabs(r)));
                    ++at;
                }
                out[i * 2] = out[i * 2 + 1] = v;
            }
            peak.store(loud, std::memory_order_relaxed);
            posted.store(at, std::memory_order_relaxed);
            if (at >= total) {
                // Only if nobody stopped it meanwhile.
                int32_t expected = Sweeping;
                state.compare_exchange_strong(expected, Done);
            }
            took = true;
        }
        busy.store(false);
        return took;
    }

  private:
    void waitIdle() const {
        while (busy.load()) {
        }
    }

    std::atomic<int32_t> state{Idle};
    std::atomic<bool> busy{false};
    std::atomic<float> toneLevel{0.0f};
    std::atomic<float> peak{0.0f};
    std::atomic<bool> deaf{false};
    std::atomic<int64_t> posted{0};
    dsp::room::SweepSpec spec;
    std::vector<float> sweep;
    std::vector<float> recorded;
    int64_t at = 0;
    double tonePhase = 0.0;
};

} // namespace acidulous
