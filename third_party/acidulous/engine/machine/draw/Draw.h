#pragma once
#include <cstdint>
#include <engine/machine/Machine.h>
#include <engine/machine/draw/DrawKinds.h>
#include <engine/machine/draw/FreeReed.h>
#include <engine/machine/draw/HarpHole.h>
#include <engine/machine/draw/PipeReed.h>

namespace acidulous::machine {

/**
 * Draw: free reeds. Harmonicas, accordions, melodica, harmonium and the reeds
 * that sound through pipes (PLAN §4.26).
 *
 * Each note is a reed blown by the pressure across it (draw::FreeReed): the
 * breath or the bellows rise and fall, and the reed speaks, settles and
 * stops on its own. Velocity and pressure blow harder, which brightens the
 * sound as a real reed's does; the level follows the house law. Each reed is
 * tuned to sound its note (DrawTuning.h, as a maker files a reed).
 *
 * Harmonicas are holes instead (draw::HarpHole): two reeds in a channel and
 * the player's mouth behind them. Played like a player, a note goes where a
 * player would find it on a Richter harp in the harp's key, natural or bent,
 * and the pitch wheel bends it by moving the tongue (DrawHarp.h).
 */
class Draw final : public Machine {
  public:
    enum P : int32_t {
        Model = 0, Tune, Pressure, Attack, Release, Set, Chamber, Air,
        Voices, VelocityAmount, BendRange, Octave, Volume,
        Register, Detune,
        HarpKey, Playing, Cup, Vibrato, VibratoRate,
        Cassotto, Shake,
        Sync, Sidechain, Words, HighRank, LowRank,
        Count
    };
    static_assert(Count <= kMaxParams, "too many parameters");

    Draw();

