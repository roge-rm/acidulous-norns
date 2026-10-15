#pragma once
#include <cstdint>
#include <engine/machine/Machine.h>
#include <engine/machine/nexus/Graph.h>
#include <memory>

// Nexus is the modular synth. Many of its modules come from the other
// machines, like Filament's string, Manual's tonewheels and rotary cabinet,
// and Trinity's wavetables.
//
//   - A module's type isn't a parameter. Changing it can mean allocating,
//     which the audio thread can't do, so types live in the patch text and
//     come in through the loader like Mosaic's zone map. Knobs, cable depths
//     and the morph are parameters, so they're smoothed and automatable.
//   - Per-voice state lives in the graph, not on the voice, because the
//     graph is replaced when the patch changes.
namespace acidulous::machine {

class Nexus final : public Machine {
  public:
    static constexpr int kVoices = nexus::kVoices;

    enum P : int32_t {
        SlotBase = 0,                                   // 16 slots x 8 knobs
        CableBase = SlotBase + nexus::kSlots * nexus::kKnobs,  // 24 cables x 2 depths
        MacroBase = CableBase + nexus::kCables * 2,     // 8 macros
        Morph = MacroBase + nexus::kMacros,
        VoiceMode, Glide, BendRange, Octave, Transpose, Fine, Volume, Pan, Drive,
        // How much velocity sets each voice's level, using the shared velocity curve.
        Velocity,
        Count
    };
    static_assert(Count <= kMaxParams, "Nexus declares more parameters than a unit can hold");

    Nexus();

    const char *typeName() const override { return "Nexus"; }
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
    void *swapObject(int32_t slot, void *object) override;

    /** UI polling: the scope trace. Audio thread writes it, this only copies. */
    int32_t readScope(float *dest, int32_t max) const;
    /** Slot levels then cable levels, for the editor to light the patch up. */
    int32_t readActivity(float *dest, int32_t max) const;

  private:
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 0;
        float pitch = 60.0f, velocity = 1.0f, random = 0.5f, trigger = 0.0f;
        int64_t age = 0;
        float quiet = 0.0f;   // seconds below the floor, for the watchdog
        // Per-note bend (MPE) in semitones, added to the channel bend.
        float bend = 0.0f;
    };

    float paramOf(int32_t p) const { return params_.get(p); }

    float sampleRate = 48000.0f;
    nexus::Graph *graph = nullptr;
    nexus::Context ctx;

    Voice voices[kVoices];
    float pitchOf[kVoices] = {}, gateOf[kVoices] = {}, velocityOf[kVoices] = {};
    float levelOf[kVoices] = {}; // velocityGain of each voice, read by the graph
    float randomOf[kVoices] = {}, triggerOf[kVoices] = {};
    // Per-note pressure and slide for the touch module, -1 until sent.
    float pressureOf[kVoices] = {}, timbreOf[kVoices] = {};
    int32_t active[kVoices] = {};
    int64_t counter = 0;

    float knobBuffer[nexus::kSlots * nexus::kKnobs] = {};
    float cableBuffer[nexus::kCables * 2] = {};
    float bendSemis = 0.0f;
    static constexpr uint32_t kRngSeed = 0x13579bdfu;
    uint32_t rng = kRngSeed;
    double tickCursor = 0.0, tickStep = 0.0;
};

} // namespace acidulous::machine
