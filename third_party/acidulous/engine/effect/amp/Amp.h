#pragma once
#include <cstdint>
#include "Cabinet.h"
#include "Stages.h"
#include <engine/core/Constants.h>
#include <engine/dsp/Oversampler.h>
#include <engine/effect/Effect.h>

// A guitar amp: a preamp that clips asymmetrically, a tone stack whose three
// controls interact, a power stage that sags under load, and a speaker cabinet.
//
// The order matters. The tone stack sits between the two nonlinear stages so
// the second one distorts an already shaped signal, and presence is inside
// the feedback loop so it acts as presence and not as treble.
//
// Everything from the bright cap to the output transformer runs oversampled
// 2x in one go. The cabinet runs at the base rate since it's linear and can't
// alias. Oversampling each stage separately would be slower and would throw
// away harmonics between the preamp stages.
namespace acidulous::effect {

class Amp final : public Effect {
  public:
    enum P {
        Drive, Bias, Bass, Mid, Treble, Stack, Presence, Master, Sag,
        Cab, Size, Cone, Mic, Edge, Room, Mix, Gain, Count
    };
    Amp() { initParams(-3.0f); }

    // Written out instead of using `ACIDULOUS_EFFECT_COMMON` to avoid
    // including Effects.h, which declares every other effect.
    const char *typeName() const override { return "Amp"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    /** Room for the latency plus a block, a power of two so the wrap is cheap. */
    static constexpr int32_t kDry = 128;

    struct Channel {
        dsp::Oversampler os;
        dsp::Biquad inShelf, bright;
        dsp::Svf inHp;
        amp::ToneStack tone;
        dsp::Biquad presence;
        amp::PowerStage power;
        amp::Cabinet cab;
        float interZ = 0.0f;
        float dry[kDry] = {0.0f};
        int32_t dryAt = 0;
    };

    /** Stage A: a diode curve with the bias knob applied. */
    static float stageA(float x, float g, float b) {
        const float off = dsp::fastTanh(b);
        const float at = dsp::fastTanh(dsp::kDriveNominal * g + b) - off;
        const float norm = at > 1e-6f ? dsp::kDriveNominal / at : 1.0f;
        return (dsp::fastTanh(x * g + b) - off) * norm;
    }
    /** Stage B: the valve curve, the same as MultiFilter's. */
    static float stageB(float x, float g) {
        return dsp::fastTanh(x * g) * (dsp::kDriveNominal / dsp::fastTanh(dsp::kDriveNominal * g));
    }

    Channel ch[2];
    float sr = 48000.0f;
    float g1 = 1.0f, g2 = 1.0f, biasOff = 0.0f, interCoeff = 0.1f;
    bool stageBOn = false, cabOn = true;
    float mixNow = 1.0f;
    float up[kBlockFrames * 2] = {0.0f};
};

} // namespace acidulous::effect
