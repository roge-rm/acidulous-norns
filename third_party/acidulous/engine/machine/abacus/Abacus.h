#pragma once
#include <cstdint>
#include <engine/core/Constants.h>
#include <engine/dsp/Adsr.h>
#include <engine/dsp/LfoGen.h>
#include <engine/machine/Machine.h>
#include <engine/machine/abacus/Borrow.h>
#include <engine/machine/abacus/PartialBank.h>

namespace acidulous::machine {

/**
 * Abacus: the additive machine.
 *
 * Each note is a row of up to 64 sine partials (see PartialBank.h). Their
 * frequencies come once a block from the note's pitch and a law: harmonic,
 * a stiff string, a free bar, a bell or a square plate, each a table of
 * mode ratios that stretch bends. Their levels come once a block from:
 *
 *   - the spectrum: a slope in dB per octave, odd against even, one formant
 *     peak, a vowel (two more formants) and a comb that takes out every nth
 *     partial; a second spectrum B, as offsets, that morph moves to;
 *   - damping: each partial decays on its own, the high ones faster;
 *   - brightness: velocity, pressure and LFO 2 tilt the slope;
 *   - shimmer: a slow seeded drift of each partial's level and pitch;
 *   - the amp envelope and velocity.
 *
 * Every gain ramps in a straight line across the block, so nothing steps.
 * Partials over 0.45 of the sample rate get gain 0, and groups of four that
 * are silent at both ends of a block are skipped. Spread places the
 * partials across the stereo field; at 0 the output is exactly mono.
 */
class Abacus final : public Machine {
  public:
    /** Output gain after `volume`. It puts Init at the house level with `volume` at its default (-14.9 dB). */
    static constexpr float kHouseTrim = 0.180f;
    static constexpr int kVoices = 16;
    static constexpr int kPartials = abacus::kMaxPartials;

    enum P : int32_t {
        // --- the spectrum ---
        Partials = 0, Slope, Odd, Peak, PeakGain, PeakWidth, Vowel, VowelAmount, Comb, CombN,
        Law, Stretch,
        // The B end of the morph, as offsets from the values above.
        BSlope, BOdd, BPeak, BComb,
        Morph, MorphEnv, MorphTime,
        // --- partial time ---
        Damp, Decay,
        // --- the amp envelope; decay2 so it isn't mistaken for the partials' decay ---
        Attack, Decay2, Sustain, Release,
        VelocityAmount, VelBright,
        // --- space and motion ---
        Spread, SpreadMode, Shimmer, ShimmerRate,
        Lfo1Wave, Lfo1Rate, Lfo1Sync, Lfo1Pitch, Lfo1Morph,
        Lfo2Wave, Lfo2Rate, Lfo2Sync, Lfo2Bright, Lfo2Pan,
        // --- pitch and expression ---
        Glide, BendRange, Octave, Transpose, Fine,
        MpeTimbre, MpePressure,
        Volume, Pan,
        // --- count: the partials play in rhythm ---
        Pattern, CRate, CSteps, CFrom, CWidth, CDepth, CGlide,
        // --- borrow: the partials' levels from another track or a file ---
        Borrow, Sidechain, BMode, BAmount, BSmooth, BFreeze, BPos, BScan,
        // --- slide: each partial travels at its own speed; strum: they enter in turn ---
        Slide, SOrder, SSpread, SLegato, Strum,
        // Eight macro knobs, each with four routes: a destination (a
        // MacroDest) and a depth. A macro's knob times a route's depth is
        // added to the destination, in its own 0..1 range. Block rate. Pitch
        // is 24 semitones per unit.
        MacroBase,
        MacroParams = 9, // knob, 4 destinations, 4 depths
        Count = MacroBase + 8 * MacroParams
    };
    // Where a macro route goes. Continuous params only: partials, combn, law,
    // spreadmode, the LFO waves and syncs, bend range, octave and transpose
    // are left out.
    enum MacroDest : int32_t {
        McOff, McSlope, McOdd, McPeak, McPeakGain, McPeakWidth, McVowel, McVowelAmount, McComb, McStretch,
        McBSlope, McBOdd, McBPeak, McBComb, McMorph, McMorphEnv, McMorphTime,
        McDamp, McDecay, McAttack, McDecay2, McSustain, McRelease, McVelocity, McVelBright,
        McSpread, McShimmer, McShimmerRate,
        McLfo1Rate, McLfo1Pitch, McLfo1Morph, McLfo2Rate, McLfo2Bright, McLfo2Pan,
        McGlide, McFine, McMpeTimbre, McMpePressure, McVolume, McPan, McPitch,
        McCDepth, McCGlide,
        McBAmount, McBSmooth, McBPos, McBScan,
        McSlide, McSSpread, McStrum,
        MacroDestCount
    };
    static constexpr int kMacros = 8;
    static_assert(Count <= kMaxParams, "too many parameters");

