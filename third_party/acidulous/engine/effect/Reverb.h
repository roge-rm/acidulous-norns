#pragma once
#include <cstdint>
#include <vector>
#include <engine/dsp/DelayLine.h>
#include <engine/effect/Effect.h>

namespace acidulous::effect {

/**
 * Reverb, in several models picked by `model`:
 *
 *   - room: eight combs and four allpasses a side, the original reverb.
 *   - plate: a figure-eight tank. The input goes through four diffusing
 *     allpasses into two loops that feed each other, each with a slowly
 *     modulated allpass, a damping filter and a second allpass. Bright and
 *     dense.
 *   - hall: eight delay lines mixed through a Householder matrix, with lows
 *     lasting longer than mids and highs, slow modulation on every line and
 *     early reflections in front.
 *   - spring: three springs, each a loop through a chain of stretched
 *     allpasses that delays the highs more than the lows, so every echo comes
 *     back as a chirp, the drip. `drip` sets how far the chirp spreads and
 *     `knock` how hard a sharp hit shakes the tank: a low thump and a burst
 *     of noise into the springs.
 *   - early: four short lines and two allpasses run at `rate` (8 to 32 kHz),
 *     fed by picking samples and held on the way out with no filtering, so the
 *     tail is grainy and metallic.
 *   - space: a room from its size. Early reflections by the image method from
 *     `width`, `depth`, `height`, where you stand (`position`) and the walls'
 *     `material`, and a tail whose decay comes from the room's volume and
 *     absorption. The taps glide when the room changes.
 *   - tube: a tuned pipe the track rings through. Closed at one end, so it
 *     rings in odd harmonics; `open` blends to a pipe open at both ends, with
 *     every harmonic. A low-pass for the bore and a soft clip in the loop, so
 *     loud hits growl.
 *   - string: a tuned string the track rings through, with a low-pass and a
 *     chain of allpasses in the loop. `stiff` pulls the overtones sharp, from
 *     a string to a metal wire, and `pick` is where the input lands on it.
 *
 *     Both are tuned by `pitch`, in semitones from middle C, or from the
 *     track's last note when `follow` is on. Two lines a hair apart, one a
 *     side, at full quality; one at lean.
 *   - drumhead: a struck skin, a square mesh of junctions passing waves to
 *     their neighbours, so it rings in a drum's crowded, out of tune modes.
 *     `size` is the drum, bigger for lower and longer, and `tension` tunes the
 *     head. `snares` puts wires under it: when the head moves past them they
 *     buzz, so a kick through it sets the snare off and a quiet note doesn't.
 *     The mesh ticks at a rate set by the pitch, so a small mesh rings low.
 *   - thunder: a metal sheet, a bank of modes from low to high in four bands.
 *     The louder a band rings, the faster its energy spills into the band
 *     above, so a hard hit hums first and blooms into a crash after it, and a
 *     soft one only hums. `metal` is how readily it spills, from a heavy gong
 *     to a thin sheet; `size` lowers the sheet and lengthens its ring.
 *   - howl: a room with a microphone in feedback. Three loops for the room's
 *     length, width and height, each through the speaker's peak and a
 *     limiter. Turning `howl` up lets the modes near the peak ring longer,
 *     then sing and hold, so any input turns into drones in the room's tuning.
 *     `size` retunes them.
 *   - flutter: two parallel walls, the echo bouncing between them heard as a
 *     pitch. `walls` is how far apart they are, from a quarter metre (a high
 *     ring) to twenty (a slow flutter).
 *
 * Shared by every model:
 *
 *   - Shimmer: the tail is fed back an octave up, so each pass climbs into a
 *     rising chord. The pitch shift is a delay line read at double speed with
 *     two crossfaded taps, and it sits inside the feedback so it builds up.
 *   - Bits and crush: only the tail goes through a bit quantiser and a
 *     sample-and-hold, so the dry stays clean.
 *   - Wobble: the tank's delay lengths drift slowly and out of step, like tape.
 *   - Freeze holds the tail forever and gate cuts it off.
 *   - Duck: the tail drops while the track plays and comes back over `duckrel`.
 *   - Distance: near to far. Longer pre-delay, quieter and later early
 *     reflections, more wet against the dry, air loss on the tail, a narrower
 *     dry and a wider tail.
 *   - Bloom: the tail starts thin and fills in over up to two seconds of
 *     input, and falls back when the input stops.
 */
class Reverb final : public Effect {
  public:
    // Appended after Gain, so old songs, patches and lanes keep their indices.
    enum P { Size, Damp, Tone, PreDelay, Mix, Freeze, Gate, Shimmer, Bits, Crush, Wobble, Gain,
             Model, Duck, DuckRel, Distance, Bloom,
             Drip, Knock, Rate, Width, Depth, Height, Position, Material,
             Pitch, Follow, Open, Stiff, Pick,
             Tension, Snares, Metal, Howl, Walls, Count };
    enum ModelKind { Room, Plate, Hall, Spring, Early, Space, Tube, String, Drumhead, Thunder, HowlRoom, Flutter, kModels };
    Reverb() { initParams(); }
    const char *typeName() const override { return "Reverb"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    bool process(float *L, float *R, int32_t frames, bool stereoIn) override;
    /**
     * The room's geometry rebuilds the space's taps, which glide by themselves;
     * drip changes the spring's stage count and rate the early tank's lengths;
     * pitch and stiff retune the tube's and string's loops; metal rebuilds the sheet's 48 modes.
     */
    bool glides(int32_t index) const override {
        return index != Width && index != Depth && index != Height && index != Position && index != Material &&
               index != Drip && index != Rate && index != Pitch && index != Stiff && index != Metal;
    }
    /** The track's last note, for `follow`. */
    void onNote(int32_t note, float hz) override { heardNote = note; heardHz = hz; }

    /** A plain circular buffer: whole-sample reads and wrapped fractional reads. */
    struct Line {
        std::vector<float> buf;
        int32_t size = 4, wr = 0;
        void prepare(int32_t n);
        void clear();
        void write(float v) { buf[static_cast<size_t>(wr)] = v; if (++wr >= size) wr = 0; }
        /** `d` samples back, 1 <= d <= size. */
        float at(int32_t d) const {
            int32_t i = wr - d;
            if (i < 0) i += size;
            return buf[static_cast<size_t>(i)];
        }
        float frac(float d) const;
    };

  private:
    /** Per block: everything the per-sample code reads. */
    struct Block;
    template <typename Tank> void run(float *L, float *R, int32_t frames, bool stereoIn, const Block &b, Tank &&tank);
    void roomSample(const Block &b, const float dry[2], float out[2]);
    void plateSample(const Block &b, const float dry[2], float out[2]);
    void hallSample(const Block &b, const float dry[2], float out[2]);
    void springSample(const Block &b, const float dry[2], float out[2]);
    void earlySample(const Block &b, const float dry[2], float out[2]);
    void spaceSample(const Block &b, const float dry[2], float out[2]);
    void tubeSample(const Block &b, const float dry[2], float out[2]);
    void stringSample(const Block &b, const float dry[2], float out[2]);
    void drumSample(const Block &b, const float dry[2], float out[2]);
    void thunderSample(const Block &b, const float dry[2], float out[2]);
    void howlSample(const Block &b, const float dry[2], float out[2]);
    void flutterSample(const Block &b, const float dry[2], float out[2]);
    void drumTick(const Block &b);
    float lofi(int c, float t, const Block &b);
    void shimmerStep(int c, float t, const Block &b);
    void clearRoom();
    void clearPlate();
    void clearHall();
    void clearSpring();
    void clearEarly();
    void clearSpace();
    void clearWave();
    void clearDrum();
    void clearThunder();
    void clearHowl();
    void clearFlutter();
    void updatePlate(float size, float damp, bool freeze);
    void updateHall(float size, float damp, bool freeze);
    void updateSpring(float size, float damp, float drip, bool freeze, bool full);
    void updateEarly(float size, float damp, float rate, bool freeze);
    void updateSpace(float damp, bool freeze, bool full, int32_t frames);
    void updateWave(bool string, float size, float damp, bool freeze, bool full);
    void updateDrum(float size, float damp, bool freeze, bool full);
    void updateThunder(float size, float damp, bool freeze, bool full);
    void updateHowl(float size, float damp, bool freeze);
    void updateFlutter(float size, float damp, bool freeze);

    float sr = 48000.0f;

    // --- room ---
    struct Comb { dsp::DelayLine line; float store = 0.0f; int32_t len = 1; };
    struct Allpass { dsp::DelayLine line; int32_t len = 1; };
    /** Eight combs and four allpasses a side at full quality, half at lean. */
    static constexpr int32_t kCombs = 8;
    static constexpr int32_t kAps = 4;
    Comb combs[2][kCombs];
    Allpass aps[2][kAps];
    float wobblePhase = 0.0f;

    // --- shared ---
    dsp::DelayLine pre[2], preLong[2];
    /** The shimmer's line: the tail, read back an octave up. */
    dsp::DelayLine shimmerLine[2];
    float shimmerPhase[2]{}, shimmerFb[2]{};
    /** The shimmer path's DC blocker. See the note where it's used. */
    float shimDcX[2]{}, shimDcY[2]{}, shimLp[2]{};
    float crushAcc[2]{}, crushHeld[2]{};
    float lp[2]{}, gateEnv = 0.0f, inputEnv = 0.0f;
    int32_t gateHold = 0;
    float duckEnv = 0.0f, bloomDet = 0.0f, bloomEnv = 0.0f, airLp[2]{};
    int32_t lastModel = -1;
    bool lastFull = true;

    // --- plate ---
    static constexpr int32_t kPlateDiff = 4;
    Line plDiff[kPlateDiff], plAp1[2], plD1[2], plAp2[2], plD2[2];
    float plDamp[2]{}, plCross[2]{};
    float plOsc[2][2]{};
    float plRot[2][2]{};
    // Lengths and gains for the current size, damp and freeze.
    float plSizeWas = -1.0f, plDampWas = -1.0f;
    bool plFreezeWas = false;
    int32_t plLenDiff[kPlateDiff]{}, plLenD1[2]{}, plLenAp2[2]{}, plLenD2[2]{};
    float plLenAp1[2]{}, plGain = 0.0f, plDampCoeff = 1.0f, plScale = 1.0f;
    int32_t plTapL[7]{}, plTapR[7]{};

    // --- hall ---
    static constexpr int32_t kHallLines = 8;
    static constexpr int32_t kHallEr = 8;
    Line haLine[kHallLines], haDiff[2][2];
    float haLow[kHallLines]{}, haHigh[kHallLines]{};
    float haOsc[kHallLines][2]{};
    float haRot[kHallLines][2]{};
    float haSizeWas = -1.0f, haDampWas = -1.0f;
    bool haFreezeWas = false;
    float haLen[kHallLines]{}, haGLow[kHallLines]{}, haGMid[kHallLines]{}, haGHigh[kHallLines]{};
    int32_t haLenDiff[2][2]{};
    float haScale = 1.0f, haLowCoeff = 0.0f, haHighCoeff = 0.0f;

    // --- spring ---
    // Three springs in lanes of four, so the chain's inner loop is one vector.
    static constexpr int32_t kSprings = 3;
    static constexpr int32_t kSpringStages = 48;
    static constexpr int32_t kSpringSlots = 16;
    /** Each stage's state, K samples deep, for every spring. */
    float spChain[kSpringStages][kSpringSlots][4]{};
    int32_t spIdx = 0, spK = 1, spStages = kSpringStages;
    Line spLine[kSprings];
    float spLp[4]{}, spOsc[kSprings][2]{}, spRot[kSprings][2]{};
    float spHpX = 0.0f, spHpY = 0.0f, spInLp = 0.0f;
    float spSizeWas = -1.0f, spDampWas = -1.0f, spDripWas = -1.0f;
    bool spFreezeWas = false, spFullWas = true;
    float spA[4]{}, spG[4]{}, spLen[kSprings]{}, spLpCoeff = 1.0f, spHpCoeff = 0.0f, spInCoeff = 1.0f;
    // Knock: the transient detector, the thump and the burst.
    float knFast = 0.0f, knSlow = 0.0f, thEnv = 0.0f, thAmp = 0.0f, thPhase = 0.0f, thHz = 0.0f, burstEnv = 0.0f;
    int32_t knHold = 0, knCapture = 0;
    uint32_t noiseState = 1;

    // --- early ---
    static constexpr int32_t kEarlyLines = 4;
    Line eaLine[kEarlyLines], eaDiff[2][2];
    float eaLp[kEarlyLines]{}, eaOsc[kEarlyLines][2]{}, eaRot[kEarlyLines][2]{};
    float eaAcc = 0.0f, eaHeld[2]{}, eaSum[2]{};
    int32_t eaCount = 0, eaLen[kEarlyLines]{}, eaLenDiff[2][2]{}, eaWobble = 0;
    float eaG[kEarlyLines]{}, eaDampCoeff = 1.0f, eaInc = 1.0f;
    float eaSizeWas = -1.0f, eaDampWas = -1.0f, eaRateWas = -1.0f;
    bool eaFreezeWas = false;

    // --- space ---
    static constexpr int32_t kSpaceLines = 8;
    /** Images up to the third reflection, less the direct path. */
    static constexpr int32_t kSpaceTaps = 62;
    Line spcIn, spcLine[kSpaceLines];
    float spcLow[kSpaceLines]{}, spcHigh[kSpaceLines]{}, spcOsc[kSpaceLines][2]{}, spcRot[kSpaceLines][2]{};
    float spcLen[kSpaceLines]{}, spcLenMid[kSpaceLines]{}, spcLenTo[kSpaceLines]{}, spcLenStep[kSpaceLines]{};
    float spcGLow[kSpaceLines]{}, spcGMid[kSpaceLines]{}, spcGHigh[kSpaceLines]{};
    // Taps a side: delay and gain now, where they're gliding to and the step per sample.
    float tapD[2][kSpaceTaps]{}, tapG[2][kSpaceTaps]{}, tapDTo[2][kSpaceTaps]{}, tapGTo[2][kSpaceTaps]{};
    int32_t tapO[2][kSpaceTaps]{};
    float tapF[2][kSpaceTaps]{}, tapDStep[2][kSpaceTaps]{}, tapGStep[2][kSpaceTaps]{};
    // The glide's block-level state in double, so it reaches the target: a float
    // step this small rounds away. tapD and tapG restart from these each block.
    double tapDMid[2][kSpaceTaps]{}, tapDAt[2][kSpaceTaps]{}, tapGAt[2][kSpaceTaps]{};
    int32_t spcTaps = kSpaceTaps;
    bool spcSnap = true, spcMoving = false;
    float spcGeomWas[5]{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    bool spcFullWas = true;
    float spcErLp[2]{}, spcErCoeff = 1.0f, spcRt = 1.0f, spcRtHigh = 1.0f, spcRtLow = 1.0f;

    // --- tube and string ---
    static constexpr int32_t kWaveLines = 2;
    static constexpr int32_t kWaveStages = 8;
    /** A read off a line with a first-order allpass for the fraction: `m` whole samples back, then the allpass. */
    struct WaveTap {
        float y = 0.0f;
        float read(const Line &l, float d);
    };
    Line wgLine[kWaveLines], wgPick[kWaveLines];
    WaveTap wgTap[kWaveLines][2];
    float wgLp[kWaveLines]{}, wgChain[kWaveLines][kWaveStages]{}, wgHpX[kWaveLines]{}, wgHpY[kWaveLines]{};
    float wgOsc[kWaveLines][2]{}, wgRot[kWaveLines][2]{};
    // Per line: the taps' delays with the filters taken off (the whole loop
    // and, for the tube, half of it), the gains, the pick's delay.
    float wgD[kWaveLines][2]{}, wgG[kWaveLines][2]{}, wgPickD[kWaveLines]{}, wgPeriod[kWaveLines]{};
    float wgLpCoeff = 1.0f, wgA = 0.0f, wgHpCoeff = 0.0f, wgHz = 261.6f, wgGLive = 0.0f;
    float wgWas[8]{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    /** The track's last note-on, or -1 for none since reset(). */
    int32_t heardNote = -1;
    float heardHz = 440.0f;

    // --- drumhead ---
    /** Junctions a side at full quality and at lean. */
    static constexpr int32_t kMeshFull = 12, kMeshLean = 8;
    static constexpr int32_t kMeshCells = kMeshFull * kMeshFull;
    /** The waves coming into each junction from the north, east, south and west, now and next tick. */
    float msIn[2][4][kMeshCells]{};
    int32_t msCur = 0, msN = kMeshFull;
    /** The edge's low-pass, one per edge port: north, east, south, west. */
    float msEdge[4][kMeshFull]{};
    float msAcc = 0.0f, msInSum[2]{}, msPrev[2]{}, msNow[2]{}, msOsc[2]{}, msRot[2]{};
    int32_t msInCount = 0;
    float msInc = 0.1f, msLoss = 1.0f, msEdgeCoeff = 1.0f, msLinkDamp = 0.0f, msScale = 1.0f, msLoop = 0.0f;
    float msWas[5]{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    /** Junction indices: where the input lands, the pickups, where the snares touch. */
    int32_t msStrikeAt[2]{}, msPickAt[2]{}, msTouchAt[3]{};
    // Snares: how far past the wires the head went on the last tick, the buzz's envelope, filters and noise.
    float snPast = 0.0f, snEnv = 0.0f, snHpX = 0.0f, snHpY = 0.0f, snLp = 0.0f, snHpCoeff = 0.0f, snLpCoeff = 1.0f;
    uint32_t snNoise = 1;

    // --- thunder sheet ---
    static constexpr int32_t kThBands = 4, kThModes = 48;
    float thRe[kThModes]{}, thIm[kThModes]{};
    /** Which band each mode is in, by its frequency. */
    int32_t thBand[kThModes]{};
    float thCos[kThModes]{}, thSin[kThModes]{}, thIn[kThModes]{}, thNz[kThModes]{}, thOutL[kThModes]{}, thOutR[kThModes]{};
    /** Each band's energy, followed. */
    float thEnergy[kThBands]{};
    float thEnergyCoeff = 0.0f, thLoop = 0.0f, thSpill = 0.0f, thDrain = 0.0f, thDrainMax = 0.0f, thScale = 1.0f, thOutScale = 1.0f;
    float thWas[5]{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    int32_t thStep = 1;
    uint32_t thNoise = 1;

    // --- howl ---
    static constexpr int32_t kHowlLines = 3;
    Line hwLine[kHowlLines];
    float hwLp[kHowlLines]{}, hwBp[kHowlLines][2]{}, hwEnv[kHowlLines]{}, hwOsc[kHowlLines][2]{}, hwRot[kHowlLines][2]{};
    float hwD[kHowlLines]{}, hwG[kHowlLines]{}, hwLpCoeff = 1.0f, hwPeak = 0.0f, hwLoop = 0.0f;
    // The speaker's peak, a band-pass with 0 dB at its centre.
    float hwB0 = 0.0f, hwA1 = 0.0f, hwA2 = 0.0f, hwEnvAtk = 1.0f, hwEnvRel = 1.0f;
    float hwWas[4]{-1.0f, -1.0f, -1.0f, -1.0f};

    // --- flutter ---
    static constexpr int32_t kFlutterLines = 2;
    Line flLine[kFlutterLines];
    float flLp[kFlutterLines]{}, flOsc[kFlutterLines][2]{}, flRot[kFlutterLines][2]{};
    float flD[kFlutterLines]{}, flG = 0.0f, flLpCoeff = 1.0f, flLoop = 0.0f;
    float flWas[4]{-1.0f, -1.0f, -1.0f, -1.0f};
};

} // namespace acidulous::effect
