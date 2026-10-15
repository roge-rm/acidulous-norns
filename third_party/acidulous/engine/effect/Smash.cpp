#include "Smash.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/**
 * Each band's attack and release at time 1, in seconds. The lows move
 * slowest, since a fast detector on a bass note follows its cycles and
 * distorts it; the highs fastest, so a hi-hat is caught.
 */
constexpr float kAttack[3] = {0.040f, 0.020f, 0.010f};
constexpr float kRelease[3] = {0.28f, 0.28f, 0.13f};
/** Under this nothing is pulled up, faded in over kKnee, so hiss and hum stay down. */
constexpr float kFloor = -60.0f, kKnee = 12.0f;
/** The most a band is pulled up, in dB. */
constexpr float kMaxBoost = 30.0f;
/** At `up` 1, a band under the threshold comes this much of the way up. */
constexpr float kUpMost = 0.75f;

inline float dbToGain(float db) { return std::exp(db * 0.115129255f); }
} // namespace

const ParamDef *Smash::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"depth", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"time", 0.1f, 10.0f, 1.0f, Curve::Exponential, 0, ""},
        {"threshold", -48.0f, 0.0f, -24.0f, Curve::Linear, 0, "dB"},
        {"down", 0.0f, 1.0f, 0.9f, Curve::Linear, 0, ""},
        {"up", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        {"low", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"mid", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"high", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"in", -12.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Smash::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &s : split) s.prepare(sr);
    timeWas = -1.0f;
    reset();
}

void Smash::reset() {
    for (auto &s : split) s.reset();
    for (float &e : env) e = 0.0f;
    for (float &e : fast) e = 0.0f;
}

bool Smash::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float time = p.get(Time);
    if (time != timeWas) {
        timeWas = time;
        for (int b = 0; b < kBands; ++b) {
            attack[b] = dsp::onePoleCoeff(kAttack[b] * time, sr);
            release[b] = dsp::onePoleCoeff(kRelease[b] * time, sr);
        }
    }
    const float depth = p.get(Depth), thr = p.get(Threshold);
    const float down = p.get(Down), up = p.get(Up) * kUpMost;
    const float inGain = dbToGain(p.get(In));
    const float out[kBands] = {dbToGain(p.get(Low)), dbToGain(p.get(Mid)), dbToGain(p.get(High))};

    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        float bl[kBands], br[kBands];
        split[0].split(inL * inGain, bl);
        split[1].split(inR * inGain, br);
        float wetL = 0.0f, wetR = 0.0f;
        for (int b = 0; b < kBands; ++b) {
            // One detector for both sides, so the image holds still.
            const float a = std::max(std::fabs(bl[b]), std::fabs(br[b]));
            env[b] += (a - env[b]) * (a > env[b] ? attack[b] : release[b]);
            env[b] = dsp::guardDenormal(env[b]);
            // The lift listens to a detector that catches a peak at once, so
            // a loud sound arriving after a quiet one isn't lifted by the
            // boost the quiet one had while the slow detector catches up.
            fast[b] = std::max(a, fast[b] + (a - fast[b]) * release[b]);
            fast[b] = dsp::guardDenormal(fast[b]);
            float g = 0.0f;
            if (env[b] > 1e-6f) {
                const float level = 20.0f * std::log10(env[b]);
                const float now = 20.0f * std::log10(std::max(fast[b], 1e-6f));
                if (level > thr) {
                    g = -(level - thr) * down;
                } else if (now < thr) {
                    // Under the threshold it comes up, eased in above the floor.
                    float w = (now - (kFloor - kKnee)) / kKnee;
                    if (w > 0.0f) {
                        if (w < 1.0f) w = w * w * (3.0f - 2.0f * w);
                        else w = 1.0f;
                        g = std::min((thr - now) * up * w, kMaxBoost);
                    }
                }
            }
            const float lin = dbToGain(g) * out[b];
            wetL += bl[b] * lin;
            wetR += br[b] * lin;
        }
        // Driven hard with the band gains up, a slow attack lets the start
        // of a loud sound through at many times full scale. Over full scale
        // it's rounded off; under it nothing changes.
        wetL = dsp::feedbackCeiling(wetL);
        wetR = dsp::feedbackCeiling(wetR);
        // Depth is how much of it is mixed in with the track as it was.
        L[i] = inL + (wetL - inL) * depth;
        if (stereoIn) R[i] = inR + (wetR - inR) * depth;
    }
    return stereoIn;
}

} // namespace acidulous::effect
