#pragma once
#include <engine/dsp/Wavetable.h>
#include <cmath>
#include <string>

// How a stored wave or table choice moves when the wavetable bank grows.
//
// Parameters are stored normalised 0..1, so a stepped list that gets longer
// moves every saved choice unless it's rescaled. Trinity's oscillator waves
// (o1_wave, o2_wave, o3_wave) went from 12 steps to 4 + kTables, and Nexus's
// wtosc table knob (the slot's first knob, sNN_p1) from 8 tables to kTables.
//
// Which files hold the old counts: songs before version 3, patches before
// version 3, Nexus patch text before `v|2` and engine lines before
// `acidulous-engine 2`. The app does the same in model/TableMigration.kt.
namespace acidulous::tables {

constexpr int kTrinityWavesBefore = 12;
constexpr int kTrinityWaves = 4 + dsp::WavetableBank::kTables;
constexpr int kNexusTablesBefore = 8;
constexpr int kNexusTables = dsp::WavetableBank::kTables;

/** A Trinity wave stored over 12 steps, on the new count: same wave. */
inline float trinityWave(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    const int step = static_cast<int>(std::floor(c * static_cast<float>(kTrinityWavesBefore - 1) + 0.5f));
    return static_cast<float>(step) / static_cast<float>(kTrinityWaves - 1);
}

/** A Nexus wtosc table knob read over 8 tables, on the new count: the middle of the same table's range. */
inline float nexusTable(float v) {
    const float c = v < 0.0f ? 0.0f : (v > 0.9999f ? 0.9999f : v);
    const int step = static_cast<int>(c * static_cast<float>(kNexusTablesBefore));
    return (static_cast<float>(step) + 0.5f) / static_cast<float>(kNexusTables);
}

inline bool isTrinityWave(const std::string &name) {
    return name == "o1_wave" || name == "o2_wave" || name == "o3_wave";
}

/** The slot of a Nexus slot's first knob, `s03_p1` -> 3, or -1 for any other name. */
inline int nexusFirstKnobSlot(const std::string &name) {
    if (name.size() != 6 || name[0] != 's' || name[3] != '_' || name[4] != 'p' || name[5] != '1') return -1;
    if (name[1] < '0' || name[1] > '9' || name[2] < '0' || name[2] > '9') return -1;
    return (name[1] - '0') * 10 + (name[2] - '0');
}

} // namespace acidulous::tables
