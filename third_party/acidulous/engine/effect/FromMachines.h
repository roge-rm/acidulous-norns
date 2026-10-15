#pragma once
#include <cstdint>
#include <vector>
#include <engine/effect/Effect.h>
#include <engine/machine/filament/Waveguide.h>
#include <engine/machine/manual/Rotary.h>

// Insert effects built from the machines' parts: Manual's rotating speaker
// cabinet, a grain cloud over the track like Pollen's live mode, and
// Filament's strings ringing in sympathy in the song's key.
namespace acidulous::effect {

#define ACIDULOUS_EFFECT_COMMON(Name)                                         \
    const char *typeName() const override { return #Name; }                  \
    const ParamDef *paramDefs(int32_t &count) const override;                 \
    void prepare(int32_t sampleRate) override;                                \
    void reset() override;                                                    \
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

/**
 * Manual's rotating speaker cabinet: a horn and a drum turning at two
 * speeds, heard by a pair of microphones. Switching speed ramps the rotors
 * up and down as the motors would.
 *
 * The extra is `tempo`: the horn turns once a note value instead of at its
 * own speed, so the swirl sits on the beat.
 */
class Rotary final : public Effect {
  public:
    enum P { Speed, Slow, Fast, Ramp, Distance, Angle, Width, Tempo, Mix, Gain, Count };
    Rotary() { initParams(); }
    ACIDULOUS_EFFECT_COMMON(Rotary)
    void onBlock(int64_t, int64_t, float bpm) override { this->bpm = bpm; }
  private:
    machine::Rotary cabinet;
    float bpm = 120.0f;
};

/**
 * A grain cloud over the last few seconds of the track: short slices of it
 * played back at other pitches, other speeds, backwards and scattered across
 * the stereo field, as Pollen's live mode does with the input.
 *
 * The extras are `freeze`, which stops listening so the cloud keeps playing
 * what it has, and `feedback`, which feeds the cloud back into what it
 * listens to, so it grows on itself.
 */
class Grain final : public Effect {
  public:
    enum P { Size, Density, Spray, Pitch, Scatter, Reverse, Freeze, Feedback, Width, Mix, Gain, Count };
    Grain() { initParams(5.0f); }
    ACIDULOUS_EFFECT_COMMON(Grain)
  private:
    static constexpr int kGrains = 32;
    static constexpr int kWindow = 1024;
    struct One {
        bool active = false;
        double pos = 0.0, inc = 1.0;
        int32_t age = 0, length = 1;
        float gainL = 0.7f, gainR = 0.7f;
    };
    float random01() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    }
    void spawn();
    std::vector<float> ringL, ringR;
    float window[kWindow + 1] = {};
    One grains[kGrains];
    int32_t size = 0, write = 0;
    float timer = 0.0f, sr = 48000.0f;
    uint32_t rng = 0x5bd1e995u;
};

/**
 * Strings tuned to a key and scale, ringing in sympathy with the track like
 * the open strings of a sitar or a piano with its dampers up. Each is one of
 * Filament's strings. Notes in the key make them ring; notes out of it
 * mostly don't.
 *
 * The extra is `metal`: stiffness in the strings, which pulls their
 * overtones sharp, from a string toward a bell.
 */
class Resonator final : public Effect {
  public:
    enum P { Key, Scale, Low, Strings, Decay, Tone, Metal, Width, Mix, Gain, Count };
    static constexpr int kMaxStrings = 16;
    Resonator() { initParams(3.5f); }
    ACIDULOUS_EFFECT_COMMON(Resonator)
  private:
    machine::Waveguide strings[kMaxStrings];
    float hzOf[kMaxStrings] = {};
    int32_t count = 0;
    float sr = 48000.0f;
};

#undef ACIDULOUS_EFFECT_COMMON

} // namespace acidulous::effect
