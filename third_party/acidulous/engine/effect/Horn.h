#pragma once
#include <cstdint>
#include <engine/dsp/PitchFollow.h>
#include <engine/effect/Effect.h>
#include <engine/machine/brazen/Bore.h>
#include <engine/machine/timber/Pipe.h>

namespace acidulous::effect {

/**
 * The track plays a wind instrument. Its pitch is followed and its level
 * becomes the breath, so a melody sung, played or synthesised comes out of
 * Brazen's horn or one of Timber's pipes: a clarinet's single reed, an
 * oboe's double reed or a flute's air jet.
 *
 * `octave` moves the instrument from the track, `tone` is the lips' tension
 * or the reed's stiffness, `bell` how bright the bell, `air` the breath
 * noise, `glide` how fast it follows a change of note and `breath` how hard
 * a given level blows it. With `snap` the notes land on semitones.
 */
class Horn final : public Effect {
  public:
    enum P { Kind, Octave, Tone, Bell, Air, Glide, Breath, Snap, Mix, Gain, Count };
    Horn() { initParams(); }
    const char *typeName() const override { return "Horn"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    float random() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
    machine::brazen::Bore bore;
    machine::timber::Pipe pipe;
    dsp::PitchFollow follow;
    float sr = 48000.0f;
    /** The note being played (MIDI), the one it's heading for, and the breath. */
    float note = 60.0f, target = 60.0f, push = 0.0f;
    bool heard = false, blowing = false;
    int kind = -1;
    int32_t countdown = 0, looking = 0;
    uint32_t rng = 0x6d2b79f5u;
};

} // namespace acidulous::effect
