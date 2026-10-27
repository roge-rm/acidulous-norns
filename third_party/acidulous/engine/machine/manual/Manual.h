#pragma once
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/Biquad.h>
#include <engine/dsp/DelayLine.h>
#include <engine/dsp/LfoGen.h>
#include <engine/machine/Machine.h>
#include <engine/machine/manual/Rotary.h>
#include <engine/machine/manual/Wheels.h>

// Manual is the organ. Upper and lower manuals each have their own drawbars
// either side of a movable split, with pedals below them.
//
// Four models share one generator (see Wheels.h):
//   tonewheel  - 91 wheels, nine drawbars, harmonic percussion, key click,
//                leakage and a scanner vibrato.
//   transistor - a combo organ dividing squares down from the top octave,
//                footage tabs instead of drawbars, and a reedy filter.
//   pipe       - ranks (principal, flute, string, reed, mixture) with chiff
//                on the attack and tracker noise.
//   reed       - free reeds under bellows pressure, with reed buzz.
//
// Extras:
//   - Two registrations per manual, morphed between by any mod source.
//   - Wheel spray: per-drawbar detune and stereo spread.
//   - A shared wind supply on every model, so big chords sag in pitch and
//     level.
//   - Horn and drum phase are mod sources, and the rotor can sync to the
//     transport.
namespace acidulous::machine {

class Manual final : public Machine {
  public:
    static constexpr int kVoices = 32;
    static constexpr int kBars = 9;       // 16' 5⅓' 8' 4' 2⅔' 2' 1⅗' 1⅓' 1'
    static constexpr int kPedalBars = 2;  // 16' 8'
    static constexpr int kMatrixSlots = 8;
    static constexpr int kMatrixParams = 3;

    enum ModelKind : int32_t { Tonewheel, Transistor, Pipe, ReedOrgan, ModelCount };
    enum Manuals : int32_t { MUpper = 0, MLower, MPedal };

    enum ModSource : int32_t {
        SrcOff, SrcOn, SrcModWheel, SrcPressure, SrcVelocity, SrcKeyTrack, SrcRandom,
        SrcEg1, SrcEg2, SrcLfo1, SrcLfo2, SrcHorn, SrcDrum, SrcScanner, SrcWind, SourceCount
    };
    enum ModDest : int32_t {
        DstOff, DstMorph, DstSpray, DstDrive, DstVolume, DstPan, DstRotorRate,
        DstPercLevel, DstClick, DstWindSag, DstTreble, DstVibDepth, DstChiff, DstPitch,
        DstUpperAll, DstLowerAll, DstBar1, DstBar2, DstBar3, DstBar4, DstBar5, DstBar6,
        DstBar7, DstBar8, DstBar9, DestCount
    };

