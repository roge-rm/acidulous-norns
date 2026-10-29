#include "DeEsser.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/** How wide the band is. */
constexpr float kQ = 1.4f;
constexpr float kAttack = 0.0005f, kRelease = 0.06f;
/** How much of the level over the threshold comes off: 5 to 1. */
constexpr float kSlope = 0.8f;
/** In relative mode the threshold counts from here: the default -36 is a band 10 dB under the whole. */
constexpr float kRelativeOffset = 26.0f;
} // namespace

const ParamDef *DeEsser::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"freq", 2000.0f, 12000.0f, 6000.0f, Curve::Exponential, 0, "Hz"},
        {"threshold", -60.0f, 0.0f, -36.0f, Curve::Linear, 0, "dB"},
        {"range", 0.0f, 24.0f, 12.0f, Curve::Linear, 0, "dB"},
        {"mode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""}, // split, wide
        {"listen", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"relative", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void DeEsser::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    freqWas = -1.0f;
    reset();
}

void DeEsser::reset() {
    detect.reset();
    band[0].reset();
    band[1].reset();
    bandEnv = fullEnv = 0.0f;
}

bool DeEsser::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float freq = p.get(Freq);
    if (freq != freqWas) {
        freqWas = freq;
        detect.bandpass(freq, kQ, sr);
        band[0].copyCoefficients(detect);
        band[1].copyCoefficients(detect);
    }
    const float threshold = p.get(Threshold), range = p.get(Range);
    const bool wide = p.get(Mode) >= 0.5f, listen = p.get(Listen) >= 0.5f, relative = p.get(Relative) >= 0.5f;
    const float atk = dsp::onePoleCoeff(kAttack, sr), rel = dsp::onePoleCoeff(kRelease, sr);
    const int chans = stereoIn ? 2 : 1;
    for (int32_t i = 0; i < frames; ++i) {
        const float l = L[i], r = stereoIn ? R[i] : l;
        const float mono = (l + r) * 0.5f;
        const float b = std::fabs(detect.process(mono)), a = std::fabs(mono);
        bandEnv = dsp::guardDenormal(bandEnv + (b - bandEnv) * (b > bandEnv ? atk : rel));
        fullEnv = dsp::guardDenormal(fullEnv + (a - fullEnv) * (a > fullEnv ? atk : rel));
        float over = 20.0f * std::log10(bandEnv + 1.0e-9f);
        // Relative: the band against the whole sound, with the threshold
        // moved up by kRelativeOffset so -36 dB means the band within 10 dB.
        over -= relative ? 20.0f * std::log10(fullEnv + 1.0e-9f) + threshold + kRelativeOffset : threshold;
        const float cut = std::clamp(over * kSlope, 0.0f, range);
        const float g = cut > 0.0f ? std::pow(10.0f, -cut * 0.05f) : 1.0f;
        for (int c = 0; c < chans; ++c) {
            float *buf = c == 0 ? L : R;
            const float x = buf[i];
            const float bx = band[c].process(x);
            if (listen) buf[i] = bx;
            else if (wide) buf[i] = x * g;
            else buf[i] = x + (g - 1.0f) * bx;
        }
    }
    return stereoIn;
}

} // namespace acidulous::effect
