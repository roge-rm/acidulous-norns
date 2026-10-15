#pragma once
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/machine/Machine.h>
#include <engine/machine/timber/Pipe.h>

// Timber is the physically modelled woodwind. A single reed, double reed or
// air jet, in a cylinder or a cone. A clarinet is a reed on a cylinder (odd
// partials, overblows a twelfth), a saxophone is a reed on a cone (every
// partial, overblows an octave) and a flute has no reed.
//
// The note is a hole part way along the tube, and the rest of the instrument
// below it is modelled too. Its length follows from the note and the size of
// the instrument, so the sound changes across the range, the same pitch
// fingered two ways sounds different, and the tone hole cutoff is a
// control. See Pipe.h.
namespace acidulous::machine {

class Timber final : public Machine {
  public:
    static constexpr int kVoices = 8;

    enum Register : int32_t { Natural = 0, Octave, Twelfth, RegisterCount };

    enum P : int32_t {
        Family = 0, Bore, Body, Lattice, Holes, Fingering, Below, Answer, Reg,
        Reed, Embouchure, Pressure, Breath, Jet, Aim,
        Bell, Loss,
        Tongue, TongueTime, Flutter, FlutterRate, Keys,
        Attack, Decay, Sustain, Release,
        Vibrato, VibratoRate, VibratoDelay,
        Cutoff, Resonance, FilterType,
        Mono, Glide, BendRange, Octave_, Transpose, Fine, VelocityAmount,
        Drive, Volume, Pan,
        // Added later. Parameters are looked up by name, so older patches get
        // the default.
        MpeTimbre,
        Count
    };
    static_assert(Count <= kMaxParams, "Timber declares more parameters than a unit can hold");

    Timber();
    const char *typeName() const override { return "Timber"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void channelPressure(uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** Where the lattice is actually cutting, for the harness. */
    float latticeHz() const { return lastLattice; }

  private:
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 60;
        float velocity = 1.0f;
        float outGain = 1.0f; // velocity as level, ramped; see velocityGain
        float freq = 261.63f, glideFrom = 261.63f, glidePos = 1.0f;
        timber::Pipe pipe;
        dsp::Adsr amp;
        dsp::MultiFilter filter;
        float vibratoPhase = 0.0f, vibratoLeft = 0.0f;
        bool lift = false;         // applied after the next tune, which needs the note
        // A note on a voice that's still sounding fades it out over 2 ms
        // first, then starts. See noteOn.
        int32_t fadeLeft = 0;
        int pendingNote = -1;
        uint8_t pendingVel = 0;
        bool pendingOff = false;
        float breathScale = 0.0f;  // the breath level, not affected by the envelope
        float tongueLeft = 0.0f;   // the tongue is still on the reed
        float keyLeft = 0.0f;      // a pad is still closing
        float keyState = 0.0f;
        int64_t age = 0;
        // Per-note expression (MPE). `bend` is in semitones, added to the
        // channel bend. `pressure` and `timbre` are -1 until the note sends
        // them, so a normal keyboard falls back to the channel values.
        float bend = 0.0f, pressure = -1.0f, timbre = -1.0f;
    };

    float paramOf(int32_t p) const { return params_.get(p); }
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }
    Voice *allocate();
    void startVoice(Voice &v, uint8_t note, uint8_t velocity, bool slurred);

    float sampleRate = 48000.0f;
    Voice voices[kVoices];
    int64_t ageCounter = 0;
    float bend = 0.0f, modWheel = 0.0f, aftertouch = 0.0f;
    float flutterPhase = 0.0f;
    static constexpr uint32_t kRngSeed = 0x2f6e2b1u;
    uint32_t rng = kRngSeed;
    float lastLattice = 1500.0f;
};

} // namespace acidulous::machine
