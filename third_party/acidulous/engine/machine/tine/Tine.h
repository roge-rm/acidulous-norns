#pragma once
#include <cstdint>
#include <vector>
#include <engine/machine/Machine.h>

namespace acidulous::machine {

/**
 * Tine: struck and plucked tuned metal and wood.
 *
 * Each note is a handful of resonant modes, tuned to the ratios of the thing
 * struck: a wooden bar undercut so its overtones sit two octaves and a third
 * above the note, a metal bar left free, a tine clamped at one end, or the
 * hammered note of a pan, tuned to octave and twelfth. A mallet, a thumb or a
 * pin strikes it at a place along it, harder or softer, which decides how
 * much of each mode it gets.
 *
 * Around that the instrument: a tube under each bar that sings along with the
 * note, the turning discs in a vibraphone's tubes, a damper bar, the buzz of
 * a rattle on a thumb piano, the octave that grows out of a pan's note after
 * it's struck, rolls on held notes, and a bow drawn across a bar under
 * pressure.
 *
 * A note can melt into another model while it rings: each mode glides its
 * ratio (on a log scale, so the middle is out of tune with both), its ring
 * time and its share of a strike to its partner in the other model, paired
 * in order. A mode only one of them has fades out, or is fed from what the
 * note is ringing with until it sounds. `melt` is how long it takes, `heat`
 * how much velocity and pressure hurry it, and `return` has it go once or
 * back and forth.
 */
class Tine final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Mallet, Position, Decay, Bright, Damp,
        Tube, Motor, Depth, Bloom, Buzz, Roll, Spread,
        Voices, VelocityAmount, BendRange, Octave, Volume,
        // A second copy of each note this many cents sharp, struck with it,
        // so the two beat as a gamelan's pairs do. 0 is off.
        Pair,
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate. Pitch
        // is 24 semitones per unit.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        // Added after the macros. The model a note melts into, how long a
        // struck note takes to get there (0 off, then 0.05 to 20 s on a log
        // scale), how much velocity and pressure hurry it, and whether it
        // goes once or back and forth.
        Into = MacroBase + 8 * MacroParams, Melt, Heat, Return,
        Count
    };
    // Where a macro route goes. Continuous params only: the model, tune,
    // voices, bend range, octave and into are left out.
    enum MacroDest : int32_t {
        McOff, McMallet, McPosition, McDecay, McBright, McDamp, McTube, McMotor, McDepth,
        McBloom, McBuzz, McRoll, McSpread, McVelocity, McVolume, McPair,
        McPitch,
        // Where the melt is: added to how far each note has melted.
        McMelt,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
    static_assert(Count <= kMaxParams, "too many parameters");

    enum Kind : int32_t {
        Marimba = 0, Vibraphone, Xylophone, Glockenspiel, Kalimba, MusicBox,
        SteelPan, Handpan, TongueDrum,
        TubularBell, Crotale, Saron, Bonang, Gong, SingingBowl, SlitDrum, TempleBlock, Cowbell, Triangle, KindCount
    };
    static constexpr int kVoices = 16;
    static constexpr int kModes = 6;

    Tine();

    const char *typeName() const override { return "Tine"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void setDampers(bool lifted) override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** Notes sounding, for tests. */
    int activeVoices() const;
    /** How far the first sounding note has melted, 0 to 1, or -1 if it isn't melting. For tests. */
    float meltOf() const;
    /** Mode [i]'s ratio now on the first sounding note, for tests. */
    float modeRatio(int i) const;

  private:
    struct Mode {
        float a1 = 0.0f, a2 = 0.0f, b0 = 0.0f;
        /** Its share of the strike, and of the bow (0 if a bow can't reach it). */
        float amp = 0.0f, bowShare = 0.0f;
        /** The bow's gain into it, made up for how long it rings. */
        float bow = 0.0f;
        float ratio = 1.0f, t60 = 1.0f, r = 0.0f;
        float y1 = 0.0f, y2 = 0.0f;
        /** Melting only: the noise feed that sounds a mode the note didn't start with, and its bloom drive. */
        float feed = 0.0f, bloomK = 0.0f;
        /** Melting only: the cosine and sine of the step it's tuned to now, and its pole. */
        float cw = 1.0f, sw = 0.0f, re = 0.0f;
    };
    /** A model as built for a note: what melting goes from and to. */
    struct Shape {
        int count = 0, bloom = -1;
        float logRatio[kModes]{}, logT60[kModes]{}, amp[kModes]{}, logAmp[kModes]{}, bowShare[kModes]{};
        float click = 0.0f, contact = 1.0f;
    };
    struct Voice {
        Mode modes[kModes];
        int modeCount = 0;
        /** A re-strike's damping of what's still ringing, spread over a few milliseconds so it doesn't click. */
        float settle = 1.0f;
        int32_t settleLeft = 0;
        bool used = false, held = false, damped = false;
        uint8_t note = 0;
        float velocity = 0.8f;
        /** The note as a MIDI pitch before bend, and the pitch the modes are tuned to now. */
        float pitch = 60.0f, builtPitch = -1000.0f;
        float gain = 0.0f, pan = 0.0f;
        /** The strike going in, and where it is. */
        std::vector<float> strike;
        int32_t strikeLength = 0, strikeAt = 0;
        float click = 0.0f, clickLast = 0.0f;
        /** The tube under the bar: a delay half the note long, fed back inverted. */
        std::vector<float> tube;
        int32_t tubeAt = 0;
        float tubeDelay = 1.0f;
        /** Samples until a roll strikes again. */
        int32_t rollLeft = 0;
        float noteBend = 0.0f, pressure = -1.0f, bowing = 0.0f;
        float buzzLast = 0.0f;
        /** Which mode is the octave a pan's note grows into (-1: none), and how hard the note drives it. */
        int bloomMode = -1;
        float bloomCoef = 0.0f;
        float level = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
        /** The pair's modes, rung beside [modes] when the note was struck with pair on. */
        Mode pairModes[kModes];
        bool paired = false;
        /** The pair's offset the modes are tuned to now, cents. */
        float builtPair = -1.0f;
        /** The pitch macro at the note-on (in [pitch]) and now, in its 0..1 units. */
        float macAt = 0.0f, macNow = 0.0f;
        /** How hard the damper was when it came down, for a melting note's ring times. */
        float dampBy = 0.0f;
        /** The model the note was struck as and the one it melts into, as built. */
        Shape from, to;
        /** Into differs from the model, so the note can melt. */
        bool canMelt = false;
        /** On the melting path: set once the melt first moves, kept until a fresh note. */
        bool melted = false;
        /** The velocity it was struck at, for heat. */
        float hit = 0.8f;
        /** How far through the melt, in melts (0..1 once; 0..2 a round trip back and forth), and the blend now. */
        float meltU = 0.0f, meltPos = 0.0f;
        /** The blend and pitch the modes were last tuned to, so a settled melt isn't worked out again. */
        float tunedPos = -1.0f, tunedPitch = -1000.0f;
        /** Each mode's level at the last melt step, as a log of its share; a ringing mode is scaled from it. */
        float lastLogAmp[kModes]{};
        /** A mode the note didn't start with: its share at the last step, and the level it's held to. */
        float fedShare[kModes]{}, fedLevel[kModes]{};
        /** Modes being fed, for the per-sample noise. */
        int fed = 0;
        /** Samples since the last retune step, at the start of a block: what a melt step moves by. */
        int32_t sinceStep = 0;
        /** The return [meltU] is counted in: back and forth, or once. */
        bool breathing = false;
    };

    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return val(p, false); }
    /** Param [p]'s target with the macros' targets added, for note-on seeds. */
    float goalOf(int32_t p) const { return val(p, true); }
    float val(int32_t p, bool goal) const;
    /** Fills [mac_] from the smoothed macro knobs and [macGoal_] from their targets. */
    void updateMacros();
    /** Fills [macGoal_] alone, for a note-on between blocks. */
    void updateMacroGoals();
    float read(int32_t p, bool goal) const { return goal ? params_.target(p) : params_.get(p); }
    Voice *voiceFor(uint8_t note);
    /** Builds the modes for a voice's note and kind, and its strike. */
    void build(Voice &v);
    /** Tunes the modes to the voice's pitch now, keeping their decay. */
    void retune(Voice &v);
    void strike(Voice &v, float velocity);
    /** Builds [s] for [kind] at frequency [f] from the knobs' targets, as build() does the modes. */
    void shapeOf(int kind, float f, Shape &s) const;
    /** Where the melt is for [v] now, 0 to 1, from its progress and the melt macro [mac]. */
    float meltBlend(const Voice &v, float mac) const;
    /** Recounts [v.meltU] in the return now set, keeping the blend where it is. */
    void meltReturn(Voice &v) const;
    /** Puts [v] on the melting path; [struck] when a strike is about to land at the blend. */
    void meltBegin(Voice &v, bool struck, int32_t span = -1);
    /**
     * Tunes a melting voice's modes to the blend at [v.meltPos]: retune() for a melting note,
     * [samples] after the last, with levels glided over the [span] samples to the next step
     * (-1: from the start of the next block).
     */
    void meltTune(Voice &v, int32_t samples = 0, int32_t span = -1);
    /** Moves a ringing mode to step [w] and pole [re] keeping its phase and level, so a glide doesn't wobble. */
    static void carry(Mode &m, float w, float re);
    /** ring() for a melting note: [nz] the noise feeding modes the note didn't start with. */
    float ringMelt(Mode *modes, const Voice &v, float x, float bow, float bloom, float nz);
    void damp(Voice &v);
    /** Rings [modes] (the voice's or its pair's) one sample on, struck by [x] and bowed by [bow]. */
    float ring(Mode *modes, const Voice &v, float x, float bow, float bloom);
    float pitchOf(const Voice &v) const {
        const float p = v.pitch + bend * paramOf(BendRange) + v.noteBend;
        // Apart so a note with no pitch macro moving is worked out as before.
        return v.macNow != v.macAt ? p + (v.macNow - v.macAt) * 24.0f : p;
    }
    float white() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>(noise >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    /** The melt's own noise, so feeding a mode leaves the knock and the rattle as they were. */
    float feedWhite() {
        feedNoise = feedNoise * 1664525u + 1013904223u;
        return static_cast<float>(feedNoise >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

    Voice voices[kVoices];
    float sampleRate = 48000.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    bool dampersUp = false;
    float motorPhase = 0.0f;
    int retuneCountdown = 0;
    uint32_t noise = 0x5bd1e995u, feedNoise = 0x27d4eb2fu;
    uint32_t clock = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
    float mac_[MacroDestCount]{}, macGoal_[MacroDestCount]{};
};

} // namespace acidulous::machine