    enum LawKind : int32_t { Harmonic = 0, String, Bar, Bell, Plate, LawCount };
    /** The count patterns, and the rates in ticks (a bar is the song's own). */
    enum CountKind : int32_t { CountOff = 0, CountUp, CountDown, CountUpDown, CountOddEven, CountFifths, CountRandom, CountKinds };
    static constexpr int kRates = 10;
    enum SpreadKind : int32_t { OddEven = 0, Scatter, LowHigh, SpreadCount };
    enum BorrowKind : int32_t { BorrowOff = 0, BorrowTrack, BorrowFile };
    enum BorrowMode : int32_t { BorrowEnvelope = 0, BorrowPartials };
    enum SlideOrder : int32_t { LowFirst = 0, HighFirst, ScatterOrder, CentreOut, SlideOrders };
    /** The slide knob's bottom is off. */
    static constexpr float kSlideMin = 0.02f;
    /** The slide time of a partial at [rank] (0 first to 1 last) under [slide] seconds and [spread], seconds. For tests. */
    static float slideTime(float slide, float spread, float rank) { return slide * (1.0f - 0.9f * spread * (1.0f - rank)); }
    /** Partial [n]'s place in the order, 0 first to 1 last; [place] is its seeded value for scatter. */
    static float rankOf(int order, int n, int count, float place);

    Abacus();
    /** Deletes the mounted file's table. */
    ~Abacus() override;

    const char *typeName() const override { return "Abacus"; }
    const ParamDef *paramDefs(int32_t &count) const override;
    void prepare(int32_t sampleRate) override;
    void reset() override;
    void onBlock(int64_t tickStart, int64_t tickEnd, float bpm) override;
    void onSongTick(double tick) override {
        songTick = tick;
        songFresh = true;
    }
    void onTransport(bool rolling, int64_t) override { rolling_ = rolling; }
    void onBar(int64_t, int32_t ticksPerBar, float swing, int64_t swingPair) override {
        barTicks = ticksPerBar > 0 ? ticksPerBar : 4 * kPPQN;
        swingPercent = swing;
        swingPairTicks = swingPair;
    }
    void noteOn(uint8_t note, uint8_t velocity) override;
    void noteOff(uint8_t note) override;
    void allNotesOff() override;
    void channelPressure(uint8_t value) override;
    void pitchBend(int16_t value14) override;
    void noteBend(uint8_t note, float semitones) override;
    void notePressure(uint8_t note, uint8_t value) override;
    void noteTimbre(uint8_t note, uint8_t value) override;
    bool render(float *L, float *R, int32_t frames) override;
    /** Slot 0 takes an abacus::Spectra, a file analysed for borrow. */
    void *swapObject(int32_t slot, void *object) override;
    int64_t heldBytes() const override { return file_ != nullptr ? file_->bytes() : 0; }

    /** Partial [h] (1 is the note) lit on step [k] of a [kind] pattern of [steps] from partial [from], [width] wide, in bar [bar]; the first partial lit. For tests. */
    static int litStart(int kind, int k, int steps, int from, int count, int bar);
    /** Voice [i]'s borrowed level for partial [n], linear, for tests; -1 when the voice isn't sounding. */
    float borrowedOf(int i, int n) const { return voices[i].used ? voices[i].bor[n] : -1.0f; }
    /** The track analysis's latest frame, for tests. */
    const abacus::Frame &liveFrame() const { return live.current(); }
    /** Voices sounding, for tests. */
    int activeVoices() const;
    /** Partial [n]'s ratio to the note under [law] and [stretch], as the machine works it out. For tests. */
    static float ratioOf(int law, float stretch, int n);

