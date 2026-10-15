#pragma once
#include <cmath>
#include <cstdint>

// Runs a nonlinearity at twice the rate so harmonics above Nyquist don't fold
// back into the audible range.
//
// `Distortion` has its own cheaper oversampling (midpoint upsampling and one
// biquad), which lets a lot of aliasing through. That's fine for a distortion
// pedal but not for an amp with two nonlinear stages. This uses a halfband
// FIR, where every even tap but the centre is zero, so the polyphase form
// needs 8 multiplies per sample each way instead of 31.
//
// 2x is enough for tanh-style curves, whose harmonics fall off fast. Hard
// clippers and wavefolders would need 4x, so don't put them inside an amp's
// oversampled section.
namespace acidulous::dsp {

class Oversampler {
  public:
    /** Taps at the doubled rate. 15 either side of the centre. */
    static constexpr int32_t kTaps = 31;
    static constexpr int32_t kHalf = kTaps / 2;
    /**
     * Round-trip latency in samples at the base rate. It's a constant because
     * the dry path is delayed by exactly this (see `Amp`), and a latency that
     * changed with the quality setting would click.
     */
    static constexpr int32_t kLatency = (kTaps - 1) / 2;

    void prepare() {
        // A Blackman-windowed sinc at a quarter of the doubled rate, which
        // gives about -74 dB in the stopband.
        for (int32_t k = 0; k < kPairs; ++k) {
            const int32_t n = 2 * k + 1; // the odd taps are the only live ones
            const float ideal = std::sin(1.5707963f * static_cast<float>(n)) /
                                (3.14159265f * static_cast<float>(n));
            const float t = static_cast<float>(n + kHalf) / static_cast<float>(kTaps - 1);
            const float w = 0.42f - 0.5f * std::cos(6.2831853f * t) + 0.08f * std::cos(12.566371f * t);
            odd[k] = ideal * w;
        }
        reset();
    }

    void reset() {
        for (auto &v : hist) v = 0.0f;
        for (auto &v : down2) v = 0.0f;
        at = 0;
        atDown = 0;
    }

    /**
     * [n] frames in, 2n out.
     *
     * The even outputs are just the (delayed) input, since only the halfband's
     * centre tap reaches them. Only the odd outputs need filtering.
     */
    void up(const float *in, int32_t n, float *out) {
        for (int32_t i = 0; i < n; ++i) {
            hist[static_cast<size_t>(at)] = in[i];
            hist[static_cast<size_t>(at + kRing)] = in[i]; // a second copy, so no wrap in the loop
            at = at + 1 >= kRing ? 0 : at + 1;

            // The centre of the window, delayed so that both branches line up.
            out[i * 2] = tap(kPairs);
            float odds = 0.0f;
            for (int32_t k = 0; k < kPairs; ++k) {
                odds += odd[k] * (tap(kPairs + k) + tap(kPairs - 1 - k));
            }
            out[i * 2 + 1] = 2.0f * odds;
        }
    }

    /** 2n frames in, [n] out. The mirror of [up]. */
    void down(const float *in, int32_t n, float *out) {
        for (int32_t i = 0; i < n; ++i) {
            push(in[i * 2]);
            push(in[i * 2 + 1]);
            // Even taps first: only the centre, at a half.
            float sum = 0.5f * dtap(kHalf);
            for (int32_t k = 0; k < kPairs; ++k) {
                const int32_t j = 2 * k + 1;
                sum += odd[k] * (dtap(kHalf - j) + dtap(kHalf + j));
            }
            out[i] = sum;
        }
    }

  private:
    static constexpr int32_t kPairs = (kHalf + 1) / 2; // 8 live taps either side
    static constexpr int32_t kRing = 32;
    static constexpr int32_t kDownRing = 64;

    /** [back] samples before the write head, at the base rate. */
    float tap(int32_t back) const {
        return hist[static_cast<size_t>(at + kRing - 1 - back)];
    }
    void push(float v) {
        down2[static_cast<size_t>(atDown)] = v;
        down2[static_cast<size_t>(atDown + kDownRing)] = v;
        atDown = atDown + 1 >= kDownRing ? 0 : atDown + 1;
    }
    float dtap(int32_t back) const {
        return down2[static_cast<size_t>(atDown + kDownRing - 1 - back)];
    }

    float odd[kPairs] = {0.0f};
    float hist[kRing * 2] = {0.0f};
    float down2[kDownRing * 2] = {0.0f};
    int32_t at = 0, atDown = 0;
};

} // namespace acidulous::dsp
