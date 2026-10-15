#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// A tuner: names the note being played into the input.
//
// It's only a readout and never touches the audio. It costs nothing until the
// record window turns it on.
//
// It listens before the input chain, since a gate or amp there could hide
// the note.
namespace acidulous::audio {

/**
 * The pitch detector, kept separate from the ring so the harness can test it
 * on its own buffers.
 *
 * Two stages:
 *
 *   - Coarse: normalised autocorrelation over every lag in range at a
 *     decimated rate. Cheap enough to search five and a half octaves, but
 *     too coarse to give cents.
 *   - Fine: at the full rate, over the few lags around the coarse result,
 *     with a parabola through the best peak for sub-sample accuracy.
 *
 * Autocorrelation often finds a peak at twice the true lag that's as strong
 * or stronger, which reads an octave low. To avoid that it takes the shortest
 * lag whose peak is within a margin of the best one.
 */
struct PitchFinder {
    /** Search range: a five-string bass's low B to past a guitar's top fret. */
    static constexpr float kMinHz = 27.0f;
    static constexpr float kMaxHz = 1400.0f;
    /** The coarse stage's rate. Well above 2 x kMaxHz, and divides 48 kHz evenly. */
    static constexpr float kCoarseRate = 12000.0f;
    /**
     * How periodic the signal must be before a note is named. A plucked
     * string stays above 0.9, and room noise or hum is under 0.5. Showing no
     * note is better than a readout that jumps around.
     */
    static constexpr float kClarity = 0.72f;
    /** How close to the best peak a shorter lag has to be to win it. */
    static constexpr float kOctaveMargin = 0.86f;

    /**
     * The frequency in [mono], or 0 when there's no note.
     *
     * If [clarity] is given it's set to the winning correlation.
     */
    static float find(const float *mono, int32_t frames, float sampleRate, float *clarity = nullptr);
};

/**
 * The ring the audio thread fills and the reader analyses.
 *
 * Single producer on the audio thread, single consumer on whichever thread
 * polls. The consumer copies the newest window out and checks the write
 * index hasn't overtaken it, since a torn window would add a false period.
 */
class Tuner {
  public:
    /** A little over half a second: sixteen periods of the lowest note in range. */
    static constexpr int32_t kWindow = 24576;

    Tuner() : ring(static_cast<size_t>(kWindow) * 2, 0.0f) {}

    void setEnabled(bool on) {
        if (!on) hzOut.store(0.0f, std::memory_order_relaxed);
        enabled.store(on, std::memory_order_release);
    }
    bool isEnabled() const { return enabled.load(std::memory_order_acquire); }

    /** Audio thread. Interleaved stereo, or nothing when no stream is open. */
    void push(const float *interleaved, int32_t frames);

    /**
     * Reader thread. The note in the last half second, or 0. The analysis
     * runs here, on the caller's thread.
     */
    float analyse(float sampleRate);

    /** The last answer, without analysing again. */
    float hz() const { return hzOut.load(std::memory_order_relaxed); }

  private:
    std::vector<float> ring;
    std::atomic<int64_t> writeIndex{0};
    std::atomic<bool> enabled{false};
    std::atomic<float> hzOut{0.0f};
    std::vector<float> window;  // reader thread only
    std::vector<float> coarse;  // reader thread only
};

} // namespace acidulous::audio
