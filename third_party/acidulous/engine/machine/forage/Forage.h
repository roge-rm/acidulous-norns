#pragma once
#include <cstdint>
#include <engine/core/Sample.h>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>
#include <string>
#include <vector>

// Forage is the sample drum machine. Thirteen pads on C2..C3, each with its
// own sample, start, end, pitch, decay, level, pan, reverse and choke, plus a
// resonant filter, a crusher and a pitch envelope. Samples are decoded
// elsewhere and mounted as objects. No samples ship with the app.
namespace acidulous::machine {

class Forage final : public Machine {
  public:
    static constexpr int32_t kPads = 13;
    static constexpr uint8_t kBaseNote = 36;
    /**
     * The slot for the shared sample, after the thirteen pads.
     *
     * A pad with no sample of its own plays this one, so a single file can be
     * sliced across the pads with start and end without decoding it thirteen
     * times. Pads with their own sample keep it.
     */
    static constexpr int32_t kSharedSlot = kPads;
    // Per-pad parameter order. The table is generated pad-major with names
    // like "p03_cutoff", and the globals follow the last pad. New parameters
    // go on the end (like Play) so existing indices don't move.
    enum PadParam : int32_t { Start, End, Pitch, Decay, Level, Pan, Reverse, Choke, Cutoff, Reso, Mode, Crush, PitchEnv, PitchDecay, Play, PadParamCount };
    enum Global : int32_t { Accent, Volume, Velocity, GlobalCount };
    static int32_t index(int32_t pad, PadParam p) { return pad * PadParamCount + p; }
    static int32_t globalIndex(Global g) { return kPads * PadParamCount + g; }

    Forage();
    const char *typeName() const override { return "Forage"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    bool render(float *L, float *R, int32_t frames) override;
    void *swapObject(int32_t slot, void *object) override;

    // Read on the UI thread. Returns what the pad will play: its own sample,
    // or the shared one if it has none.
    const SampleData *sampleAt(int32_t pad) const {
        if (pad < 0 || pad >= kPads) return nullptr;
        return pads[pad].sample != nullptr ? pads[pad].sample : shared;
    }

  private:
    struct Pad {
        const SampleData *sample = nullptr;
        double pos = 0.0;      // in frames
        bool playing = false;
        float amp = 0.0f, ampCoeff = 0.0f;
        float penv = 0.0f, penvCoeff = 0.0f;
        float gain = 1.0f;
        dsp::Svf filter;
        float holdL = 0.0f, holdR = 0.0f; // crusher sample-and-hold
        float holdPhase = 0.0f;
        int32_t age = 0; // frames since the trigger, for the edge ramp
        bool held = false; // a while-held pad, waiting for its note off
    };

    void trigger(int32_t pad, float velocity01, bool accent);

    float sr = 48000.0f;
    Pad pads[kPads];
    const SampleData *shared = nullptr;
};

} // namespace acidulous::machine
