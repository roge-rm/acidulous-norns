#pragma once
#include <cstdint>
#include <engine/dsp/Envelope.h>
#include <engine/dsp/Filter.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * Reflux's filter on any track: two resonant stages with a little
 * saturation between them and drive after, swept by an envelope in octaves.
 *
 * What starts the envelope is `pattern`. At 0 it's the track itself: each
 * hit sets it off, and a loud one is an accent. Above 0 it's one of eight
 * sixteen-step patterns on the tempo, with accents, so a pad or a drum loop
 * squelches like a bass line. `accent` is how much further an accented
 * sweep goes and how much louder it is.
 */
class Acid final : public Effect {
  public:
    enum P { Cutoff, Resonance, Env, Decay, Accent, Pattern, Drive, Mode, Mix, Gain, Count };
    static constexpr int kPatterns = 8;
    Acid() { initParams(-3.5f); }
    const char *typeName() const override { return "Acid"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override {
        blockStart = tickStart;
        blockEnd = tickEnd;
        (void)bpm;
    }
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    void hit(bool accent);
    float sr = 48000.0f;
    dsp::Svf first[2], second[2];
    dsp::DecayEnv sweep, accentEnv;
    bool accented = false;
    // The track's own hits: a fast follower against a slow one.
    float fast = 0.0f, slow = 0.0f, loud = 0.0f;
    bool armed = true;
    int64_t blockStart = 0, blockEnd = 0, lastStep = -1;
    int32_t countdown = 0;
};

} // namespace acidulous::effect
