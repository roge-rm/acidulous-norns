#pragma once
#include "Math.h"
#include <cstdint>

// A tempo-aware LFO. Rates are note values in quarter notes; phase can be
// derived from the transport tick so the sweep stays locked to the bar.
namespace acidulous::dsp {

class Lfo {
  public:
    // The note values a rate knob steps through, shortest first, triplets
    // and dotted ones in their places: a wobble talks by moving between
    // eighths, eighth triplets and sixteenths. In quarter notes, each a whole
    // number of ticks at 240 PPQN. The app's names are NOTE_RATES in
    // SlotsPanel.kt, and a song saved with the old eight (1/16 to 8 bars) is
    // moved onto these as it loads (RateMigration.kt).
    static constexpr int kRates = 17;
    static constexpr float kBeats[kRates] = {
        0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.375f, 0.5f, 2.0f / 3.0f, 0.75f, // 1/32 .. 1/8.
        1.0f, 4.0f / 3.0f, 1.5f, 2.0f, 3.0f,                                   // 1/4 .. 1/2.
        4.0f, 8.0f, 16.0f, 32.0f,                                              // 1 .. 8 bars
    };
    static float beatsOf(int rateIndex) { return kBeats[rateIndex < 0 ? 0 : (rateIndex >= kRates ? kRates - 1 : rateIndex)]; }
    // Phase 0..1 for a rate [beats] quarter notes long, from a tick position (240 PPQN).
    static float phaseAt(int64_t tick, float beats) {
        const auto period = static_cast<int64_t>(beats * 240.0f + 0.5f);
        if (period <= 0) return 0.0f;
        return static_cast<float>(tick % period) / static_cast<float>(period);
    }
    // Phase advance per sample for a rate [beats] quarter notes long, at a tempo.
    static float phaseInc(float beats, float bpm, float sr) { return bpm / (beats * 60.0f * sr); }
    static float sine(float phase) { return std::sin(phase * kTwoPi); }
    static float triangle(float phase) { return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase; }
};

} // namespace acidulous::dsp
