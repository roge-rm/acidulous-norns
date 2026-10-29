#include "Transient.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/** The followers at time 1, in seconds. */
constexpr float kFastAttack = 0.0003f, kSlowAttack = 0.015f, kRelease = 0.05f;
constexpr float kHoldRelease = 0.4f, kFallRelease = 0.04f;
/** Added to every follower, so silence and hiss read as no change (-80 dB). */
constexpr float kFloor = 1.0e-4f;
/**
 * How far the followers may part, in octaves of level (6 dB each), and how
 * much of that `attack` or `sustain` at 1 turns into gain: up to 12 dB.
 */
constexpr float kMostApart = 4.0f, kDepth = 0.5f;
/** The gain's range, in octaves: -24 to +12 dB. */
constexpr float kLeast = -4.0f, kMost = 2.0f;
/** At the bottom of `split` the whole sound is shaped. */
constexpr float kSplitOff = 21.0f;

inline float follow(float &env, float x, float attack, float release) {
    env = dsp::guardDenormal(env + (x - env) * (x > env ? attack : release));
    return env;
}
} // namespace

const ParamDef *Transient::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"attack", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"sustain", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"time", 0.25f, 4.0f, 1.0f, Curve::Exponential, 0, ""},
        {"split", 20.0f, 8000.0f, 20.0f, Curve::Exponential, 0, "Hz"},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Transient::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    timeWas = -1.0f;
    reset();
}

void Transient::reset() {
    low[0] = low[1] = 0.0f;
    fastA = slowA = holdS = fallS = 0.0f;
}

bool Transient::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float time = p.get(Time);
    if (time != timeWas) {
        timeWas = time;
        atkFast = dsp::onePoleCoeff(kFastAttack, sr);
        atkSlow = dsp::onePoleCoeff(kSlowAttack * time, sr);
        relA = dsp::onePoleCoeff(kRelease * time, sr);
        relHold = dsp::onePoleCoeff(kHoldRelease * time, sr);
        relFall = dsp::onePoleCoeff(kFallRelease * time, sr);
    }
    const float attack = p.get(Attack) * kDepth, sustain = p.get(Sustain) * kDepth;
    const float split = p.get(Split);
    const bool splitting = split > kSplitOff;
    const float lc = splitting ? dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * split), sr) : 0.0f;
    const int chans = stereoIn ? 2 : 1;
    // The low filter only runs while split is on; it starts from silence each time.
    if (!splitting) low[0] = low[1] = 0.0f;
    for (int32_t i = 0; i < frames; ++i) {
        // The part that's shaped: above the split, or all of it.
        float hi[2] = {L[i], stereoIn ? R[i] : 0.0f};
        if (splitting) {
            for (int c = 0; c < chans; ++c) {
                low[c] = dsp::guardDenormal(low[c] + (hi[c] - low[c]) * lc);
                hi[c] -= low[c];
            }
        }
        const float x = std::max(std::fabs(hi[0]), std::fabs(hi[1]));
        follow(fastA, x, atkFast, relA);
        follow(slowA, x, atkSlow, relA);
        follow(holdS, x, atkFast, relHold);
        follow(fallS, x, atkFast, relFall);
        const float apart = std::clamp(std::log2((fastA + kFloor) / (slowA + kFloor)), 0.0f, kMostApart);
        const float ring = std::clamp(std::log2((holdS + kFloor) / (fallS + kFloor)), 0.0f, kMostApart);
        const float g = std::exp2(std::clamp(attack * apart + sustain * ring, kLeast, kMost));
        if (splitting) {
            L[i] = low[0] + hi[0] * g;
            if (stereoIn) R[i] = low[1] + hi[1] * g;
        } else {
            L[i] *= g;
            if (stereoIn) R[i] *= g;
        }
    }
    return stereoIn;
}

} // namespace acidulous::effect
