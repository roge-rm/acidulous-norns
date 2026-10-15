#pragma once
#include "Math.h"
#include <cstdint>

// A modulation LFO: nine waves, free or tempo-synced, with delay, start
// phase, slew and one-shot. Advanced once per block (750 Hz), which is plenty
// for modulation and keeps a full matrix cheap.
namespace acidulous::dsp {

class LfoGen {
  public:
    enum Wave : int32_t { Sine, Triangle, SawUp, SawDown, Square, SampleHold, SmoothRandom, Step8, Step16, WaveCount };
    static const char *waveName(int w) {
        static const char *n[WaveCount] = {"sine", "tri", "saw+", "saw-", "sqr", "s&h", "rand", "step8", "step16"};
        return n[w < 0 ? 0 : (w >= WaveCount ? WaveCount - 1 : w)];
    }

    void reset(float startPhase) { phase = startPhase; out = target = 0.0f; delayLeft = 0.0f; held = 0.0f; lastStep = -1; done = false; }
    void trigger(float startPhase, float delaySeconds) { reset(startPhase); delayLeft = delaySeconds; }

    // dt is the block length in seconds.
    float advance(int wave, float hz, float dt, float slew01, bool oneShot) {
        if (delayLeft > 0.0f) { delayLeft -= dt; return out; }
        const float prev = phase;
        phase += hz * dt;
        if (phase >= 1.0f) {
            phase -= std::floor(phase);
            if (oneShot) done = true;
        }
        if (done) { target = 0.0f; }
        else target = shape(wave, phase, prev);
        // Slew smooths every wave, so a square becomes a ramp and s&h glides.
        const float k = slew01 <= 0.0f ? 1.0f : clampf(dt / (slew01 * 0.4f + dt), 0.0f, 1.0f);
        out += (target - out) * k;
        return out;
    }
    float value() const { return out; }

  private:
    float shape(int wave, float p, float prev) {
        switch (wave) {
        case Triangle: return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
        case SawUp: return 2.0f * p - 1.0f;
        case SawDown: return 1.0f - 2.0f * p;
        case Square: return p < 0.5f ? 1.0f : -1.0f;
        case SampleHold: {
            const int step = static_cast<int>(p * 8.0f);
            if (step != lastStep || p < prev) { lastStep = step; held = rnd() * 2.0f - 1.0f; }
            return held;
        }
        case SmoothRandom: {
            const int step = static_cast<int>(p * 4.0f);
            if (step != lastStep || p < prev) { lastStep = step; from = held; held = rnd() * 2.0f - 1.0f; }
            const float f = p * 4.0f - static_cast<float>(step);
            return from + (held - from) * f * f * (3.0f - 2.0f * f);
        }
        case Step8: case Step16: {
            // Fixed interval sequences: an LFO that plays a shape, not a curve.
            static const float s8[8] = {0.0f, 0.583f, 0.25f, 1.0f, -0.25f, 0.583f, -0.583f, 0.25f};
            static const float s16[16] = {0.0f, 0.25f, 0.583f, 0.25f, 1.0f, 0.583f, 0.25f, 0.0f,
                                          -0.25f, -0.583f, -0.25f, 0.0f, 0.583f, 1.0f, 0.583f, 0.25f};
            if (wave == Step8) return s8[static_cast<int>(p * 8.0f) & 7];
            return s16[static_cast<int>(p * 16.0f) & 15];
        }
        default: return std::sin(p * kTwoPi);
        }
    }
    float rnd() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return static_cast<float>(seed & 0xffffff) / 16777216.0f; }

    float phase = 0.0f, out = 0.0f, target = 0.0f, held = 0.0f, from = 0.0f, delayLeft = 0.0f;
    int lastStep = -1;
    bool done = false;
    uint32_t seed = 0x51ed2701u;
};

} // namespace acidulous::dsp
