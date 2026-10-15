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
                       Velocity, Count };

    Reflux();

    const char *typeName() const override { return "Reflux"; }
    const ParamDef *paramDefs(int32_t &count) const override;

    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    static constexpr int kStack = 16;
    static constexpr uint8_t kAccentVelocity = 100;

    void startNote(uint8_t note, bool legato, bool accent, float level = 1.0f);

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
};

} // namespace acidulous::machine
