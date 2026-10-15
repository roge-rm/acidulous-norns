#pragma once
#include <cstdint>
#include <engine/effect/Effect.h>
#include <engine/machine/formulate/Expr.h>

namespace acidulous::effect {

/**
 * A formula, written the way Formulate's are, shapes the track a sample at a
 * time. The track comes in as `x`, 0 to 255 with silence at 128, and what
 * the formula gives, taken as 0 to 255, goes out. So `x` alone is the track
 * cut to 8 bits, `x^t>>4` mangles it in time with a counter, `x&a` throws
 * bits away as `a` is turned, and so on. With no formula the track passes
 * untouched.
 *
 * `t` counts at `rate`, and `a`, `b` and `c` are three knobs, 0 to 255.
 * `drive` pushes the track harder into the formula and `smooth` rounds off
 * the steps it leaves.
 *
 * The formula is compiled on a worker and handed over (swapObject), as
 * Formulate's is.
 */
class Formula final : public Effect {
  public:
    enum P { A, B, C, Rate, Drive, Smooth, Mix, Gain, Count };
    Formula() { initParams(); }
    ~Formula() override { delete expr; }
    const char *typeName() const override { return "Formula"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    /** Slot 0 is the formula, a formulate::Expr. Returns the one it replaces. */
    void *swapObject(int32_t slot, void *object) override;

  private:
    const machine::formulate::Expr *expr = nullptr;
    float sr = 48000.0f;
    double clock = 0.0;
    int32_t t = 0;
    float smoothed[2] = {};
    uint32_t rng = 0x2545f491u;
};

} // namespace acidulous::effect