  private:
    struct Voice {
        bool used = false, gate = false;
        uint8_t note = 0;
        /** Velocity's gain and its tilt on the slope, dB per octave, set at note-on. */
        float velGain = 1.0f, velBright = 0.0f;
        /** The pitch as a note number, gliding from [glideFrom] while [glidePos] is under 1. */
        float pitch = 60.0f, glideFrom = 60.0f, glidePos = 1.0f;
        /** Per-note expression: bend in semitones; pressure and slide -1 until this note sends them. */
        float bend = 0.0f, pressure = -1.0f, timbre = -1.0f, prsGlide = 0.0f;
        /** The morph envelope, 1 at note-on falling toward 0. */
        float morphEnv = 1.0f;
        dsp::Adsr amp;
        uint32_t age = 0;
        /** Blocks the voice has been silent at both ends. */
        int quiet = 0;
        /** Its own generator for shimmer and scatter, seeded at note-on. */
        uint32_t rng = 1;
        /** What the pan gains were worked out for. */
        float panFor = -1.0f;
        int panModeFor = -1;
        abacus::PartialBank bank;
        alignas(16) float panL[kPartials] = {};
        alignas(16) float panR[kPartials] = {};
        /** Each partial's damping, log2 of its gain, 0 at note-on and falling. */
        alignas(16) float damp[kPartials] = {};
        /** Each partial's seeded place for the scatter spread, -1 to 1. */
        alignas(16) float place[kPartials] = {};
        /** Shimmer: each partial's phase through its current step, its rate against the knob, and the level and pitch it moves from and to, -1 to 1. */
        alignas(16) float shPhase[kPartials] = {};
        alignas(16) float shRate[kPartials] = {};
        alignas(16) float shL0[kPartials] = {};
        alignas(16) float shL1[kPartials] = {};
        alignas(16) float shP0[kPartials] = {};
        alignas(16) float shP1[kPartials] = {};
        /** Borrow: seconds the file has been scanned since note-on, and the borrowed levels, linear, once set. */
        float scan = 0.0f;
        bool borSet = false;
        alignas(16) float bor[kPartials] = {};
        /**
         * The source looked up at this voice's partials, kept until the
         * source's frame, the mode, the count or the pitch (by a cent) move.
         * A track's goal is linear; a file's two frames either side are log2
         * shapes, mixed every block as the voice scans.
         */
        int32_t borKey = -1, borMode = -1, borCount = 0;
        float borF0 = 0.0f;
        alignas(16) float borGoal[kPartials] = {};
        alignas(16) float borA[kPartials] = {};
        alignas(16) float borB[kPartials] = {};
        /**
         * While borrowing, the peak and vowel terms of the own level, log2,
         * kept until the pitch, the law or their knobs move.
         */
        float ownKey[8] = {};
        uint32_t ownLaw = 0;
        alignas(16) float peakLg[kPartials] = {};
        alignas(16) float vowelLg[kPartials] = {};
        /**
         * Slide and strum: seconds since note-on (counted only while either
         * runs), the slide's start in octaves under the new pitch and its
         * length, the strum's length, and each partial's 1 / slide time and
         * strum delay.
         */
        float sTime = 0.0f, sFrom = 0.0f, sEnd = 0.0f, strumEnd = 0.0f;
        alignas(16) float sInv[kPartials] = {};
        alignas(16) float sDly[kPartials] = {};
    };

    /** Param [p], smoothed, with the macros' routes to it added. */
    float paramOf(int32_t p) const { return val(p, false); }
    /** Param [p]'s target with the macros' targets added, for note-on seeds. */
    float goalOf(int32_t p) const { return val(p, true); }
    float val(int32_t p, bool goal) const;
    int32_t steppedOf(int32_t p) const;
    void updateMacros();
    void updateMacroGoals();

    Voice *allocate();
    void startVoice(Voice &v, uint8_t note, uint8_t velocity);
    /** Works out the ratio and damping tables when the law, stretch or damp move. */
    void updateLaw(int law, float stretch, float damp);
    /** Moves the voice's borrowed levels toward the source's, for a voice at log2 [f0] Hz. */
    void borrowFor(Voice &v, float log2f0, float sec);
    /** Fills the voice's peakLg and vowelLg when what they depend on has moved. */
    void ownTerms(Voice &v, float log2f0, float peakLog2, float peakInv, float peakGain);
    /** Frame [f]'s envelope read at the first [count] partials over log2 [f0] Hz into [out], log2. */
    void lookUp(const abacus::Frame &f, float log2f0, int count, float *out) const;
    /** Renders one voice's block into the lane accumulators. */
    void renderVoice(Voice &v, int32_t frames);

