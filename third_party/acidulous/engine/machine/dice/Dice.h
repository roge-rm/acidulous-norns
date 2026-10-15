#pragma once
#include <cstdint>
#include <vector>
#include <engine/core/Take.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/dsp/Wsola.h>
#include <engine/machine/Machine.h>

// Dice, a loop slicer.
//
// A loop is sliced at its transients (using Pollen's detector) or on a grid,
// and each slice goes on a pad.
//
// Every trigger rolls against a few probabilities: swap the slice for
// another, reverse it, stutter it, drop it or jump it up an octave. So the
// loop reshuffles as it plays. Hold keeps the same rolls every pass, so a
// pattern you like can be recorded.
namespace acidulous::machine {

class Dice final : public Machine {
  public:
    static constexpr int kSlices = 16;
    // One per pad, so every slice of a 16-slice loop can sound at once
    // without stealing, which would cut audio mid-slice.
    static constexpr int kVoices = 16;
    static constexpr uint8_t kBaseNote = 36;

    enum SliceP : int32_t { Level = 0, Pan, Pitch, Decay, Direction, SliceParamCount };
    enum P : int32_t {
        SliceBase = 0,
        CutMode = SliceBase + kSlices * SliceParamCount, // onsets or grid
        SliceCount, Gate, Rate, RootPitch, Fine,
        Swap, Reverse, Stutter, StutterDiv, Drop, Jump, JumpRange, Hold, Seed,
        Cutoff, Resonance, FilterType, Drive, Volume, MasterPan, Accent,
        /**
         * Whether the loop follows the song's tempo.
         *
         * Off, a slice plays at the speed it was cut at, so a 90 bpm break in
         * a 126 bpm song leaves gaps. On, each slice goes through the same
         * stretcher that frozen clips and Bias takes use, at the ratio of song
         * tempo to loop tempo, so it fits the bar and keeps its pitch.
         */
        Follow,
        /**
         * How many bars the loop is, used to work out its tempo. Auto uses
         * `Take::bars`, the guess made when it was loaded. The others are
         * 1/2, 1, 2, 4, 8 or 16 bars.
         */
        Bars,
        Count
    };
    static_assert(Count <= kMaxParams, "Dice declares more parameters than a unit can hold");

    Dice();
    const char *typeName() const override { return "Dice"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    bool render(float *L, float *R, int32_t frames) override;
    void *swapObject(int32_t slot, void *object) override;
    /** The song's tempo, used for the follow ratio. */
    void onBlock(int64_t, int64_t, float bpm) override { songBpm = bpm; }

    /** The loop's tempo, or 0 with no loop. For the panel. */
    float loopBpm() const;

    /** Where the cuts fall, for the panel to draw. Not audio-thread state. */
    int32_t sliceCount() const { return slices; }
    int32_t sliceStart(int32_t i) const { return i >= 0 && i < slices ? bounds[i] : 0; }

  private:
    struct Voice {
        bool used = false;
        int32_t slice = 0;
        double pos = 0.0, inc = 1.0;
        int32_t left = 0;          // frames until this slice runs out
        int32_t repeats = 0;       // stutter passes still owed
        int32_t repeatLen = 0;
        int32_t start = 0, end = 0;
        float gainL = 0.5f, gainR = 0.5f;
        float env = 1.0f, envCoeff = 0.0f;
        // Frames since this slice (or stutter repeat) started, for the fade
        // in. On an onset cut a slice ends on the next transient, so both
        // ends need a short fade or they click.
        int32_t age = 0;
        uint8_t note = 0;
        // Following the song: the slice comes out of `stretch` at
        // [stretchRate] into `held`, which is read at [pitch]. So the stretch
        // sets the length and the pitch knob only sets pitch.
        bool follows = false;
        float stretchRate = 1.0f, pitch = 1.0f, timeRate = 1.0f;
        dsp::StereoStretch stretch;
        std::vector<float> held[2];
        int32_t heldHave = 0;
        double heldPos = 0.0;
        // One filter per channel, so both sides get filtered.
        dsp::MultiFilter filter, filterR;
    };

    float sliceParam(int32_t slice, int32_t which) const {
        return params_.get(SliceBase + slice * SliceParamCount + which);
    }
    float paramOf(int32_t p) const { return params_.get(p); }
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }
    void recut();
    void startStretch(Voice &v) const;
    bool pullStretch(Voice &v, const float *left, const float *right) const;
    Voice *allocate();
    uint32_t rollFor(int32_t slice);

    float sampleRate = 48000.0f;
    float songBpm = 120.0f;
    const audio::Take *take = nullptr;
    int32_t bounds[kSlices + 1] = {};
    int32_t slices = 0;
    int32_t builtMode = -1, builtCount = -1, builtFrames = -1;
    Voice voices[kVoices];
    static constexpr uint32_t kRngSeed = 0x5bd1e995u;
    uint32_t rng = kRngSeed;
    int64_t triggers = 0;
};

} // namespace acidulous::machine
