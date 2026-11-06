#pragma once
#include <cstdint>
#include <vector>
#include <engine/dsp/DelayLine.h>
#include <engine/dsp/Filter.h>
#include <engine/machine/Machine.h>

namespace acidulous::machine {

/**
 * Divisi: the string ensemble.
 *
 * Twelve top oscillators, one per pitch class, divided down in octaves as the
 * old boxes did, so a note and its octaves are locked in phase. A key gates
 * its note into six registers, each with a level: violin and viola at the
 * note, cello an octave down, contrabass two, a choir (a pulse and saw mix
 * through three formants, the vowel knob moving them, each key levelled by how
 * much of its harmonic series the formants pass) and brass. Their filters are
 * fixed voicings that the tone knob moves together.
 *
 * Brass is a saw of its own per key, driven softly into a low-pass whose
 * cutoff follows the note's loudness and velocity: it opens a little after the
 * level, overshoots and settles, then falls slowly while the note is held, and
 * the pitch scoops up into the note. In poly mode each key has its own filter;
 * in paraphonic the keys share one, opened on the first key.
 *
 * In paraphonic mode one envelope swells the whole sound: a key joins the
 * sound as it is, and the release starts only when every key is up. In poly
 * mode each note has its own. After that an ensemble chorus (one delay line
 * read at two places a side, each moved by a slow and a fast LFO a third of a
 * turn apart, the right side moved the opposite way to the left) and a
 * phaser. The dry sound comes from the line's middle, 6 ms late.
 *
 * Tunings and a finger's own bend: a key follows its pitch class's divider
 * while it sounds the octave of it. A key whose tuning isn't an octave of its
 * pitch class, or that a finger bends, gets a divider chain of its own,
 * started from where the shared one was so it doesn't jump. The channel bend
 * moves the top oscillators, so every key bends together.
 *
 * Players: each register is a section of 1 to 16 players. A player is a tap
 * on the registers' mix, its delay moved by a slow random walk (its pitch
 * drifts by the delay's speed), a vibrato of its own phase and rate, and a
 * seat delay that spreads the players' entries (scatter, re-dealt on a note
 * from silence). Player n moves the same way in every section, so the
 * registers stay locked to each other as the dividers made them and a patch
 * keeps its level, but sits at its own place across each section's width:
 * the mix is kept twice, as is and weighted by each section's width, and a
 * player's place is how much of the second it adds on one side and takes
 * off the other. One player with no drift or vibrato reads the newest
 * sample, so it sounds as the machine did before players. The paraphonic
 * envelope and the volume come after the players.
 *
 * The stage: each section has a seat on a stage, scaled by the stage knob
 * from all in the middle to spread out, violins front left, cellos right,
 * basses back right, brass back left of middle and the choir at the back.
 * Before the players each section goes into a line of its own and is read
 * late by its distance, panned by its direction, made quieter and duller
 * the further back it sits, and run through a chorus of its own (the same
 * shape as the ensemble, its LFOs its own), which takes over from the shared
 * ensemble as the stage opens so the delay through the machine stays the
 * same. The room knob adds two reflections a section off the side walls,
 * worked out from its seat, and sends it into a short dark ambience, the
 * back rows more. The players then read the stage's left and right as well
 * as the width-weighted mix. With stage and room at 0 the stage is skipped
 * and the machine sounds as it did before.
 *
 * The split knob gives each section its own part of the keyboard, set per
 * note as it's played, so held notes never change; at 0 every section plays
 * every note as before.
 *
 * Motion: a slow shape gives each section a value from -1 to 1 once a
 * block (drift, a seeded curve each; swell, all together, with a level for
 * the whole ensemble; tide, one wave in seat order; steps, a seeded balance
 * a step, glided in). Its depths turn that into each section's gain (the
 * sections kept as loud together), filter brightness and chorus depth (kept
 * as loud), and the choir's vowel. Gains and depths are ramped across the
 * block; filters and the vowel move a block at a time, as their knobs do.
 * Synced, the position comes from the song's tick (onSongTick()), as the
 * inserts' mods have it, so a bar
 * always sounds the same; the clock runs at the tempo stopped too. Free,
 * or outside an engine, it runs from reset(). Either way renders repeat. A
 * jump (a new shape, a locate, play starting) glides out over a quarter
 * second unless the machine was silent. Off, nothing is touched and the
 * machine sounds as it did before.
 *
 * Age turns the choruses (the shared ensemble and the sections' own alike)
 * from clean to an old bucket-brigade box. Their wet sound, the sections'
 * summed a side, is squeezed 2:1 by a slow follower, picks up hiss and,
 * past half way, a faint whine at an eighth of the delay's clock (taken as
 * 1024 stages, so a longer delay whines lower), is soft clipped against the
 * level it reaches, opened back out 1:2 by a follower of its own, so the
 * hiss breathes with the sound and the tails pump a little, and loses its
 * highs to a low-pass with a small hump, open at 0 and about 3.5 kHz at 1.
 * The squeeze and its opening are worked out every 16 samples and ramped.
 * Hiss and whine follow the sound in, so a quiet machine still sleeps. A
 * seeded slow wander and a warble move every wet read, the same on both
 * sides; they turn while asleep, as the LFOs do. At 0 none of it runs and
 * the machine sounds as it did before.
 */
class Divisi final : public Machine {
  public:
    enum P : int32_t {
        Violin = 0, Viola, Cello, Contrabass, Choir, Vowel, Brass, Sweep, BrassDecay, Blat,
        Tone, Mode, Attack, Release,
        Ensemble, Speed, Phaser, Rate, Feedback,
        Tune, Octave, BendRange, VelocityAmount, Volume,
        Players, Drift, Vibrato, Vibrate, Scatter,
        Stage, Room, Split,
        Motion, MotionRate, MotionSync, MotionBalance, MotionVowel, MotionTone, MotionEnsemble,
        Age,
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate. Pitch
        // is 24 semitones per unit.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        Count = MacroBase + 8 * MacroParams
    };
    // Where a macro route goes. Continuous params only: the mode, tune,
    // octave and bend range are left out.
    enum MacroDest : int32_t {
        McOff, McViolin, McViola, McCello, McContrabass, McChoir, McVowel, McBrass, McSweep, McBrassDecay, McBlat,
        McTone, McAttack, McRelease, McEnsemble, McSpeed, McPhaser, McRate, McFeedback,
        McVelocity, McVolume, McPitch,
        McMotionBalance, McMotionVowel, McMotionTone, McMotionEnsemble, McMotionRate,
        McAge,
        McPlayers, McDrift, McVibrato, McVibrate, McScatter, McStage, McRoom, McSplit,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
    static_assert(Count <= kMaxParams, "too many parameters");

