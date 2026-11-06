#pragma once
#include <cstdint>
#include <engine/dsp/ModeBank.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * The track through a body: a bank of up to 32 tuned modes taken from a
 * material, struck by the track and heard from a pickup.
 *
 *   - material: glass, wood, metal, skin, stone, bell, plate or tube. Each is
 *     a set of mode ratios, a level for each and how fast it dies: the high
 *     modes of wood and stone go quickly, metal and glass ring on.
 *   - size: the fundamental, as a note. `follow` tunes it to the track's
 *     last note instead, once one has come; otherwise `key` and `scale` snap
 *     it to the nearest note of the scale.
 *   - decay: the fundamental's ring time. damp shortens the higher modes.
 *   - strike: where the track lands on the body. A mode with a node there
 *     isn't excited. pickup: where it's heard, left and right a little apart.
 *   - drive: into the body through a soft clip, so loud hits bark.
 *
 * Modes past 0.45 of the sample rate are dropped. Retunes glide over
 * 5 ms. The wet is held to 12 dB over the track's own peak, so a held note
 * sitting on a mode can't build without limit, and its power to a little over
 * the track's, so a body tuned to the notes playing through it is no louder
 * than one that isn't.
 */
class Vessel final : public Effect {
  public:
    enum P { Material, Size, Decay, Damp, Strike, Pickup, Drive, Follow, Key, Scale, Mix, Gain, Count };
    enum Kind { Glass, Wood, Metal, Skin, Stone, Bell, Plate, Tube, KindCount };
    static constexpr int kModes = 32;

    Vessel() { initParams(kTrimDb); }
    const char *typeName() const override { return "Vessel"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    /** The track's last note, for `follow`. */
    void onNote(int32_t note, float hz) override {
        heardNote = note;
        heardHz = hz;
    }
    /** The modes are rebuilt once a call and glide to it over 5 ms, so a mod moves the body's knobs once a call. */
    bool glides(int32_t index) const override { return index == Mix || index == Gain || index == Drive; }

    /** The fundamental [process] tunes to now, in hertz: for tests. */
    float fundamentalHz() const { return builtHz; }

    /** Makes up for a full mix at the default 0.5 losing half of each. */
    static constexpr float kTrimDb = 2.0f;

  private:
    float targetHz() const;
    void build(float hz);

    dsp::ModeBank<kModes> bank;
    float sr = 48000.0f;
    int32_t heardNote = -1;
    float heardHz = 0.0f;
    // What the modes were last built from, so an unchanged block skips the build.
    float builtHz = -1.0f, builtDecay = -1.0f, builtDamp = -1.0f, builtStrike = -1.0f, builtPickup = -1.0f;
    int builtKind = -1;
    float longest = 1.0f; // the longest mode's t60, seconds
    // Last call's drive and mix, ramped from across this one.
    float driveWas = -1.0f, mixWas = -1.0f;
    // The wet guard: the input's peak (falling as the body does), the wet's,
    // and the gain that holds one under the other.
    float envIn = 0.0f, envWet = 0.0f, guard = 1.0f;
    // The same for power: the input's mean square, that held (falling as the
    // body does), and the wet's.
    float msIn = 0.0f, powIn = 0.0f, powWet = 0.0f;
    float wetL[256] = {}, wetR[256] = {}, drive[256] = {};
};

} // namespace acidulous::effect
