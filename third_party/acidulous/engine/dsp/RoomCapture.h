#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Capturing a room's impulse: an exponential sine sweep is played into the
// room and recorded, then deconvolved with its inverse (time-reversed, with
// 6 dB/oct taken off as it falls). The room's linear response comes out at
// the sweep's length plus the latency; the harmonics of any distortion come
// out earlier and are cut off with everything before the direct sound.
//
// Off the audio thread. RoomSweep (engine/core) plays the sweep.
namespace acidulous::dsp::room {

/** The sweep's lowest frequency, Hz. */
constexpr double kLowHz = 20.0;
/** The highest: 20 kHz, or just under Nyquist at a low rate. */
double topHz(int32_t rate);

/** A sweep and how it was played: [repeats] passes of the sweep and then [tailFrames] of silence. */
struct SweepSpec {
    int32_t rate = 48000;
    int32_t sweepFrames = 0;
    int32_t tailFrames = 0;
    int32_t repeats = 1;
    int32_t passFrames() const { return sweepFrames + tailFrames; }
    int64_t totalFrames() const { return static_cast<int64_t>(passFrames()) * repeats; }
};

/** The sweep, kLowHz to topHz over [frames] at peak [level], faded in over 50 ms and out over 10 ms. */
std::vector<float> makeSweep(int32_t frames, int32_t rate, float level);

/**
 * The inverse of makeSweep(frames, rate, 1): the sweep reversed, falling
 * 6 dB per octave as it falls in pitch, scaled so that the sweep convolved
 * with it is a unit pulse in the sweep's band.
 */
std::vector<float> inverseSweep(int32_t frames, int32_t rate);

/** What came back from a room. */
struct Impulse {
    /** From a few ms before the direct sound to where the recording ends. [right] is empty when mono. */
    std::vector<float> left, right;
    /** Where the tail meets the noise floor: the length to keep. */
    int32_t autoFrames = 0;
    /** Where the direct sound is in [left]. */
    int32_t directFrame = 0;
    /** From the sweep leaving to the direct sound arriving, frames. */
    int32_t latencyFrames = 0;
    /** The loudest 10 ms against the noise floor, dB. */
    float snrDb = 0.0f;
    int32_t rate = 48000;
    /** Empty, or why there is no impulse. */
    std::string error;
    bool stereo() const { return !right.empty(); }
    int32_t frames() const { return static_cast<int32_t>(left.size()); }
};

/**
 * Averages the passes of [recorded] (interleaved stereo,
 * spec.totalFrames() of it), deconvolves, finds the direct sound and the
 * noise floor. [inverse] replaces inverseSweep, for the harness to prove it
 * can tell a wrong one.
 */
Impulse deconvolve(const float *recorded, int64_t frames, const SweepSpec &spec,
                   const std::vector<float> *inverse = nullptr);

/**
 * [impulse] cut to [frames], faded out over its last 15% (20 to 200 ms),
 * peak at 0.9. [right] is left empty for a mono impulse.
 */
void finish(const Impulse &impulse, int32_t frames, std::vector<float> &left, std::vector<float> &right);

/** [columns] peaks of [impulse] over its whole length, each in dB, 0 at the loudest, -90 at the least. */
std::vector<float> picture(const Impulse &impulse, int32_t columns);

} // namespace acidulous::dsp::room
