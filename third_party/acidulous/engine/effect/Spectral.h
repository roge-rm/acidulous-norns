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
 * It runs one frame, about 40 ms, behind the track; `mix` keeps the dry
 * signal in step.
 */
class Spectral final : public Effect {
  public:
    enum P { Freeze, Blur, Smear, Robot, Peaks, Tilt, Mix, Gain, Count };
    Spectral() { initParams(); }
    const char *typeName() const override { return "Spectral"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

    /** Samples the wet signal runs behind the dry. */
    int32_t latency() const { return size; }

  private:
    void frame(int c);
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
    bool frozen = false, stereo = true;
    std::vector<float> dry[2];
    int32_t dryAt = 0;
    uint32_t rng = 0x9e3779b9u;
};

} // namespace acidulous::effect
