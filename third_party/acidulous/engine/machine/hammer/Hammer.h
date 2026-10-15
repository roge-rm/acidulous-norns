#pragma once
#include <cstdint>
#include <engine/machine/Machine.h>
#include <engine/machine/hammer/Board.h>
#include <engine/machine/hammer/Course.h>
#include <engine/machine/hammer/Electric.h>
#include <engine/machine/hammer/Felt.h>
#include <engine/machine/hammer/Keys.h>

// Hammer: a modelled piano, and in time everything else a hammer strikes
// (PLAN §4.24, M71).
//
// A key is a felt hammer thrown at a course of one to three stiff strings
// (Felt.h, Course.h), its sound through one soundboard shared by every key
// (Board.h). What each key is - how stiff its strings, how long they ring,
// how far the prompt sound falls before the aftersound, how bright the
// strike - is measured from reference recordings (Keys.h, KeyTables.h).
//
// One voice per key: striking a key that is still ringing strikes the same
// strings again, as on the instrument.
namespace acidulous::machine {

class Hammer final : public Machine {
  public:
    enum P : int32_t {
        // The instrument.
        Model = 0, Size, Age, Seed,
        // The hammer.
        Hardness, HardKey, Weight, Position, FeltSoft, Tacks, VelocityAmount,
        // The strings.
        Sustain, SustainKey, Tone, ToneKey, Stiffness, Stretch, Unison, Strings, Couple, Polar, Tension, Clang,
        // Dampers and pedals.
        Dampers, DampTime, PedalAt, PedalSpan, UnaCorda, Sympathy, Duplex, Noises,
        // The board.
        BoardLevel, Lid, Tail, Mic, Width,
        // Electric.
        Pickup, Offset, Tonebar, Pickups, Mute, Drive, Tremolo, TremRate, TremSync, TremWide,
        // Preparations.
        Prep, PrepKeys, PrepAt, PrepAmt,
        // Out.
        Detail, Voices, Volume, Pan, BendRange, Octave, Transpose, Fine,
        Count
    };
    static_assert(Count <= kMaxParams, "too many parameters");

    Hammer();

