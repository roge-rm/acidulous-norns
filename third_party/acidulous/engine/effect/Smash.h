#pragma once
#include <cstdint>
#include <engine/dsp/Bands.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * Three-band compression both ways at once: in each of low, mid and high,
 * whatever is over the threshold is pushed down and whatever is under it is
 * pulled up, so every band ends up crowding the threshold. Mixed in with
 * `depth`, it's density and presence; at full depth, every tail, breath and
 * room comes up to the level of the hits, and the hits come down to meet
 * them.
 *
 * `down` and `up` set how hard each way works. `time` scales every band's
 * attack and release together: slow is smooth, fast pumps and grinds. The
 * band gains set the balance after it, and `in` drives it harder.
 *
 * The bands are split as Swell's are (dsp/Bands.h), so at depth 0, or with
 * both ways off, the track comes through untouched.
 *
 * The extra is the beat: each band can duck on a rhythm of its own, played
 * from the song's position and swung with the track (`lowduck`, `midduck`,
 * `highduck`: beats, offbeats, 8ths, 16ths, 2 and 4, halves, 3-3-2 or
 * dotted), so the lows can duck on the kick and the highs on the offbeats.
 * Or a band ducks under another track, at `sidechain`: that track's level
 * over the threshold pushes the band down by as much. `lowdepth` and the
 * others are how far each band goes down, `recover` how long it takes to
 * come back. The duck works on the whole band, the track as it was too, so
 * it's as deep at any `depth`. With every band's duck off it plays exactly
 * as without it.
 */
class Smash final : public Effect {
  public:
    enum P {
        Depth, Time, Threshold, Down, Up, Low, Mid, High, In, Gain,
        Sidechain, LowDuck, MidDuck, HighDuck, LowDepth, MidDepth, HighDepth, Recover, Count
    };
    Smash() { initParams(); }
    const char *typeName() const override { return "Smash"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    void onBlock(int64_t tickStart, int64_t, float bpm) override {
        blockStart = tickStart;
        bpmNow = bpm;
        newBlock = true;
    }

  private:
    static constexpr int kBands = 3;
    dsp::ThreeBands split[2];
    float env[kBands] = {}, fast[kBands] = {};
    float sr = 48000.0f;
    float timeWas = -1.0f;
    float attack[kBands] = {}, release[kBands] = {};

    // The beat. The song's tick at the start of the block, kept to a fraction
    // of a tick: the engine's block start is whole ticks, and a block is often
    // less than one. `advanced` is the frames run since it was set.
    int64_t blockStart = 0;
    float bpmNow = 120.0f;
    bool newBlock = false, synced = false;
    double clock = 0.0;
    int64_t advanced = 0;
    // Each band's duck as it is now, in dB, which moves no faster than
    // kDuckSlew a sample; and the sidechain's level.
    float duckNow[kBands] = {};
    float keyEnv = 0.0f;
    // Each band's last hit and next, in song ticks, found again when the
    // song passes the next one, jumps back or the rhythm changes.
    double hitLast[kBands] = {}, hitNext[kBands] = {};
    uint32_t maskWas[kBands] = {};
    /** The song tick sixteenth [step] sounds at, swung with the track. */
    double stepTick(int64_t step) const;
    /** Finds band [b]'s hits either side of song tick [tick] in [mask]. */
    void findHits(int b, uint32_t mask, double tick);
};

} // namespace acidulous::effect
