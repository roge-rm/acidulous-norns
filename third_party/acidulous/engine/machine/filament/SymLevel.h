#pragma once
#include <cmath>
#include <string>

// How a stored Filament sympathetic level moves now the bank is louder.
//
// The bank's output went from 0.2 to kSymOut (3). The level scales both what
// the bank is fed and what it gives back, so a level stored before is scaled
// by sqrt(0.2 / 3) to sound the same: `symlevel` (added at its old default
// when a file never set it) and the depth of each matrix slot aimed at the
// bank (m1_amt .. m8_amt while mN_dst is DstSympathetic).
//
// Which files hold the old level: songs and patches before version 5 and
// engine lines before `acidulous-engine 4`. The app does the same in
// model/FilamentMigration.kt.
namespace acidulous::filament_levels {

constexpr float kOutBefore = 0.2f;
constexpr float kOut = 3.0f; // Filament.cpp's kSymOut
/** `symlevel`'s default, normalised. */
constexpr float kLevelDefault = 0.35f;
/** Filament's matrix destinations and the bank's place among them. */
constexpr int kDests = 18;
constexpr int kDstSympathetic = 12;

inline float scale() { return std::sqrt(kOutBefore / kOut); }

/** A stored `symlevel`, normalised over 0..1, on today's bank. */
inline float level(float v) { return v * scale(); }

/** A stored matrix depth, normalised over -1..1, on today's bank. */
inline float depth(float v) { return ((v * 2.0f - 1.0f) * scale() + 1.0f) * 0.5f; }

/** Whether a stored mN_dst picks the bank. */
inline bool aimsAtBank(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<int>(std::floor(c * static_cast<float>(kDests - 1) + 0.5f)) == kDstSympathetic;
}

/** The slot (1..8) of `mN_amt` or `mN_dst` with [suffix] "amt" or "dst", or 0. */
inline int matrixSlot(const std::string &name, const char *suffix) {
    if (name.size() != 6 || name[0] != 'm' || name[1] < '1' || name[1] > '8' || name[2] != '_') return 0;
    return name.compare(3, 3, suffix) == 0 ? name[1] - '0' : 0;
}

} // namespace acidulous::filament_levels
