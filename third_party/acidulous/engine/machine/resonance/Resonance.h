#pragma once
#include <cstdint>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>

// Resonance is modal percussion. Eight pads, each a bank of resonators tuned
// to the modes of a shape (membrane, bar, plate, tube, bowl or metal), struck
// at a position and left to ring.
//
// The pads can also excite each other by a set amount, like the snare buzzing
// when the kick is hit, so the kit rings together.
namespace acidulous::machine {

class Resonance final : public Machine {
  public:
    static constexpr int kPads = 8;
    static constexpr int kMaxModes = 24;
    static constexpr uint8_t kBaseNote = 36;

    enum Shape : int32_t { Membrane, Bar, Plate, Tube, Bowl, Metal, ShapeCount };

    // Per-pad parameters, then the globals. Names are generated as
    // "p00_tune" and so on, like Forage's.
    enum PadP : int32_t {
        Kind = 0, Tune, Decay, Damp, Inharm, Hit, Hard, Noise, Bend, BendTime,
        Drive, Level, Pan, Couple, PadParamCount
    };
    enum P : int32_t {
        PadBase = 0,
        Modes = PadBase + kPads * PadParamCount,
        Coupling, Humanise, Accent, Volume, MasterPan,
        // Eight macro knobs, each with four routes: a destination (a Dest)
        // and a depth. A macro's knob times a route's depth is added to the
        // destination, in its own 0..1 range. An all-pads destination adds
        // to each pad's own value. Pitch is 24 semitones per unit and
        // multiplies every pad's tune.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        // Added after the macros. How much of a driven pad's bend reaches the
        // coupling bus, 0 the clean pad.
        DriveCouple = MacroBase + 8 * MacroParams,
        Count
    };
    enum Dest : int32_t {
        DstOff, DstDecay, DstDamp, DstInharm, DstHit, DstHard, DstNoise, DstDrive, DstPitch,
        DstCoupling, DstHumanise, DstAccent, DstVolume,
        DstTune, // DstTune + pad
        DstLevel = DstTune + kPads, // DstLevel + pad
        DstDriveCouple = DstLevel + kPads, // after the levels, so they keep their numbers
        DestCount
    };
    static constexpr int kMacros = 8;
    static_assert(Count <= kMaxParams, "Resonance declares more parameters than a unit can hold");

    Resonance();
    const char *typeName() const override { return "Resonance"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t) override {} // a struck thing rings until it stops
    void allNotesOff() override;
    bool render(float *L, float *R, int32_t frames) override;

  private:
    /** One mode: a two-pole resonator. */
    struct Mode {
        float a1 = 0.0f, a2 = 0.0f, b0 = 0.0f;
        /**
         * `b0` without the strike position's node gain. The hit position
         * moves on every note (humanise), and only the node gain depends on
         * it, so keeping the rest here lets a restrike skip the expensive
         * rebuild and get the same numbers.
         */
        float b0Base = 0.0f;
        float y1 = 0.0f, y2 = 0.0f;
        float step(float x) {
            const float y = b0 * x + a1 * y1 + a2 * y2;
            y2 = y1;
            y1 = y;
            return y;
        }
        void clear() { y1 = y2 = 0.0f; }
    };
    struct Pad {
        Mode modes[kMaxModes];
        int32_t modeCount = 8;
        float exciteLeft = 0.0f, exciteStep = 0.0f, exciteGain = 0.0f;
        float bendLeft = 0.0f, bendCoeff = 0.0f, bendDepth = 0.0f;
        float velocity = 1.0f;
        // Where the last strike landed, humanise included. -1 until struck.
        float hit = -1.0f;
        float last = 0.0f;   // this pad's last output, for the coupling bus
        // The last two excitation samples, so modes can be fed the
        // difference. See render().
        float x1 = 0.0f, x2 = 0.0f;
        // Per-pad coupling loss filter. See render() for why it can't be one
        // filter on the shared bus.
        float coupleLp = 0.0f;
        // How much coupling this pad accepts. A resonator's gain is about
        // 1/sqrt(1-r), so long decays have far more gain than short ones and
        // need trimming.
        float couplingTrim = 1.0f;
        bool ringing = false;
        // What the modes were built from, so they're only rebuilt when
        // something changes.
        float builtTune = -1.0f, builtDecay = -1.0f, builtDamp = -1.0f, builtInharm = -1.0f, builtHit = -1.0f;
        int32_t builtKind = -1, builtModes = -1;
    };

    float padParam(int32_t pad, int32_t which) const { return params_.get(PadBase + pad * PadParamCount + which); }
    int32_t padStep(int32_t pad, int32_t which) const { return static_cast<int32_t>(padParam(pad, which) + 0.5f); }
    /** Fills [mac_] from the macro knobs, smoothed or (when [goal]) their targets. */
    void updateMacros(bool goal = false);
    /** Param [p] with the macros' routes to it added in its own 0..1 range. */
    float val(int32_t p, bool goal = false) const;
    /** A param's smoothed value, or its target when [goal] (note-on seeds). */
    float read(int32_t p, bool goal) const { return goal ? params_.target(p) : params_.get(p); }
    /** A pad's param with the macros added. Tune includes the pitch macro. */
    float padVal(int32_t pad, int32_t which, bool goal = false) const {
        const float v = val(PadBase + pad * PadParamCount + which, goal);
        return which == Tune ? v * pitchMul_ : v;
    }
    void buildPad(int32_t pad);
    /** Update only the node gains, for when only the strike has moved. */
    void restrike(int32_t pad);
    /**
     * Where a pad was struck, humanise included. Kept on the pad rather than
     * written back to the Hit parameter, so it doesn't drift and a reset
     * clears it.
     */
    float hitOf(int32_t pad) const {
        return pads[pad].hit >= 0.0f ? pads[pad].hit : padVal(pad, Hit);
    }
    float noise() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return static_cast<float>(rng) * (2.0f / 4294967296.0f) - 1.0f;
    }

    float sr = 48000.0f;
    Pad pads[kPads];
    // Two coupling buses. The knock comes from the strike only, so it can't
    // feed back, and it's broadband so it excites neighbouring pads well. The
    // ring comes from the resonators and is a loop, so it's sent at a
    // fraction and trimmed by how resonant the receiving pad is.
    float knockBus = 0.0f, ringBus = 0.0f;
    float mac_[DestCount]{};
    float pitchMul_ = 1.0f; // every pad's tune, from the pitch destination
    static constexpr uint32_t kRngSeed = 0x2545f491u;
    uint32_t rng = kRngSeed;
};

} // namespace acidulous::machine
