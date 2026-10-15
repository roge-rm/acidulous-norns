#pragma once
#include <cstdint>
#include <vector>
#include <engine/dsp/Biquad.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * A worn tape machine: the speed wanders slowly (`wow`) and quickly
 * (`flutter`), the tape saturates as it's driven with a bump in the lows and
 * the highs rolling off (`tone`), and the hiss is there underneath.
 *
 * The extra is `stop`: switched on, the tape slows to a halt over `stoptime`,
 * pitch falling with it; switched off, it's back at speed at once.
 */
class Tape final : public Effect {
  public:
    enum P { Wow, Flutter, Drive, Tone, Hiss, Age, Stop, StopTime, Mix, Gain, Count };
    Tape() { initParams(); }
    const char *typeName() const override { return "Tape"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    float random01() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    }
    std::vector<float> ring[2];
    int32_t size = 0, write = 0;
    /** Where the head reads, in samples behind the write, which grows as the tape slows. */
    double behind = 0.0;
    float speed = 1.0f, sr = 48000.0f;
    float wowPhase = 0.0f, flutterPhase = 0.0f, drift = 0.0f, driftTarget = 0.0f;
    float lowpass[2] = {}, hissLp = 0.0f;
    dsp::Biquad bump[2];
    float bumpDb = -100.0f;
    uint32_t rng = 0x2f6b9a1du;
};

} // namespace acidulous::effect
