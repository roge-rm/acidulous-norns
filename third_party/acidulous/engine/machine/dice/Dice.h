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
    // One per pad, so every slice of a 16-slice loop can sound at once, and
    // four more so a voice that has to make way can fade out while the new
    // slice starts in a free one.
    static constexpr int kVoices = 20;
    static constexpr int kSpare = 4;
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
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate; what
        // a trigger reads when it starts takes the knobs' targets. Pitch is
        // 24 semitones per unit.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        Count = MacroBase + 8 * MacroParams
    };
    // Where a macro route goes. The five dice first. The per-slice settings,
    // cut, hold, follow, bars and the filter type are left out.
    enum MacroDest : int32_t {
        McOff, McSwap, McReverse, McStutter, McDrop, McJump,
        McRolls, McJumpRange, McSeed, McSlices,
        McGate, McRate, McCutoff, McResonance, McDrive, McVolume, McPan, McAccent, McPitch,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
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
    int64_t heldBytes() const override;
    // Frees what was mounted and not swapped back out (on the retire worker).
    ~Dice() override;
    /** The song's tempo, used for the follow ratio. */
    void onBlock(int64_t, int64_t, float bpm) override { songBpm = bpm; }

    /** The loop's tempo, or 0 with no loop. For the panel. */
    float loopBpm() const;

    /** Where the cuts fall, for the panel to draw. Not audio-thread state. */
    int32_t sliceCount() const { return slices; }
    int32_t sliceStart(int32_t i) const { return i >= 0 && i < slices ? bounds[i] : 0; }
    /** The loop's length in frames, or 0 with no loop. */
    int32_t loopFrames() const { return take != nullptr ? take->frames : 0; }
    /**
     * How long the loop's bars are, in frames: the loop less any quiet before
     * its first hit and any ringing past its last bar. Its tempo comes from
     * this. 0 with no loop.
     */
    int32_t barsFrames() const;

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
        // Which trigger started this voice, so stealing takes the oldest note.
        int64_t started = 0;
        // Fading out to make way for a newer slice.
        bool leaving = false;
        // The fade at the end: over the last [fadeLen] frames of [left], from
        // [fadeTop] down. Cutting a voice short starts a new fade from where
        // the old one had got to.
        int32_t fadeLen = 96;
        float fadeTop = 1.0f;
        // The gate: frames until it closes in this piece (a slice, or one
        // stroke of a stutter), what it opens to at each stroke, and the fade
        // before it closes. gateFade 0 is a gate that never closes.
        int32_t open = 0, pieceOpen = 0, gateFade = 0;
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
    };

    float sliceParam(int32_t slice, int32_t which) const {
        return params_.get(SliceBase + slice * SliceParamCount + which);
    }
    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return withMacros(p, params_.get(p), mac_); }
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }
    /** Param [p]'s target, with the macro knobs' targets added: for what a trigger reads when it starts. */
    float goalOf(int32_t p) const { return withMacros(p, params_.target(p), macGoal_); }
    int32_t steppedGoalOf(int32_t p) const { return static_cast<int32_t>(goalOf(p) + 0.5f); }
    float withMacros(int32_t p, float v, const float *mac) const;
    /** Fills [mac_] from the smoothed macro knobs and [macGoal_] from their targets. */
    void updateMacros();
    float mac_[MacroDestCount]{}, macGoal_[MacroDestCount]{};
    void recut();
    int32_t stretchFrom(const Voice &v) const;
    void startStretch(Voice &v) const;
    bool pullStretch(Voice &v, const float *left, const float *right) const;
    Voice *allocate();
    /** Ends [v] in [frames], fading from where its fade has got to. */
    static void shorten(Voice &v, int32_t frames);
    uint32_t rollFor(int32_t slice);

    float sampleRate = 48000.0f;
    float songBpm = 120.0f;
    const audio::Take *take = nullptr;
    int32_t bounds[kSlices + 1] = {};
    int32_t slices = 0;
    int32_t builtMode = -1, builtCount = -1, builtFrames = -1;
    Voice voices[kVoices];
    // One filter per side on the sum of the voices. The filter is linear, so
    // this is the same as one per voice, except that a voice's filter tail
    // goes on ringing after the voice ends instead of being cut off.
    dsp::MultiFilter filterL, filterR;
    static constexpr uint32_t kRngSeed = 0x5bd1e995u;
    uint32_t rng = kRngSeed;
    int64_t triggers = 0;
};

} // namespace acidulous::machine
