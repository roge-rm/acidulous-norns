#pragma once
#include <cstdint>
#include <engine/effect/Effect.h>
#include <engine/machine/diction/Throat.h>

namespace acidulous::effect {

/**
 * Diction's throat on any track: five formants and a nose, so whatever goes
 * through it is shaped into a vowel, from oo through oh, ah and eh to ee.
 *
 * `move` says what moves the vowel: an LFO on the tempo, the track's own
 * level (louder opens the mouth, like a wah that talks), or another track
 * through the sidechain. `depth` is how far it moves from `vowel`.
 */
class Mouth final : public Effect {
  public:
    enum P { Vowel, Size, Nasal, Move, Rate, Depth, Mix, Gain, Sidechain, Count };
    Mouth() { initParams(-5.5f); }
    const char *typeName() const override { return "Mouth"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t, float bpm) override { tick = tickStart; this->bpm = bpm; }
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    machine::diction::Throat throat[2];
    float sr = 48000.0f, bpm = 120.0f, follow = 0.0f, vowelNow = 0.5f;
    int64_t tick = 0;
    int32_t countdown = 0;
};

} // namespace acidulous::effect
