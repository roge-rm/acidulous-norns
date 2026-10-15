#pragma once
#include "Math.h"

// Band-limited saw and pulse via PolyBLEP. Phase in [0, 1).
namespace acidulous::dsp {

class Osc {
  public:
    void setSampleRate(float sr) { sampleRate = sr; }
    void setFrequency(float hz) { inc = hz / sampleRate; }
    void reset(float ph = 0.0f) { phase = ph; }

    float saw() {
        float v = 2.0f * phase - 1.0f;
        v -= polyBlep(phase, inc);
        advance();
        return v;
    }

    float pulse(float width = 0.5f) {
        float v = phase < width ? 1.0f : -1.0f;
        v += polyBlep(phase, inc);
        float t = phase + 1.0f - width;
        if (t >= 1.0f) t -= 1.0f;
        v -= polyBlep(t, inc);
        advance();
        return v;
    }

  private:
    void advance() {
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    static float polyBlep(float t, float dt) {
        if (dt <= 0.0f) return 0.0f;
        if (t < dt) {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt) {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    float phase = 0.0f;
    float inc = 0.0f;
    float sampleRate = 48000.0f;
};

} // namespace acidulous::dsp
