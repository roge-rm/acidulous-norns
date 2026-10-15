#include "Formula.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

using dsp::clampf;
namespace formulate = machine::formulate;

const ParamDef *Formula::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"a", 0.0f, 255.0f, 0.0f, Curve::Linear, 0, ""},
        {"b", 0.0f, 255.0f, 0.0f, Curve::Linear, 0, ""},
        {"c", 0.0f, 255.0f, 0.0f, Curve::Linear, 0, ""},
        // How fast t counts, as the classic formulas expect it: 8 kHz.
        {"rate", 1000.0f, 48000.0f, 8000.0f, Curve::Exponential, 0, "Hz"},
        {"drive", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"smooth", 500.0f, 20000.0f, 20000.0f, Curve::Exponential, 0, "Hz"},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Formula::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    reset();
}

void Formula::reset() {
    clock = 0.0;
    t = 0;
    smoothed[0] = smoothed[1] = 0.0f;
    rng = 0x2545f491u;
}

void *Formula::swapObject(int32_t slot, void *object) {
    if (slot != 0) return object;
    const auto *old = expr;
    expr = static_cast<const formulate::Expr *>(object);
    return const_cast<formulate::Expr *>(old);
}

bool Formula::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const formulate::Expr *e = expr;
    // No formula: the track as it is.
    if (e == nullptr || e->empty()) return stereoIn;
    const auto &p = params_;
    formulate::Vars v;
    v.a = static_cast<int32_t>(p.get(A) + 0.5f);
    v.b = static_cast<int32_t>(p.get(B) + 0.5f);
    v.c = static_cast<int32_t>(p.get(C) + 0.5f);
    v.sr = static_cast<int32_t>(sr);
    const double step = p.get(Rate) / sr;
    const float drive = std::pow(10.0f, p.get(Drive) / 20.0f);
    const float smoothHz = p.get(Smooth);
    // At the top it's off: the steps go out as they are.
    const float smoothCoef = smoothHz >= 19999.0f ? 1.0f : 1.0f - std::exp(-6.2831853f * smoothHz / sr);
    const float mix = p.get(Mix);
    const int channels = stereoIn ? 2 : 1;

    for (int32_t i = 0; i < frames; ++i) {
        clock += step;
        while (clock >= 1.0) {
            clock -= 1.0;
            ++t;
        }
        rng = rng * 1664525u + 1013904223u;
        v.t = t;
        v.r = static_cast<int32_t>((rng >> 16) & 0xff);
        for (int c = 0; c < channels; ++c) {
            float *buf = c == 0 ? L : R;
            const float in = buf[i];
            v.x = std::min(255, static_cast<int32_t>(std::lround(clampf(in * drive, -1.0f, 1.0f) * 128.0f)) + 128);
            const int32_t value = e->eval(v) & 0xff;
            const float shaped = (static_cast<float>(value) - 128.0f) / 128.0f;
            smoothed[c] += (shaped - smoothed[c]) * smoothCoef;
            smoothed[c] = dsp::guardDenormal(smoothed[c]);
            buf[i] = in + (smoothed[c] - in) * mix;
        }
    }
    return stereoIn;
}

} // namespace acidulous::effect
