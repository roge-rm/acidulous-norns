#pragma once
#include <cmath>
#include <cstdint>

// How per-note expression is stored, used by everything that records it,
// saves it or plays it back.
//
// A note has three curves: bend, pressure and slide. All three are stored as
// 0..1, the same as an automation lane, so the editor can draw them with the
// same code.
//
// Bend is stored as scaled semitones, not the raw 14-bit value. Controllers
// set to different bend ranges send the same bytes for different pitches, so
// the raw value would play back wrong on another controller. Scaling to MPE's
// +/-48 semitone maximum still resolves to well under a cent.
namespace acidulous {

/** Full scale, either way. MPE's default bend range, and its widest. */
constexpr float kExprBendSemis = 48.0f;

/** Which of a note's three curves. The ordinals are saved in the song. */
enum class Expr : int32_t { Bend = 0, Pressure = 1, Timbre = 2, Count = 3 };

/** Signed semitones to 0..1, with the centre at a half. */
inline float exprBendTo01(float semitones) {
    const float v = 0.5f + semitones / (2.0f * kExprBendSemis);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/** And back. */
inline float exprBendFrom01(float v01) { return (v01 - 0.5f) * 2.0f * kExprBendSemis; }

/** A seven-bit controller value to 0..1, and back. */
inline float expr7To01(uint8_t v) { return static_cast<float>(v) / 127.0f; }
inline uint8_t expr7From01(float v01) {
    const float v = v01 * 127.0f + 0.5f;
    return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 127.0f ? 127.0f : v));
}

/** The value a curve says nothing at: centred bend, no pressure, no slide. */
inline float exprNeutral(Expr kind) { return kind == Expr::Bend ? 0.5f : 0.0f; }

} // namespace acidulous