    enum EnvelopeMode : int32_t { Paraphonic = 0, Poly, ModeCount };
    enum MotionShape : int32_t { MotionOff = 0, MotionDrift, MotionSwell, MotionTide, MotionSteps, MotionShapeCount };
    /** The synced lengths after off, in quarter notes: 1/4, 1/2, then 1 to 16 bars of 4/4. */
    static constexpr int kMotionSyncs = 7;
    static constexpr int kKeys = 24;
    static constexpr int kMaxPlayers = 16;

    Divisi();

    const char *typeName() const override { return "Divisi"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void pitchBend(int16_t value14) override;
    void channelPressure(uint8_t value) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void onSongTick(double tick) override {
        songClock = tick;
        inSong = true;
    }

    /** Keys sounding, for tests. */
    int activeKeys() const;
    /** Keys sounding on a divider chain of their own, for tests. */
    int ownChains() const;
    /** Section [s]'s motion at the end of the last block, -1 to 1, or the swell's level (6), -1 to 0, for tests. */
    float motionOf(int s) const { return (s >= 0 && s < kMotionChannels) ? motionShown[s] : 0.0f; }

  private:
    /** A rising-and-falling level: a straight attack to full and a release that falls 100 dB in its time. */
    struct Env {
        float level = 0.0f;
        bool open = false;
        void step(float rise, float fall) {
            if (open) {
                level += rise;
                if (level > 1.0f) level = 1.0f;
            } else if (level > 0.0f) {
                level *= fall;
                if (level < 1e-5f) level = 0.0f;
            }
        }
        bool idle() const { return !open && level == 0.0f; }
    };

    /** The brass filter's brightness: loudness followed a little late, the overshoot, and the slow fall. */
    struct Bright {
        /** Loudness followed; the overshoot's fast and slow halves; how much of the fall is still to come, 1 to 0. */
        float y = 0.0f, fast = 0.0f, slow = 0.0f, settle = 0.0f;
        void start() { fast = slow = settle = 1.0f; }
    };
    /** Per-block coefficients for Bright, stepped every kBrassEvery samples. */
    struct BrightCoefs {
        float up = 0.0f, down = 0.0f, fast = 0.0f, slow = 0.0f, settle = 0.0f, over = 0.0f;
    };
    /** Moves [b] one coefficient step toward [target] and returns its brightness, 0 to about 1.3. */
    static float stepBright(Bright &b, float target, float level, const BrightCoefs &c);
    /** A low-pass for one key's brass, the Svf's low-pass with the coefficients set from outside. */
    struct BrassLp {
        float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        void set(float g, float k) {
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        float run(float v0) {
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = dsp::undenormal(2.0f * v1 - ic1);
            ic2 = dsp::undenormal(2.0f * v2 - ic2);
            return v2;
        }
    };

    struct Key {
        bool used = false, held = false;
        uint8_t note = 0;
        /** The note it sounds after the octave knob, 0 to 127; its pitch class and octave. */
        int sounding = 60, pc = 0, octave = 5;
        /** Its level from velocity, and the velocity, 0 to 1. */
        float gain = 0.0f, vel = 0.0f;
        /** The key's own switch into the registers (paraphonic): rises in a few ms, falls in a few tens. */
        float gate = 0.0f;
        bool gateOn = false;
        /** Its own envelope (poly). */
        Env env;
        /** A divider chain of its own: the 32' phase and step, a fraction of 2^32. */
        bool own = false;
        uint32_t ownPhase = 0, ownStep = 0;
        /** The finger's own bend, semitones, and its pressure (-1 for the channel's), and that glided. */
        float noteBend = 0.0f, pressure = -1.0f, pressed = 0.0f;
        /** The pressure's swell this block and last, for a ramp across the block. */
        float swell = 1.0f;
        float level = 0.0f;
        /** Its choir level against the formants, this block; 0 before its first. */
        float choirComp = 0.0f;
        uint32_t age = 0;
        /** Its brass saw's 16' phase, free of the divider so it can scoop; the scoop, semitones below the note. */
        bool brassOn = false;
        uint32_t brassPhase = 0;
        float scoop = 0.0f;
        /** Its brightness, filter, drive and the samples to its next coefficient step (poly). */
        Bright bright;
        BrassLp brassLp;
        float drive = 1.0f, invDrive = 1.0f;
        int brassCountdown = 0;
        /** How much of it each section plays, fixed at note-on; parted once any is not 1. */
        float part[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        bool parted = false;
    };

    /** One player of a section: a tap on the section's line. */
    struct Player {
        /** Its random walk: its own generator, the target, two smoothing stages (-1 to 1) and the chunks to the next target. */
        uint32_t rng = 1;
        float target = 0.0f, y1 = 0.0f, y2 = 0.0f;
        int32_t nextPick = 0;
        /** The drift's delay excursion, seconds, either side of the middle of its reach. */
        float x = 0.0f;
        /** Its vibrato's phase, turns, and its rate against the knob. */
        float vibPhase = 0.0f, vibRate = 1.0f;
        /** Its place in the scatter (0 to 1, the first player 0) and in the seating order (0 to 1). */
        float seat = 0.0f, order = 0.0f;
        /** The scatter delay now, samples, which glides to its place; the whole delay at the end of the last chunk. */
        float seatDelay = 0.0f, delay = 0.0f;
        /** Its gains on the mix and on the width-weighted mix now, and its place across a section, -1 to 1. */
        float mid = 0.0f, side = 0.0f, pos = 0.0f;
    };
    /** Deals every player's walk, vibrato and seat from fixed seeds. */
    void seatPlayers();
    /**
     * Runs the players over [n] samples of the registers' mix [mid] (the
     * stage's left when [stereo]), its right [right] and its width-weighted
     * twin [wide], into [outL] and [outR]. On the stage a player's place
     * tips the balance by [width], [tilt0] to [tilt1] of the way from the
     * width-weighted mix.
     */
    void runPlayers(const float *mid, const float *right, const float *wide, bool stereo, float tilt0, float tilt1,
                    float width, float *outL, float *outR, int32_t n);

    static constexpr int kSections = 6;
    static constexpr int kReflections = 2;
    /** Where a section sits for this block: its delays, samples, and its gains. */
    struct SeatAt {
        /** The direct sound's delay, the chorus's reach either side of it, and the reflections' delays. */
        float delay = 0.0f, reach = 0.0f, erDelay[kReflections] = {};
        /** The direct sound's gains left and right and into the width-weighted mix, and how much of its air loss is heard. */
        float gL = 1.0f, gR = 1.0f, gW = 0.0f, air = 0.0f;
        float erL[kReflections] = {}, erR[kReflections] = {};
        /** What it sends into the ambience, left and right. */
        float sendL = 0.0f, sendR = 0.0f;
    };
    /** A section's seat: its chorus's LFO phases and rates, its air filter, and what it was set to at the end of the last chunk. */
    struct Seat {
        float slowPhase = 0.0f, fastPhase = 0.0f, slowRate = 1.0f, fastRate = 1.0f;
        float airState = 0.0f;
        /** Samples its line has heard only silence. */
        int32_t quiet = 0;
        SeatAt at;
    };
    /**
     * Works out every section's seat for [stage] and [room] into [goal], with
     * the sections' choruses [k] of the way in at ensemble depth [ens], and
     * the gain that keeps the level of the sections at [lv] as they were in
     * the middle.
     */
    /** How much of [note] each section plays, split [split] of the way into its own range. */
    void partsOf(int note, float split, float part[kSections]) const;
    void placeSeats(float stage, float room, float k, float ens, const float lv[kSections], SeatAt goal[kSections],
                    float &norm) const;
    /**
     * Writes the sections' [n] samples [sec] into their lines and, when
     * [staged], reads them back from their seats into [outL], [outR] and the
     * width-weighted [outW]. Each section's chorus is [ens0] to [ens1] deep
     * across the chunk, its direct sound [keep0] to [keep1] as loud. When
     * [aging], the choruses' reads move by [wow] samples and their wet sound,
     * summed a side, goes through the aged bucket, its whine stepping by
     * [clockL] and [clockR].
     */
    void runStage(float sec[][64], bool staged, const SeatAt goal[kSections], const float ens0[kSections],
                  const float ens1[kSections], const float keep0[kSections], const float keep1[kSections], float ensMove,
                  float slowStep, float fastStep, float norm0, float norm1, float *outL, float *outR, float *outW, int32_t n,
                  const float *wow, const float *clockL, const float *clockR, bool aging);

    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return val(p, false); }
    /** Param [p]'s target with the macros' targets added, for note-on seeds. */
    float goalOf(int32_t p) const { return val(p, true); }
    float val(int32_t p, bool goal) const;
    void updateMacros();
    void updateMacroGoals();
    float read(int32_t p, bool goal) const { return goal ? params_.target(p) : params_.get(p); }

    Key *keyFor(uint8_t note);
    /** Semitones every key is moved by now: tune, the channel bend and the pitch macro. */
    float offset() const;
    /** Sets the top oscillators' steps, and each key's chain, for this block. */
    void tuneChains();
    /** Puts every filter, line and phase back as a reset leaves them. */
    void clearState();
    int heldCount() const;
    /** The formants' centres, Hz, their filters' k (one over Q) and levels, at [vowel], 0 to 1. */
    static void formantsAt(float vowel, float hz[3], float k[3], float level[3]);
    /** The choir's power for [note] at vowel row [row], through the formants as heard. */
    float choirPower(int row, int note) const;
    /** A key's choir level, so every note comes out of the formants about as loud as C4's aah. */
    float compFor(float vowel, int note) const;

    Key keys[kKeys];
    float sampleRate = 48000.0f;
    float bend = 0.0f, channelPressure_ = 0.0f;
    /** The twelve dividers' lowest stage: a phase and a step per pitch class, a fraction of 2^32. */
    uint32_t topPhase[12] = {}, topStep[12] = {};
    /** That stage's frequency, Hz. */
    float topHz[12] = {};
    /** The shared envelope, and the shared brass brightness and drive (paraphonic). */
    Env shared;
    Bright sharedBright;
    float sharedDrive = 1.0f;
    /** The registers' voicings. */
    dsp::Svf violinHigh, violinLow, violaHigh, violaLow, celloHigh, celloLow, bassLow;
    dsp::Svf choirHigh, choirLow, formant[3];
    dsp::Svf brassLow;
    int brassCountdown = 0;
    /**
     * The choir's per-note levels at each vowel row, all worked out in
     * prepare(), since they depend only on the sample rate.
     */
    static constexpr int kVowelRows = 17;
    float compCache[kVowelRows][128] = {};
    /**
     * The players: their state, their two lines (one buffer, the mix's
     * power-of-two line then the wide one's, written together), the write
     * place, the samples the lines have heard only silence, and this
     * block's player settings.
     */
    Player players_[kMaxPlayers];
    std::vector<float> playerLines;
    int32_t lineSize = 0, lineMask = 0, lineWrite = 0, lineQuiet = 0;
    uint32_t dealRng = 1;
    bool snapPlayers = true;
    int playerCount = 1, seatedFor = 0;
    float driftRatio = 0.0f, vibRatio = 0.0f, vibHz = 5.0f, scatterSamples = 0.0f, playerNorm = 1.0f;
    /** The sections' seats and lines (one buffer, a power-of-two line each), and the write place. */
    Seat seats_[kSections];
    std::vector<float> seatLines;
    int32_t seatSize = 0, seatMask = 0, seatWrite = 0;
    bool snapSeats = true;
    /** The stage and room knobs glided, so a seat's delays move slowly; the chorus's share of the ensemble; the level kept. */
    float stageNow = 0.0f, roomNow = 0.0f, handNow = 0.0f, stageNorm = 1.0f;
    /** The last seats worked out, and the stage, room, chorus share and section levels they were for. */
    SeatAt placed[kSections];
    float placedFor[4 + kSections] = {-1.0f};
    float placedNorm = 1.0f;
    /**
     * The stage's ambience: a short, dark diffuse sound from what the
     * sections send it. Two all-passes a side smear the input, then four
     * lines fed back through a mixing matrix, each through a low-pass.
     */
    struct Ambience {
        std::vector<float> lines, diffusers;
        int32_t lineSize = 0, lineMask = 0, apSize = 0, apMask = 0, write = 0;
        int32_t len[4] = {}, apLen[4] = {};
        float back[4] = {}, damp[4] = {};
        float dampCoef = 0.0f, out = 0.0f;
        /** Samples since it last heard anything or rang above a whisper. */
        int32_t quiet = 0;
        void prepare(float rate);
        void clear();
        bool ringing() const { return quiet <= lineSize * 4; }
        /** Adds the ambience of [n] samples of [inL] and [inR] to [outL] and [outR]. */
        void run(const float *inL, const float *inR, float *outL, float *outR, int32_t n);
    };
    Ambience ambience;
    /** The reflections' shared air loss, a side. */
    float erAir[2] = {};
    /** The ensemble: a line a side read at its middle and two places, and its two LFOs' phases, turns. */
    dsp::DelayLine line, lineR;
    float slowPhase = 0.0f, fastPhase = 0.0f;
    /** The phaser: four all-pass stages a side, the fed-back sample, and its LFO's phase. */
    float apState[2][4] = {};
    float apBack[2] = {};
    float apCoef[2] = {};
    float phaserPhase = 0.0f;
    int phaserCountdown = 0;
    uint32_t clock = 0;
    int32_t quietSamples = 0;
    /** Samples since the players' lines, and the stage's, last heard more than silence. */
    int32_t lineHeard = 0, stageHeard = 0;
    /** True when every player and seat has read back past the last sound its line heard. */
    bool linesSpent() const;
    bool asleep = true;
    /**
     * Motion: each section's value of the shape at the end of the last
     * block, -1 to 1, and after them the whole ensemble's level (the swell's,
     * -1 to 0); what a jump left to glide out; the position running from
     * reset(), in periods.
     */
    static constexpr int kMotionChannels = kSections + 1;
    float motionShown[kMotionChannels] = {}, motionGlide[kMotionChannels] = {};
    double motionPos = 0.0;
    int motionShapeWas = -1, motionSyncWas = -1;
    bool motionLocked = false, motionRestart = false, sleptLast = true;
    /** The song's tick where the next render starts, and where the last ended; whether an engine gives one; the tempo. */
    double songClock = 0.0, lockedEnd = 0.0;
    bool inSong = false;
    float tempo = 120.0f;
    /** What motion did at the end of the last block: section gains, chorus depths and their level, and the shared chorus's. */
    bool motionWasLive = false;
    float motionGain[kSections] = {}, motionEns[kSections] = {}, motionEnsKeep[kSections] = {};
    float motionEnsShared = 0.0f, motionEnsSharedKeep = 1.0f;
    /** Moves the motion on by [frames]: the position, jumps, the glide; true when it moves anything. */
    bool stepMotion(int32_t frames);
    /** The shape's value for each section at [pos] periods. */
    static void shapeAt(int shape, double pos, float out[kMotionChannels]);
    float mac_[MacroDestCount]{}, macGoal_[MacroDestCount]{};

    /** Age's settings for one block: the squeeze's power, its level, and what the bucket adds. */
    struct AgeAt {
        /** The squeeze's power on the level (0.5 at 2:1), the opening's, the level it's centred on and its floor. */
        float squeeze = 0.0f, open = 0.0f, nominal = 1.0f, floor = 0.0f;
        /** The followers' coefficients up and down, and the hiss's and whine's gate per unit of the input's level. */
        float up = 0.0f, down = 0.0f, gate = 0.0f;
        /** The hiss's and whine's sizes; the clip's share, where it bends and the gain that keeps the nominal level. */
        float hiss = 0.0f, whine = 0.0f, clip = 0.0f, bendAt = 1.0f, keep = 1.0f;
        /** The low-pass's coefficients. */
        float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 0.0f;
    };
    /**
     * One side of an aged chorus: the followers, the gains now and their steps, the low-pass and the whine's phase.
     * [entering] is set while age is off, so turning it on mid-sound starts with the gains cancelling and the
     * low-pass settled on the sound, rather than from silence.
     */
    struct Bucket {
        float in = 0.0f, out = 0.0f, gIn = 1.0f, gOut = 1.0f, dIn = 0.0f, dOut = 0.0f;
        float ic1 = 0.0f, ic2 = 0.0f, whine = 0.0f;
        int32_t count = 0;
        bool entering = false;
    };
    /** An aged chorus's two sides and its hiss's generator. */
    struct Aged {
        Bucket side[2];
        uint32_t noise = 1;
        void clear(uint32_t seed, bool entering = false) {
            side[0] = side[1] = Bucket();
            side[0].entering = side[1].entering = entering;
            noise = seed;
        }
    };
    /** Runs [x], one sample of side [s]'s wet sound, through [a]; its whine moves on [whineStep] turns. */
    static float runAged(Aged &a, int s, const AgeAt &at, float x, float whineStep);
    /** Sets [at] for [age] with its level centred on [nominal], its hiss and whine scaled by [noise]. */
    void ageSettings(float age, float nominal, float noise, AgeAt &at) const;
    /** The shared ensemble's and the sections' aged choruses, and this block's settings for each. */
    Aged agedShared, agedStage;
    AgeAt ageShared, ageStage;
    /** Age's wander: its generator, target, two smoothing stages and samples to the next target; the warble's phase. */
    uint32_t wowRng = 1;
    float wowTarget = 0.0f, wowY1 = 0.0f, wowY2 = 0.0f, warblePhase = 0.0f;
    int32_t wowNext = 0;
    /** Moves the wander on by [n] samples, returning its start and end, -1 to 1. */
    void stepWow(int32_t n, float &from, float &to);
};

} // namespace acidulous::machine