    enum P : int32_t {
        Model = 0, Age, Leakage, Hum, Click, ClickRelease, ContactSpread,
        Split, PedalSplit, UpperLevel, LowerLevel, PedalLevel, LowerOn, PedalOn, PedalSustain,
        UpperA,                       // 9
        UpperB = UpperA + kBars,      // 9
        LowerA = UpperB + kBars,      // 9
        LowerB = LowerA + kBars,      // 9
        PedalA = LowerB + kBars,      // 2
        PedalB = PedalA + kPedalBars, // 2
        Morph = PedalB + kPedalBars, MorphSource, MorphAmount,
        Spray, SprayRate, SprayWidth, SprayPattern,
        PercOn, PercHarmonic, PercLevel, PercFast, PercDecay, PercPoly, PercKey, PercSteal,
        VibType, VibRate, VibDepth, VibUpper, VibLower, VibStereo,
        WindSag, WindResponse, WindNoise, TremRate, TremDepth,
        RankPrincipal, RankFlute, RankString, RankReed, RankMixture, Chiff, Tracker,
        ComboWave, Tab16, Tab8, Tab4, Tab2, TabII, TabIV, ComboReedy, ComboAttack,
        ReedPressure, ReedBuzz, ReedTremolo,
        RotOn, RotSpeed, RotHornSlow, RotHornFast, RotDrumSlow, RotDrumFast,
        RotRampUp, RotRampDown, RotMicDistance, RotMicAngle, RotSpread, RotSync,
        Drive, Bias, Bass, Mid, Treble, Volume, Pan,
        AmpAttack, AmpRelease,
        Eg1A, Eg1D, Eg1S, Eg1R, Eg2A, Eg2D, Eg2S, Eg2R,
        Lfo1Wave, Lfo1Rate, Lfo1Sync, Lfo1Depth, Lfo1Phase,
        Lfo2Wave, Lfo2Rate, Lfo2Sync, Lfo2Depth, Lfo2Phase,
        MatrixBase,
        VoiceBase = MatrixBase + kMatrixSlots * kMatrixParams,
        BendRange = VoiceBase, Octave, Transpose, Fine, VelocityAmount, Expression,
        // Added later. Parameters are saved by name, so older patches just
        // get the default, and 0 is off for both.
        // How much a closed expression darkens as well as quietens, as a
        // swell box's shutters do. A lowpass, ramped per sample.
        SwellBox,
        // Seconds for drawbar, tab, rank and morph moves to reach a new
        // value, from anything that moves them. Ramped per sample.
        BarGlide,
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate. Pitch
        // is 24 semitones per unit.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        Count = MacroBase + 8 * MacroParams
    };
    // Where a macro route goes. Continuous params only: the model, the
    // splits, the switches and stepped choices, the matrix and the tuning
    // steps are left out.
    enum MacroDest : int32_t {
        McOff,
        McAge, McLeakage, McHum, McClick, McClickOff, McContacts,
        McUpperLevel, McLowerLevel, McPedalLevel, McPedalSustain,
        McUpperA,                      // 9
        McUpperB = McUpperA + kBars,   // 9
        McLowerA = McUpperB + kBars,   // 9
        McLowerB = McLowerA + kBars,   // 9
        McPedalA = McLowerB + kBars,   // 2
        McPedalB = McPedalA + kPedalBars, // 2
        McMorph = McPedalB + kPedalBars, McMorphAmount,
        McSpray, McSprayRate, McSprayWidth,
        McPercLevel, McPercDecay, McPercKey,
        McVibRate, McVibDepth, McVibStereo,
        McWindSag, McWindResponse, McWindNoise, McTremRate, McTremDepth,
        McPrincipal, McFlute, McString, McReed, McMixture, McChiff, McTracker,
        McTab16, McTab8, McTab4, McTab2, McTabII, McTabIV, McComboReedy, McComboAttack,
        McReedPressure, McReedBuzz, McReedTremolo,
        McHornSlow, McHornFast, McDrumSlow, McDrumFast, McRampUp, McRampDown,
        McMicDistance, McMicAngle, McRotSpread,
        McDrive, McBias, McBass, McMid, McTreble, McVolume, McPan,
        McAttack, McRelease,
        McEg1A, McEg1D, McEg1S, McEg1R, McEg2A, McEg2D, McEg2S, McEg2R,
        McLfo1Rate, McLfo1Depth, McLfo2Rate, McLfo2Depth,
        McVelocity, McSwellBox, McBarGlide,
        McPitch,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
    static_assert(Count <= kMaxParams, "Manual declares more parameters than a unit can hold");
    enum MatP { XSrc = 0, XDest, XDepth };

    Manual();

    const char *typeName() const override { return "Manual"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void channelPressure(uint8_t value) override;
    void pitchBend(int16_t value14) override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    struct Voice {
        bool used = false;
        bool gate = false;
        uint8_t note = 0;
        uint8_t manual = MUpper;
        float velocity = 1.0f;
        float velGain = 1.0f; // see velocityGain, fixed at the note-on
        float key01 = 0.5f;
        dsp::Adsr amp;
        float perc = 0.0f;       // harmonic percussion, one-shot decay
        float percCoeff = 0.0f;
        float click = 0.0f;      // contact bounce, a few milliseconds
        float clickCoeff = 0.0f;
        float clickEnv = 0.0f;   // the burst still sounding from the last make or break
        float chiff = 0.0f;
        float chiffCoeff = 0.0f;
        float contactPhase[kBars] = {}; // per-contact make time, in samples
        int32_t wheel[kBars] = {};
        float barLevel[kBars] = {};     // resolved once a block, not per sample
        float rnd = 0.0f;
        float mod[DestCount] = {};
        // The bar glide: where barLevel was a block ago, the matrix's moves
        // on the bars as they glide, and whether the voice has had a block
        // yet (a new note starts where its bars are, not from the old ones).
        float barFrom[kBars] = {};
        float barMod[DestCount] = {};
        bool fresh = true;
    };

    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return val(p, false); }
    /** Param [p]'s target with the macros' targets added, for note-on seeds. */
    float goalOf(int32_t p) const { return val(p, true); }
    float val(int32_t p, bool goal) const;
    /** Fills [mac_] from the macro knobs, smoothed or (when [goal]) their targets. */
    void updateMacros(bool goal = false);
    float read(int32_t p, bool goal) const { return goal ? params_.target(p) : params_.get(p); }
    /** A drawbar, tab, rank or morph knob as the bar glide has it. */
    float reg(int32_t p) const { return glided[p]; }
    int32_t steppedOf(int32_t p) const;
    void rebuildTuning();
    int32_t wheelFor(int32_t note, int32_t bar) const;
    float sourceValue(int32_t src, const Voice &v) const;
    void applyMatrix(const Voice &v, float *dest) const;
    float drawbarLevel(int32_t manual, int32_t bar, const float *mod) const;
    int32_t timbreFor(int32_t bar) const;
    int32_t timbreForRank(int32_t rank) const;

