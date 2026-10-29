#pragma once
#include <cstdint>
#include <engine/dsp/Biquad.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * Mid/side: the sound split into what both sides share (mid) and how they
 * differ (side), each with its own level and tilt, then put back.
 *
 * `width` scales the side: 0 is mono, 2 twice as wide. `mono` takes the side
 * out below a frequency, so the bass sits in the middle; at the bottom of
 * its range it's off. Each tilt turns its half's highs up and lows down
 * around 650 Hz, or the other way.
 *
 * The extra is `breathe`: the side drops while the mid is loud and opens up
 * in the gaps, so a part goes narrow when it plays and wide as it rings.
 *
 * A mono track has no side, so only the mid's level and tilt act on it.
 */
class MidSide final : public Effect {
  public:
    enum P { Width, Mid, Side, MidTilt, SideTilt, Mono, Breathe, Gain, Count };
    MidSide() { initParams(); }
    const char *typeName() const override { return "Midside"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    void design();
    float sr = 48000.0f;
    dsp::Biquad midLo, midHi, sideLo, sideHi, sideCut;
    float midTiltWas = 1e9f, sideTiltWas = 1e9f, monoWas = -1.0f;
    float env = 0.0f;
};

} // namespace acidulous::effect
