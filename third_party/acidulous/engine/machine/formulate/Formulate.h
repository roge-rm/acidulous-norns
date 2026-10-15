#pragma once
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/machine/Machine.h>
#include <engine/machine/formulate/Program.h>

// Formulate is a chip oscillator (pulse, 16-step triangle, saw, shift
// register noise) combined with a bytebeat style formula.
//
//   - The formula can read the oscillator's sample as `x`, and its result
//     can replace it, ring it, gate it or xor it.
//   - Every voice has its own `t` that runs with the note's pitch, so a
//     formula plays like an instrument.
//   - Arpeggio, duty and volume tables step together in tracker notation
//     ("0 4 7 | 12"), at a rate in Hz or synced to the transport.
//   - The knobs `a`, `b` and `c` can be read by the formula and automated.
namespace acidulous::machine {

class Formulate final : public Machine {
  public:
    static constexpr int kVoices = 8;

    enum Wave : int32_t { Pulse, Triangle, Saw, Noise, Silence, WaveCount };
    enum Combine : int32_t { CombineOff, Replace, Ring, Gate, Xor, CombineCount };

    enum P : int32_t {
        WaveForm = 0, Duty, Bits, Crush, SubLevel, NoiseShort, PwmDepth, PwmRate,
        FormulaMix, FormulaMode, TimeKeyed, TimeScale, MacroA, MacroB, MacroC, Smooth,
        FrameRate, FrameSync, TableRetrigger,
        Cutoff, Resonance, FilterType,
        AmpAttack, AmpDecay, AmpSustain, AmpRelease,
        Mono, Glide, BendRange, Octave, Transpose, Fine, VelocityAmount,
        Drive, Volume, Pan,
        Count
    };

    Formulate();

    const char *typeName() const override { return "Formulate"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    void controlChange(uint8_t cc, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;
    void *swapObject(int32_t slot, void *object) override;

  private:
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 0;
        float velocity = 1.0f;
        float freq = 440.0f, glideFrom = 440.0f, glidePos = 1.0f;
        uint32_t phase = 0, subPhase = 0;
        uint32_t lfsr = 0x7fffu;
        float noisePhase = 0.0f;
        double timeAcc = 0.0;   // the formula's own clock for this voice
        int64_t t = 0;
        int64_t step = 0;       // which table step
        float frameAcc = 0.0f;
        float held = 0.0f;      // the crusher's held sample
        float crushAcc = 0.0f;
        float smoothed = 0.0f;
        dsp::Adsr amp;
        dsp::MultiFilter filter;
        int64_t age = 0;
        // Per-note bend (MPE) in semitones, added to the channel bend.
        float bend = 0.0f;
    };

    float paramOf(int32_t p) const { return params_.get(p); }
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }
    Voice *allocate();
    int32_t oscSample(Voice &v, int32_t wave, int32_t duty, float dt, float freq, int32_t subLevel);

    float sampleRate = 48000.0f;
    const formulate::Program *program = nullptr;
    Voice voices[kVoices];
    // The output DC blocker's state; see kDcPole in the cpp.
    float dcX1 = 0.0f, dcPrev = 0.0f;
    int64_t ageCounter = 0;
    float bpm = 120.0f;
    float bend = 0.0f, modWheel = 0.0f;
    static constexpr uint32_t kRngSeed = 0x1234567u;
    uint32_t rng = kRngSeed;
    float pwmPhase = 0.0f;
};

} // namespace acidulous::machine
