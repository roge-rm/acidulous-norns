#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <engine/dsp/Fft.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * The track taken apart into its frequencies, about forty times a second,
 * changed there and put back together.
 *
 * `freeze` holds the sound as it is, a chord or a vowel that goes on for as
 * long as it's held. `blur` lets each frequency fade rather than stop, so
 * notes run into each other. `smear` scrambles the timing within a frame,
 * for a washed, distant sound. `robot` throws the timing away altogether,
 * for a flat, buzzing voice. `peaks` keeps only the strongest frequencies,
 * a glassy, whistling sound, and `tilt` leans the whole thing darker or
 * brighter.
 *
 * `stutter` takes one spectrum per `grid` step on the tempo and holds it for
 * the step, and after `length` steps loops them in `order`: a loop that
 * keeps the colour of each step. `fade` fades each step inside its length,
 * from held to a short blip, and `renew` takes the loop again from the track
 * after that many laps. Letting go fades back into the track.
 *
 * It runs one frame, about 40 ms, behind the track; `mix` keeps the dry
 * signal in step.
 */
class Spectral final : public Effect {
  public:
    enum P { Freeze, Blur, Smear, Robot, Peaks, Tilt, Mix, Gain, Stutter, Grid, Length, Order, Fade, Renew, Count };
    /** The loop's order, as `order` has them. */
    enum Orders { Forward, Reverse, Bounce, Random };
    /** The most steps the stutter loop holds. */
    static constexpr int32_t kRing = 16;
    Spectral() { initParams(); }
    const char *typeName() const override { return "Spectral"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t, float tempo) override {
        tick = tickStart;
        bpm = tempo;
    }
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    /** Tilt rebuilds the bins' table, and only a frame reads it, so a mod moves it once a block. */
    bool glides(int32_t index) const override { return index != Tilt; }

    /** Samples the wet signal runs behind the dry. */
    int32_t latency() const { return size; }

  private:
    void frame(int c);
    /** At a hop with the stutter on: which step plays, whether it's taken now, and its level. [now] is the song's position for the frame, in ticks. */
    void stutterStep(double now);
    /** On a mono hop: the left channel's stutter copied to the right. */
    void copyStutterToRight();
    float random() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    float sr = 48000.0f;
    int32_t size = 2048, hop = 512, fill = 0;
    std::unique_ptr<dsp::Fft> fft;
    std::vector<float> window, re, im;
    /** For each channel: the input frame, the output being added up, and per bin the held level, the last phase, how fast it turns, and the phase played out. */
    std::vector<float> in[2], out[2], held[2], last[2], turn[2], played[2];
    std::vector<float> lean; // the tilt, as a gain for each bin
    float leanDb = 1000.0f;
    bool leanDirty = false;      // leanDb moved; the next frame rebuilds lean
    std::vector<float> octaves; // each bin's octaves from 1 kHz, for the tilt
    bool frozen = false, stereo = true;
    std::vector<float> dry[2];
    int32_t dryAt = 0;
    uint32_t rng = 0x9e3779b9u;

    // The stutter. The song's tick at the block's start and the tempo, from onBlock.
    int64_t tick = 0;
    float bpm = 120.0f;
    /** For each channel, kRing spectra of bins each: the level, how far each bin turns a hop, and its phase when taken. */
    std::vector<float> ringMag[2], ringTurn[2], ringPhase[2];
    /** For each channel, the stutter's level and phase per bin, played over the track's own. */
    std::vector<float> stutterMag[2], stutterPhase[2];
    std::vector<float> playMag, playPhase; // what a stuttering frame plays, per bin
    bool stuttering = false;
    int32_t have = 0;       // spectra taken since it started
    int64_t lastStep = 0;   // the grid step the last hop was in
    int64_t looped = 0;     // steps played from the loop, after the first time through
    int32_t slot = 0;       // the spectrum this hop plays
    bool take = false;      // this hop's spectrum goes into the ring at `slot`
    bool starts = false;    // this hop starts a step
    float stepLevel = 1.0f; // the fade, this far into the step
    int32_t letGo = 0;      // hops left of the fade back to the track
};

} // namespace acidulous::effect
