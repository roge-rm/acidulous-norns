#include "MidSide.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/** Where both tilts pivot. */
constexpr float kTiltHz = 650.0f;
/** At the bottom of `mono` the side is left alone. */
constexpr float kMonoOff = 21.0f;
/** The mid level `breathe` counts as loud: -12 dBFS. */
constexpr float kLoud = 0.25f;
constexpr float kBreatheAttack = 0.01f, kBreatheRelease = 0.3f;
} // namespace

const ParamDef *MidSide::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"width", 0.0f, 2.0f, 1.0f, Curve::Linear, 0, ""},
        {"mid", -24.0f, 6.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"side", -24.0f, 6.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"midtilt", -6.0f, 6.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"sidetilt", -6.0f, 6.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"mono", 20.0f, 500.0f, 20.0f, Curve::Exponential, 0, "Hz"},
        {"breathe", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void MidSide::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    midTiltWas = sideTiltWas = 1e9f;
    monoWas = -1.0f;
    reset();
}

void MidSide::reset() {
    midLo.reset(); midHi.reset(); sideLo.reset(); sideHi.reset(); sideCut.reset();
    env = 0.0f;
}

void MidSide::design() {
    const auto &p = params_;
    const float mt = p.get(MidTilt), st = p.get(SideTilt), mono = p.get(Mono);
    if (mt != midTiltWas) {
        midTiltWas = mt;
        midLo.lowShelf(kTiltHz, -mt, sr);
        midHi.highShelf(kTiltHz, mt, sr);
    }
    if (st != sideTiltWas) {
        sideTiltWas = st;
        sideLo.lowShelf(kTiltHz, -st, sr);
        sideHi.highShelf(kTiltHz, st, sr);
    }
    if (mono != monoWas) {
        monoWas = mono;
        sideCut.highpass(mono, 0.707f, sr);
    }
}

bool MidSide::process(float *L, float *R, int32_t frames, bool stereoIn) {
    design();
    const auto &p = params_;
    const float midGain = dsp::dbToGain(p.get(Mid));
    const float sideGain = dsp::dbToGain(p.get(Side)) * p.get(Width);
    const float breathe = p.get(Breathe);
    const bool cutting = p.get(Mono) > kMonoOff;
    // The side high-pass only runs while mono is on; it starts from silence each time.
    if (!cutting) sideCut.reset();
    const float atk = dsp::onePoleCoeff(kBreatheAttack, sr), rel = dsp::onePoleCoeff(kBreatheRelease, sr);
    for (int32_t i = 0; i < frames; ++i) {
        const float l = L[i], r = stereoIn ? R[i] : l;
        float m = (l + r) * 0.5f, s = (l - r) * 0.5f;
        m = midHi.process(midLo.process(m)) * midGain;
        const float a = std::fabs(m);
        env = dsp::guardDenormal(env + (a - env) * (a > env ? atk : rel));
        if (!stereoIn) {
            L[i] = m;
            continue;
        }
        if (cutting) s = sideCut.process(s);
        s = sideHi.process(sideLo.process(s));
        const float open = 1.0f - breathe * std::min(1.0f, env / kLoud);
        s *= sideGain * open;
        L[i] = m + s;
        R[i] = m - s;
    }
    return stereoIn;
}

} // namespace acidulous::effect
