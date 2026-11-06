#pragma once
#include <cmath>
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
 *
 * `follow` pulls the cutoff from its knob onto a note in key, so at high
 * resonance the filter sings that note and the sweep rises from it. The note
 * comes from the track's played notes or from `scale` and `key`, a note a
 * hit or a step, up `octave`s. Between notes it either jumps or glides over
 * `glide`: a note played while another is held glides, as Reflux's slide,
 * and `slides` sets how many of the steps or hits glide, each pattern's own
 * slide steps first. At follow 0 none of it is heard. Off a track there
 * are no notes, so `from` is always the scale there.
 */
class Acid final : public Effect {
  public:
    enum P {
        Cutoff, Resonance, Env, Decay, Accent, Pattern, Drive, Mode, Mix, Gain,
        Follow, From, Key, Scale, Octave, Glide, Slides, Count
    };
    /** Where the notes in key come from (`from`). */
    enum Source { FromScale, FromNotes };
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
    void onNote(int32_t note, float hz) override;
    void onNoteOff(int32_t note) override;
    void onAllNotesOff() override;

  private:
    void hit(bool accent);
    /** A step or a hit, number [index] in its line of sixteen: picks the next note in key. */
    void pickNote(int32_t row, int32_t index);
    /** Moves the key note to [hz], gliding if [slide]. */
    void aimAt(float hz, bool slide);
    bool slides(int32_t row, int32_t index) const;
    /** Off a track (a group, the master, the input) no notes arrive, so it's always the scale. */
    bool fromNotes() const { return hearsNotes_ && steppedTarget(From) == FromNotes; }
    int32_t steppedTarget(int32_t i) const { return static_cast<int32_t>(std::lround(params_.target(i))); }
    float sr = 48000.0f;
    dsp::Svf first[2], second[2];
    dsp::DecayEnv sweep, accentEnv;
    bool accented = false;
    // The track's own hits: a fast follower against a slow one.
    float fast = 0.0f, slow = 0.0f, loud = 0.0f;
    bool armed = true;
    int64_t blockStart = 0, blockEnd = 0, lastStep = -1;
    int32_t countdown = 0;
    // The key: the held notes in hertz, and the note the cutoff is on, in
    // octaves (log2 Hz), gliding toward its target by keyCoef every update.
    bool held[128]{};
    float heldHz[128]{};
    int32_t heldCount = 0;
    bool keySet = false;
    float keyAt = 0.0f, keyTo = 0.0f, keyCoef = 1.0f;
    int32_t hits = 0, notes = 0, cycle = 0;
};

} // namespace acidulous::effect
