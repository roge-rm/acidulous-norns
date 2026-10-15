#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A sung take with its pitch marks found, read by Molt.
//
// Where a Take finds transients, this finds the glottal pulses, which lets a
// voice be pitch shifted without changing its formants.
//
// Built on a worker and never changed afterwards. The audio thread only reads
// it, and a new one is swapped in.
namespace acidulous::audio {

/**
 * One pitch mark.
 *
 * [at] is where a period starts, [period] its length in frames, and [voiced]
 * whether this part has a pitch. Unvoiced parts (consonants) get marks at a
 * fixed rate and are copied through without being pitched.
 */
struct Epoch {
    int32_t at = 0;
    float period = 0.0f;
    bool voiced = false;
};

/**
 * Removes everything under [hz] from [x], in place, with [poles] one-pole
 * high-passes in series. Shared by the tracker and the take.
 */
void removeRumble(std::vector<float> &x, float sampleRate, float hz, int poles);

struct Utterance {
    /** Mono. Filled by the caller, then analysed. */
    std::vector<float> mono;
    int32_t frames = 0;
    std::vector<Epoch> epochs;
    /**
     * The median of the take's voiced pitch. The note nearest this plays the
     * take at its own pitch, like a sampler's root key.
     */
    float rootHz = 0.0f;
    std::string name;

    /**
     * Removes rumble from `mono` and then finds the pitch marks.
     *
     * The rumble is removed from `mono` itself because everything downstream
     * reads it. Phone recordings can have a lot of handling noise below
     * `PitchTrack::kMinHz`, which would pull the marks off the glottal pulses
     * and leave a DC offset in each grain.
     *
     * Worker thread. Allocates, and takes a few hundred ms for a ten second
     * take.
     */
    void analyse(float sampleRate);

    bool usable() const { return frames > 1 && !epochs.empty(); }

    /**
     * The index of the last epoch at or before [pos]. A binary search, since
     * the audio thread calls this for every grain.
     */
    int32_t epochAt(float pos) const;
};

/**
 * Finds the pitch track by autocorrelation at a decimated rate. Kept separate
 * so the harness can test it on its own.
 *
 * Voice pitch is between about 70 and 800 Hz, so 8 kHz is plenty. The
 * decimation filter is a six-tap average, which is enough since nothing it
 * lets through can be mistaken for a pitch that low.
 */
struct PitchTrack {
    static constexpr float kMinHz = 70.0f;
    static constexpr float kMaxHz = 800.0f;
    static constexpr float kHopMs = 10.0f;
    static constexpr float kWindowMs = 40.0f;
    /**
     * How periodic a window has to be to count as voiced (normalised
     * autocorrelation at the best lag). A held vowel is above 0.8 and a
     * fricative below 0.2.
     */
    static constexpr float kVoiced = 0.35f;
    /**
     * The lower threshold to stay voiced once voiced. The hysteresis stops a
     * vowel near the threshold flipping in and out, which would change the
     * grain length and spacing.
     */
    static constexpr float kVoicedHold = 0.25f;

    std::vector<float> hz;    // per hop, 0 where unvoiced
    std::vector<float> clarity; // the winning correlation, for the harness
    float hopFrames = 0.0f;   // at the original rate

    /**
     * Set [cleaned] if the caller has already removed the rumble. Filtering
     * twice doubles the slope and can remove a low voice's fundamental.
     */
    void find(const std::vector<float> &mono, int32_t frames, float sampleRate,
              bool cleaned = false);

    /** The period in frames at [pos], interpolated between hops. 0 if unvoiced. */
    float periodAt(float pos, float sampleRate) const;
};

} // namespace acidulous::audio
