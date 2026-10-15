#pragma once
#include <cstdint>
#include <engine/dsp/PitchFollow.h>
#include <engine/machine/Machine.h>
#include <engine/machine/diction/Phones.h>
#include <engine/machine/diction/Throat.h>
#include <engine/machine/tongue/Reed.h>

namespace acidulous::machine {

/**
 * Tongue: a jaw harp.
 *
 * A reed rings through a slot in its frame. While it's in the slot it pushes
 * air like a piston; out of it, the air goes round. That switch, twice a
 * cycle, chops the air into sharp pulses: a drone with every harmonic at much
 * the same level (as measured, PLAN §4.25). Breath adds air through the gap
 * and, hard enough, keeps the reed going. The mouth (Diction's vowels, as
 * peaks added to the sound) brings single harmonics forward: that's the
 * melody, over a drone that doesn't move.
 *
 * Ten kinds of harp share the model, each with its own reed, slot, ring and
 * frame (kKindVoices); the knobs move each kind's sound from where it sits.
 * A harp can have up to five reeds, tuned as a chord.
 *
 * Keys play it two ways: a harp at each key's pitch, or one harp on a drone
 * note with each key moving the mouth onto the drone's nearest harmonic, as
 * a player does. A pattern replucks held harps on the song's grid. The
 * mouth can follow another track's melody instead (`sidechain`): its pitch,
 * folded into the drone's 3rd to 12th harmonics. And it can say a clip's
 * words: each note's sounds move the mouth, the hissing ones with a hiss.
 */
class Tongue final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Set, Edge, Ring, Pluck, Overtones,
        Mouth, Focus, Depth, Glide,
        Breath, Air, Sustain, Stop,
        Voices, VelocityAmount, BendRange, Octave, Volume,
        Reeds, Chord, Strum, Order,
        Play, Drone, Repluck, Pattern, Accent, Ratchet,
        Sidechain, Words,
        Count
    };
    static_assert(Count <= kMaxParams, "too many parameters");

    /** The kinds of jaw harp, for `model`. */
    enum Kind : int32_t {
        Steel = 0, Munnharpe, Khomus, Morsing, TemirKomuz, Brass, Bamboo, Mukkuri, Genggong, Kouxian, kKinds
    };
    /** How the reeds of a harp are tuned, for `chord`; Auto is the kind's own. */
    enum ChordKind : int32_t { AutoChord = 0, Unison, Octaves, Fifths, Major, Minor, Pentatonic, kChords };
    /** The order a harp's reeds are plucked in, for `order`. */
    enum OrderKind : int32_t { Up = 0, Down, InTurn, Scatter, kOrders };
    /** What the keys do, for `play`. */
    enum PlayKind : int32_t { DroneKeys = 0, MouthKeys, kPlays };
    /** When a key in mouth mode plucks the harp again, for `repluck`. */
    enum RepluckKind : int32_t { EveryNote = 0, FirstNote, LoudNotes, kReplucks };
    /** The plucking rhythms, for `pattern`. */
    enum PatternKind : int32_t { NoPattern = 0, Eighths, Sixteenths, Gallop, Triplets, Runs, Groupings, kPatterns };

    Tongue();

    const char *typeName() const override { return "Tongue"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void channelPressure(uint8_t value) override;
    void lyric(const uint8_t *phones, int32_t count) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** Voices sounding, for tests. */
    int activeVoices() const;
    /** How many times a harp has been plucked since reset, and when (samples since reset) the last 64 were; for tests. */
    int strikes() const { return strikeCount; }
    int64_t strikeTime(int i) const { return strikeLog[i & 63]; }
    /** Where the mouth's formants are going, Hz; for tests. */
    float formantTarget(int k) const { return mouthGoal[k]; }
    /** Where the mouth's picking resonance is going, Hz, or 0 when it's off; for tests. */
    float pickTarget() const { return pickOn ? pickHz : 0.0f; }
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;

    /**
     * What makes each kind itself. set, edge and pluck are where the kind
     * sits when those knobs are at their defaults; ring scales the ring knob.
     */
    struct KindVoice {
        float set, edge, pluck;
        float ring;
        /** The overtones' ring against the note's. */
        float overRing;
        /** The reed's second and third modes against its first. */
        float ratio2, ratio3;
        /** A first-order high-pass on the sound, Hz; 0 for none. */
        float lowCut;
        /** The frame's knock: Hz, ring in seconds, level against the pluck. */
        float frameHz, frameRing, frameLevel;
        /** A string pull's draw in ms; 0 for a finger. */
        float pullMs;
        /** The breath's hiss against steel's. */
        float air;
        /** Level, so every kind sits at the same loudness. */
        float level;
        /** Reeds and tuning when those knobs are on auto. */
        int reeds;
        int chord;
    };
    static const KindVoice kKindVoices[kKinds];

  private:
    static constexpr int kVoices = 4;
    static constexpr int kReeds = 5;
    /** Samples between moves of the mouth. */
    static constexpr int kControl = 32;
    /** A formant's peak over the sound at full depth (about 18 dB), and each formant's share. */
    static constexpr float kMouthBoost = 8.0f;
    /** The furthest off the slot's middle the reed rests, as a share of its swing. */
    static constexpr float kReach = 0.8f;
    /** The slot's half width at the default fit (edge 0.55), in swings. */
    static constexpr float kDefaultWidth = 0.25f - 0.23f * 0.55f;
    static constexpr float kFormantShare[3] = {1.00f, 0.70f, 0.40f};
    /**
     * The mouth's formants (Hz) along the vowel control, oo oh ah eh ee. A
     * player's mouth round a harp sits higher than speech: F1 0.5 to 1.6 kHz
     * and F2 1.4 to 3.2 kHz in the recordings (PLAN 4.25).
     */
    static constexpr float kMouths[5][3] = {
        {350, 1700, 2700}, {900, 1900, 2800}, {1650, 1800, 3000}, {950, 2400, 3200}, {800, 2750, 3500},
    };

    /** One reed of a harp and what its slot does with it. */
    struct Tine {
        tongue::Reed reed;
        bool retuned = false;
        /** Semitones from the harp's note. */
        float offset = 0.0f;
        /** Samples a radian of the note: a change a sample, taken back to one a radian. */
        float perRadian = 40.0f;
        float q = 0.0f, air = 0.0f;
        /** The slot's half width this block, and the level that goes with it. */
        float width = kDefaultWidth, fit = kDefaultWidth;
        float level = 0.0f;
        /** How far the reed swings, followed over about a millisecond. */
        float swing = 0.0f;
        float gain = 0.0f;
        /** A pluck waiting its turn in a strum: samples to go, or -1. */
        int wait = -1;
        float waitSwing = 0.0f, waitContact = 0.0f, waitSnap = 0.0f, waitGain = 0.0f;
    };

    struct Voice {
        Tine tines[kReeds];
        int count = 1;
        /** The kind this harp was plucked as. */
        int kind = Steel;
        /** The next reed to pluck, played in turn. */
        int next = 0;
        tongue::Knock knock;
        bool used = false, held = false;
        uint8_t note = 0;
        float baseNote = 60.0f;
        /** A finger's own bend (semitones), pressure (0 to 1, -1 for the channel's) and slide (0 to 1), MPE. */
        float noteBend = 0.0f, pressure = -1.0f, slide = 0.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };

    /** One step of a word: where the mouth goes, its hiss, and for how long (-1 held). */
    struct Said {
        float f[3];
        float noise;
        float band[2][3];
        int32_t samples;
    };
    static constexpr int kSaid = 24;
    static constexpr int kMaxPhones = 32;

    /** A pluck waiting for its sample: from a pattern step or a ratchet. */
    struct Due {
        int64_t at = 0;
        float velocity = 0.0f;
    };
    static constexpr int kDue = 16;
    /** A key down in mouth mode. */
    static constexpr int kKeys = 16;

    float paramOf(int32_t i) const { return params_.get(i); }
    Voice *voiceFor(uint8_t note);
    void retune(Voice &v);
    /** Plucks a sounding harp again, all its reeds as the order says. */
    void strike(Voice &v, float vel);
    /** Starts a harp on [note] and plucks it. */
    void startHarp(Voice &v, uint8_t note, float noteNumber, float vel);
    /** Mouth mode: the harmonic of the drone nearest [note], as Hz. */
    float harmonicFor(uint8_t note) const;
    /** The harmonic of the drone nearest [hz], from the 2nd up; folded by octaves into the 3rd to 12th when [fold]. */
    float harmonicNear(float hz, bool fold) const;
    /** Turns the next note's words into steps for the mouth. */
    void planWords();
    /** The mouth's hiss for the word step it's on. */
    void startSaid();
    /** The note is let go: its word's end sounds. */
    void endWords();
    /** Following has stopped: the mouth goes back to the keys, or off in drone mode. */
    void letGoOfFollow();
    /** The drone as it sounds: the lowest reed, or the drone note when nothing does. */
    float droneHz() const;
    /** The pattern's steps from here to the end of the block, as plucks due. */
    void schedulePattern(int32_t frames);
    void addDue(int64_t at, float velocity);
    /** A pattern or ratchet pluck: every held harp, or the drone. */
    void repluckHeld(float velocity);
    /** Sets a reed going now: by finger or string, as its kind is played. */
    void pluckTine(const Voice &v, Tine &t, float swing, float contact, float snap, float gain);
    /** The mouth: formants from the vowel control, narrowed by focus, and the gain that keeps the level. */
    void moveMouth(bool jump);

    Voice voices[kVoices];
    /** The mouth's three formants on the sound, and on the breath (twice as wide). */
    diction::Bandpass mouth[3], mouthAir[3];
    float sampleRate = 48000.0f;
    float formants[3] = {730, 1090, 2440};
    float mouthGain = 1.0f, mouthGainTarget = 1.0f;
    /** What the mouth's gain was last worked out for: formants, focus, lift, pitch and the pick. */
    float lastMouth[8] = {};
    /** Mouth mode's picking resonance: on one harmonic of the drone, gliding to [pickHz]. */
    diction::Bandpass pick;
    float pickHz = 0.0f, pickAt = 0.0f;
    bool pickOn = false;
    uint8_t keys[kKeys] = {};
    /** Following another track: its pitch, blocks to the next look, and blocks since it was last sure. */
    dsp::PitchFollow follower;
    int followLeft = 0, followLost = 0;
    bool following = false;
    float followHz = 0.0f;
    /** The words: the next note's sounds, the steps they make, and where in them the mouth is. */
    uint8_t pending[kMaxPhones] = {};
    int pendingCount = 0;
    Said said[kSaid];
    int saidCount = 0, saidAt = 0, heldStep = 0;
    int32_t saidLeft = 0;
    bool saying = false;
    diction::Bandpass hissBands[2];
    float hissShare[2] = {0.0f, 0.0f}, hissLevel = 0.0f;
    float mouthGoal[3] = {730, 1090, 2440};
    int keyCount = 0;
    float lastVelocity = 0.8f;
    /** The song's position in ticks at the start of this block, kept to the sample, and its tempo. */
    double ticks = 0.0, samplesPerTick = 100.0;
    int64_t nextStep = -1;
    int64_t stepCount = 0;
    /** Samples rendered since reset, to time plucks due. */
    int64_t now = 0;
    int strikeCount = 0;
    int64_t strikeLog[64] = {};
    /** The sample being rendered, like [now]: when a strike happens. */
    int64_t sampleNow = 0;
    /** When a key last plucked, in samples like [now]. */
    int64_t lastKeyPluck = -1000000;
    Due due[kDue];
    int dueCount = 0;
    float bend = 0.0f, wheel = 0.0f, pressure = 0.0f;
    /** The breath this sample, on its way to the block's. */
    float blown = 0.0f;
    float dcIn = 0.0f, dcOut = 0.0f;
    /** The kind's low cut: last input and output. */
    float cutIn = 0.0f, cutOut = 0.0f;
    uint32_t noise = 0x9e3779b9u, clock = 0;
    int controlLeft = 0, quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
