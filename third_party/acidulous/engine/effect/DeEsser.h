#pragma once
#include <cstdint>
#include <engine/dsp/Biquad.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * De-esser: listens to a band around `freq` and turns it down when it goes
 * over `threshold`, by up to `range`. In `split` mode only that band is
 * turned down; `wide` turns the whole sound down instead, which is gentler
 * on a voice with a lot of air. `listen` plays the band it's listening to,
 * to find the frequency.
 *
 * The extra is `relative`: the band is measured against the whole sound
 * rather than against full scale, so the same setting catches an s on a
 * quiet line and a loud one. Its threshold is then how far under the rest
 * of the sound the band may come up to (-36 dB is 10 dB): an s is nearly all band, a vowel
 * has it 20 dB or more down.
 */
class DeEsser final : public Effect {
  public:
    enum P { Freq, Threshold, Range, Mode, Listen, Relative, Gain, Count };
    DeEsser() { initParams(); }
    const char *typeName() const override { return "Deesser"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    float sr = 48000.0f;
    dsp::Biquad detect, band[2];
    float freqWas = -1.0f;
    float bandEnv = 0.0f, fullEnv = 0.0f;
};

} // namespace acidulous::effect