    float sampleRate = 48000.0f;
    const WheelBank *bank = nullptr;

    Voice voices[kVoices];
    float wheelPhase[WheelBank::kWheels] = {};
    float wheelStep[WheelBank::kWheels] = {};
    float wheelTrim[WheelBank::kWheels] = {};
    float wheelOut[WheelBank::kWheels] = {};
    // Gains are summed per wheel rather than per voice, since many keys can
    // draw on one wheel.
    static constexpr int kSlots = 5;
    float wheelGain[kSlots][WheelBank::kWheels] = {};
    float wheelPeak[kSlots][WheelBank::kWheels] = {};
    uint16_t wheelStamp[kSlots][WheelBank::kWheels] = {};
    int16_t usedWheel[kSlots][WheelBank::kWheels] = {};
    int16_t usedCount[kSlots] = {};
    uint16_t frameStamp = 0;
    float sprayPan[WheelBank::kWheels] = {};
    float sprayDetune[WheelBank::kWheels] = {}; // how many cents, per wheel
    float sprayPhase[WheelBank::kWheels] = {};
    float sprayStep[WheelBank::kWheels] = {};
    float sprayDrift = 0.0f;
    float modelAge = -1.0f;
    float modelSpray = -1.0f;
    // The tuning table the wheels were last set to; see rebuildTuning.
    const float *modelTuning = nullptr;

    Rotary rotary;
    dsp::LfoGen lfo[2];
    float lfoValue[2] = {0.0f, 0.0f};
    dsp::Adsr eg[2];
    float scanPhase = 0.0f, scanValue = 0.0f;
    dsp::DelayLine scanner;
    float windPressure = 1.0f;
    float tremPhase = 0.0f;
    /** Blocks in a row with no key down and nothing over -120 dB; see `render`. */
    int32_t quietBlocks = 0;
    /** The click's own noise, and the gain that gives a 1.8 ms burst one sample's energy. */
    static constexpr uint32_t kClickSeed = 0x5eed1c1cu;
    uint32_t clickRng = kClickSeed;
    float clickBurst = 0.26f;
    /** How much of the idle sounds (leakage, hum, blower) is up; see `render`. */
    float presence = 0.0f;
    float humPhase = 0.0f;
    float leakSum = 0.0f;
    dsp::Biquad bassEq, midEq, trebleEq, reedyFilter, chiffFilter;
    float bendSemis = 0.0f;
    float modWheel = 0.0f, pressure = 0.0f, expression = 1.0f;
    float bpm = 120.0f;
    static constexpr uint32_t kRngSeed = 0x1234567u;
    uint32_t rngState = kRngSeed;
    int32_t heldCount = 0;
    float blockMod[DestCount] = {};
    float mac_[MacroDestCount]{};
    // The bar glide's copy of the registration params, by param index; only
    // the drawbars, tabs, ranks and morph are used. Copied straight through
    // when the glide is 0. `glideFresh` copies on the first block after a
    // reset.
    float glided[Count] = {};
    bool glideFresh = true;
    // The pipe stop gains a block ago, for ramping them while gliding.
    float stopFrom[kBars][5] = {};
    bool stopValid = false;
    // The swell box: its depth (0 open, 1 closed at full), smoothed a block
    // at a time, and the lowpass states for the dry signal and the two
    // spray sides. `swellRunning` is false until the filter has a state.
    float swellDepth = 0.0f;
    float swellS[3] = {};
    bool swellRunning = false;
};

} // namespace acidulous::machine
