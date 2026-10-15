#pragma once
#include <cstdint>
#include <engine/core/Params.h>

// A MIDI processor between the clip player and the machine, such as Scale,
// Chord and Arp.
namespace acidulous {

struct MidiSink {
    virtual ~MidiSink() = default;
    virtual void send(uint8_t status, uint8_t d1, uint8_t d2) = 0;
};

// Bypass shares the effects' pseudo-parameter index (unit mod1/2/3, name "bypass").
constexpr int32_t kInputModBypassIndex = -2;

class InputMod {
  public:
    virtual ~InputMod() = default;
    virtual const char *typeName() const = 0;
    virtual const ParamDef *paramDefs(int32_t &count) const = 0;
    virtual void reset() = 0;
    virtual void handleMidi(uint8_t status, uint8_t d1, uint8_t d2, MidiSink &out) = 0;
    // Once per block, with the block's tick range and tempo, for anything clocked.
    virtual void onBlock(int64_t /*tickStart*/, int64_t /*tickEnd*/, float /*bpm*/, MidiSink & /*out*/) {}
    // Release everything this modifier is sounding, then forget it (transport stop, bypass).
    virtual void allNotesOff(MidiSink &out) = 0;
    ParamSet &params() { return params_; }

    // Once per block from the rack: params tick, then the clocked part runs.
    void run(int64_t tickStart, int64_t tickEnd, float bpm, MidiSink &out) {
        params_.tick();
        if (!bypass_) onBlock(tickStart, tickEnd, bpm, out);
    }
    void setBypass(bool on, MidiSink &out) {
        if (on && !bypass_) allNotesOff(out);
        bypass_ = on;
    }
    bool bypassed() const { return bypass_; }

  protected:
    void initParams() {
        int32_t n = 0;
        const ParamDef *defs = paramDefs(n);
        params_.init(defs, n);
    }
    ParamSet params_;
    bool bypass_ = false;
};

} // namespace acidulous
