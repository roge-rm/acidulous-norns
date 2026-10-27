#pragma once
#include <cstdint>
#include <engine/dsp/Envelope.h>
#include <engine/dsp/Filter.h>
#include <engine/dsp/Osc.h>
#include <engine/machine/Machine.h>

// Reflux is the acid bass: saw or pulse into a resonant lowpass with an
// envelope on the cutoff, decay, accent and slide.
//
// Monophonic with last-note priority. Slide is legato: a note-on while another
// is held glides the pitch and leaves the envelopes alone. Accent is velocity
// at or above kAccentVelocity and gives more envelope and more level.
namespace acidulous::machine {

class Reflux final : public Machine {
  public:
    enum P : int32_t { Wave, Tune, Cutoff, Resonance, EnvMod, Decay, Accent, Slide, Drive, Volume,
                       // the open layer
                       PulseWidth, Sub, Mode,
                       // added later: how much velocity sets the level
                       Velocity,
                       // Mix fades from the chosen wave toward the other one.
                       // Filter is the lowpass mode's type (lp24, lp12, hp, notch).
                       Mix, Filter,
                       // One LFO onto cutoff, pitch and pulse width.
                       LfoRate, LfoSync, LfoShape, LfoRetrig, LfoCutoff, LfoPitch, LfoPw,
                       // Eight macro knobs, each with four routes: a
                       // destination (a Dest) and a depth. A macro's knob
                       // times a route's depth is added to the destination.
                       MacroBase,
                       MacroParams = 9, // knob, 4 destinations, 4 depths
                       Count = MacroBase + 8 * MacroParams };
    // Macro destinations. Most move their knob in its own 0..1 range; pitch
    // is 24 semitones per unit and LFO rate 4 octaves per unit.
    enum Dest : int32_t { DstOff, DstCutoff, DstResonance, DstEnvMod, DstDecay, DstAccent, DstSlide, DstDrive,
                          DstAmp, DstPw, DstSub, DstMix, DstPitch, DstLfoRate, DstLfoCutoff, DstLfoPitch, DstLfoPw,
                          DestCount };
    enum Shape : int32_t { Sine, Triangle, Square, Saw, Random, ShapeCount };
    static constexpr int kMacros = 8;

    Reflux();

    const char *typeName() const override { return "Reflux"; }
    const ParamDef *paramDefs(int32_t &count) const override;

    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    static constexpr int kStack = 16;
    static constexpr uint8_t kAccentVelocity = 100;

    void startNote(uint8_t note, bool legato, bool accent, float level = 1.0f);
    /** The macros' sum onto [dest], from the knobs' smoothed values or, at note-on, their targets. */
    float macroSum(int dest, bool target) const;
    /** Param [p] moved by [off] in its own 0..1 range, from its smoothed value or its target. */
    float moved(int32_t p, float off, bool target) const;
    float lfoShape(int shape) const;

    float sampleRate = 48000.0f;
    dsp::Osc osc;
    dsp::Osc sub;
    dsp::Svf svf1, svf2;
    dsp::DecayEnv filterEnv;
    dsp::DecayEnv accentEnv;
    dsp::AsrEnv ampEnv;

    uint8_t stack[kStack]{};
    int32_t stackSize = 0;
    float targetPitch = 48.0f;
    float pitch = 48.0f;      // glides toward targetPitch
    float glideCoeff = 1.0f;  // per-sample
    bool gliding = false;
    bool accented = false;
    float velLevel = 1.0f; // velocity as level, set as a note starts; a slide keeps it
    int32_t coeffCountdown = 0;
    float bpm = 120.0f;
    float lfoPhase = 0.0f;  // 0..1
    float lfoHeld = 0.0f;   // the random shape's value for this cycle
    uint32_t lfoSeed = 0x2545f491u;
};

} // namespace acidulous::machine
