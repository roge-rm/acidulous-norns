#pragma once
#include "Constants.h"
#include <cmath>
#include <cstdint>
#include <cstring>

// Parameters as the engine sees them.
//
// A unit (machine, effect) publishes a static table of ParamDef. The UI and
// automation address parameters by index into that table, and names are used
// in files and for display. Values travel as 0..1 and the curve maps them to
// the unit's range. Every parameter is smoothed on the audio thread so knob
// turns and automation don't zipper.
namespace acidulous {

enum class Curve : uint8_t { Linear, Exponential, Stepped };

struct ParamDef {
    const char *name;
    float min;
    float max;
    float def;       // in unit range
    Curve curve;
    int32_t steps;   // for Stepped: number of discrete values
    const char *unit; // "Hz", "ms", "dB", "" - display only

    float map(float v01) const {
        if (v01 < 0.0f) v01 = 0.0f;
        if (v01 > 1.0f) v01 = 1.0f;
        switch (curve) {
        case Curve::Exponential:
            return min * std::pow(max / min, v01);
        case Curve::Stepped:
            return min + std::floor(v01 * (steps - 1) + 0.5f) * ((max - min) / (steps - 1));
        default:
            return min + (max - min) * v01;
        }
    }

    float unmap(float value) const {
        switch (curve) {
        case Curve::Exponential:
            return std::log(value / min) / std::log(max / min);
        default:
            return (value - min) / (max - min);
        }
    }
};

// One-pole smoother toward a target, in unit range. ~5 ms at 48 kHz per block.
class Smoothed {
  public:
    void init(float value, float coeffPerBlock = 0.35f) {
        current = target = value;
        coeff = coeffPerBlock;
    }
    void set(float v) { target = v; }
    void jump(float v) { current = target = v; }
    // Call once per block; returns the value to use for this block.
    float next() {
        current += (target - current) * coeff;
        if (std::fabs(target - current) < 1e-6f) current = target;
        return current;
    }
    float value() const { return current; }
    float goal() const { return target; }

  private:
    float current = 0.0f;
    float target = 0.0f;
    float coeff = 0.35f;
};

// A unit's live parameter state: normalised targets plus smoothed unit-range values.
class ParamSet {
  public:
    void init(const ParamDef *defs, int32_t count) {
        this->defs = defs;
        this->count = count < kMaxParams ? count : kMaxParams;
        for (int32_t i = 0; i < this->count; ++i) {
            norm[i] = defs[i].unmap(defs[i].def);
            smooth[i].init(defs[i].def, defs[i].curve == Curve::Stepped ? 1.0f : 0.35f);
        }
    }

    // Audio thread.
    void set(int32_t index, float v01) {
        if (index < 0 || index >= count) return;
        norm[index] = v01;
        smooth[index].set(defs[index].map(v01));
    }
    // For step locks: jumps straight to the value. See Lane::linear.
    void jump(int32_t index, float v01) {
        if (index < 0 || index >= count) return;
        norm[index] = v01;
        smooth[index].jump(defs[index].map(v01));
    }
    void jumpAll() {
        for (int32_t i = 0; i < count; ++i) smooth[i].jump(defs[i].map(norm[i]));
    }
    // Once per block, before reading.
    void tick() {
        for (int32_t i = 0; i < count; ++i) smooth[i].next();
    }
    float get(int32_t index) const { return smooth[index].value(); }
    float normalized(int32_t index) const { return norm[index]; }

    /**
     * The value the parameter is heading to, in unit range, ignoring the
     * smoother. Use this to seed per-note state. `get()` depends on how
     * recently the knob moved, so an export could sound different from
     * playback.
     */
    float target(int32_t index) const { return defs[index].map(norm[index]); }

    int32_t indexOf(const char *name) const {
        for (int32_t i = 0; i < count; ++i) {
            if (std::strcmp(defs[i].name, name) == 0) return i;
        }
        return -1;
    }
    int32_t size() const { return count; }
    const ParamDef &def(int32_t i) const { return defs[i]; }

  private:
    const ParamDef *defs = nullptr;
    int32_t count = 0;
    float norm[kMaxParams]{};
    Smoothed smooth[kMaxParams];
};

} // namespace acidulous
