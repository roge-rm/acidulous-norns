#pragma once
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::dsp {

/**
 * Splits a signal into low, mid and high with two gentle one-pole filters,
 * each band what's left after the one below is taken out. With no gain on
 * them they add back to exactly the input: no phase shift and no dip at the
 * crossovers, which a steeper split can't promise.
 *
 * Used by Swell and Smash, at 300 Hz and 5 kHz.
 */
class ThreeBands {
  public:
    void prepare(float sampleRate, float lowHz = 300.0f, float highHz = 5000.0f) {
        lowCoef = 1.0f - std::exp(-2.0f * 3.14159265f * lowHz / sampleRate);
        highCoef = 1.0f - std::exp(-2.0f * 3.14159265f * highHz / sampleRate);
        reset();
    }
    void reset() { low = mid = 0.0f; }

    /** One sample into [band]: low, mid, high. */
    void split(float x, float *band) {
        low += (x - low) * lowCoef;
        low = guardDenormal(low);
        const float rest = x - low;
        mid += (rest - mid) * highCoef;
        mid = guardDenormal(mid);
        band[0] = low;
        band[1] = mid;
        band[2] = rest - mid;
    }

  private:
    float lowCoef = 0.04f, highCoef = 0.48f;
    float low = 0.0f, mid = 0.0f;
};

} // namespace acidulous::dsp
