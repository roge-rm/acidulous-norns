#pragma once
#include <cstdint>
#include <vector>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * The track cut into slices on the tempo, some of them replaced as they
 * play: by `chance`, a slice repeats the one before it, plays it backwards,
 * or drops out, in the proportions `repeat`, `reverse` and `drop` set.
 * `gate` shortens every slice, `pitch` shifts the repeats, and `seed`
 * chooses which slices change, the same ones every time the song plays.
 */
class Slicer final : public Effect {
  public:
    enum P { Rate, Chance, Repeat, Reverse, Drop, Gate, Pitch, Seed, Mix, Gain, Count };
    Slicer() { initParams(); }
    const char *typeName() const override { return "Slicer"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override {
        blockStart = tickStart;
        blockEnd = tickEnd;
        this->bpm = bpm;
    }
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;

  private:
    enum Action : uint8_t { Play, Again, Backwards, Rest };
    Action choose(int64_t slice) const;
    std::vector<float> ring[2];
    int32_t size = 0, write = 0;
    float sr = 48000.0f, bpm = 120.0f;
    int64_t blockStart = 0, blockEnd = 0, slice = -1;
    /** Where the slice now playing started and where the one before it did, as ring positions. */
    int32_t sliceStart = 0, lastStart = 0;
    int32_t sliceLength = 1, age = 0;
    Action action = Play;
    double head = 0.0;
};

} // namespace acidulous::effect