    const char *typeName() const override { return "Hammer"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void setDampers(bool lifted) override;
    bool takesPedals() const override { return true; }
    void pedal(int32_t which, float level01) override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    bool render(float *L, float *R, int32_t frames) override;

    // For tools/hammer_test and tools/hammer_render: what each key is, to
    // read or adjust (then refreshKeys) when calibrating against references.
    int activeVoices() const;
    int lastContactSamples() const { return lastContact; }
    /** [key] of the instrument the model parameter has now. */
    hammer::KeySpec &keySpec(int key) { return keys[modelNow()][key < 0 ? 0 : (key > kKeys - 1 ? kKeys - 1 : key)]; }
    void refreshKeys();

  private:
    static constexpr int kVoices = 32;
    static constexpr int kKeys = 128;
    /** Longest strike-to-bridge delay, samples (A0 is about 800). */
    static constexpr int kKnockLine = 2048;
    /** The instruments with key tables (all of hammer::Model). */
    static constexpr int kKeyModels = hammer::Cimbalom + 1;
    /** A tine, a reed, a celesta or a toy piano: a bar, not strings. */
    static bool isBar(int model) {
        return model == hammer::Tine || model == hammer::Reed || model == hammer::Celesta || model == hammer::Toy;
    }
    /** Heard through pickups and the amp, not the board. */
    static bool isElectric(int model) { return model >= hammer::Tine && model <= hammer::Tangent; }

    struct Voice {
        bool used = false;
        /** The key is down (the dampers are off it while it is, and while the pedal is). */
        bool held = false;
        /** Fading out to make room for another note, by [fade] a sample. */
        bool retiring = false;
        float fade = 0.0f;
        int key = 0;
        int note = 0;
        /** A finger's own bend, semitones (MPE). */
        float noteBend = 0.0f;
        /** What its key is, on the instrument it was struck on, and which instrument. */
        const hammer::KeySpec *spec = nullptr;
        int model = 0;
        /** A tine's or reed's bar, as struck, and its pickup; where the bar is. */
        hammer::Bar bar;
        hammer::Bar::Spec barSpec;
        hammer::Pickup pick;
        float barX = 0.0f;
        /** How fast it was struck, m/s: what a tangent's release gives back. */
        float speed = 0.0f;
        hammer::Course course;
        hammer::Felt felt;
        hammer::Course::Design design;
        bool designed = false;
        /** The damper: 0 off the strings, 1 on, moving between over a few blocks. */
        float damp = 0.0f, dampTarget = 0.0f;
        /** The output gain for the velocity law, and where it's ramping to. */
        float gain = 0.0f, gainTarget = 0.0f;
        float panL = 0.7071f, panR = 0.7071f;
        /** The phantom partials' high-pass. */
        float phantomIn = 0.0f, phantomOut = 0.0f;
        /** The blow's force last sample, N: the knock is its change. */
        float force = 0.0f;
        /** This blow's knock against the note's level. */
        float knock = 1.0f;
        /**
         * The knock on its way to the bridge: the board hears the blow when
         * the string brings it, not when the felt lands.
         */
        float knockLine[kKnockLine] = {};
        int knockAt = 0, knockDelay = 0, knockLive = 0;
        int64_t age = 0;
        int quietBlocks = 0;
    };

    /** The key's strike: the course designed for it (if it changed) and the hammer thrown. */
    void strike(Voice &v, int key, float hz, float velocity01);
    /** Bends a voice's strings or bar by the wheel's and its finger's bend. */
    void bendVoice(Voice &v);
    /** A tine's or reed's strike: its bar tuned and struck, its pickup placed. */
    void strikeBar(Voice &v, int model, int key, float hz, float velocity01);
    /** How loud [v] is now, whatever it is. */
    float loudnessOf(const Voice &v) const;
    /** A voice for [key]: its own if it still rings, otherwise a free one or the quietest let go. */
    Voice *voiceFor(int key);
    /** The damper's decay times blended in for [v] at its damper position. */
    void applyDamper(Voice &v);
    int voiceCap() const;
    bool fullDetail() const;
    /** What [key]'s strings are made of, at [hz], with this detail. */
    hammer::Course::Design designFor(int key, float hz, bool full);
    /** The bass keys' stiffness sections, lean and full, designed ahead. */
    void designSections();

    float paramOf(int32_t i) const { return params_.get(i); }
    /** The instrument the model parameter asks for, as far as there are key tables (the rest play the grand, for now). */
    int modelNow() const;
    /** [key]'s note at [shifted] (a MIDI note, fractional), stretched as the instrument is tuned. */
    float hzOf(int key, float shifted) const;
    /** The board as the instrument, the lid and the mic make it. */
    hammer::Board::Voicing voicing() const;
    /** What's on [key]'s strings, if anything (the prepare section), and the level that makes up for it. */
    hammer::Course::Prep prepFor(int key, float impedance, float *makeup) const;

    float sampleRate = 48000.0f;
    Voice voices[kVoices];
    hammer::KeySpec keys[kKeyModels][kKeys];
    float stiffness[kKeyModels][kKeys] = {};
    hammer::Board board;
    /** Keys below this have stiffness sections (Hammer.cpp), kept here, lean and full. */
    static constexpr int kSectionKeys = 36;
    hammer::Course::Sections kept[kSectionKeys][2];
    /** Each key's loss as last designed, lean and full (see Course::Loss). */
    hammer::Course::Loss keptLoss[kKeys][2];
    /** A course only for designing [kept]. */
    hammer::Course warmer;
    bool warmed = false;
    /**
     * The next bass key to design sections for, a key a block, after
     * something that changes them (the instrument, its size, the stiffness):
     * the first note on each would otherwise wait for its own.
     */
    int warmKey = kSectionKeys;
    float warmFor[4] = {};
    /** How far down the sustain and soft pedals are, 0 to 1. */
    float sustainPedal = 0.0f, softPedal = 0.0f;
    /** How far the pedal has the dampers off the strings, 0 to 1 (pedal at, pedal span). */
    float lift() const;
    /** Where [v]'s damper goes now: on the strings, off, or between with half a pedal. */
    float damperFor(const Voice &v) const;
    /**
     * Strings nobody played that ring with the pedal down: tuned to keys
     * that share partials with what was struck (an octave, a twelfth, a
     * fifth away), driven by what the played strings bring to the bridge.
     * Lean uses the first two.
     */
    static constexpr int kBank = 12, kLeanBank = 2;
    Voice bank[kBank];
    int bankNext = 0;
    /** Gives [key]'s partials somewhere to ring, if the pedal's down. */
    void wakeSympathy(int key);
    /** The pedal's own thump, waiting to go into the board. */
    float pedalThump = 0.0f;
    float excite[kBlockFrames] = {};
    float bend = 0.0f;
    int64_t clock = 0;
    int lastContact = 0;
    int quietBlocks = 0;
    bool asleep = true;
    float busL[kBlockFrames] = {}, busR[kBlockFrames] = {}, knockBus[kBlockFrames] = {};
    /** The bass keys' own bus, for the bass bridge's way across the board. */
    float lowL[kBlockFrames] = {}, lowR[kBlockFrames] = {};
    /** The electric pianos' bus, into the amp. */
    float elecL[kBlockFrames] = {}, elecR[kBlockFrames] = {};
    hammer::Amp amp;
    float modWheel = 0.0f, bpm = 120.0f;
    int64_t tick = 0, syncedTick = -1;
};

} // namespace acidulous::machine
