#pragma once
#include <cstdint>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * Transient shaper: `attack` brings the start of each hit up or down and
 * `sustain` the ring after it, whatever the level, so there's no threshold.
 *
 * Attack is a fast follower against a slow one: they part at an onset and
 * meet again once the hit settles. Sustain is a follower that holds against
 * one that lets go quickly, which part in the tail. `time` scales the
 * followers together.
 *
 * The extra is `split`: only what's above it is shaped (and listened to), so
 * a drum loop's hats and snare can snap while its kick stays as it was. At
 * the bottom of its range the whole sound is shaped.
 *
 * The second extra is the accent grid: with `grid` on, each hit is shaped by
 * where it lands in the bar. A row of steps (`a01`..`a16`, `length` of them,
 * each `rate` long, counted from the song's start and swung with the track)
 * says down, flat or up, and an up step gives the hit's attack `depth` dB
 * more, a down step that much less. The step is chosen at the hit's onset
 * and held for the whole hit. With grid off it plays exactly as without it.
 */
class Transient final : public Effect {
  public:
    enum P {
        Attack, Sustain, Time, Split, Gain, Grid, Depth, Rate, Length,
        A01, A02, A03, A04, A05, A06, A07, A08, A09, A10, A11, A12, A13, A14, A15, A16, Count
    };
    Transient() { initParams(); }
    const char *typeName() const override { return "Transient"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float) override {
        blockStart = tickStart;
        blockEnd = tickEnd;
    }

  private:
    float sr = 48000.0f;
    float low[2] = {}; // the one-pole under the split, a side
    float fastA = 0.0f, slowA = 0.0f, holdS = 0.0f, fallS = 0.0f;
    float timeWas = -1.0f;
    float atkFast = 0.0f, atkSlow = 0.0f, relA = 0.0f, relHold = 0.0f, relFall = 0.0f;

    // The accent grid: the block's ticks from onBlock; whether the song
    // moves and the track's swing come in modTransport_.
    int64_t blockStart = 0, blockEnd = 0;
    // Hits are found two ways, so a roll of snares and a sub kick both count.
    // The high way: the energy above a few hundred Hz over 1 ms (hiE) jumping
    // over its peak in the last few ms (hiRecent, which hears it 2 ms late
    // through hiPast), not too soon after the last spike
    // unless it's well over its recent peak (hiPeak), so a saw's click every
    // cycle isn't a hit each time. The low way: the level's envelope
    // (env, slow to fall so a low wave's zero crossings don't count) jumping
    // over its recent low (envLow). After either, neither can start a hit for
    // a short wait (wait, in frames).
    float hpLow[2][4] = {}; // the high way's filter: four one-poles, each taken away, a side
    float hpCoeff = 0.0f;
    float hiE = 0.0f, hiECoeff = 0.0f, hiPeak = 0.0f, hiFall = 0.0f, hiRecent = 0.0f, recentFall = 0.0f;
    static constexpr int32_t kPastMost = 512;
    float hiPast[kPastMost] = {};
    int32_t pastAt = 0, pastLength = 1;
    // Spikes, sudden jumps of hiE: frames since the last started, the gap
    // before it, and the gap a hit needs (all clamped there).
    int32_t sinceSpike = 0, spikeGap = 0, spikeGapMost = 1;
    bool inSpike = false;
    float env = 0.0f, envLow = 0.0f, envRelease = 0.0f, envUp = 0.0f, hitPeak = 0.0f;
    int32_t hitFrames = 0, wait = 0, quick = 0;
    bool inHit = false;
    bool asleep = false; // the grid was off and the detector didn't run
    // The step is picked at the start and held for the hit: accent is its -1,
    // 0 or +1. accentNow is the accent's share of the gain, in octaves. It
    // moves no faster than accentStep a sample, so it can't click, or by up to
    // quickStep for quick frames after a hit starts, while the high way still
    // hears it rising.
    float accent = 0.0f, accentNow = 0.0f, accentStep = 0.0f, quickStep = 0.0f;
    float stepAccent(int64_t songTick) const;
};

} // namespace acidulous::effect
