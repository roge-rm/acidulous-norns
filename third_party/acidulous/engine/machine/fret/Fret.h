#pragma once
#include <cstdint>
#include <vector>
#include <engine/dsp/Filter.h>
#include <engine/effect/amp/Cabinet.h>
#include <engine/machine/Machine.h>
#include <engine/machine/filament/Waveguide.h>

namespace acidulous::machine {

/**
 * Fret: electric guitars and basses.
 *
 * Each note is a string (Filament's waveguide), plucked by a pick, a finger
 * or a slapping thumb at a point along it, and heard not as it sounds in the
 * air but as the pickups hear it: a magnet at a place under the string, which
 * hears some harmonics and is deaf to others, through the coil's own
 * resonance and the tone knob.
 *
 * On top of that a player's hands: a palm resting on the strings by the
 * bridge, a finger touching a node for a harmonic, slides from note to note,
 * the frets buzzing when a string is hit hard, and chords strummed across the
 * strings rather than struck at once. And an amp: clean, or driven, with a
 * speaker, and loud enough that what comes out of it shakes the strings back,
 * so a held note sustains or jumps up to a harmonic.
 */
class Fret final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Pickup, Coil, Tone, Stroke, Hardness, Position,
        Mute, Sustain, Bright, Buzz, Harmonic,
        Strum, Direction, Slide, Vibrato,
        Drive, Feedback,
        Voices, VelocityAmount, BendRange, Octave, Volume,
        Count
    };
    static_assert(Count <= kMaxParams, "too many parameters");

    enum Kind : int32_t { Guitar = 0, Twelve, Baritone, Bass, Bass5, Hollow, Steel, Fretless, KindCount };
    enum Stroke_ : int32_t { Pick = 0, Finger, Slap, StrokeCount };
    static constexpr int kVoices = 6;

    Fret();

    const char *typeName() const override { return "Fret"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** Strings sounding, for tests. */
    int activeVoices() const;

  private:
    struct Voice {
        Waveguide string, partner;
        bool used = false, held = false, hasPartner = false;
        uint8_t note = 0;
        float velocity = 0.8f;
        /** The note it's playing now and the one it's sliding to, as MIDI notes before bend. */
        float pitch = 60.0f, aim = 60.0f;
        /** The partner string's distance from the main one, semitones: an octave or a unison a hair apart. */
        float partnerAt = 0.0f;
        float gain = 0.0f;
        /** The stroke still going into the string, and where it is. */
        std::vector<float> stroke;
        int32_t strokeLength = 0, strokeAt = 0;
        /** Which harmonic it sounds: 0 the note, 1 to 3 the 12th, 7th and 5th fret. */
        int harmonic = 0;
        /** Samples a slap's buzz still has. */
        int32_t slapLeft = 0;
        /** The note's own bend and pressure (MPE); pressure is -1 until it sends one. */
        float noteBend = 0.0f, pressure = -1.0f;
        /** How long the note has been let go of, samples, for the fingers' damping. */
        int32_t released = 0;
        float level = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };
    /** A note waiting its turn in a strum. */
    struct Waiting {
        uint8_t note = 0, velocity = 0;
        int32_t delay = 0;
        bool placed = false;
    };

    float paramOf(int32_t i) const { return params_.get(i); }
    Voice *voiceFor(uint8_t note);
    void start(uint8_t note, uint8_t velocity);
    void shapeStroke(Voice &v);
    /** Tunes and damps a voice's strings for its pitch now. */
    void retune(Voice &v, float vibrato);
    float hz(const Voice &v, float vibrato) const;
    float white() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    Voice voices[kVoices];
    static constexpr int kWaiting = 16;
    Waiting waiting[kWaiting];
    int waitingCount = 0;
    bool strumUp = false;
    float sampleRate = 48000.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    float vibratoPhase = 0.0f;
    int retuneCountdown = 0;
    /** The pickups' coils: a resonant low-pass each side. */
    dsp::Svf coil;
    float coilBuiltFor = -1.0f;
    /** The amp's speaker, and what came out of it a moment ago, to feed back to the strings. */
    effect::amp::Cabinet cabinet;
    std::vector<float> air;
    int32_t airAt = 0;
    float dcIn = 0.0f, dcOut = 0.0f;
    uint32_t noise = 0x7f4a7c15u;
    uint32_t clock = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
