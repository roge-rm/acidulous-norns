#pragma once
#include <cstdint>
#include <string>
#include <engine/core/Constants.h>

// A module in a patch: eight knobs, some inputs, some outputs and a step()
// that runs once per sample. It owns no note or voice state. The graph owns
// that, because the graph is replaced when the patch changes.
//
// Everything is a float and any jack can connect to any other, like a real
// modular. Audio is nominally -1..1 and control 0..1 or -1..1. The colours in
// the editor are only a hint.
namespace acidulous::machine::nexus {

constexpr int kKnobs = 8;      // per module, fixed
constexpr int kPorts = 8;      // inputs and outputs, each
constexpr int kSlots = 16;     // modules in a patch
constexpr int kCables = 24;    // cables whose depth is a parameter
constexpr int kVoices = 8;

// The editor's activity meters read one sample in sixteen. The decay drops a
// level to about a third in a tenth of a second, so a cable stops glowing
// when the note does but audio-rate signals don't flicker.
constexpr int kMeterEvery = 16;
constexpr float kMeterDecay = 0.9964f;
constexpr int kMacros = 8;

/** Shared by the whole graph: the transport and the performance controls. */
struct Context {
    float sampleRate = 48000.0f;
    float bpm = 120.0f;
    double tick = 0.0;         // interpolated within the block, so clocks are sample-accurate
    double tickInc = 0.0;
    float modWheel = 0.0f;
    float pressure = 0.0f;
    float bend = 0.0f;
    float macro[kMacros] = {};
    // The voice this call is for, or -1 for a mono module. Per-voice values
    // live in the machine and are read through here, so modules never hold
    // pointers that a patch rebuild could invalidate.
    int32_t voice = -1;
    const float *pitchOf = nullptr;
    const float *gateOf = nullptr;
    const float *velocityOf = nullptr;
    const float *randomOf = nullptr;
    const float *triggerOf = nullptr;
    // Per-note pressure and slide, -1 if none has been sent.
    const float *pressureOf = nullptr;
    const float *timbreOf = nullptr;

    float voicePitch() const { return voice >= 0 && pitchOf != nullptr ? pitchOf[voice] : 60.0f; }
    float voiceGate() const { return voice >= 0 && gateOf != nullptr ? gateOf[voice] : 0.0f; }
    float voiceVelocity() const { return voice >= 0 && velocityOf != nullptr ? velocityOf[voice] : 1.0f; }
    float voiceRandom() const { return voice >= 0 && randomOf != nullptr ? randomOf[voice] : 0.5f; }
    float voiceTrigger() const { return voice >= 0 && triggerOf != nullptr ? triggerOf[voice] : 0.0f; }
    float voicePressure() const {
        const float own = voice >= 0 && pressureOf != nullptr ? pressureOf[voice] : -1.0f;
        return own >= 0.0f ? own : pressure;
    }
    float voiceTimbre() const {
        const float own = voice >= 0 && timbreOf != nullptr ? timbreOf[voice] : -1.0f;
        return own >= 0.0f ? own : 0.0f;
    }
};

/**
 * One module. Constructed on a worker thread (where it may allocate), then
 * only touched by the audio thread.
 */
class Module {
  public:
    virtual ~Module() = default;

    /** Worker thread. Allocate here or not at all. */
    virtual void prepare(float sampleRate, int32_t voices) = 0;
    /** Audio thread. Forget everything that was sounding. */
    virtual void reset() = 0;

    /** Once a block: the eight knob values, already smoothed. */
    virtual void setKnobs(const float *knobs) = 0;

    /**
     * Once a block: which of this module's inputs have a cable in them. Bit n
     * is input port n. Only the output sink uses it.
     *
     * Set per block rather than at build time because a graph hand-over
     * moves live instances into the new graph, with new wiring.
     */
    virtual void setConnected(uint32_t) {}

    /**
     * Worker thread, before prepare(): the module's text from the patch, for
     * a module that's programmed with one (the formula's expression). False
     * with a reason if it can't be used; the module then stays silent.
     */
    virtual bool setText(const std::string &, std::string &) { return true; }

    /**
     * One sample. `in` is kPorts wide, already summed and scaled by the
     * cables. Write up to kPorts outputs.
     */
    virtual void step(const float *in, float *out, const Context &ctx) = 0;

    /** Does this module need one instance per voice, or can it be shared? */
    virtual bool polyCapable() const { return true; }
    virtual bool monoCapable() const { return true; }
};

} // namespace acidulous::machine::nexus
