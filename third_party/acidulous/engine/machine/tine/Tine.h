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
        Count = MacroBase + 8 * MacroParams
    };
    // Where a macro route goes. Continuous params only: the model, tune,
    // voices, bend range and octave are left out.
    enum MacroDest : int32_t {
        McOff, McMallet, McPosition, McDecay, McBright, McDamp, McTube, McMotor, McDepth,
        McBloom, McBuzz, McRoll, McSpread, McVelocity, McVolume, McPair,
        McPitch,
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

  private:
    struct Mode {
        float a1 = 0.0f, a2 = 0.0f, b0 = 0.0f;
        /** Its share of the strike, and of the bow (0 if a bow can't reach it). */
        float amp = 0.0f, bowShare = 0.0f;
        /** The bow's gain into it, made up for how long it rings. */
        float bow = 0.0f;
        float ratio = 1.0f, t60 = 1.0f, r = 0.0f;
        float y1 = 0.0f, y2 = 0.0f;
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

    Voice voices[kVoices];
    float sampleRate = 48000.0f;
    float bend = 0.0f, wheel = 0.0f, channelPressure_ = 0.0f;
    bool dampersUp = false;
    float motorPhase = 0.0f;
    int retuneCountdown = 0;
    uint32_t noise = 0x5bd1e995u;
    uint32_t clock = 0;
    int32_t quietSamples = 0;
    bool asleep = true;
    float mac_[MacroDestCount]{}, macGoal_[MacroDestCount]{};
};

} // namespace acidulous::machine
