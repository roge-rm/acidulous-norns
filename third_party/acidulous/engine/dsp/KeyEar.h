#pragma once
#include <cmath>
#include <engine/dsp/Filter.h>
#include <engine/dsp/Math.h>

namespace acidulous::dsp {

/**
 * Listens to a sound through a narrow band on each key: low-passed twice,
 * taken down [down] times, and each band's energy held over a memory set by
 * the caller. Sympath hears a track's chords with it, Chanter its bass.
 * Audio thread; holds no buffers.
 */
template <int kMaxBands>
class KeyEar {
  public:
    void prepare(float sampleRate, int down, float lowHz) {
        down_ = down;
        lowCoef = 1.0f - std::exp(-6.2831853f * lowHz / sampleRate);
        for (Svf &b : bands) b.setSampleRate(sampleRate / static_cast<float>(down));
    }
    void reset() {
        for (Svf &b : bands) b.reset();
        for (float &h : heard) h = 0.0f;
        lp[0] = lp[1] = sum = 0.0f;
        phase = 0;
    }
    /** Puts band [b] on [hz], about two thirds of a semitone wide, and forgets what it heard. */
    void setBand(int b, float hz) {
        bands[b].set(hz, 1.0f);
        bands[b].reset();
        heard[b] = 0.0f;
    }
    void setCount(int n) { count = n < kMaxBands ? n : kMaxBands; }
    /**
     * Takes one sample. True when it finished a taken-down step and the bands
     * moved; [keep] is the one-pole coefficient they remember by, per step.
     */
    bool step(float x, float keep) {
        lp[0] += (x - lp[0]) * lowCoef;
        lp[1] += (lp[0] - lp[1]) * lowCoef;
        sum += lp[1];
        if (++phase < down_) return false;
        const float y0 = sum * (1.0f / static_cast<float>(down_));
        phase = 0;
        sum = 0.0f;
        for (int b = 0; b < count; ++b) {
            const float y = bands[b].step(y0).bp * bands[b].bandNorm();
            heard[b] = undenormal(heard[b] + (y * y - heard[b]) * keep);
        }
        return true;
    }
    /** Once a block, after the last step. */
    void settle() {
        lp[0] = undenormal(lp[0]);
        lp[1] = undenormal(lp[1]);
    }

    /** Each band's energy. */
    float heard[kMaxBands]{};
    int count = 0;

  private:
    Svf bands[kMaxBands];
    float lp[2] = {0.0f, 0.0f}, sum = 0.0f, lowCoef = 0.0f;
    int phase = 0, down_ = 1;
};

} // namespace acidulous::dsp
