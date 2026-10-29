#pragma once
#include <cstdint>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * Transient shaper: `attack` brings the start of each hit up or down and
 * `sustain` the ring after it, whatever the level, so there's no threshold.
 *
 * Attack is a fast follower against a slow one: they part at an onset and
 * meet again once the hit settles. Sustain is a follower that holds against
 * one that lets go quickly, which part in the tail. `time` scales the
 * followers together.
 *
 * The extra is `split`: only what's above it is shaped (and listened to), so
 * a drum loop's hats and snare can snap while its kick stays as it was. At
 * the bottom of its range the whole sound is shaped.
 */
class Transient final : public Effect {
  public:
    enum P { Attack, Sustain, Time, Split, Gain, Count };
    Transient() { initParams(); }
    const char *typeName() const override { return "Transient"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    float sr = 48000.0f;
    float low[2] = {}; // the one-pole under the split, a side
    float fastA = 0.0f, slowA = 0.0f, holdS = 0.0f, fallS = 0.0f;
    float timeWas = -1.0f;
    float atkFast = 0.0f, atkSlow = 0.0f, relA = 0.0f, relHold = 0.0f, relFall = 0.0f;
};

} // namespace acidulous::effect
