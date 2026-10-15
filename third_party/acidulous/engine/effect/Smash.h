#pragma once
#include <cstdint>
#include <engine/dsp/Bands.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * Three-band compression both ways at once: in each of low, mid and high,
 * whatever is over the threshold is pushed down and whatever is under it is
 * pulled up, so every band ends up crowding the threshold. Mixed in with
 * `depth`, it's density and presence; at full depth, every tail, breath and
 * room comes up to the level of the hits, and the hits come down to meet
 * them.
 *
 * `down` and `up` set how hard each way works. `time` scales every band's
 * attack and release together: slow is smooth, fast pumps and grinds. The
 * band gains set the balance after it, and `in` drives it harder.
 *
 * The bands are split as Swell's are (dsp/Bands.h), so at depth 0, or with
 * both ways off, the track comes through untouched.
 */
class Smash final : public Effect {
  public:
    enum P { Depth, Time, Threshold, Down, Up, Low, Mid, High, In, Gain, Count };
    Smash() { initParams(); }
    const char *typeName() const override { return "Smash"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    static constexpr int kBands = 3;
    dsp::ThreeBands split[2];
    float env[kBands] = {}, fast[kBands] = {};
    float sr = 48000.0f;
    float timeWas = -1.0f;
    float attack[kBands] = {}, release[kBands] = {};
};

} // namespace acidulous::effect
