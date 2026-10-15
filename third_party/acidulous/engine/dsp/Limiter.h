#pragma once
#include "Math.h"
#include <cstdint>

// A block-lookahead peak limiter. Output lags input by one block, so when a
// block is output the next block's peak is already known and the gain ramps
// down before it arrives, keeping every sample under the ceiling. Release is
// exponential back to unity.
namespace acidulous::dsp {

template <int32_t kBlock>
class Limiter {
  public:
    void prepare(int32_t sampleRate) {
        releasePerBlock = 1.0f - std::exp(-static_cast<float>(kBlock) / (0.12f * static_cast<float>(sampleRate)));
        reset();
    }
    void reset() {
        for (int32_t i = 0; i < kBlock; ++i) heldL[i] = heldR[i] = 0.0f;
        heldGainTarget = 1.0f;
        gain = 1.0f;
        primed = false;
    }

    // drive: 0..1 -> up to +12 dB into the limiter.
    void set(float driveAmt, float ceilingLinear) {
        drive = 1.0f + clampf(driveAmt, 0.0f, 1.0f) * 3.0f;
        ceiling = ceilingLinear;
    }

    // In place, exactly kBlock frames.
    void process(float *L, float *R) {
        // Gain this incoming block would need.
        float peak = 0.0f;
        for (int32_t i = 0; i < kBlock; ++i) {
            L[i] *= drive;
            R[i] *= drive;
            const float a = std::fabs(L[i]), b = std::fabs(R[i]);
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        const float incomingTarget = peak > ceiling ? ceiling / peak : 1.0f;

        // Output the held block with a gain that already honours the incoming one.
        const float desired = heldGainTarget < incomingTarget ? heldGainTarget : incomingTarget;
        const float endGain = desired < gain ? desired : gain + (desired - gain) * releasePerBlock;
        const float step = (endGain - gain) / static_cast<float>(kBlock);
        float g = gain;
        for (int32_t i = 0; i < kBlock; ++i) {
            g += step;
            const float l = heldL[i] * g, r = heldR[i] * g;
            heldL[i] = L[i];
            heldR[i] = R[i];
            L[i] = primed ? l : 0.0f;
            R[i] = primed ? r : 0.0f;
        }
        gain = endGain;
        heldGainTarget = incomingTarget;
        primed = true;
    }

  private:
    float heldL[kBlock]{}, heldR[kBlock]{};
    float heldGainTarget = 1.0f;
    float gain = 1.0f;
    float drive = 1.0f, ceiling = 0.95f;
    float releasePerBlock = 0.05f;
    bool primed = false;
};

} // namespace acidulous::dsp
