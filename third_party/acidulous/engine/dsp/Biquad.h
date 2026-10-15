#pragma once
#include "Math.h"

// Second-order sections from the standard cookbook formulae. Direct form I,
// stable under per-block coefficient updates for the ranges we use.
namespace acidulous::dsp {

class Biquad {
  public:
    void reset() { x1 = x2 = y1 = y2 = 0.0f; }
    float process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = guardDenormal(y);
        return y;
    }
    void lowShelf(float hz, float dB, float sr) { shelf(hz, dB, sr, true); }
    void highShelf(float hz, float dB, float sr) { shelf(hz, dB, sr, false); }
    void peak(float hz, float dB, float q, float sr) {
        const float A = std::pow(10.0f, dB / 40.0f), w = kTwoPi * clampf(hz, 20.0f, sr * 0.45f) / sr;
        const float alpha = std::sin(w) / (2.0f * (q < 0.1f ? 0.1f : q)), c = std::cos(w);
        const float a0 = 1.0f + alpha / A;
        b0 = (1.0f + alpha * A) / a0; b1 = -2.0f * c / a0; b2 = (1.0f - alpha * A) / a0;
        a1 = -2.0f * c / a0; a2 = (1.0f - alpha / A) / a0;
    }
    void lowpass(float hz, float q, float sr) {
        const float w = kTwoPi * clampf(hz, 20.0f, sr * 0.45f) / sr, alpha = std::sin(w) / (2.0f * q), c = std::cos(w);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f - c) / 2.0f / a0; b1 = (1.0f - c) / a0; b2 = b0; a1 = -2.0f * c / a0; a2 = (1.0f - alpha) / a0;
    }
    void highpass(float hz, float q, float sr) {
        const float w = kTwoPi * clampf(hz, 20.0f, sr * 0.45f) / sr, alpha = std::sin(w) / (2.0f * q), c = std::cos(w);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f + c) / 2.0f / a0; b1 = -(1.0f + c) / a0; b2 = b0; a1 = -2.0f * c / a0; a2 = (1.0f - alpha) / a0;
    }
    void allpass(float hz, float q, float sr) {
        const float w = kTwoPi * clampf(hz, 20.0f, sr * 0.45f) / sr, alpha = std::sin(w) / (2.0f * q), c = std::cos(w);
        const float a0 = 1.0f + alpha;
        b0 = (1.0f - alpha) / a0; b1 = -2.0f * c / a0; b2 = 1.0f; a1 = b1; a2 = b0;
    }

    /**
     * The complex response of this section at [w] radians per sample.
     *
     * Used by the amp's cabinet to trim its level, by the harness to check a
     * filter chain without rendering audio, and by Timber.
     */
    void at(float w, float &re, float &im) const {
        const float c1 = std::cos(w), s1 = std::sin(w);
        const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
        const float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const float dr = 1.0f + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        const float den = dr * dr + di * di + 1e-20f;
        re = (nr * dr + ni * di) / den;
        im = (ni * dr - nr * di) / den;
    }

    /** Just the magnitude, at [hz]. */
    float magnitudeAt(float hz, float sr) const {
        float re = 0.0f, im = 0.0f;
        at(kTwoPi * hz / sr, re, im);
        return std::sqrt(re * re + im * im);
    }

    /** How long this section rings, from its poles: the t60, in seconds. */
    float ringSeconds(float sr) const {
        const float r = std::sqrt(a2 < 0.0f ? -a2 : a2);
        if (r <= 0.0f || r >= 1.0f) return 0.0f;
        return -6.9078f / (sr * std::log(r));
    }

  private:
    void shelf(float hz, float dB, float sr, bool low) {
        const float A = std::pow(10.0f, dB / 40.0f), w = kTwoPi * clampf(hz, 20.0f, sr * 0.45f) / sr;
        const float c = std::cos(w), s = std::sin(w), beta = std::sqrt(A) * s * 1.4142f / 2.0f * 2.0f;
        const float ap1 = A + 1.0f, am1 = A - 1.0f;
        if (low) {
            const float a0 = ap1 + am1 * c + beta;
            b0 = A * (ap1 - am1 * c + beta) / a0; b1 = 2.0f * A * (am1 - ap1 * c) / a0; b2 = A * (ap1 - am1 * c - beta) / a0;
            a1 = -2.0f * (am1 + ap1 * c) / a0; a2 = (ap1 + am1 * c - beta) / a0;
        } else {
            const float a0 = ap1 - am1 * c + beta;
            b0 = A * (ap1 + am1 * c + beta) / a0; b1 = -2.0f * A * (am1 + ap1 * c) / a0; b2 = A * (ap1 + am1 * c - beta) / a0;
            a1 = 2.0f * (am1 - ap1 * c) / a0; a2 = (ap1 - am1 * c - beta) / a0;
        }
    }
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
};

} // namespace acidulous::dsp
