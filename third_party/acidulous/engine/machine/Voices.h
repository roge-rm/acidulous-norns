#pragma once
#include <cstddef>
#include <cmath>
#include <cstdint>

// Shared voice helpers for per-note expression, used by the machines that
// keep an array of voices.
//
// voiceForNote only finds voices that are still held. A released note may
// still be ringing out, and expression for a new note on the same key
// shouldn't bend its tail.
namespace acidulous {

/** A voice's own bend as a frequency multiplier, for machines that scale. */
template <typename Voice>
inline float noteBendMul(const Voice &v) {
    return v.bend == 0.0f ? 1.0f : std::exp2(v.bend / 12.0f);
}

template <typename Voice, size_t N>
Voice *voiceForNote(Voice (&voices)[N], uint8_t note) {
    for (size_t i = 0; i < N; ++i) {
        if (voices[i].used && voices[i].gate && voices[i].note == note) return &voices[i];
    }
    return nullptr;
}


/**
 * The pressure a voice should use: its own if it sent any, otherwise the
 * channel's, glided once per block.
 *
 * Pressure comes in 128 steps and is applied once a block, so it's glided
 * (about 5 ms at a 64-frame block) to avoid clicks. No pressure gives exactly
 * zero, so patches played without it sound the same.
 */
inline float glidePressure(float &state, float own, float channel) {
    const float target = own >= 0.0f ? own : channel;
    state += (target - state) * 0.3f;
    if (state < 1e-5f && target == 0.0f) state = 0.0f;
    return state;
}

/**
 * Velocity to gain, the same on every machine.
 *
 * At [amount] 1 the gain is velocity squared, 40 log10(v/127) dB: 127 is
 * full, 64 is 12 dB down, 32 is 24 dB down and 1 is almost silent. A lower
 * [amount] narrows the range in proportion and 0 turns velocity off. Fix a
 * controller's response with the MIDI velocity curve, not here.
 */
inline float velocityGain(float v01, float amount) {
    if (amount <= 0.0f) return 1.0f;
    return std::pow(std::fmin(std::fmax(v01, 1.0f / 127.0f), 1.0f), 2.0f * amount);
}

} // namespace acidulous
