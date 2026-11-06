#pragma once
#include <cstdint>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/DelayLine.h>
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
 *
 * The second extra is `throw`: what the de-esser takes off goes into its own
 * echo, `time` long on the beat, bouncing left and right with `feedback`.
 * Every s comes back as a bright trail while the voice stays clean. The loop
 * is high-passed and softly limited, so the repeats stay airy and can't run
 * away. At throw 0 with the echo rung out it plays exactly as without it.
 */
class DeEsser final : public Effect {
  public:
    enum P { Freq, Threshold, Range, Mode, Listen, Relative, Gain, Throw, Time, Feedback, Count };
    DeEsser() { initParams(); }
    const char *typeName() const override { return "Deesser"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    void onBlock(int64_t, int64_t, float b) override { bpm = b; }

  private:
    float sr = 48000.0f;
    dsp::Biquad detect, band[2];
    float freqWas = -1.0f;
    float bandEnv = 0.0f, fullEnv = 0.0f;

    // The throw's echo. Left takes the cut and the right's repeat, right the
    // left's, so the repeats bounce.
    dsp::ForgetfulDelayLine echo[2];
    float bpm = 120.0f;
    // The read in frames. A new note value or tempo crossfades from the old
    // read (fadeFrom) over fadeLeft more frames; a small drift glides.
    float readSamples = 0.0f, fadeFrom = 0.0f;
    int32_t fadeLeft = 0, fadeLen = 1;
    bool snapRead = true;     // jump to it on the first block after a reset
    // Per-frame smoothed send, feedback and echo level, so block-rate knobs don't step.
    float sendNow = 0.0f, fbNow = 0.0f, outNow = 0.0f;
    float hpIn[2] = {}, hpOut[2] = {}; // one-pole high-pass in the loop
    // Frames since the echo last held anything audible; past the line's
    // length it has rung out and stops running.
    int32_t quietFrames = 0;
    bool echoing = false;
    void wakeEcho();
};

} // namespace acidulous::effect