    const char *typeName() const override { return "Draw"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void controlChange(uint8_t cc, uint8_t value) override;
    void lyric(const uint8_t *phones, int32_t count) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteBend(uint8_t note, float semitones) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;

    /** Voices sounding, for tests. */
    int activeVoices() const;
    /** A voice's first reed, for tests: the one playing [note], or null. */
    const draw::FreeReed *reedFor(uint8_t note) const;
    /** Reeds a note sounds at most. */
    static constexpr int kReeds = 5;
    /** Pipes a note sounds at most: a shō's chord. */
    static constexpr int kPipes = 6;
    /** How a harmonica note is played: on a hole as it is, bent, or on a reed of its own. */
    enum HarpWay : int32_t { Natural = 0, Bent, Single };

  private:
    static constexpr int kVoices = 8;

    struct Voice {
        /** The note's reeds, one for each rank of its register; [count] of them sound. */
        draw::FreeReed reeds[kReeds];
        int count = 1;
        int32_t stops = 1;
        /** Each reed's level against the first, and how much its rank and the cassotto darken it, with their low-pass. */
        float rankLevel[kReeds] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        float chamber[kReeds] = {};
        float chamberLow[kReeds] = {};
        float share = 1.0f;
        bool used = false, held = false;
        uint8_t note = 0;
        float baseNote = 60.0f;
        int32_t kind = draw::Accordion;
        /** The pressure blowing the reed now, and where it's going, Pa. */
        float blown = 0.0f, aim = 0.0f;
        /** The pressure the note was struck at, Pa: what pressure added since is louder than. */
        float struck = 1.0f;
        /** How much louder pressure added since the note started makes it, smoothed. */
        float squeeze = 1.0f;
        /** The note's own pressure (MPE), 0 to 1, or -1 for the channel's. */
        float pressure = -1.0f;
        /** The note's own bend (MPE), semitones, and its slide, 0 to 1 (0 until a finger sends one). */
        float noteBend = 0.0f, slide = 0.0f;
        float velocity = 0.8f;
        float gain = 0.0f;
        float level = 0.0f;
        /** The breath's or bellows' slow wander: a smoothed noise, two poles. */
        float wander1 = 0.0f, wander2 = 0.0f;
        /** A pipe instrument's pipes for the note, and the MIDI note each sounds (before bend and tune). */
        draw::PipeReed pipes[kPipes];
        int pipeCount = 0;
        float pipeNote[kPipes] = {};
        /** A harmonica's holes (two on a tremolo or octave harp), none for other kinds. */
        draw::HarpHole holes[2];
        int holeCount = 0;
        /** Blown (+1) or drawn (-1). */
        float sign = 1.0f;
        /** How the note is played, on which hole, and how far the note itself is bent, semitones. */
        int32_t way = Single;
        int hole = 0, bentBy = 0, key = 5;
        /** The mouth's resonance, Hz, and where it's going. */
        float mouth = 3000.0f, mouthAim = 3000.0f;
        /** How fast the tongue is moving, octaves a second, and its unsteadiness, a smoothed noise. */
        float mouthSpeed = 0.0f, tongue1 = 0.0f, tongue2 = 0.0f;
        /** A bend held by ear: whether it's listening, the note it wants, Hz, and how far it has moved the tongue, octaves. */
        bool listening = false;
        float wanted = 440.0f, ear = 0.0f;
        /** How much harder than the note's own breath a bend is blown, and where that's going. */
        float breath = 1.0f, breathAim = 1.0f;
        int quietBlocks = 0;
        uint32_t age = 0;
    };

    float paramOf(int32_t i) const { return params_.get(i); }
    Voice *voiceFor(uint8_t note);
    /** Makes the voice's reeds for its note, each tuned to sound it and then apart. */
    void retune(Voice &v);
    /** The pressure a held voice is blown at, Pa. */
    float aimFor(const Voice &v) const;
    draw::ReedMake makeFor(const Voice &v) const;
    /** The bend a voice plays at: the channel's and its own finger's, semitones. */
    float bendOf(const Voice &v) const;
    /** Turns the words waiting for the next note into the mouth's steps. */
    void planWords();
    /** Places a pipe instrument's note: its pipes, a shō's chord. */
    void planPipes(Voice &v);
    /** Places a harmonica note: which hole and how, or a reed of its own. */
    void planHarp(Voice &v);
    /** Makes a harmonica voice's reeds and aims its mouth for the note and the pitch wheel. */
    void retuneHarp(Voice &v);

    Voice voices[kVoices];
    float sampleRate = 48000.0f;
    float bend = 0.0f, channelPressure_ = 0.0f;
    /** The body's filters: a one-pole low-pass and a resonance, per kind, on the sum. */
    float lowState = 0.0f;
    float bodyX1 = 0.0f, bodyX2 = 0.0f, bodyY1 = 0.0f, bodyY2 = 0.0f;
    float dcIn = 0.0f, dcOut = 0.0f;
    /** The hands cupped round a harp: a low-pass, its two states, its coefficients and the share that leaks past. */
    float cupLow = 0.0f, cupBand = 0.0f, cupG = 0.0f, cupK = 1.0f, cupLeak = 1.0f;
    /** Where the hands are, 0 open to 1 closed, how fast they're moving, and their tremble. */
    float hand = 0.0f, handSpeed = 0.0f, handNoise1 = 0.0f, handNoise2 = 0.0f;
    /** The mod wheel, 0 to 1; the throat's vibrato, its phase in turns, and this cycle's rate and depth. */
    float wheel = 0.0f, vibratoPhase = 0.0f, vibratoRate = 1.0f, vibratoDepth = 1.0f;
    int mouthCountdown = 0;
    /** The bellows shaken: where in a push and a pull they are, in turns, this one's speed, and which way they go. */
    float shakePhase = 0.0f, shakeRate = 1.0f;
    bool pulling = false;
    /** The bellows' pressure as many reeds draw on them, a share. */
    float sag = 1.0f;
    /** The song's clock, ticks, kept to the sample; and samples a tick. */
    double ticks = 0.0, samplesPerTick = 100.0;
    /** Another track's level, followed, for breath from a track. */
    float trackLevel = 0.0f;
    /** A harp player's words: the phones waiting, the mouth's steps for the note, and where it is in them. */
    static constexpr int kMaxPhones = 32, kSaid = 48;
    struct Said {
        float f1, f2;
        bool lips;
        int32_t samples; // -1 held
    };
    uint8_t pending[kMaxPhones] = {};
    int pendingCount = 0;
    Said said[kSaid];
    int saidCount = 0, saidAt = 0;
    int32_t saidLeft = 0;
    /** The formants the words have the mouth at now, Hz, and whether the lips are shut. */
    float talkF1 = 500.0f, talkF2 = 1500.0f, talkLips = 0.0f;
    bool talking = false;
    /** The words' formants on the way out: two band-passes, their states. */
    float talkLow1 = 0.0f, talkBand1 = 0.0f, talkLow2 = 0.0f, talkBand2 = 0.0f;
    uint32_t noise = 0x2545f491u, clock = 0;
    int quietSamples = 0;
    bool asleep = true;
};

} // namespace acidulous::machine
