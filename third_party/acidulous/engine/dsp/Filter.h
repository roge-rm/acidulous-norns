#pragma once
#include "Math.h"

// Topology-preserving state-variable filter (Simper). Stays stable when the
// cutoff moves every sample, as it does on an acid line, without blowing up
// or zippering.
namespace acidulous::dsp {

class Svf {
  public:
    void setSampleRate(float sr) { sampleRate = sr; }

    // resonance 0..1 -> k from 2 (no resonance) down to a self-oscillating edge.
    void set(float cutoffHz, float resonance) {
        const float fc = clampf(cutoffHz, 20.0f, sampleRate * 0.45f);
        g = std::tan(kPi * fc / sampleRate);
        k = 2.0f - 1.96f * clampf(resonance, 0.0f, 1.0f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() { ic1eq = ic2eq = 0.0f; }

    /**
     * Multiply the band-pass output by this for a peak gain of one.
     *
     * `lp` and `hp` peak at unity but `bp` peaks at 1/k, so with no resonance
     * it's 6 dB quieter (12 dB with two in series). Use this so switching to
     * band-pass doesn't drop the level.
     */
    float bandNorm() const { return k; }

    // All three outputs of one step, for filters that mix them.
    struct Out { float lp, bp, hp; };
    Out step(float v0) {
        const float v3 = v0 - ic2eq;
        const float v1 = a1 * ic1eq + a2 * v3;
        const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = undenormal(2.0f * v1 - ic1eq);
        ic2eq = undenormal(2.0f * v2 - ic2eq);
        return {v2, v1, v0 - k * v1 - v2};
    }
    float resonanceK() const { return k; }

    // Band output of the same step; call instead of lowpass(), not as well.
    float bandpass(float v0) {
        const float v3 = v0 - ic2eq;
        const float v1 = a1 * ic1eq + a2 * v3;
        const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = undenormal(2.0f * v1 - ic1eq);
        ic2eq = undenormal(2.0f * v2 - ic2eq);
        return v1;
    }

    float highpass(float v0) {
        const float v3 = v0 - ic2eq;
        const float v1 = a1 * ic1eq + a2 * v3;
        const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = undenormal(2.0f * v1 - ic1eq);
        ic2eq = undenormal(2.0f * v2 - ic2eq);
        return v0 - k * v1 - v2;
    }

    float lowpass(float v0) {
        const float v3 = v0 - ic2eq;
        const float v1 = a1 * ic1eq + a2 * v3;
        const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = undenormal(2.0f * v1 - ic1eq);
        ic2eq = undenormal(2.0f * v2 - ic2eq);
        return v2;
    }

  private:
    float sampleRate = 48000.0f;
    float g = 0.1f, k = 2.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1eq = 0.0f, ic2eq = 0.0f;
};

} // namespace acidulous::dsp
