#pragma once
#include <cstdint>
#include <cmath>

namespace acidulous::dsp {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float mtof(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }
inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// Pade-style tanh: cheap, monotonic, good to ~1e-3 in the range that matters.
//
// The clamp is at 3 because that's where the rational part reaches exactly 1
// with zero slope, so the curve and the clamp meet smoothly. Past 3 it goes
// above 1.
inline float fastTanh(float x) {
    if (x < -3.0f) return -1.0f;
    if (x > 3.0f) return 1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/**
 * A ceiling for a feedback loop: nothing up to full scale is touched, and
 * above it the signal bends towards 2 and can't go further. A loop whose gain
 * can pass 1 then saturates, like a tape delay, instead of running away.
 */
inline float feedbackCeiling(float x) {
    const float a = std::fabs(x);
    if (a <= 1.0f) return x;
    return std::copysign(1.0f + fastTanh(a - 1.0f), x);
}

// Per-sample one-pole coefficient for a time constant in seconds.
inline float onePoleCoeff(float seconds, float sampleRate) {
    if (seconds <= 0.0f) return 1.0f;
    return 1.0f - std::exp(-1.0f / (seconds * sampleRate));
}

/**
 * Flushes a denormal to zero.
 *
 * Denormals are inaudible but very slow on most CPUs, and anything decaying
 * toward zero ends up in them (filter states, envelope followers, feedback).
 * Adding and subtracting a tiny number leaves a normal float unchanged and
 * turns a denormal into zero.
 */
inline float undenormal(float v) {
    static constexpr float kTiny = 1.0e-20f;
    return v + kTiny - kTiny;
}

/**
 * [undenormal] on WebAssembly, where the CPU's flush-to-zero can't be set
 * (see Denormals.h). The web build defines ACID_SOFT_DENORMALS. Everywhere
 * else this just returns the value.
 */
inline float guardDenormal(float v) {
#if defined(ACID_SOFT_DENORMALS)
    return undenormal(v);
#else
    return v;
#endif
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/**
 * Where to read in a circular buffer, for a write head at [writeHead] and a
 * delay of [samples]. Writes the interpolation fraction to [frac].
 *
 * Use this for every circular buffer read. Adding the buffer length to a
 * tiny negative float position can round to exactly the length (e.g. -0.01 +
 * 96000.0f == 96000), one past the end. The third line catches that.
 *
 * The check is written as a failed less-than so a NaN position also goes to 0.
 */
inline int32_t wrappedReadIndex(int32_t writeHead, float samples, int32_t size, float &frac) {
    float pos = static_cast<float>(writeHead) - samples;
    while (pos < 0.0f) pos += static_cast<float>(size);
    if (!(pos < static_cast<float>(size))) pos = 0.0f;
    const auto i = static_cast<int32_t>(pos);
    frac = pos - static_cast<float>(i);
    return i;
}

} // namespace acidulous::dsp