    static float nextRand(uint32_t &state) {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }

    float sampleRate = 48000.0f;
    Voice voices[kVoices];
    uint32_t ageCounter = 0;
    static constexpr uint32_t kSeed = 0x2545f491u;
    uint32_t rng = kSeed;
    /** The last note started, for glide; -1 before any. */
    float lastPitch = -1.0f;
    /** Seconds since a note was released, and whether a key was held when the note being started came in. */
    float offAge = 1.0f;
    bool legatoNow = false;

    dsp::LfoGen lfo[2];
    float lfoValue[2] = {0.0f, 0.0f};
    float bpm = 120.0f;
    float bend = 0.0f, pressure_ = 0.0f;
    /** The song's tick at the block's start, set by onSongTick() and used once; else the count runs from reset(). */
    double songTick = 0.0, freeTick = 0.0;
    bool songFresh = false, songLocked = false, rolling_ = false;
    int32_t barTicks = 4 * kPPQN;
    float swingPercent = 50.0f;
    int64_t swingPairTicks = 0;
    /** Each partial's count weight this chunk, 1 when it plays in full. */
    alignas(16) float cw[kPartials] = {};
    void countWeights(double tick, int count);
    float mac_[MacroDestCount]{}, macGoal_[MacroDestCount]{};

    /** The law's ratios and their log2, and the damping's speed per partial in log2 of gain per second before decay divides it. */
    alignas(16) float ratio[kPartials] = {};
    alignas(16) float log2Ratio[kPartials] = {};
    alignas(16) float dampPow[kPartials] = {};
    /** 1 for the partials the comb takes, every combn-th. */
    alignas(16) float combMask[kPartials] = {};
    int combFor = -1;
    int lawFor = -1;
    /** Counts the ratio table's rebuilds, 0 never, so voices know their cached terms are stale. */
    uint32_t lawGen = 0;
    float stretchFor = 0.0f, dampFor = -1.0f;

    /** This block's values shared by every voice, set in render(). */
    struct Block {
        int count = 64;
        float dt = 0.0f, blockSec = 0.0f, cutoffHz = 21600.0f;
        float slope = -6.0f, odd = 0.0f, peakLog2 = 10.0f, peakGain = 0.0f, peakHalf = 0.35f;
        float bSlope = 0.0f, bOdd = 0.0f, bPeak = 0.0f, bComb = 0.0f, comb = 0.0f;
        int combN = 3;
        float vowelF1 = 9.5f, vowelF2 = 10.1f, vowelAmount = 0.0f;
        float morph = 0.0f, morphEnv = 0.0f, morphFall = 1.0f;
        float decay = 30.0f, shimmer = 0.0f, shimmerStep = 0.0f;
        float spread = 0.0f;
        int spreadMode = 0;
        float tune = 0.0f, bendSemis = 0.0f, glide = 0.0f;
        float lfo1Pitch = 0.0f, lfo1Morph = 0.0f, lfo2Bright = 0.0f;
        float mpeTimbre = 0.5f, mpePressure = 0.5f;
        int countKind = 0, cSteps = 8, cFrom = 4, cWidth = 1, cRateTicks = 60;
        float cDepth = 1.0f, cGlide = 0.2f;
        float ampA = 0.005f, ampD = 1.0f, ampS = 1.0f, ampR = 0.3f;
        /** Borrow this block: from where (off when the source isn't there), how it reads, and its knobs. */
        int borrow = 0, bMode = 0;
        bool bFreeze = false;
        float bAmount = 1.0f, bCoef = 1.0f, bPos = 0.0f, bScan = 0.0f;
    } blk;

    /** The track borrowed from, analysed as it plays. */
    abacus::LiveSource live;
    /** The file borrowed from, mounted in slot 0, or null. */
    const abacus::Spectra *file_ = nullptr;
    /** Counts the files mounted, so voices look a new one up. */
    int32_t fileGen = 0;

    /** Every voice's lanes summed, four floats a frame, left and right. */
    alignas(16) float accL[64 * 4] = {};
    alignas(16) float accR[64 * 4] = {};
    /** This block's per-partial target gains for the voice being rendered. */
    alignas(16) float tl[kPartials] = {};
    alignas(16) float tr[kPartials] = {};
    /** The output gains at the end of the last block, left and right; false until the first block sets them. */
    float outL = 0.0f, outR = 0.0f;
    bool outSet = false;
};

} // namespace acidulous::machine
