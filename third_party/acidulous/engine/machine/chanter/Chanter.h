#pragma once
#include <cstdint>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>
#include <engine/machine/filament/Waveguide.h>
#include <engine/machine/timber/Pipe.h>

namespace acidulous::machine {

/**
 * Chanter: drone instruments, bagpipes and the hurdy-gurdy.
 *
 * A bagpipe is a chanter (a conical pipe with a double reed, Timber's) and
 * two or three drones (narrow cylinders with single reeds), all blown from a
 * bag that keeps the pressure up between notes, so the drones go on while the
 * bag lasts. There are no rests on a chanter and no tonguing, so notes are
 * set apart by grace notes: a quick, high note before each one.
 *
 * A hurdy-gurdy is a rosined wheel bowing its strings for as long as it
 * turns: melody strings stopped by the keys, drone strings tuned to the key,
 * and the trompette, whose bridge (the dog) is loose, so it hops and buzzes
 * once the wheel turns fast enough. A player sets it buzzing in rhythm by
 * pushing the wheel on the beat.
 */
class Chanter final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Key, Drones, Bag, Reed, Grace, Drift, Air,
        Wheel, Rosin, Dog, Threshold, Coup,
        VelocityAmount, BendRange, Octave, Volume,
        Count
    };
    static_assert(Count <= kMaxParams, "too many parameters");

    enum Kind : int32_t { Highland = 0, Smallpipes, Gaita, Gurdy,
                         Uilleann, Gaida, Cornemuse, Musette, Sackpipa, Dudy,
                         Zampogna, Tulum, Launeddas, KindCount };
    static constexpr int kDrones = 3;
    static constexpr int kHeld = 16;

    Chanter();

    const char *typeName() const override { return "Chanter"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** How full the bag is, or how fast the wheel's turning, 0 to 1, for tests. */
    float wind() const { return bag; }
    /** How hard the dog is buzzing now, for tests. */
    float dogLevel() const { return dogNow; }

  private:
    /** A bowed string on the wheel. */
    struct Bowed {
        Waveguide wave;
        float dc = 0.0f;
        float hz = 0.0f;
        int kickLeft = 0, kickLength = 1;
    };

    float paramOf(int32_t i) const { return params_.get(i); }
    /** The note the chanter or the melody strings play now: the last key still held. */
    int heldNote() const { return heldCount > 0 ? held[heldCount - 1] : -1; }
    void startNote(int note, float velocity, bool legato);
    void setupPipes();
    float bow(Bowed &s, float speed, float grip);
    float white() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    timber::Pipe chanter;
    /** The second chanter, for the other hand: the key held before the last. */
    timber::Pipe second;
    bool secondLifted = false;
    timber::Pipe drones[kDrones];
    Bowed melody[2];
    Bowed gurdyDrones[2];
    Bowed trompette;
    /** The keys held, oldest first. */
    uint8_t held[kHeld] = {};
    int heldCount = 0;
    float velocity = 0.8f;
    /** The melody note, as a MIDI pitch, and a grace note still to play first: samples left. */
    float pitch = 60.0f;
    float gracePitch = 60.0f;
    int32_t graceLeft = 0;
    /** A closed chanter shut between notes: samples left. */
    int32_t shutLeft = 0;
    bool sounding = false, lifted = false;
    /** Strings to nudge into motion on the next block. */
    bool kickMelody = false, kickDrones = false;
    /** The bag's fill or the wheel's speed, and how long since the last key let go, seconds. */
    float bag = 0.0f;
    float sinceRelease = 1e9f;
    float driftNow = 0.0f, driftAim = 0.0f;
    int32_t driftLeft = 0;
    float sampleRate = 48000.0f;
    float bpm = 120.0f;
    double beatAt = 0.0;
    float coupNow = 0.0f;
    float dogNow = 0.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f, notePress = -1.0f, noteBend_ = 0.0f;
    int retuneCountdown = 0;
    /** The trompette's dog, ringing where it strikes the soundboard. */
    dsp::Svf dogTone;
    float dogLast = 0.0f;
    /** The hurdy-gurdy's box. */
    dsp::Svf box;
    float dcIn = 0.0f, dcOut = 0.0f;
    uint32_t noise = 0x3b9aca07u;
    int32_t quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
