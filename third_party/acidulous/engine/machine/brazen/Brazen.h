#pragma once
#include <cstdint>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/LfoGen.h>
#include <engine/dsp/MultiFilter.h>
#include <engine/machine/Machine.h>
#include <engine/machine/brazen/Bore.h>

// Brazen, a modelled brass instrument, one player or a section.
//
// A tube, a bell and a pair of lips, from tuba to trumpet. The lips are a
// valve whose opening depends on the pressure behind them and the pressure
// in the tube, which is why brass locks to its resonances and gets brighter
// as you blow harder.
//
// Each player in the section is a full instrument. The lock knob pulls their
// pitches toward the section's average: at 0 they're all slightly out, at 1
// they lock into one big horn.
namespace acidulous::machine {

class Brazen final : public Machine {
  public:
    static constexpr int kVoices = 6;
    static constexpr int kPlayers = 4;

    enum Mute : int32_t { Open, Straight, Cup, Harmon, MuteCount };

    enum P : int32_t {
        Size = 0, Bell, Loss, MuteKind, MuteTone,
        Tension, LipDamp, Pressure, Breath, Bite, Brassiness,
        Growl, GrowlRate,
        Players, Spread, Scatter, Lock, Drift, Width,
        Attack, Decay, Sustain, Release,
        Vibrato, VibratoRate, VibratoDelay,
        Cutoff, Resonance, FilterType,
        Mono, Glide, BendRange, Octave, Transpose, Fine, VelocityAmount,
        Drive, Volume, Pan,
        // Added later. Parameters are saved by name, so older patches just
        // get the default.
        MpeTimbre,
        // A swell in the breath from each note-on: positive starts the breath
        // low and rises to the set pressure, negative starts above it and
        // falls back. 0 is off. A slurred note carries on the one it joined.
        Swell, SwellTime,
        // Flutter tongue: the air chopped at a rate, a raised cosine per
        // sample. 0 is off.
        Flutter, FlutterRate,
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate. Pitch
        // is 24 semitones per unit.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        // Added after the macros. Mono only. Legato on slurs a note onto a
        // held key, gliding over `glide` or jumping at 0; off tongues it,
        // unless glide is on. Choke on makes a note after a release cut the
        // old tail: it slurs from it with glide on, and is tongued on that
        // voice at 0. Off lets the tail ring on its own voice.
        Legato = MacroBase + 8 * MacroParams, Choke,
        Count
    };
    // Where a macro route goes. Continuous params only: mute, players,
    // filter type, mono and the tuning steps are left out.
    enum MacroDest : int32_t {
        McOff, McSize, McBell, McLoss, McMuteTone,
        McTension, McLipDamp, McPressure, McBreath, McBite, McBrassiness,
        McGrowl, McGrowlRate,
        McSpread, McScatter, McLock, McDrift, McWidth,
        McAttack, McDecay, McSustain, McRelease,
        McVibrato, McVibratoRate, McVibratoDelay,
        McCutoff, McResonance, McGlide, McVelocity,
        McDrive, McVolume, McPan, McMpeTimbre,
        McSwell, McSwellTime, McFlutter, McFlutterRate,
        McPitch,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
    static_assert(Count <= kMaxParams, "Brazen declares more parameters than a unit can hold");

    Brazen();
    const char *typeName() const override { return "Brazen"; }
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
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** How far apart the section is, in cents. For the test harness. */
    float sectionSpreadCents() const;

  private:
    struct Player {
        brazen::Bore bore;
        float offsetCents = 0.0f;  // this player's pitch offset from the note
        float home = 0.0f;         // where they think the note is
        float walk = 0.0f;         // how far they've drifted from it
        float delayLeft = 0.0f;    // players don't all come in together
        float breath = 1.0f;       // or blow equally hard
        float pan = 0.0f;      // position in the section, -1..1
        /** Pan gains, worked out once a block instead of per sample. */
        float panL = 0.70710678f, panR = 0.70710678f;
        /** The block's mouth pressure, split so the sample loop can ramp it. */
        float pushScale = 0.0f, pushBias = 0.0f;
        /**
         * A late player's own attack ramp.
         *
         * With `scatter`, a player's tube isn't stepped until its turn, and
         * by then the envelope has already risen. Without this ramp the
         * player would jump straight in at full pressure and click.
         */
        float entry = 1.0f;
        /** Set at note-on, used after the next tune since that needs the note. */
        bool tongue = false;
        static constexpr uint32_t kSeed = 1u;
        uint32_t rng = kSeed;
    };
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 60;
        float velocity = 1.0f;
        float outGain = 1.0f; // velocity as level, ramped, see velocityGain
        float freq = 261.63f, glideFrom = 261.63f, glidePos = 1.0f;
        Player players[kPlayers];
        dsp::Adsr amp;
        dsp::MultiFilter filterL, filterR;
        float vibratoPhase = 0.0f, vibratoLeft = 0.0f;
        /**
         * How loudly this voice is still ringing.
         *
         * The envelope drives the mouth pressure, not the output, and the
         * tube keeps ringing after the player stops. A voice is only freed
         * once this is quiet, otherwise its tail gets cut off with a tick.
         */
        float ring = 0.0f;
        float muteLpL = 0.0f, muteLpR = 0.0f, muteHpL = 0.0f, muteHpR = 0.0f;
        int64_t age = 0;
        // Per-note expression (MPE). `bend` is in semitones and adds to the
        // channel bend. `pressure` and `timbre` are -1 until this note sends
        // them, and until then the channel's values are used.
        float bend = 0.0f, pressure = -1.0f, timbre = -1.0f;
        // The breath swell: the pressure it starts at as a fraction of the
        // set one, how far through it is (0..1) and how far a sample moves it.
        float swellFrom = 1.0f, swellPos = 1.0f, swellStep = 0.0f;
        // Where the flutter is in its cycle, 0..1, from the tongued note.
        float flutterPhase = 0.0f;
        // A mono note tongued onto a voice that is still sounding waits for
        // it to fade over kFadeFrames, then starts at the next block.
        // pendingNote is -1 when nothing waits.
        int32_t fadeLeft = 0;
        int pendingNote = -1;
        uint8_t pendingVel = 0;
        bool pendingOff = false;
    };

    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return val(p, false); }
    /** Param [p]'s target with the macros' targets added, for note-on seeds. */
    float goalOf(int32_t p) const { return val(p, true); }
    float val(int32_t p, bool goal) const;
    /** Fills [mac_] from the macro knobs, smoothed or (when [goal]) their targets. */
    void updateMacros(bool goal = false);
    float read(int32_t p, bool goal) const { return goal ? params_.target(p) : params_.get(p); }
    // `steppedTargetOf` comes from Machine.
    int32_t steppedOf(int32_t p) const { return static_cast<int32_t>(paramOf(p) + 0.5f); }
    Voice *allocate();
    void startVoice(Voice &v, uint8_t note, uint8_t velocity);
    /** True when a note on [v] carries on the sound there instead of tonguing it. */
    bool slursOnto(const Voice &v) const;

    float nextRandom() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    }

    float sampleRate = 48000.0f;
    Voice voices[kVoices];
    int64_t ageCounter = 0;
    float bend = 0.0f, modWheel = 0.0f, pressure = 0.0f;
    float growlPhase = 0.0f;
    static constexpr uint32_t kRngSeed = 0x6d2b79f5u;
    uint32_t rng = kRngSeed;
    float lastSpreadCents = 0.0f;
    /** The flutter depth the last block ended on, so a moving knob ramps. */
    float lastFlutter = 0.0f;
    float mac_[MacroDestCount]{};
};

} // namespace acidulous::machine
