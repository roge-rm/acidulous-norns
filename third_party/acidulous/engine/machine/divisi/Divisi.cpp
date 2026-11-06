#include "Divisi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level for a key at full velocity, set so a chord sits with the other machines. */
constexpr float kHouse = 0.2f;
/** Each register's level at full against the violin's, so each knob at full is about as loud. */
constexpr float kViolinTrim = 1.2f, kViolaTrim = 1.05f, kCelloTrim = 1.18f, kBassTrim = 1.02f;
constexpr float kChoirTrim = 2.62f, kBrassTrim = 1.25f;
/** A key's switch into the registers: rises in 3 ms, falls in 30 ms. */
constexpr float kGateRise = 0.003f, kGateFall = 0.03f;
/** Octaves the brass filter opens over at full sweep, its cutoff closed, Hz, and its resonance. */
constexpr float kBrassOctaves = 7.0f, kBrassClosed = 180.0f, kBrassRes = 0.3f;
/** How much of its cutoff a poly key's brass filter moves with the key's pitch, octaves per octave. */
constexpr float kBrassTrack = 0.5f;
/** Brightness: how far a held note falls to, and how loud a note is at no velocity against full. */
constexpr float kBrassFloor = 0.7f, kBrassVelFloor = 0.3f;
/** The brightness's rise at no blat and the most blat adds, its fall, and the overshoot's two halves, seconds. */
constexpr float kBrightUp = 0.015f, kBrightUpBlat = 0.04f, kBrightDown = 0.08f;
constexpr float kOverFast = 0.02f, kOverSlow = 0.1f;
/** The overshoot at full blat, in brightness, and the scoop, semitones, and its time, seconds. */
constexpr float kOverMax = 0.6f, kScoopMax = 0.6f, kScoopTime = 0.03f;
/** The drive into the brass filter: at no loudness and what full loudness adds. */
constexpr float kDriveLow = 0.4f, kDriveSpan = 1.4f;
/** Samples between moves of the phaser's all-passes, and of the brass filters and their brightness. */
constexpr int kCoefEvery = 8, kBrassEvery = 16;
/** The ensemble: the lines' middle delay, and the slow and fast LFOs' reach either side of it, ms, and their rates, Hz. */
constexpr float kLineMs = 6.0f, kSlowMs = 1.8f, kFastMs = 0.3f;
constexpr float kSlowHz = 0.55f, kFastHz = 5.8f;
/** The phaser's sweep, Hz. */
constexpr float kPhaseLow = 180.0f, kPhaseHigh = 2600.0f;
/** Below this a key is silent, and the whole machine after this long of it and its lines empty. */
constexpr float kSilent = 1e-6f;
constexpr int32_t kSleepAfter = 4096;
/** Where the counts of samples since the lines last heard sound stop. */
constexpr int32_t kHeardCap = 1 << 30;
constexpr float kTwoTo32 = 4294967296.0f;
/** A pitch ratio's departure from 1 per cent, near enough for a few tens of cents. */
constexpr float kPerCent = 0.69314718f / 1200.0f;
/** A player's walk: the time between new targets (shortest and the most added), its two smoothing stages, s. */
constexpr float kPickMin = 0.3f, kPickSpan = 0.6f, kWalkTime = 0.4f;
/** How fast the walk's delay is pulled back to the middle, s: how long a player stays sharp or flat. */
constexpr float kDriftHold = 1.5f;
/** How far either way the vibrato rate is spread over the players. */
constexpr float kVibSpread = 0.07f;
/** How fast a scatter delay glides to a new place, samples a sample (about 17 cents while it moves). */
constexpr float kSeatGlide = 0.01f;
/** The players' gains glide in about this long, s. */
constexpr float kGainTime = 0.02f;
/** The longest a player's line holds: scatter, drift and vibrato at their ends, with room, s. */
constexpr float kPlayerLine = 0.14f;
/** Each section's width, how far its players spread either side of its middle: violin, viola, cello, contrabass, choir, brass. */
constexpr float kWidth[6] = {0.7f, 0.6f, 0.6f, 0.4f, 0.8f, 0.5f};
/**
 * How far apart the players' delays typically are, ms, per ms of scatter and
 * per cent of drift and of vibrato (at 5.5 Hz), and the spread at which they
 * stop adding as one louder sound and start adding by power.
 */
constexpr float kSpreadScatter = 0.29f, kSpreadDrift = 0.09f, kSpreadVibrato = 0.014f, kSpreadLoose = 0.5f;
/** How loud the width-weighted mix is against the mix, by power, for the pan law's extra level at the sides. */
constexpr float kWideShare = 0.4f;

/**
 * The stage, metres. The listener sits this far in front of the stage's
 * front middle; each section's seat at full stage, across (left negative)
 * and back from the front: violin, viola, cello, contrabass, choir, brass.
 */
constexpr float kListener = 5.0f;
constexpr float kSeatX[6] = {-4.0f, 1.5f, 4.0f, 5.5f, 0.0f, -2.0f};
constexpr float kSeatY[6] = {0.8f, 2.5f, 1.5f, 6.0f, 13.0f, 10.0f};
/** The side walls either side of the middle, and what they give back. */
constexpr float kWallX = 8.0f, kWallGain = 0.7f;
/** The side walls' reflections' level at full room, against their path lengths. */
constexpr float kRoomLevel = 1.0f;
/**
 * The stage's ambience: what a section sends into it at full room, and how
 * much more of that the back rows send than the front, over the metres to the
 * back; it's about as loud wherever you sit, the direct sound is not.
 */
constexpr float kSendLevel = 0.55f, kSendFront = 0.3f, kSendDepth = 13.0f;
/** The ambience's power against what it's sent, on held chords, whose partials sit where it rings longest. */
constexpr float kAmbiencePower = 1.5f;
/** Sound's speed, m/s; a direction's pan per radian off the middle (45 degrees is fully to one side). */
constexpr float kSound = 343.0f, kPanPerRadian = 4.0f / dsp::kPi;
/** How the direct sound falls with distance, a power of the listener's distance over the seat's, softer than 1/r. */
constexpr float kFall = 0.8f;
/** Air loss: the low-pass the far seats lean toward, Hz, the metres before it starts, how much of it per metre after, and the most. */
constexpr float kAirHz = 2500.0f, kAirFrom = 1.5f, kAirPerMetre = 0.075f, kAirMost = 0.85f;
/** The reflections' low-pass, Hz. */
constexpr float kReflectionHz = 4000.0f;
/**
 * How much of the reflections' power is heard in the level while the shared
 * ensemble runs, since on held notes they cancel the direct sound about as
 * often as they add; once the sections' choruses move the direct sound they
 * add by power.
 */
constexpr float kReflectionShare = 0.35f;
/** How much two string sections, and a string section and the choir or brass or those two, add as one in the middle. */
constexpr float kLockedStrings = 0.25f, kLockedOthers = 0.07f;
/** How long the sections' choruses take to take over from the shared ensemble when the stage opens, s. */
constexpr float kHandTime = 0.05f;
/** A chorus's power at [depth] against its dry sound's, its two taps a side wandering apart from it and each other. */
inline float chorusPower(float depth) {
    constexpr float taps = 0.65f * 0.65f + 0.35f * 0.35f;
    return (1.0f - depth) * (1.0f - depth) + depth * depth * taps;
}
/** The ambience: its decay to -60 dB, s; the four lines and the four all-passes, ms; the all-passes' gain; its loop's low-pass, Hz. */
constexpr float kAmbienceTime = 0.9f;
constexpr float kAmbienceMs[4] = {29.7f, 37.1f, 41.3f, 44.9f};
constexpr float kDiffuseMs[4] = {4.7f, 3.3f, 5.3f, 3.9f};
constexpr float kDiffuse = 0.6f, kAmbienceHz = 4500.0f;
/** Its output level, so a held chord in the middle registers comes out about as loud. */
constexpr float kAmbienceOut = 0.46f;
/**
 * The sections' own ranges, split: the middle of each string section's,
 * violin to contrabass, as sounding notes; the fade where two meet, either
 * side of halfway, semitones. The choir and brass each keep a window in the
 * middle, full between the inner pair and fading out to the outer.
 */
constexpr float kRangeCentre[4] = {67.0f, 58.0f, 49.0f, 38.0f};
constexpr float kRangeFade = 4.0f;
/** What a note loses to the split it gets back, as a power of the power it lost. */
constexpr float kPartComp = 0.55f;
constexpr float kChoirRange[4] = {45.0f, 50.0f, 67.0f, 72.0f};
constexpr float kBrassRange[4] = {41.0f, 45.0f, 60.0f, 65.0f};
/** How long the stage and room knobs take to move a seat, s. */
constexpr float kSeatTime = 0.15f;
/** The longest a seat's line holds: the furthest reflection, the chorus's middle and reach, with room, s. */
constexpr float kSeatLine = 0.1f;
/** The sections' chorus rates against the ensemble's, slow and fast. */
constexpr float kSeatSlow[6] = {1.0f, 0.83f, 1.13f, 0.91f, 0.77f, 1.07f};
constexpr float kSeatFast[6] = {1.0f, 1.09f, 0.88f, 1.16f, 0.94f, 0.81f};

/** The synced motion lengths, quarter notes: 1/4, 1/2, 1, 2, 4, 8 and 16 bars. */
constexpr double kMotionBeats[Divisi::kMotionSyncs] = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0};
/** How long a jump in the motion (a new shape, a locate, play from stopped) takes to glide out, s. */
constexpr float kMotionGlide = 0.25f;
/** Balance: dB a section moves at full depth; swell's whole-ensemble fall at the bottom of its breath is twice that. */
constexpr float kBalanceDb = 6.0f;
/** Brightness: octaves the sections' filters move at full depth. */
constexpr float kToneOctaves = 1.0f;
/** Vowel and chorus depth: how much of their knob's range they move either way at full depth. */
constexpr float kVowelReach = 0.5f, kEnsembleReach = 0.5f;
/** Steps: the part of each step spent gliding to the new balance. */
constexpr float kStepGlide = 0.2f;
/**
 * Tide: where each section is in the wave's path, 0 to 1, front left to back
 * right, from the stage's seats (across over 9.5 m plus back over 12.2 m,
 * over the largest): violin, viola, cello, contrabass, choir, brass; and
 * how much of a turn the wave spreads over that path.
 */
constexpr float kTidePlace[6] = {0.0f, 0.504f, 0.631f, 1.0f, 0.996f, 0.676f};
constexpr float kTideSpread = 0.5f;

/** Age: the level a loud chord's wet sound reaches in the shared chorus, which the squeeze is centred on and the bucket's clip and noise are sized by. */
constexpr float kAgeNominal = 0.15f;
/** The squeeze's followers, up and down, s; its floor, against the nominal level. */
constexpr float kAgeUp = 0.008f, kAgeDown = 0.08f, kAgeFloor = 1e-3f;
/** The hiss and whine come in fully once the input is this much of the nominal level. */
constexpr float kAgeGate = 0.01f;
/** Hiss and whine at full age, against the nominal level; the clip bends at three times it. */
constexpr float kAgeHiss = 0.0093f, kAgeWhine = 0.02f, kAgeBend = 3.0f;
/** The sections' bucket adds its hiss and whine after their depth and gains, so it comes out louder: brought back to about the shared one's. */
constexpr float kAgeStageNoise = 0.5f;
/** The low-pass, Hz, open at 0 and shut at 1, and its damping from flat to a small hump. */
constexpr float kAgeOpenHz = 20000.0f, kAgeShutHz = 3500.0f, kAgeFlatK = 1.414f, kAgeHumpK = 0.96f;
/** Turns a sample of whine at a delay of one sample: an eighth of a 1024-stage clock, 1024 / (2 * 8). */
constexpr float kWhineTurns = 64.0f;
/** The wander's depth at full age, ms, its new target every 0.25 to 0.85 s, smoothed twice over 0.2 s; the warble's depth, ms, rate and how far the wander moves its rate. */
constexpr float kWowMs = 0.35f, kWowPickMin = 0.25f, kWowPickSpan = 0.6f, kWowTime = 0.2f;
constexpr float kWarbleMs = 0.05f, kWarbleHz = 6.3f, kWarbleWander = 0.25f;

/** A seeded value for section [s] at step [i], -1 to 1: the same wherever play starts. */
inline float motionHash(int s, int64_t i) {
    uint32_t x = static_cast<uint32_t>(i) * 0x9e3779b1u ^ static_cast<uint32_t>(static_cast<uint64_t>(i) >> 32) * 0x85ebca77u;
    x ^= static_cast<uint32_t>(s + 1) * 0xc2b2ae3du;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<float>(x >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

/** A step of a player's own generator, 0 to 1. */
inline float nextRand(uint32_t &r) {
    r = r * 1664525u + 1013904223u;
    return static_cast<float>(r >> 8) * (1.0f / 16777216.0f);
}
constexpr float kInvTwoTo32 = 1.0f / 4294967296.0f;

/** A soft clip close to tanh, flat at 1 from 3. */
inline float softClip(float u) {
    if (u >= 3.0f) return 1.0f;
    if (u <= -3.0f) return -1.0f;
    const float u2 = u * u;
    return u * (27.0f + u2) / (27.0f + 9.0f * u2);
}

/** A sine of [turns], close enough for an LFO. */
inline float lfoSin(float turns) {
    const float x = turns - std::floor(turns + 0.5f); // -0.5..0.5
    const float y = 8.0f * x - 16.0f * x * std::fabs(x);
    return 0.225f * (y * std::fabs(y) - y) + y;
}

inline float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/** A band-limited saw from a divider's phase and step, both fractions of 2^32. */
inline float sawOf(uint32_t phase, uint32_t step) {
    const float t = static_cast<float>(phase) * kInvTwoTo32;
    const float dt = static_cast<float>(step) * kInvTwoTo32;
    return 2.0f * t - 1.0f - polyBlep(t, dt);
}

/** The choir's vowels, dark to bright: three formants each, Hz, and their levels. */
struct VowelShape {
    float hz[3];
    float level[3];
};
constexpr VowelShape kVowels[5] = {
    {{325.0f, 700.0f, 2530.0f}, {1.0f, 0.35f, 0.08f}},   // u
    {{450.0f, 800.0f, 2830.0f}, {1.0f, 0.5f, 0.1f}},     // o
    {{800.0f, 1150.0f, 2900.0f}, {1.0f, 0.6f, 0.15f}},   // a
    {{400.0f, 1600.0f, 2700.0f}, {1.0f, 0.35f, 0.2f}},   // e
    {{300.0f, 2100.0f, 3000.0f}, {1.0f, 0.25f, 0.2f}},   // i
};
/** A formant's width, Hz: a floor and a part of its centre, as wide as a section of voices rather than one. */
constexpr float kFormantWidth = 80.0f, kFormantSpread = 0.12f;
/** The choir's excitation: the 8' square, the 16' square and the 8' saw, which fills in the even harmonics. */
constexpr float kChoirSq8 = 0.7f, kChoirSq16 = 0.5f, kChoirSaw8 = 0.45f;
/** The choir's high-pass and its low-pass at the middle tone, Hz. */
constexpr float kChoirHigh = 120.0f, kChoirLow = 5000.0f;
/** Highest harmonic the choir's levelling counts, Hz, and how far it may move a key, either way. */
constexpr float kCompTop = 8000.0f, kCompMax = 6.0f;
/** The note the choir is levelled to, C4, and its vowel, aah. */
constexpr int kCompNote = 60;

/** A complex number, for the levelling's filter responses. */
struct Cx {
    float re, im;
};
inline Cx mul(Cx a, Cx b) { return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
inline Cx divide(Cx a, Cx b) {
    const float d = b.re * b.re + b.im * b.im;
    return {(a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d};
}

} // namespace

Divisi::Divisi() { initParams(); }

const ParamDef *Divisi::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        {"violin", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        {"viola", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"cello", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"contrabass", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"choir", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // The choir's vowel: u, o, a, e, i.
        {"vowel", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"brass", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // How far the brass filter opens on a note, and how long a held note takes to fall back.
        {"sweep", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"brassdecay", 0.05f, 4.0f, 0.8f, Curve::Exponential, 0, "s"},
        // The brass attack: how far the filter overshoots, how late it opens and how far the pitch scoops up.
        {"blat", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"tone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // paraphonic, poly
        {"mode", 0.0f, static_cast<float>(ModeCount - 1), 0.0f, Curve::Stepped, ModeCount, ""},
        {"attack", 0.002f, 5.0f, 0.25f, Curve::Exponential, 0, "s"},
        {"release", 0.02f, 10.0f, 1.5f, Curve::Exponential, 0, "s"},
        {"ensemble", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"speed", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"phaser", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"rate", 0.05f, 5.0f, 0.4f, Curve::Exponential, 0, "Hz"},
        {"feedback", 0.0f, 0.9f, 0.4f, Curve::Linear, 0, ""},
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"velocity", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // Each section's players, and how they differ: pitch drift, vibrato depth and rate, and how late each comes in.
        {"players", 1.0f, 16.0f, 1.0f, Curve::Stepped, 16, ""},
        {"drift", 0.0f, 25.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"vibrato", 0.0f, 60.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"vibrate", 2.0f, 9.0f, 5.5f, Curve::Exponential, 0, "Hz"},
        {"scatter", 0.0f, 80.0f, 0.0f, Curve::Linear, 0, "ms"},
        // How far the sections spread over the stage, and how much of the hall's early reflections is heard.
        {"stage", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"room", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // How far each section keeps to its own part of the keyboard.
        {"split", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // Motion: off, drift, swell, tide, steps; its rate free, or synced (off, 1/4, 1/2, 1, 2, 4, 8, 16 bars);
        // and how far it moves the sections' balance, the vowel, the brightness and the chorus.
        {"motion", 0.0f, static_cast<float>(MotionShapeCount - 1), 0.0f, Curve::Stepped, MotionShapeCount, ""},
        {"mrate", 0.02f, 2.0f, 0.1f, Curve::Exponential, 0, "Hz"},
        {"msync", 0.0f, static_cast<float>(kMotionSyncs), 0.0f, Curve::Stepped, kMotionSyncs + 1, ""},
        {"mbalance", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mvowel", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"mtone", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        {"mensemble", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        // The choruses from clean to an old bucket-brigade box.
        {"age", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
    };
    static ParamDef defs[Count];
    static char names[kMacros * MacroParams][12];
    static bool built = false;
    if (!built) {
        for (int32_t i = 0; i < MacroBase; ++i) defs[i] = first[i];
        auto put = [&](int32_t i, const char *n, float mn, float mx, float df, Curve c, int32_t steps) {
            char *name = names[i - MacroBase];
            std::snprintf(name, 12, "%s", n);
            defs[i] = ParamDef{name, mn, mx, df, c, steps, ""};
        };
        for (int k = 0; k < kMacros; ++k) {
            char n[12];
            const int32_t b = MacroBase + k * MacroParams;
            std::snprintf(n, sizeof(n), "mac%d", k + 1);
            put(b, n, 0.0f, 1.0f, 0.0f, Curve::Linear, 0);
            for (int r = 0; r < 4; ++r) {
                std::snprintf(n, sizeof(n), "mac%d_d%d", k + 1, r + 1);
                put(b + 1 + r, n, 0.0f, static_cast<float>(MacroDestCount - 1), 0.0f, Curve::Stepped, MacroDestCount);
                std::snprintf(n, sizeof(n), "mac%d_a%d", k + 1, r + 1);
                put(b + 5 + r, n, -1.0f, 1.0f, 0.0f, Curve::Linear, 0);
            }
        }
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Divisi::Count];
    DestMap() {
        using D = Divisi;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {D::Violin, D::McViolin}, {D::Viola, D::McViola}, {D::Cello, D::McCello},
            {D::Contrabass, D::McContrabass}, {D::Choir, D::McChoir}, {D::Vowel, D::McVowel},
            {D::Brass, D::McBrass}, {D::Sweep, D::McSweep}, {D::BrassDecay, D::McBrassDecay}, {D::Blat, D::McBlat},
            {D::Tone, D::McTone}, {D::Attack, D::McAttack}, {D::Release, D::McRelease},
            {D::Ensemble, D::McEnsemble}, {D::Speed, D::McSpeed}, {D::Phaser, D::McPhaser},
            {D::Rate, D::McRate}, {D::Feedback, D::McFeedback},
            {D::VelocityAmount, D::McVelocity}, {D::Volume, D::McVolume},
            {D::MotionBalance, D::McMotionBalance}, {D::MotionVowel, D::McMotionVowel},
            {D::MotionTone, D::McMotionTone}, {D::MotionEnsemble, D::McMotionEnsemble},
            {D::MotionRate, D::McMotionRate}, {D::Age, D::McAge},
            {D::Players, D::McPlayers}, {D::Drift, D::McDrift}, {D::Vibrato, D::McVibrato},
            {D::Vibrate, D::McVibrate}, {D::Scatter, D::McScatter}, {D::Stage, D::McStage},
            {D::Room, D::McRoom}, {D::Split, D::McSplit},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Divisi::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : (goal ? macGoal_ : mac_)[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Divisi::updateMacroGoals() {
    for (float &m : macGoal_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = read(b, true);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            const int dest = static_cast<int>(read(b + 1 + r, true) + 0.5f);
            if (dest > McOff && dest < MacroDestCount) macGoal_[dest] += knob * read(b + 5 + r, true);
        }
    }
    // Two octaves either way.
    macGoal_[McPitch] = std::clamp(macGoal_[McPitch], -1.0f, 1.0f);
}

void Divisi::updateMacros() {
    updateMacroGoals();
    for (float &m : mac_) m = 0.0f;
    for (int k = 0; k < kMacros; ++k) {
        const int32_t b = MacroBase + k * MacroParams;
        const float knob = read(b, false);
        if (knob == 0.0f) continue;
        for (int r = 0; r < 4; ++r) {
            // The routes' destinations are steps, so their targets.
            const int dest = static_cast<int>(read(b + 1 + r, true) + 0.5f);
            if (dest > McOff && dest < MacroDestCount) mac_[dest] += knob * read(b + 5 + r, false);
        }
    }
    mac_[McPitch] = std::clamp(mac_[McPitch], -1.0f, 1.0f);
}

void Divisi::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (dsp::Svf *f : {&violinHigh, &violinLow, &violaHigh, &violaLow, &celloHigh, &celloLow, &bassLow,
                        &choirHigh, &choirLow, &formant[0], &formant[1], &formant[2], &brassLow}) {
        f->setSampleRate(sampleRate);
    }
    // The middle delay and both LFOs at their widest and fastest, with room.
    const int32_t ensembleLength = static_cast<int32_t>((kLineMs + 2.0f * (kSlowMs + kFastMs)) * 0.001f * sampleRate) + 8;
    line.prepare(ensembleLength);
    lineR.prepare(ensembleLength);
    lineSize = 1;
    while (static_cast<float>(lineSize) < kPlayerLine * sampleRate + 4.0f) lineSize <<= 1;
    lineMask = lineSize - 1;
    playerLines.assign(static_cast<size_t>(lineSize * 3), 0.0f);
    seatSize = 1;
    while (static_cast<float>(seatSize) < kSeatLine * sampleRate + 4.0f) seatSize <<= 1;
    seatMask = seatSize - 1;
    seatLines.assign(static_cast<size_t>(seatSize * kSections), 0.0f);
    ambience.prepare(sampleRate);
    // The whole table here, off the audio thread, since a moving vowel crosses rows while notes are held.
    // It takes tens of milliseconds, so it's worked out once per sample rate and shared by every Divisi.
    {
        static std::mutex lock;
        static float sharedRate = 0.0f;
        static float shared[kVowelRows][128];
        std::lock_guard<std::mutex> hold(lock);
        if (sharedRate != sampleRate) {
            const float ref = choirPower((kVowelRows - 1) / 2, kCompNote);
            for (int r = 0; r < kVowelRows; ++r) {
                for (int n = 0; n < 128; ++n) {
                    shared[r][n] = clampf(std::sqrt(ref / std::fmax(choirPower(r, n), 1e-12f)), 1.0f / kCompMax, kCompMax);
                }
            }
            sharedRate = sampleRate;
        }
        std::copy(&shared[0][0], &shared[0][0] + kVowelRows * 128, &compCache[0][0]);
    }
    reset();
}

void Divisi::clearState() {
    for (dsp::Svf *f : {&violinHigh, &violinLow, &violaHigh, &violaLow, &celloHigh, &celloLow, &bassLow,
                        &choirHigh, &choirLow, &formant[0], &formant[1], &formant[2], &brassLow}) {
        f->reset();
    }
    line.clear();
    lineR.clear();
    std::fill(playerLines.begin(), playerLines.end(), 0.0f);
    lineWrite = lineQuiet = 0;
    lineHeard = stageHeard = kHeardCap;
    snapPlayers = true;
    std::fill(seatLines.begin(), seatLines.end(), 0.0f);
    seatWrite = 0;
    snapSeats = true;
    for (Seat &st : seats_) {
        st.airState = 0.0f;
        st.quiet = seatSize + 65;
        st.at = SeatAt();
    }
    erAir[0] = erAir[1] = 0.0f;
    ambience.clear();
    placedFor[0] = -1.0f;
    for (auto &side : apState) for (float &s : side) s = 0.0f;
    apBack[0] = apBack[1] = 0.0f;
    sharedBright = Bright();
    sharedDrive = 1.0f;
    shared = Env();
    agedShared.clear(0x6d2b79f5u);
    agedStage.clear(0x1b873593u);
}

void Divisi::reset() {
    for (Key &k : keys) k = Key();
    clearState();
    for (float &m : mac_) m = 0.0f;
    for (float &m : macGoal_) m = 0.0f;
    bend = channelPressure_ = 0.0f;
    // The dividers start spread, as free-running oscillators would be.
    uint32_t seed = 0x9e3779b9u;
    for (int pc = 0; pc < 12; ++pc) {
        seed = seed * 1664525u + 1013904223u;
        topPhase[pc] = seed;
        topStep[pc] = 0;
        topHz[pc] = 0.0f;
    }
    slowPhase = 0.0f;
    fastPhase = 0.0f;
    for (int s = 0; s < kSections; ++s) {
        Seat &st = seats_[s];
        st.slowPhase = static_cast<float>(s) * 0.37f - std::floor(static_cast<float>(s) * 0.37f);
        st.fastPhase = static_cast<float>(s) * 0.61f - std::floor(static_cast<float>(s) * 0.61f);
        st.slowRate = kSeatSlow[s];
        st.fastRate = kSeatFast[s];
    }
    phaserPhase = 0.0f;
    apCoef[0] = apCoef[1] = 0.0f;
    brassCountdown = phaserCountdown = 0;
    clock = 0;
    quietSamples = 0;
    asleep = true;
    seatPlayers();
    motionPos = 0.0;
    for (int c = 0; c < kMotionChannels; ++c) motionShown[c] = motionGlide[c] = 0.0f;
    motionShapeWas = motionSyncWas = -1;
    motionLocked = motionRestart = motionWasLive = false;
    sleptLast = true;
    wowRng = 0x2545f491u;
    wowTarget = wowY1 = wowY2 = warblePhase = 0.0f;
    wowNext = 0;
}

void Divisi::stepWow(int32_t n, float &from, float &to) {
    from = wowY2;
    wowNext -= n;
    if (wowNext <= 0) {
        wowTarget = 2.0f * nextRand(wowRng) - 1.0f;
        wowNext += static_cast<int32_t>((kWowPickMin + kWowPickSpan * nextRand(wowRng)) * sampleRate);
    }
    const float c = 1.0f - std::exp(-static_cast<float>(n) / (kWowTime * sampleRate));
    wowY1 += c * (wowTarget - wowY1);
    wowY2 += c * (wowY1 - wowY2);
    to = wowY2;
}

void Divisi::ageSettings(float age, float nominal, float noise, AgeAt &at) const {
    at.squeeze = 0.5f * age;
    at.open = at.squeeze / (1.0f - at.squeeze);
    at.nominal = nominal;
    at.floor = nominal * kAgeFloor;
    at.up = 1.0f - std::exp(-1.0f / (kAgeUp * sampleRate));
    at.down = 1.0f - std::exp(-1.0f / (kAgeDown * sampleRate));
    at.gate = 1.0f / (nominal * kAgeGate);
    at.hiss = kAgeHiss * noise * nominal * age;
    // Nothing below half way, then rising.
    const float w = std::fmax(0.0f, 2.0f * age - 1.0f);
    at.whine = kAgeWhine * noise * nominal * w * w;
    at.clip = age;
    at.bendAt = kAgeBend * nominal;
    {
        // The clip keeps the nominal level where it is.
        const float u = 1.0f / kAgeBend;
        at.keep = u / softClip(u);
    }
    const float hz = std::fmin(kAgeOpenHz * std::pow(kAgeShutHz / kAgeOpenHz, age), 0.45f * sampleRate);
    const float g = std::tan(dsp::kPi * hz / sampleRate);
    at.k = kAgeFlatK + (kAgeHumpK - kAgeFlatK) * age;
    at.a1 = 1.0f / (1.0f + g * (g + at.k));
    at.a2 = g * at.a1;
    at.a3 = g * at.a2;
}

float Divisi::runAged(Aged &a, int s, const AgeAt &at, float x, float whineStep) {
    Bucket &b = a.side[s];
    const float ax = std::fabs(x);
    b.in = dsp::undenormal(b.in + (ax > b.in ? at.up : at.down) * (ax - b.in));
    if (b.count <= 0) {
        // The squeeze and its opening every 16 samples, ramped there.
        constexpr int32_t kEvery = 16;
        b.count = kEvery;
        const float gi = std::pow(at.nominal / std::fmax(b.in, at.floor), at.squeeze);
        const float go = b.out > 0.0f ? std::pow(b.out / at.nominal, at.open) : 0.0f;
        b.dIn = (gi - b.gIn) * (1.0f / kEvery);
        b.dOut = (go - b.gOut) * (1.0f / kEvery);
        if (b.entering) {
            // Age has come on with sound passing: the squeeze and the opening start where they cancel.
            b.gIn = gi;
            b.gOut = 1.0f / gi;
            b.dIn = b.dOut = 0.0f;
        }
    }
    --b.count;
    b.gIn += b.dIn;
    b.gOut += b.dOut;
    float y = x * b.gIn;
    // The bucket's hiss and whine, let in by the sound.
    const float gate = std::fmin(1.0f, b.in * at.gate);
    if (gate > 0.0f) {
        a.noise = a.noise * 1664525u + 1013904223u;
        float add = at.hiss * static_cast<float>(static_cast<int32_t>(a.noise)) * (1.0f / 2147483648.0f);
        if (at.whine > 0.0f) {
            b.whine += whineStep;
            b.whine -= std::floor(b.whine);
            add += at.whine * lfoSin(b.whine);
        }
        y += gate * add;
    }
    if (at.clip > 0.0f) y += at.clip * (at.bendAt * at.keep * softClip(y / at.bendAt) - y);
    const float ay = std::fabs(y);
    b.out = dsp::undenormal(b.out + (ay > b.out ? at.up : at.down) * (ay - b.out));
    const float z = y * b.gOut;
    if (b.entering) {
        // The low-pass settled on the sound, as it would be on a steady level.
        b.entering = false;
        b.ic1 = 0.0f;
        b.ic2 = z;
    }
    // The low-pass, a state-variable one.
    const float v3 = z - b.ic2;
    const float v1 = at.a1 * b.ic1 + at.a2 * v3;
    const float v2 = b.ic2 + at.a2 * b.ic1 + at.a3 * v3;
    b.ic1 = dsp::undenormal(2.0f * v1 - b.ic1);
    b.ic2 = dsp::undenormal(2.0f * v2 - b.ic2);
    return v2;
}

void Divisi::onBlock(int64_t, int64_t, float bpm) { tempo = bpm > 1.0f ? bpm : 120.0f; }

void Divisi::shapeAt(int shape, double pos, float out[kMotionChannels]) {
    const double whole = std::floor(pos);
    const auto i = static_cast<int64_t>(whole);
    const float f = static_cast<float>(pos - whole);
    out[kSections] = 0.0f;
    switch (shape) {
    case MotionDrift:
        // A smooth curve through a seeded point a period, each section its own.
        for (int s = 0; s < kSections; ++s) {
            const float p0 = motionHash(s, i - 1), p1 = motionHash(s, i), p2 = motionHash(s, i + 1), p3 = motionHash(s, i + 2);
            const float v = p1 + 0.5f * f * (p2 - p0 + f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + f * (3.0f * (p1 - p2) + p3 - p0)));
            out[s] = clampf(v, -1.0f, 1.0f);
        }
        break;
    case MotionSwell: {
        // Every section together, from the bottom of the breath; the level falls with it.
        const float v = lfoSin(f - 0.25f);
        for (int s = 0; s < kSections; ++s) out[s] = v;
        out[kSections] = 0.5f * (v - 1.0f);
        break;
    }
    case MotionTide:
        for (int s = 0; s < kSections; ++s) out[s] = lfoSin(f - kTideSpread * kTidePlace[s]);
        break;
    case MotionSteps: {
        // The last step's balance to this one's, over the start of the step.
        const float t = clampf(f / kStepGlide, 0.0f, 1.0f);
        const float g = t * t * (3.0f - 2.0f * t);
        for (int s = 0; s < kSections; ++s) {
            const float a = motionHash(s + kSections, i - 1), b = motionHash(s + kSections, i);
            out[s] = a + (b - a) * g;
        }
        break;
    }
    default:
        for (int s = 0; s < kSections; ++s) out[s] = 0.0f;
        break;
    }
}

bool Divisi::stepMotion(int32_t frames) {
    const int shape = std::clamp(steppedTargetOf(Motion), 0, MotionShapeCount - 1);
    const int sync = std::clamp(steppedTargetOf(MotionSync), 0, kMotionSyncs);
    const double seconds = static_cast<double>(frames) / static_cast<double>(sampleRate);
    const double ticksPerSample = static_cast<double>(tempo) * kPPQN / (60.0 * static_cast<double>(sampleRate));
    bool jump = shape != motionShapeWas || sync != motionSyncWas;
    motionShapeWas = shape;
    motionSyncWas = sync;
    // Synced in an engine, the song's position sets it, so a bar always
    // sounds the same; otherwise it runs from reset() at the free rate or the
    // tempo, and a swell starts its rise again on a note from silence.
    const bool locked = sync > 0 && inSong;
    if (motionRestart && shape == MotionSwell && !locked) {
        motionPos = 0.0;
        jump = true;
    }
    motionRestart = false;
    const double perSecond = sync > 0 ? tempo / (60.0 * kMotionBeats[sync - 1]) : static_cast<double>(paramOf(MotionRate));
    motionPos += perSecond * seconds;
    double pos = motionPos;
    if (locked) {
        const double start = songClock;
        const double end = start + static_cast<double>(frames) * ticksPerSample;
        if (!motionLocked || std::fabs(start - lockedEnd) > 2.0) jump = true;
        lockedEnd = end;
        pos = end / (kMotionBeats[sync - 1] * kPPQN);
    } else if (motionLocked) {
        jump = true;
    }
    motionLocked = locked;
    // A render of part of a block carries on from where the last part ended.
    songClock += static_cast<double>(frames) * ticksPerSample;

    float raw[kMotionChannels];
    shapeAt(shape, pos, raw);
    // A jump glides out from where the sound was, unless the machine was silent.
    const float fade = std::exp(static_cast<float>(-seconds) / kMotionGlide);
    bool live = shape != MotionOff;
    for (int c = 0; c < kMotionChannels; ++c) {
        float &g = motionGlide[c];
        if (jump) g = sleptLast ? 0.0f : motionShown[c] - raw[c];
        else g *= fade;
        if (std::fabs(g) < 1e-4f) g = 0.0f;
        motionShown[c] = raw[c] + g;
        live = live || g != 0.0f;
    }
    return live;
}

void Divisi::seatPlayers() {
    for (int p = 0; p < kMaxPlayers; ++p) {
        Player &pl = players_[p];
        pl = Player();
        pl.rng = 0x2545f491u * static_cast<uint32_t>(p + 1) + 0x9e3779b9u;
        for (int i = 0; i < 3; ++i) nextRand(pl.rng);
        pl.target = nextRand(pl.rng) + nextRand(pl.rng) - 1.0f;
        pl.y1 = pl.y2 = pl.target;
        pl.nextPick = static_cast<int32_t>(nextRand(pl.rng) * kPickSpan * sampleRate);
        pl.vibPhase = nextRand(pl.rng);
        pl.vibRate = 1.0f + kVibSpread * (2.0f * nextRand(pl.rng) - 1.0f);
        pl.seat = p == 0 ? 0.0f : nextRand(pl.rng);
        pl.order = nextRand(pl.rng);
    }
    // The first player alone sits in the middle at full.
    players_[0].mid = 1.0f;
    dealRng = 0x51ed270bu;
    seatedFor = 0;
}

int Divisi::activeKeys() const {
    int n = 0;
    for (const Key &k : keys) n += k.used ? 1 : 0;
    return n;
}

int Divisi::ownChains() const {
    int n = 0;
    for (const Key &k : keys) n += (k.used && k.own) ? 1 : 0;
    return n;
}

int Divisi::heldCount() const {
    int n = 0;
    for (const Key &k : keys) n += (k.used && k.held) ? 1 : 0;
    return n;
}

Divisi::Key *Divisi::keyFor(uint8_t note) {
    for (Key &k : keys) {
        if (k.used && k.note == note) return &k;
    }
    for (Key &k : keys) {
        if (!k.used) return &k;
    }
    // The quietest let-go key, then the oldest held one.
    Key *pick = nullptr;
    for (Key &k : keys) {
        if (!k.held && (pick == nullptr || k.level < pick->level)) pick = &k;
    }
    if (pick != nullptr) return pick;
    pick = &keys[0];
    for (Key &k : keys) {
        if (k.age < pick->age) pick = &k;
    }
    return pick;
}

float Divisi::offset() const {
    return paramOf(Tune) / 100.0f + bend * paramOf(BendRange) + mac_[McPitch] * 24.0f;
}

void Divisi::formantsAt(float vowel, float hz[3], float k[3], float level[3]) {
    const float at = clampf(vowel, 0.0f, 1.0f) * 4.0f;
    const int i0 = std::min(static_cast<int>(at), 3);
    const float t = at - static_cast<float>(i0);
    const VowelShape &a = kVowels[i0], &b = kVowels[i0 + 1];
    for (int f = 0; f < 3; ++f) {
        hz[f] = a.hz[f] * std::pow(b.hz[f] / a.hz[f], t);
        const float q = clampf(hz[f] / (kFormantWidth + kFormantSpread * hz[f]), 1.0f, 20.0f);
        k[f] = 1.0f / q;
        level[f] = a.level[f] + (b.level[f] - a.level[f]) * t;
    }
}

float Divisi::choirPower(int row, int note) const {
    // The digital response of each Svf, from the bilinear transform: at
    // frequency f, x = tan(pi f / rate) / g.
    float hz[3], k[3], level[3];
    formantsAt(static_cast<float>(row) / static_cast<float>(kVowelRows - 1), hz, k, level);
    const float top = 0.45f * sampleRate;
    auto gOf = [&](float fc) { return std::tan(dsp::kPi * clampf(fc, 20.0f, top) / sampleRate); };
    float g[3];
    for (int j = 0; j < 3; ++j) g[j] = gOf(hz[j]);
    const float gHigh = gOf(kChoirHigh), gLow = gOf(kChoirLow);
    // Equal-tempered, so the cache holds under any tuning; a tuning moves a note too little to matter here.
    const float f8 = 440.0f * std::exp2((static_cast<float>(note) - 69.0f) / 12.0f);
    const float half = 0.5f * f8, end = std::fmin(kCompTop, top);
    double power = 0.0;
    for (int m = 1; half * static_cast<float>(m) < end; ++m) {
        // Harmonic m of the 16': odd ones are the 16' square's, even ones the
        // 8' square's and saw's, which share their phase.
        float amp;
        if (m & 1) {
            amp = kChoirSq16 * 2.0f / static_cast<float>(m);
        } else {
            const int n = m / 2;
            amp = ((n & 1) ? kChoirSq8 * 2.0f / static_cast<float>(n) : 0.0f) + kChoirSaw8 / static_cast<float>(n);
        }
        const float f = half * static_cast<float>(m);
        const float w = std::tan(dsp::kPi * f / sampleRate);
        Cx sum{0.0f, 0.0f};
        for (int j = 0; j < 3; ++j) {
            const float x = w / g[j];
            const Cx bp = divide({0.0f, x}, {1.0f - x * x, k[j] * x});
            sum.re += level[j] * k[j] * bp.re;
            sum.im += level[j] * k[j] * bp.im;
        }
        const float xh = w / gHigh, xl = w / gLow;
        const Cx hp = divide({-xh * xh, 0.0f}, {1.0f - xh * xh, 2.0f * xh});
        const Cx lp = divide({1.0f, 0.0f}, {1.0f - xl * xl, 2.0f * xl});
        const Cx h = mul(mul(sum, hp), lp);
        // As the ear weights it: about 4 dB more above 1.5 kHz.
        const float r = (f / 1500.0f) * (f / 1500.0f);
        const float ear = (1.0f + 2.5f * r) / (1.0f + r);
        power += static_cast<double>(amp * amp * (h.re * h.re + h.im * h.im) * ear);
    }
    return static_cast<float>(power);
}

float Divisi::compFor(float vowel, int note) const {
    note = std::clamp(note, 0, 127);
    const float at = clampf(vowel, 0.0f, 1.0f) * static_cast<float>(kVowelRows - 1);
    const int r0 = std::min(static_cast<int>(at), kVowelRows - 2);
    const float t = at - static_cast<float>(r0);
    const float c0 = compCache[r0][note], c1 = compCache[r0 + 1][note];
    return c0 + (c1 - c0) * t;
}

void Divisi::tuneChains() {
    const float off = offset();
    const double toStep = static_cast<double>(kTwoTo32) / static_cast<double>(sampleRate);
    // The dividers' lowest stage: the 32' of MIDI's lowest octave, tuned from
    // the middle octave's notes.
    for (int pc = 0; pc < 12; ++pc) {
        topHz[pc] = noteHz(60.0f + static_cast<float>(pc) + off) / 128.0f;
        topStep[pc] = static_cast<uint32_t>(std::fmin(static_cast<double>(topHz[pc]) * toStep, 2147483647.0));
    }
    for (Key &k : keys) {
        if (!k.used) continue;
        // The 8' the key wants against the 8' its divider gives.
        const float wanted = noteHz(static_cast<float>(k.sounding) + off + k.noteBend);
        const float divided = topHz[k.pc] * static_cast<float>(1 << k.octave) * 4.0f;
        if (!k.own && std::fabs(wanted / divided - 1.0f) > 1e-5f) {
            // Its own chain from here, starting where the shared one is.
            k.own = true;
            k.ownPhase = topPhase[k.pc] << k.octave;
        }
        if (k.own) k.ownStep = static_cast<uint32_t>(std::fmin(static_cast<double>(wanted) * 0.25 * toStep, 2147483647.0));
    }
}

bool Divisi::linesSpent() const {
    // The longest any player reads back, and any seat, with its chorus's swing.
    float player = 0.0f, seat = 0.0f;
    for (int p = 0; p < kMaxPlayers; ++p) {
        if (p < playerCount || players_[p].mid != 0.0f) player = std::fmax(player, players_[p].delay);
    }
    for (const Seat &st : seats_) {
        seat = std::fmax(seat, st.at.delay);
        for (float d : st.at.erDelay) seat = std::fmax(seat, d);
    }
    seat += (kSlowMs + kFastMs) * 0.001f * sampleRate;
    return static_cast<float>(lineHeard) > player + 64.0f && static_cast<float>(stageHeard) > player + seat + 64.0f;
}

void Divisi::noteOn(uint8_t note, uint8_t velocity) {
    updateMacroGoals(); // a note reads where the knobs are going, so renders repeat
    const bool poly = steppedTargetOf(Mode) == Poly;
    const bool first = heldCount() == 0;
    if (activeKeys() == 0) {
        motionRestart = true;
        // A note from silence: the players come in in a new order, at once,
        // when their lines hold nothing. While a room's tail still rings
        // through them they keep their seats, so their reads don't jump.
        if (asleep || linesSpent()) {
            const float scatter = clampf(goalOf(Scatter), 0.0f, 80.0f) * 0.001f * sampleRate;
            for (int p = 1; p < kMaxPlayers; ++p) {
                players_[p].seat = nextRand(dealRng);
                players_[p].seatDelay = scatter * players_[p].seat;
            }
        }
    }
    Key *k = keyFor(note);
    const bool fresh = !k->used || k->note != note;
    if (!poly && first) {
        // Paraphonic, from no keys down: whatever is still ringing from the
        // last chord lets go, and the new chord swells from where the
        // envelope is.
        for (Key &o : keys) {
            if (&o != k && o.used) o.gateOn = false;
        }
    }
    if (!poly && first) sharedBright.start();
    if (fresh) {
        *k = Key();
        k->note = note;
        k->sounding = std::clamp(static_cast<int>(note) + 12 * steppedTargetOf(Octave), 0, 127);
        k->pc = k->sounding % 12;
        k->octave = k->sounding / 12;
        partsOf(k->sounding, clampf(goalOf(Split), 0.0f, 1.0f), k->part);
        k->parted = false;
        for (float p : k->part) k->parted = k->parted || p != 1.0f;
    }
    const float vel = static_cast<float>(velocity) / 127.0f;
    k->gain = kHouse * velocityGain(vel, goalOf(VelocityAmount));
    k->vel = vel;
    // The brass scoops up into every note; a poly key opens its own filter.
    k->scoop = -kScoopMax * clampf(goalOf(Blat), 0.0f, 1.0f);
    if (poly) k->bright.start();
    k->held = true;
    k->gateOn = true;
    k->env.open = true;
    k->noteBend = 0.0f;
    k->pressure = -1.0f;
    k->used = true;
    k->age = ++clock;
    shared.open = true;
    asleep = false;
    quietSamples = 0;
}

void Divisi::noteOff(uint8_t note) {
    const bool poly = steppedTargetOf(Mode) == Poly;
    bool released = false;
    for (Key &k : keys) {
        if (k.used && k.held && k.note == note) {
            k.held = false;
            k.env.open = false;
            released = true;
        }
    }
    if (!released) return;
    if (heldCount() == 0) {
        // The last key up: the envelope lets the chord go as it is.
        shared.open = false;
    } else if (!poly) {
        for (Key &k : keys) {
            if (k.used && !k.held && k.note == note) k.gateOn = false;
        }
    }
}

void Divisi::allNotesOff() {
    for (Key &k : keys) {
        k.held = false;
        k.env.open = false;
    }
    shared.open = false;
}

void Divisi::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Divisi::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Divisi::noteBend(uint8_t note, float semitones) {
    for (Key &k : keys) {
        if (k.used && k.held && k.note == note) k.noteBend = semitones;
    }
}

void Divisi::notePressure(uint8_t note, uint8_t value) {
    for (Key &k : keys) {
        if (k.used && k.held && k.note == note) k.pressure = static_cast<float>(value) / 127.0f;
    }
}

namespace {
/** One over the overshoot's peak, the most its slow half less its fast half reaches. */
float overshootNorm() {
    const float t = kOverFast * kOverSlow / (kOverSlow - kOverFast) * std::log(kOverSlow / kOverFast);
    return 1.0f / (std::exp(-t / kOverSlow) - std::exp(-t / kOverFast));
}
} // namespace

float Divisi::stepBright(Bright &b, float target, float level, const BrightCoefs &c) {
    b.y = dsp::undenormal(b.y + (target - b.y) * (target > b.y ? c.up : c.down));
    b.fast *= c.fast;
    b.slow *= c.slow;
    if (b.slow < 1e-6f) b.fast = b.slow = 0.0f;
    // The fall runs as the note sounds, so a slow swell doesn't spend it.
    b.settle = dsp::undenormal(b.settle - b.settle * c.settle * level);
    return b.y * (kBrassFloor + (1.0f - kBrassFloor) * b.settle) + c.over * (b.slow - b.fast) * target;
}

namespace {
/** A pan from -1 to 1 as gains left and right, both 1 in the middle. */
inline void panGains(float pan, float g, float &l, float &r) {
    const float a = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * dsp::kPi;
    l = g * 1.41421356f * std::cos(a);
    r = g * 1.41421356f * std::sin(a);
}
} // namespace

void Divisi::partsOf(int note, float split, float part[kSections]) const {
    for (int s = 0; s < kSections; ++s) part[s] = 1.0f;
    if (split <= 0.0f) return;
    const float lvl[kSections] = {
        clampf(goalOf(Violin), 0.0f, 1.0f) * kViolinTrim, clampf(goalOf(Viola), 0.0f, 1.0f) * kViolaTrim,
        clampf(goalOf(Cello), 0.0f, 1.0f) * kCelloTrim, clampf(goalOf(Contrabass), 0.0f, 1.0f) * kBassTrim,
        clampf(goalOf(Choir), 0.0f, 1.0f) * kChoirTrim, clampf(goalOf(Brass), 0.0f, 1.0f) * kBrassTrim,
    };
    const float x = static_cast<float>(note);
    // An equal-power fade up across [a, b].
    auto rise = [](float v, float a, float b) {
        const float t = clampf((v - a) / (b - a), 0.0f, 1.0f);
        return std::sin(0.5f * dsp::kPi * t);
    };
    float r[kSections] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // The string sections playing, highest first; each hands over to the next one down halfway between them.
    int on[4], count = 0;
    for (int s = 0; s < 4; ++s) {
        if (lvl[s] > 0.0f) on[count++] = s;
    }
    for (int j = 0; j + 1 < count; ++j) {
        const int hi = on[j], lo = on[j + 1];
        const float mid = 0.5f * (kRangeCentre[hi] + kRangeCentre[lo]);
        const float up = rise(x, mid - kRangeFade, mid + kRangeFade);
        r[hi] *= up;
        r[lo] *= rise(-x, -mid - kRangeFade, -mid + kRangeFade);
    }
    if (count > 0) {
        // With strings to carry the rest, the choir and brass keep to the middle.
        r[4] = rise(x, kChoirRange[0], kChoirRange[1]) * rise(-x, -kChoirRange[3], -kChoirRange[2]);
        r[5] = rise(x, kBrassRange[0], kBrassRange[1]) * rise(-x, -kBrassRange[3], -kBrassRange[2]);
    }
    // The note keeps its power over the sections that play it.
    float all = 0.0f, kept = 0.0f;
    for (int s = 0; s < kSections; ++s) {
        part[s] = 1.0f + split * (r[s] - 1.0f);
        all += lvl[s] * lvl[s];
        kept += lvl[s] * lvl[s] * part[s] * part[s];
    }
    const float comp = kept > 0.0f ? std::fmin(std::pow(all / kept, kPartComp), 2.5f) : 1.0f;
    for (int s = 0; s < kSections; ++s) part[s] *= comp;
}

void Divisi::placeSeats(float stage, float room, float k, float ens, const float lv[kSections], SeatAt goal[kSections],
                        float &norm) const {
    // Delays are counted from where a seat at the stage's front middle hears it, plus the chorus's middle.
    const float centre = (std::round(kLineMs * 0.001f * sampleRate) - 1.0f) * k;
    const float top = static_cast<float>(seatSize - 2);
    const float perMetre = sampleRate / kSound;
    float before = 0.0f, after = 0.0f;
    // The reflections come off the section's line before its chorus, which takes some of the direct sound's power.
    const float thinned = std::sqrt(chorusPower(ens * k));
    // How far the sections' choruses have pulled them apart, from each other and from their reflections.
    const float apart = k * std::fmin(1.0f, 2.0f * ens + stage);
    const float share = kReflectionShare + (1.0f - kReflectionShare) * apart;
    for (int s = 0; s < kSections; ++s) {
        SeatAt &g = goal[s];
        const float x = kSeatX[s] * stage, y = kSeatY[s] * stage + kListener;
        const float r = std::sqrt(x * x + y * y);
        const float level = std::pow(kListener / r, kFall);
        const float pan = std::atan2(x, y) * kPanPerRadian;
        g.delay = std::fmin((r - kListener) * perMetre + centre, top);
        g.reach = k;
        panGains(pan, level, g.gL, g.gR);
        g.gW = kWidth[s] * level;
        g.air = clampf(kAirPerMetre * (r - kListener - kAirFrom), 0.0f, kAirMost);
        // The reflections, from the seat's images in the side walls.
        const float ix[kReflections] = {-2.0f * kWallX - x, 2.0f * kWallX - x};
        float energy = level * level;
        for (int j = 0; j < kReflections; ++j) {
            const float ri = std::sqrt(ix[j] * ix[j] + y * y);
            const float gi = room * kRoomLevel * kWallGain * kListener / ri * thinned;
            g.erDelay[j] = std::fmin((ri - kListener) * perMetre + centre, top);
            panGains(std::atan2(ix[j], y) * kPanPerRadian, gi, g.erL[j], g.erR[j]);
            energy += share * gi * gi;
        }
        // Into the ambience from where it sits, the back rows more.
        const float send = room * kSendLevel * thinned *
                           (kSendFront + (1.0f - kSendFront) * std::fmin(1.0f, (r - kListener) / kSendDepth));
        panGains(pan, send, g.sendL, g.sendR);
        energy += kAmbiencePower * send * send;
        before += lv[s] * lv[s];
        after += lv[s] * lv[s] * energy;
    }
    // The sections together keep their power: further back is quieter against the rest, not quieter overall.
    norm = after > 0.0f ? std::sqrt(before / after) : 1.0f;
    // In the middle the sections partly add as one, by amplitude, as the
    // dividers lock them; with choruses of their own, or far enough apart,
    // they add by power. Put back what that takes.
    float together = 0.0f;
    for (int i = 0; i < kSections; ++i) {
        for (int j = i + 1; j < kSections; ++j) {
            together += 2.0f * lv[i] * lv[j] * (i < 4 && j < 4 ? kLockedStrings : kLockedOthers);
        }
    }
    if (before > 0.0f) norm *= std::sqrt((before + together) / (before + (1.0f - apart) * together));
}

void Divisi::runStage(float sec[][64], bool staged, const SeatAt goal[kSections], const float ens0[kSections],
                      const float ens1[kSections], const float keep0[kSections], const float keep1[kSections], float ensMove,
                      float slowStep, float fastStep, float norm0, float norm1, float *outL, float *outR, float *outW, int32_t n,
                      const float *wow, const float *clockL, const float *clockR, bool aging) {
    const float perSample = 1.0f / static_cast<float>(n);
    float erInL[64], erInR[64], amInL[64], amInR[64], wetL[64], wetR[64];
    bool anyEr = false;
    if (staged) {
        for (int32_t i = 0; i < n; ++i) outL[i] = outR[i] = outW[i] = erInL[i] = erInR[i] = amInL[i] = amInR[i] = 0.0f;
        if (aging) for (int32_t i = 0; i < n; ++i) wetL[i] = wetR[i] = 0.0f;
    }
    const float airC = 1.0f - std::exp(-2.0f * dsp::kPi * kAirHz / sampleRate);
    for (int s = 0; s < kSections; ++s) {
        Seat &st = seats_[s];
        float *const buf = seatLines.data() + static_cast<size_t>(s) * static_cast<size_t>(seatSize);
        const float *x = sec[s];
        // The chorus's LFOs turn whether it's heard or not.
        const float slow0 = st.slowPhase, fast0 = st.fastPhase;
        st.slowPhase += slowStep * st.slowRate * static_cast<float>(n);
        st.fastPhase += fastStep * st.fastRate * static_cast<float>(n);
        st.slowPhase -= std::floor(st.slowPhase);
        st.fastPhase -= std::floor(st.fastPhase);

        // Counted to a chunk past the line's length, so the samples a skipped chunk leaves are silent ones too.
        bool silent = true;
        for (int32_t i = 0; i < n && silent; ++i) silent = x[i] == 0.0f;
        st.quiet = silent ? std::min(st.quiet + n, seatSize + 65) : 0;
        const SeatAt &g = goal[s];
        SeatAt &a = st.at;
        if (!staged) {
            // Where the stage starts from: in the middle, as the mix and its width-weighted twin.
            a = SeatAt();
            a.gW = kWidth[s];
        } else if (snapSeats) {
            a = g;
        }
        if (st.quiet > seatSize + 64) {
            // Its line is all zeros: nothing to write or read.
            st.airState = 0.0f;
            if (staged) a = g;
            continue;
        }
        if (!staged) {
            for (int32_t i = 0; i < n; ++i) buf[(seatWrite + i) & seatMask] = x[i];
            st.airState = 0.0f;
            continue;
        }
        // Air loss, then into the line.
        float airS = st.airState;
        const float air0 = a.air, dAir = (g.air - a.air) * perSample;
        for (int32_t i = 0; i < n; ++i) {
            airS = dsp::undenormal(airS + airC * (x[i] - airS));
            const float amt = air0 + dAir * static_cast<float>(i + 1);
            buf[(seatWrite + i) & seatMask] = x[i] + amt * (airS - x[i]);
        }
        st.airState = airS;

        auto readAt = [&](int32_t i, float d) {
            const int32_t whole = static_cast<int32_t>(d);
            const float frac = d - static_cast<float>(whole);
            const int32_t at = seatWrite + i - whole;
            const float p = buf[at & seatMask], q = buf[(at - 1) & seatMask];
            return p + frac * (q - p);
        };
        const float d0 = a.delay, dd = (g.delay - a.delay) * perSample;
        const float gl0 = a.gL, dgl = (g.gL - a.gL) * perSample;
        const float gr0 = a.gR, dgr = (g.gR - a.gR) * perSample;
        const float gw0 = a.gW, dgw = (g.gW - a.gW) * perSample;
        const float sl0 = a.sendL, dsl = (g.sendL - a.sendL) * perSample;
        const float sr0 = a.sendR, dsr = (g.sendR - a.sendR) * perSample;
        const bool chorus = (ens0[s] > 0.0f || ens1[s] > 0.0f) && (a.reach > 0.0f || g.reach > 0.0f);
        if (chorus) {
            // Its own chorus, the ensemble's shape: two reads a side, the
            // left moved one way and the right the other about the middle,
            // as deep as it has taken over.
            float m0[2], m1[2], dm[2];
            for (int j = 0; j < 2; ++j) {
                const float third = static_cast<float>(j) / 3.0f;
                m0[j] = a.reach * ensMove * (kSlowMs * lfoSin(slow0 + third) + kFastMs * lfoSin(fast0 + third));
                m1[j] = g.reach * ensMove * (kSlowMs * lfoSin(st.slowPhase + third) + kFastMs * lfoSin(st.fastPhase + third));
                dm[j] = (m1[j] - m0[j]) * perSample;
            }
            const float k0 = a.reach, dk = (g.reach - a.reach) * perSample;
            // Motion moves the depth and keeps the level; without it these are the knob and 1.
            const float e0 = ens0[s], de = (ens1[s] - ens0[s]) * perSample;
            const float c0 = keep0[s], dc = (keep1[s] - keep0[s]) * perSample;
            if (aging) {
                // The same, with the wet sound kept apart for the bucket, and its reads moved by the wander.
                for (int32_t i = 0; i < n; ++i) {
                    const float t = static_cast<float>(i + 1);
                    const float k = k0 + dk * t;
                    const float d = d0 + dd * t, ma = m0[0] + dm[0] * t, mb = m0[1] + dm[1] * t, wob = wow[i] * k;
                    const float depth = (e0 + de * t) * k;
                    const float keep = c0 + dc * t;
                    const float dry = readAt(i, d);
                    const float wl = 0.65f * readAt(i, d + ma + wob) + 0.35f * readAt(i, d + mb + wob);
                    const float wr = 0.65f * readAt(i, d - ma + wob) + 0.35f * readAt(i, d - mb + wob);
                    const float gl = (gl0 + dgl * t) * keep, gr = (gr0 + dgr * t) * keep;
                    outL[i] += gl * (dry - depth * dry);
                    outR[i] += gr * (dry - depth * dry);
                    wetL[i] += gl * depth * wl;
                    wetR[i] += gr * depth * wr;
                    outW[i] += (gw0 + dgw * t) * dry;
                    amInL[i] += (sl0 + dsl * t) * dry;
                    amInR[i] += (sr0 + dsr * t) * dry;
                }
            } else for (int32_t i = 0; i < n; ++i) {
                const float t = static_cast<float>(i + 1);
                const float d = d0 + dd * t, ma = m0[0] + dm[0] * t, mb = m0[1] + dm[1] * t;
                const float depth = (e0 + de * t) * (k0 + dk * t);
                const float keep = c0 + dc * t;
                const float dry = readAt(i, d);
                const float wl = 0.65f * readAt(i, d + ma) + 0.35f * readAt(i, d + mb);
                const float wr = 0.65f * readAt(i, d - ma) + 0.35f * readAt(i, d - mb);
                outL[i] += (gl0 + dgl * t) * keep * (dry + depth * (wl - dry));
                outR[i] += (gr0 + dgr * t) * keep * (dry + depth * (wr - dry));
                outW[i] += (gw0 + dgw * t) * dry;
                amInL[i] += (sl0 + dsl * t) * dry;
                amInR[i] += (sr0 + dsr * t) * dry;
            }
        } else {
            for (int32_t i = 0; i < n; ++i) {
                const float t = static_cast<float>(i + 1);
                const float dry = readAt(i, d0 + dd * t);
                outL[i] += (gl0 + dgl * t) * dry;
                outR[i] += (gr0 + dgr * t) * dry;
                outW[i] += (gw0 + dgw * t) * dry;
                amInL[i] += (sl0 + dsl * t) * dry;
                amInR[i] += (sr0 + dsr * t) * dry;
            }
        }
        bool still = true, heard = false;
        for (int j = 0; j < kReflections; ++j) {
            still = still && a.erDelay[j] == g.erDelay[j] && a.erL[j] == g.erL[j] && a.erR[j] == g.erR[j];
            heard = heard || a.erL[j] != 0.0f || a.erR[j] != 0.0f || g.erL[j] != 0.0f || g.erR[j] != 0.0f;
        }
        if (heard && still) {
            // The usual case, nothing moving: four fixed reads.
            anyEr = true;
            int32_t whole[kReflections];
            float frac[kReflections];
            for (int j = 0; j < kReflections; ++j) {
                whole[j] = static_cast<int32_t>(g.erDelay[j]);
                frac[j] = g.erDelay[j] - static_cast<float>(whole[j]);
            }
            for (int32_t i = 0; i < n; ++i) {
                const int32_t now = seatWrite + i;
                float l = 0.0f, r = 0.0f;
                for (int j = 0; j < kReflections; ++j) {
                    const int32_t at = now - whole[j];
                    const float p = buf[at & seatMask], q = buf[(at - 1) & seatMask];
                    const float v = p + frac[j] * (q - p);
                    l += g.erL[j] * v;
                    r += g.erR[j] * v;
                }
                erInL[i] += l;
                erInR[i] += r;
            }
        } else if (heard) {
            anyEr = true;
            for (int j = 0; j < kReflections; ++j) {
                const float e0 = a.erDelay[j], de = (g.erDelay[j] - a.erDelay[j]) * perSample;
                const float l0 = a.erL[j], dl = (g.erL[j] - a.erL[j]) * perSample;
                const float r0 = a.erR[j], dr = (g.erR[j] - a.erR[j]) * perSample;
                for (int32_t i = 0; i < n; ++i) {
                    const float t = static_cast<float>(i + 1);
                    const float v = readAt(i, e0 + de * t);
                    erInL[i] += (l0 + dl * t) * v;
                    erInR[i] += (r0 + dr * t) * v;
                }
            }
        }
        a = g;
    }
    seatWrite = (seatWrite + n) & seatMask;
    if (!staged) {
        erAir[0] = erAir[1] = 0.0f;
        return;
    }
    if (aging) {
        for (int32_t i = 0; i < n; ++i) {
            outL[i] += runAged(agedStage, 0, ageStage, wetL[i], clockL[i]);
            outR[i] += runAged(agedStage, 1, ageStage, wetR[i], clockR[i]);
        }
    }
    // The reflections lose their highs to the walls; then everything is levelled.
    const float erC = 1.0f - std::exp(-2.0f * dsp::kPi * kReflectionHz / sampleRate);
    if (anyEr || erAir[0] != 0.0f || erAir[1] != 0.0f) {
        float el = erAir[0], er = erAir[1];
        for (int32_t i = 0; i < n; ++i) {
            el = dsp::undenormal(el + erC * (erInL[i] - el));
            er = dsp::undenormal(er + erC * (erInR[i] - er));
            outL[i] += el;
            outR[i] += er;
        }
        erAir[0] = el;
        erAir[1] = er;
    }
    ambience.run(amInL, amInR, outL, outR, n);
    const float dn = (norm1 - norm0) * perSample;
    for (int32_t i = 0; i < n; ++i) {
        const float g = norm0 + dn * static_cast<float>(i + 1);
        outL[i] *= g;
        outR[i] *= g;
        outW[i] *= g;
    }
}

void Divisi::Ambience::prepare(float rate) {
    float longest = 0.0f, longestAp = 0.0f;
    for (int j = 0; j < 4; ++j) {
        len[j] = static_cast<int32_t>(kAmbienceMs[j] * 0.001f * rate);
        apLen[j] = static_cast<int32_t>(kDiffuseMs[j] * 0.001f * rate);
        longest = std::fmax(longest, static_cast<float>(len[j]));
        longestAp = std::fmax(longestAp, static_cast<float>(apLen[j]));
        // Each line's share of the decay, by its length.
        back[j] = std::pow(10.0f, -3.0f * static_cast<float>(len[j]) / (kAmbienceTime * rate));
    }
    lineSize = 1;
    while (static_cast<float>(lineSize) < longest + 2.0f) lineSize <<= 1;
    apSize = 1;
    while (static_cast<float>(apSize) < longestAp + 2.0f) apSize <<= 1;
    lineMask = lineSize - 1;
    apMask = apSize - 1;
    lines.assign(static_cast<size_t>(lineSize * 4), 0.0f);
    diffusers.assign(static_cast<size_t>(apSize * 4), 0.0f);
    dampCoef = 1.0f - std::exp(-2.0f * dsp::kPi * kAmbienceHz / rate);
    clear();
}

void Divisi::Ambience::clear() {
    std::fill(lines.begin(), lines.end(), 0.0f);
    std::fill(diffusers.begin(), diffusers.end(), 0.0f);
    for (float &d : damp) d = 0.0f;
    write = 0;
    quiet = lineSize * 8;
}

void Divisi::Ambience::run(const float *inL, const float *inR, float *outL, float *outR, int32_t n) {
    bool silent = true;
    for (int32_t i = 0; i < n && silent; ++i) silent = inL[i] == 0.0f && inR[i] == 0.0f;
    // Long after its input stopped it has rung out: nothing to do. (A
    // whisper is left in its lines, the same every time.)
    if (silent && quiet > lineSize * 4) {
        write = (write + n) & lineMask;
        return;
    }
    float *const ap = diffusers.data();
    float *const ln = lines.data();
    float peak = 0.0f;
    for (int32_t i = 0; i < n; ++i) {
        const int32_t w = (write + i);
        // One input, the sum, smeared two different ways.
        const float sum = 0.5f * (inL[i] + inR[i]);
        float x[2] = {sum, sum};
        for (int c = 0; c < 2; ++c) {
            for (int a = 0; a < 2; ++a) {
                const int j = 2 * c + a;
                float *b = ap + j * apSize;
                const float delayed = b[(w - apLen[j]) & apMask];
                const float v = x[c] - kDiffuse * delayed;
                b[w & apMask] = v;
                x[c] = delayed + kDiffuse * v;
            }
        }
        float y[4];
        for (int j = 0; j < 4; ++j) {
            const float d = ln[j * lineSize + ((w - len[j]) & lineMask)];
            damp[j] = dsp::undenormal(damp[j] + dampCoef * (d - damp[j]));
            y[j] = damp[j];
        }
        // An orthogonal mix of the four, so the loop keeps its energy but for each line's decay.
        const float s01 = y[0] + y[1], d01 = y[0] - y[1], s23 = y[2] + y[3], d23 = y[2] - y[3];
        const float f[4] = {0.5f * (s01 + s23), 0.5f * (d01 + d23), 0.5f * (s01 - s23), 0.5f * (d01 - d23)};
        const float feed[4] = {x[0], x[1], x[0], -x[1]};
        for (int j = 0; j < 4; ++j) ln[j * lineSize + (w & lineMask)] = back[j] * f[j] + feed[j];
        const float l = kAmbienceOut * (y[0] + y[2]), r = kAmbienceOut * (y[1] - y[3]);
        outL[i] += l;
        outR[i] += r;
        peak = std::fmax(peak, std::fmax(std::fabs(l), std::fabs(r)));
    }
    write = (write + n) & lineMask;
    quiet = (silent && peak < 1e-7f) ? std::min(quiet + n, lineSize * 8) : 0;
}

void Divisi::runPlayers(const float *mid, const float *right, const float *wide, bool stereo, float tilt0, float tilt1,
                         float width, float *outL, float *outR, int32_t n) {
    float *const bufM = playerLines.data();
    float *const bufW = bufM + lineSize;
    float *const bufR = bufW + lineSize;
    // Lines that have heard only silence for their whole length are all
    // zeros: nothing to write or read. Counted to a chunk past the length,
    // so the samples a skipped chunk leaves are silent ones too.
    bool silent = true;
    for (int32_t i = 0; i < n && silent; ++i) silent = mid[i] == 0.0f && wide[i] == 0.0f && right[i] == 0.0f;
    lineQuiet = silent ? std::min(lineQuiet + n, lineSize + 65) : 0;
    const bool empty = lineQuiet > lineSize + 64;
    if (!empty) {
        for (int32_t i = 0; i < n; ++i) {
            const int32_t w = (lineWrite + i) & lineMask;
            bufM[w] = mid[i];
            bufW[w] = wide[i];
            bufR[w] = right[i];
        }
    }

    const float dt = static_cast<float>(n) / sampleRate;
    const float walk = 1.0f - std::exp(-dt / kWalkTime);
    const float glide = 1.0f - std::exp(-dt / kGainTime);
    // The walk's delay stays within this either way, seconds, so it reads from the middle of its reach.
    const float bound = kDriftHold * driftRatio;
    const float seatStep = kSeatGlide * static_cast<float>(n);
    const float top = static_cast<float>(lineSize - 2);
    const float perSample = 1.0f / static_cast<float>(n);
    for (int p = 0; p < kMaxPlayers; ++p) {
        Player &pl = players_[p];
        // Every player walks whether it plays or not, so adding one doesn't move the others.
        pl.nextPick -= n;
        if (pl.nextPick <= 0) {
            pl.target = nextRand(pl.rng) + nextRand(pl.rng) - 1.0f;
            pl.nextPick += static_cast<int32_t>((kPickMin + kPickSpan * nextRand(pl.rng)) * sampleRate);
        }
        pl.y1 += (pl.target - pl.y1) * walk;
        pl.y2 += (pl.y1 - pl.y2) * walk;
        // The delay's speed is the pitch's departure, pulled back to the middle.
        pl.x = clampf(pl.x + (driftRatio * pl.y2 - pl.x / kDriftHold) * dt, -bound, bound);
        const float vibF = vibHz * pl.vibRate;
        pl.vibPhase += vibF * dt;
        pl.vibPhase -= std::floor(pl.vibPhase);
        const float vibDelay = vibRatio / (2.0f * dsp::kPi * vibF) * (1.0f + lfoSin(pl.vibPhase));
        const float seatGoal = scatterSamples * pl.seat;
        pl.seatDelay = pl.seatDelay < seatGoal ? std::fmin(seatGoal, pl.seatDelay + seatStep)
                                               : std::fmax(seatGoal, pl.seatDelay - seatStep);
        const float d1 = clampf(pl.seatDelay + (pl.x + bound + vibDelay) * sampleRate, 0.0f, top);
        const float d0 = snapPlayers ? d1 : pl.delay;
        pl.delay = d1;

        const float tM = p < playerCount ? playerNorm : 0.0f;
        const float tS = tM * pl.pos;
        const float m0 = snapPlayers ? tM : pl.mid, s0 = snapPlayers ? tS : pl.side;
        pl.mid = std::fabs(tM - m0) < 1e-6f ? tM : m0 + (tM - m0) * glide;
        pl.side = std::fabs(tS - s0) < 1e-6f ? tS : s0 + (tS - s0) * glide;
        if (empty || (m0 == 0.0f && pl.mid == 0.0f)) continue;

        const float dd = (d1 - d0) * perSample;
        const float dm = (pl.mid - m0) * perSample;
        if (stereo) {
            // The stage's left and right. A player's place tips the balance
            // between them, as far as the sections are wide on average; until
            // the sections' choruses have taken over, partly by the
            // width-weighted mix as in the middle.
            const float ds = (pl.side - s0) * perSample;
            const float dTilt = (tilt1 - tilt0) * perSample;
            if (tilt0 == 1.0f && tilt1 == 1.0f) {
                for (int32_t i = 0; i < n; ++i) {
                    const float t = static_cast<float>(i + 1);
                    const float d = d0 + dd * t;
                    const int32_t whole = static_cast<int32_t>(d);
                    const float frac = d - static_cast<float>(whole);
                    const int32_t at = lineWrite + i - whole, i0 = at & lineMask, i1 = (at - 1) & lineMask;
                    const float g = m0 + dm * t, b = (s0 + ds * t) * width;
                    outL[i] += (g - b) * (bufM[i0] + frac * (bufM[i1] - bufM[i0]));
                    outR[i] += (g + b) * (bufR[i0] + frac * (bufR[i1] - bufR[i0]));
                }
                continue;
            }
            for (int32_t i = 0; i < n; ++i) {
                const float t = static_cast<float>(i + 1);
                const float d = d0 + dd * t;
                const int32_t whole = static_cast<int32_t>(d);
                const float frac = d - static_cast<float>(whole);
                const int32_t at = lineWrite + i - whole, i0 = at & lineMask, i1 = (at - 1) & lineMask;
                const float g = m0 + dm * t, sd = s0 + ds * t, h = tilt0 + dTilt * t;
                const float l = bufM[i0] + frac * (bufM[i1] - bufM[i0]), r = bufR[i0] + frac * (bufR[i1] - bufR[i0]);
                const float w = (1.0f - h) * sd * (bufW[i0] + frac * (bufW[i1] - bufW[i0]));
                const float b = h * sd * width;
                outL[i] += (g - b) * l - w;
                outR[i] += (g + b) * r + w;
            }
            continue;
        }
        if (s0 == 0.0f && pl.side == 0.0f) {
            // In the middle: the mix alone.
            for (int32_t i = 0; i < n; ++i) {
                const float t = static_cast<float>(i + 1);
                const float d = d0 + dd * t;
                const int32_t whole = static_cast<int32_t>(d);
                const float frac = d - static_cast<float>(whole);
                const int32_t at = lineWrite + i - whole;
                const float a = bufM[at & lineMask], b = bufM[(at - 1) & lineMask];
                const float v = (m0 + dm * t) * (a + frac * (b - a));
                outL[i] += v;
                outR[i] += v;
            }
            continue;
        }
        const float ds = (pl.side - s0) * perSample;
        for (int32_t i = 0; i < n; ++i) {
            const float t = static_cast<float>(i + 1);
            const float d = d0 + dd * t;
            const int32_t whole = static_cast<int32_t>(d);
            const float frac = d - static_cast<float>(whole);
            const int32_t at = lineWrite + i - whole, i0 = at & lineMask, i1 = (at - 1) & lineMask;
            const float v = (m0 + dm * t) * (bufM[i0] + frac * (bufM[i1] - bufM[i0]));
            const float w = (s0 + ds * t) * (bufW[i0] + frac * (bufW[i1] - bufW[i0]));
            outL[i] += v - w;
            outR[i] += v + w;
        }
    }
}

bool Divisi::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    // The motion turns while asleep too, so where it is depends only on time since reset().
    const bool moving = stepMotion(frames);
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    const float speed = std::exp2((clampf(paramOf(Speed), 0.0f, 1.0f) - 0.5f) * 2.0f);
    const float slowStep = kSlowHz * speed / sampleRate, fastStep = kFastHz * speed / sampleRate;
    const float phaserStep = paramOf(Rate) / sampleRate;
    if (asleep) {
        // The dividers and the LFOs keep turning.
        for (int pc = 0; pc < 12; ++pc) topPhase[pc] += topStep[pc] * static_cast<uint32_t>(frames);
        slowPhase += slowStep * static_cast<float>(frames);
        fastPhase += fastStep * static_cast<float>(frames);
        phaserPhase += phaserStep * static_cast<float>(frames);
        slowPhase -= std::floor(slowPhase);
        fastPhase -= std::floor(fastPhase);
        phaserPhase -= std::floor(phaserPhase);
        for (Seat &st : seats_) {
            st.slowPhase += slowStep * st.slowRate * static_cast<float>(frames);
            st.fastPhase += fastStep * st.fastRate * static_cast<float>(frames);
            st.slowPhase -= std::floor(st.slowPhase);
            st.fastPhase -= std::floor(st.fastPhase);
        }
        // Age's wander and warble keep turning too.
        for (int32_t at = 0; at < frames; at += 64) {
            const int32_t n = std::min<int32_t>(64, frames - at);
            float w0, w1;
            stepWow(n, w0, w1);
            warblePhase += kWarbleHz * (1.0f + kWarbleWander * w1) / sampleRate * static_cast<float>(n);
            warblePhase -= std::floor(warblePhase);
        }
        sleptLast = true;
        motionWasLive = false;
        return true;
    }

    tuneChains();
    const bool poly = steppedTargetOf(Mode) == Poly;
    const float attack = std::fmax(paramOf(Attack), 0.001f);
    const float release = std::fmax(paramOf(Release), 0.005f);
    const float rise = 1.0f / (attack * sampleRate);
    const float fall = std::pow(10.0f, -5.0f / (release * sampleRate));
    const float gateRise = 1.0f / (kGateRise * sampleRate), gateFall = 1.0f / (kGateFall * sampleRate);
    const float sweep = clampf(paramOf(Sweep), 0.0f, 1.0f);

    // The registers' levels and voicings.
    const float tone = std::exp2((clampf(paramOf(Tone), 0.0f, 1.0f) - 0.5f) * 3.0f);
    const float lv[6] = {
        clampf(paramOf(Violin), 0.0f, 1.0f) * kViolinTrim, clampf(paramOf(Viola), 0.0f, 1.0f) * kViolaTrim,
        clampf(paramOf(Cello), 0.0f, 1.0f) * kCelloTrim, clampf(paramOf(Contrabass), 0.0f, 1.0f) * kBassTrim,
        clampf(paramOf(Choir), 0.0f, 1.0f) * kChoirTrim, clampf(paramOf(Brass), 0.0f, 1.0f) * kBrassTrim,
    };
    const float ens = clampf(paramOf(Ensemble), 0.0f, 1.0f);

    // Motion: each section's gain, brightness and chorus depth, and the
    // choir's vowel, for the end of this block. The gains and depths ramp
    // from where the last block left them; without motion they are 1 and
    // the knobs, and nothing is ramped.
    const bool ramping = moving || motionWasLive;
    float toneMove[kSections] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float vowelMove = 0.0f;
    float gainFrom[kSections], gainTo[kSections], ensFrom[kSections], ensTo[kSections], keepFrom[kSections], keepTo[kSections];
    float ensSharedTo = ens, keepSharedTo = 1.0f;
    for (int s = 0; s < kSections; ++s) {
        gainTo[s] = 1.0f;
        ensTo[s] = ens;
        keepTo[s] = 1.0f;
    }
    if (moving) {
        const float *m = motionShown;
        const float byBalance = clampf(paramOf(MotionBalance), 0.0f, 1.0f), byVowel = clampf(paramOf(MotionVowel), 0.0f, 1.0f);
        const float byTone = clampf(paramOf(MotionTone), 0.0f, 1.0f), byEns = clampf(paramOf(MotionEnsemble), 0.0f, 1.0f);
        if (byBalance > 0.0f) {
            // Each section up or down, the sections together kept as loud; the swell's level on top.
            constexpr float kOctavesPerDb = 1.0f / 6.0206f;
            float before = 0.0f, after = 0.0f, g[kSections];
            for (int s = 0; s < kSections; ++s) {
                g[s] = std::exp2(kBalanceDb * kOctavesPerDb * byBalance * m[s]);
                before += lv[s] * lv[s];
                after += lv[s] * lv[s] * g[s] * g[s];
            }
            const float norm = after > 0.0f ? std::sqrt(before / after) : 1.0f;
            const float level = std::exp2(2.0f * kBalanceDb * kOctavesPerDb * byBalance * m[kSections]);
            for (int s = 0; s < kSections; ++s) gainTo[s] = g[s] * norm * level;
        }
        if (byTone > 0.0f) {
            for (int s = 0; s < kSections; ++s) toneMove[s] = std::exp2(kToneOctaves * byTone * m[s]);
        }
        vowelMove = kVowelReach * byVowel * m[4];
        if (byEns > 0.0f) {
            // Each section's chorus deeper or shallower, as loud; the shared one by the sections' mean, by level.
            const float base = chorusPower(ens);
            float mean = 0.0f, sum = 0.0f;
            for (int s = 0; s < kSections; ++s) {
                ensTo[s] = clampf(ens + kEnsembleReach * byEns * m[s], 0.0f, 1.0f);
                keepTo[s] = std::sqrt(base / chorusPower(ensTo[s]));
                mean += lv[s] * m[s];
                sum += lv[s];
            }
            mean = sum > 0.0f ? mean / sum : m[0];
            ensSharedTo = clampf(ens + kEnsembleReach * byEns * mean, 0.0f, 1.0f);
            keepSharedTo = std::sqrt(base / chorusPower(ensSharedTo));
        }
    }
    float ensSharedFrom = ensSharedTo, keepSharedFrom = keepSharedTo;
    for (int s = 0; s < kSections; ++s) {
        gainFrom[s] = motionWasLive ? motionGain[s] : gainTo[s];
        ensFrom[s] = motionWasLive ? motionEns[s] : ensTo[s];
        keepFrom[s] = motionWasLive ? motionEnsKeep[s] : keepTo[s];
        motionGain[s] = gainTo[s];
        motionEns[s] = ensTo[s];
        motionEnsKeep[s] = keepTo[s];
    }
    if (motionWasLive) {
        ensSharedFrom = motionEnsShared;
        keepSharedFrom = motionEnsSharedKeep;
    }
    motionEnsShared = ensSharedTo;
    motionEnsSharedKeep = keepSharedTo;
    motionWasLive = moving;

    violinHigh.set(280.0f, 0.0f);
    violinLow.set(5500.0f * tone * toneMove[0], 0.2f);
    violaHigh.set(140.0f, 0.0f);
    violaLow.set(2400.0f * tone * toneMove[1], 0.15f);
    celloHigh.set(45.0f, 0.0f);
    celloLow.set(1600.0f * tone * toneMove[2], 0.15f);
    bassLow.set(450.0f * tone * toneMove[3], 0.1f);
    choirHigh.set(kChoirHigh, 0.0f);
    choirLow.set(kChoirLow * tone * toneMove[4], 0.0f);
    const float vowel = clampf(paramOf(Vowel) + vowelMove, 0.0f, 1.0f);
    float formantLevel[3];
    {
        float hz[3], k[3], level[3];
        formantsAt(vowel, hz, k, level);
        for (int f = 0; f < 3; ++f) {
            formant[f].set(hz[f], (2.0f - k[f]) / 1.96f);
            formantLevel[f] = level[f] * formant[f].bandNorm();
        }
    }
    const float reach = 0.4f + 0.6f * ens;
    const float msToSamples = 0.001f * sampleRate;
    // The line's middle, a whole number of samples so the dry sound isn't smoothed by the read.
    const float centre = std::round(kLineMs * msToSamples);
    const float phaser = clampf(paramOf(Phaser), 0.0f, 1.0f);
    const float feedback = clampf(paramOf(Feedback), 0.0f, 0.9f);
    const float volume = paramOf(Volume);
    // Age, for the shared chorus (after the volume) and the sections' (before it).
    const float age = clampf(paramOf(Age), 0.0f, 1.0f);
    const bool aging = age > 0.0f;
    if (aging) {
        ageSettings(age, kAgeNominal, 1.0f, ageShared);
        ageSettings(age, kAgeNominal / std::fmax(volume, 0.05f), kAgeStageNoise, ageStage);
    }
    const float wowSamples = age * kWowMs * msToSamples, warbleSamples = age * kWarbleMs * msToSamples;
    const float warbleStep = kWarbleHz / sampleRate;

    // Each key's pressure as a swell, ramped across the block.
    // And its choir level, ramped the same way; a fresh key starts at its own.
    float swellFrom[kKeys], swellTo[kKeys], compFrom[kKeys], compTo[kKeys];
    const bool choirOn = lv[4] > 0.0f;
    for (int i = 0; i < kKeys; ++i) {
        Key &k = keys[i];
        if (!k.used) continue;
        swellFrom[i] = k.swell;
        k.swell = 1.0f + 0.5f * glidePressure(k.pressed, k.held ? k.pressure : 0.0f, k.held ? channelPressure_ : 0.0f);
        swellTo[i] = k.swell;
        if (choirOn) {
            const float c = compFor(vowel, k.sounding);
            compFrom[i] = k.choirComp > 0.0f ? k.choirComp : c;
            k.choirComp = c;
            compTo[i] = c;
        }
    }
    const float perFrame = 1.0f / static_cast<float>(frames);

    // The brass: its brightness steps, cutoff, scoop and drive.
    const bool brassOn = lv[5] > 0.0f;
    const float blat = clampf(paramOf(Blat), 0.0f, 1.0f);
    const float coefRate = sampleRate / static_cast<float>(kBrassEvery);
    static const float overNorm = overshootNorm();
    BrightCoefs bc;
    bc.up = dsp::onePoleCoeff(kBrightUp + kBrightUpBlat * blat, coefRate);
    bc.down = dsp::onePoleCoeff(kBrightDown, coefRate);
    bc.fast = std::exp(-1.0f / (kOverFast * coefRate));
    bc.slow = std::exp(-1.0f / (kOverSlow * coefRate));
    bc.settle = dsp::onePoleCoeff(paramOf(BrassDecay) / 4.6f, coefRate);
    bc.over = kOverMax * blat * overNorm;
    const float brassBase = kBrassClosed * tone * toneMove[5], brassOct = kBrassOctaves * sweep;
    const float brassK = 2.0f - 1.96f * kBrassRes;
    const float brassTop = 0.45f * sampleRate, piOverRate = dsp::kPi / sampleRate;
    const float scoopFall = std::exp(-1.0f / (kScoopTime * sampleRate));
    const float scoopChunk = std::pow(scoopFall, 64.0f);
    // Paraphonic: the loudest key sounding sets how bright the shared filter gets.
    float paraVel = 0.0f;
    for (const Key &k : keys) {
        if (k.used && k.gateOn) paraVel = std::fmax(paraVel, k.vel);
    }
    const float paraVelB = kBrassVelFloor + (1.0f - kBrassVelFloor) * paraVel;

    // The players, and where each sits for this count.
    playerCount = std::clamp(static_cast<int>(std::lround(goalOf(Players))), 1, kMaxPlayers);
    if (playerCount != seatedFor) {
        // Their places across a section, evenly spread in each player's own order.
        seatedFor = playerCount;
        for (int p = 0; p < playerCount; ++p) {
            int rank = 0;
            for (int q = 0; q < playerCount; ++q) {
                const float oq = players_[q].order, op = players_[p].order;
                rank += (oq < op || (oq == op && q < p)) ? 1 : 0;
            }
            players_[p].pos = playerCount == 1 ? 0.0f
                : (2.0f * static_cast<float>(rank) + 1.0f) / static_cast<float>(playerCount) - 1.0f;
        }
    }
    {
        const float drift = clampf(paramOf(Drift), 0.0f, 25.0f), vib = clampf(paramOf(Vibrato), 0.0f, 60.0f);
        const float scatterMs = clampf(paramOf(Scatter), 0.0f, 80.0f);
        driftRatio = drift * kPerCent;
        vibRatio = vib * kPerCent;
        vibHz = clampf(paramOf(Vibrate), 2.0f, 9.0f);
        scatterSamples = scatterMs * 0.001f * sampleRate;
        // Players reading the same place add as one louder sound, by
        // amplitude; spread out, they add by power. In between, between.
        const float a = kSpreadScatter * scatterMs, b = kSpreadDrift * drift, c = kSpreadVibrato * vib * 5.5f / vibHz;
        const float together = std::exp(-(a * a + b * b + c * c) / (kSpreadLoose * kSpreadLoose));
        if (playerCount == 1) {
            playerNorm = 1.0f;
        } else {
            const float count = static_cast<float>(playerCount);
            const float sides = (count * count - 1.0f) / (3.0f * count * count); // the mean of a place squared
            playerNorm = std::pow(count, -(0.5f + 0.5f * together)) / std::sqrt(1.0f + kWideShare * sides);
        }
    }

    // The stage: the knobs glided, the sections' choruses' share, and each seat.
    {
        const float stageGoal = clampf(paramOf(Stage), 0.0f, 1.0f), roomGoal = clampf(paramOf(Room), 0.0f, 1.0f);
        if (snapSeats) {
            stageNow = stageGoal;
            roomNow = roomGoal;
        } else {
            const float c = 1.0f - std::exp(-static_cast<float>(frames) / (kSeatTime * sampleRate));
            stageNow = std::fabs(stageGoal - stageNow) < 1e-5f ? stageGoal : stageNow + (stageGoal - stageNow) * c;
            roomNow = std::fabs(roomGoal - roomNow) < 1e-5f ? roomGoal : roomNow + (roomGoal - roomNow) * c;
        }
    }
    // The sections' choruses take over in a moment once the stage is open at all, and hand back once it's shut.
    const float handGoal = stageNow > 0.0f ? 1.0f : 0.0f;
    if (snapSeats) {
        handNow = handGoal;
    }
    const float handFrom = handNow;
    {
        const float step = static_cast<float>(frames) / (kHandTime * sampleRate);
        handNow = handGoal > handNow ? std::fmin(handGoal, handNow + step) : std::fmax(handGoal, handNow - step);
    }
    // Staged too while the ambience still rings.
    const bool staged = stageNow > 0.0f || roomNow > 0.0f || handNow > 0.0f || handFrom > 0.0f || ambience.ringing();
    SeatAt seatGoal[kSections];
    // The sections' width on average, by level, for the players' places on the stage.
    float meanWidth = 0.0f;
    {
        float sum = 0.0f;
        for (int s = 0; s < kSections; ++s) {
            meanWidth += kWidth[s] * lv[s];
            sum += lv[s];
        }
        meanWidth = sum > 0.0f ? meanWidth / sum : 0.0f;
    }
    const float normFrom = stageNorm;
    if (staged) {
        // Worked out again only when something they depend on has moved.
        const float key[4 + kSections] = {stageNow, roomNow, handNow, ens, lv[0], lv[1], lv[2], lv[3], lv[4], lv[5]};
        if (!std::equal(key, key + 4 + kSections, placedFor)) {
            placeSeats(stageNow, roomNow, handNow, ens, lv, placed, placedNorm);
            std::copy(key, key + 4 + kSections, placedFor);
        }
        std::copy(placed, placed + kSections, seatGoal);
        stageNorm = placedNorm;
    } else {
        stageNorm = 1.0f;
    }
    const float normStart = snapSeats ? stageNorm : normFrom;

    float outPeak = 0.0f;
    constexpr int32_t kChunk = 64;
    float mid[kChunk], wide[kChunk], outL[kChunk], outR[kChunk], envAt[kChunk];
    float right[kChunk], sec[kSections][kChunk];
    float bus4[kChunk], bus8[kChunk], bus16[kChunk], bus32[kChunk], busSq[kChunk], busBr[kChunk];
    // The string sections' inputs from keys split between them.
    float partIn[4][kChunk];
    bool anyParted = false;
    for (const Key &k : keys) anyParted = anyParted || (k.used && k.parted);
    if (!staged || !aging) agedStage.clear(0x1b873593u, true);
    if (!aging) agedShared.clear(0x6d2b79f5u, true);
    float wowAt[kChunk], clockL[kChunk], clockR[kChunk];
    for (int32_t at = 0; at < frames; at += kChunk) {
        const int32_t n = std::min(kChunk, frames - at);
        // Age's wander and warble over this chunk, in samples, and the whine's steps from the shared chorus's delay a side.
        {
            float w0, w1;
            stepWow(n, w0, w1);
            if (aging) {
                const float dw = (w1 - w0) / static_cast<float>(n);
                for (int32_t i = 0; i < n; ++i) {
                    const float w = w0 + dw * static_cast<float>(i + 1);
                    warblePhase += warbleStep * (1.0f + kWarbleWander * w);
                    if (warblePhase >= 1.0f) warblePhase -= 1.0f;
                    wowAt[i] = wowSamples * w + warbleSamples * lfoSin(warblePhase);
                }
                if (ageShared.whine > 0.0f) {
                    for (int32_t i = 0; i < n; ++i) {
                        const float fi = static_cast<float>(i);
                        const float m = reach * msToSamples *
                                        (kSlowMs * lfoSin(slowPhase + slowStep * fi) + kFastMs * lfoSin(fastPhase + fastStep * fi));
                        clockL[i] = kWhineTurns / (centre + m);
                        clockR[i] = kWhineTurns / (centre - m);
                    }
                } else {
                    for (int32_t i = 0; i < n; ++i) clockL[i] = clockR[i] = 0.0f;
                }
            } else {
                warblePhase += warbleStep * (1.0f + kWarbleWander * w1) * static_cast<float>(n);
                warblePhase -= std::floor(warblePhase);
            }
        }
        for (int32_t i = 0; i < n; ++i) bus4[i] = bus8[i] = bus16[i] = bus32[i] = busSq[i] = busBr[i] = 0.0f;
        if (anyParted) {
            for (int32_t i = 0; i < n; ++i) partIn[0][i] = partIn[1][i] = partIn[2][i] = partIn[3][i] = 0.0f;
        }

        for (int ki = 0; ki < kKeys; ++ki) {
            Key &k = keys[ki];
            if (!k.used) continue;
            // The key's 32' phase and step; the higher footages are shifts of it.
            const int shift = k.octave;
            uint32_t phase = k.own ? k.ownPhase : (topPhase[k.pc] + topStep[k.pc] * static_cast<uint32_t>(at)) << shift;
            const uint32_t step = k.own ? k.ownStep : topStep[k.pc] << shift;
            // A footage is played while it's under 0.45 of the rate and its step hasn't wrapped.
            const double hz32 = (k.own ? static_cast<double>(k.ownStep) : static_cast<double>(topStep[k.pc]) * static_cast<double>(1 << shift)) *
                                static_cast<double>(sampleRate) / static_cast<double>(kTwoTo32);
            const double limit = 0.45 * static_cast<double>(sampleRate);
            const bool has32 = hz32 < limit, has16 = hz32 * 2.0 < limit, has8 = hz32 * 4.0 < limit, has4 = hz32 * 8.0 < limit;
            float peak = 0.0f;

            // The brass saw's 16' step across the chunk, gliding up out of the scoop.
            const float r0 = k.scoop == 0.0f ? 1.0f : std::exp2(k.scoop / 12.0f);
            k.scoop *= n == kChunk ? scoopChunk : std::pow(scoopFall, static_cast<float>(n));
            if (k.scoop > -1e-4f) k.scoop = 0.0f;
            const float r1 = k.scoop == 0.0f ? 1.0f : std::exp2(k.scoop / 12.0f);
            const bool kb = brassOn && has16 && k.part[5] > 0.0f;
            const uint32_t step16 = step << 1;
            const bool exact = r0 == 1.0f;
            float bstep = static_cast<float>(step16) * r0;
            const float dstep = static_cast<float>(step16) * (r1 - r0) / static_cast<float>(n);
            const float velB = kBrassVelFloor + (1.0f - kBrassVelFloor) * k.vel;
            const float trackOct = kBrassTrack * static_cast<float>(k.sounding - 60) / 12.0f;
            if (kb && !k.brassOn) {
                // From the divider's own phase, so it starts in step with the strings.
                k.brassOn = true;
                k.brassPhase = phase << 1;
                k.brassLp = BrassLp();
                k.brassCountdown = 0;
            } else if (!kb) {
                k.brassOn = false;
            }
            if (kb && !poly) {
                k.drive = kDriveLow + kDriveSpan * shared.level * velB;
                k.invDrive = 1.0f / k.drive;
            }
            for (int32_t i = 0; i < n; ++i) {
                // Its switch, and its own envelope.
                if (k.gateOn) k.gate = std::fmin(1.0f, k.gate + gateRise);
                else k.gate = std::fmax(0.0f, k.gate - gateFall);
                k.env.step(rise, fall);
                const float swell = swellFrom[ki] + (swellTo[ki] - swellFrom[ki]) * static_cast<float>(at + i + 1) * perFrame;
                const float g = k.gain * swell * k.gate * (poly ? k.env.level : 1.0f);
                if (g > 0.0f) {
                    const float s32 = has32 ? sawOf(phase, step) : 0.0f;
                    const float s16 = has16 ? sawOf(phase << 1, step << 1) : 0.0f;
                    const float s8 = has8 ? sawOf(phase << 2, step << 2) : 0.0f;
                    const float s4 = has4 ? sawOf(phase << 3, step << 3) : 0.0f;
                    if (k.parted) {
                        // Each section its own share of the key.
                        partIn[0][i] += g * k.part[0] * (s8 + 0.35f * s4);
                        partIn[1][i] += g * k.part[1] * (s8 + 0.25f * s16);
                        partIn[2][i] += g * k.part[2] * s16;
                        partIn[3][i] += g * k.part[3] * s32;
                    } else {
                        bus32[i] += g * s32;
                        bus16[i] += g * s16;
                        bus8[i] += g * s8;
                        bus4[i] += g * s4;
                    }
                    // A divider's square is twice its saw less the next saw up.
                    const float sq8 = has4 ? 2.0f * s8 - s4 : 0.0f;
                    const float sq16 = has8 ? 2.0f * s16 - s8 : 0.0f;
                    if (choirOn && k.part[4] > 0.0f) {
                        const float comp = compFrom[ki] + (compTo[ki] - compFrom[ki]) * static_cast<float>(at + i + 1) * perFrame;
                        busSq[i] += g * comp * k.part[4] * (kChoirSq8 * sq8 + kChoirSq16 * sq16 + kChoirSaw8 * s8);
                    }
                    peak = std::fmax(peak, g);
                }
                if (kb) {
                    if (poly && k.brassCountdown-- <= 0) {
                        // This key's brightness, from its loudness and velocity and the pressure's swell.
                        k.brassCountdown = kBrassEvery - 1;
                        const float lvl = k.env.level * k.gate;
                        const float target = lvl * velB * swell;
                        const float b = stepBright(k.bright, target, lvl, bc);
                        const float fc = std::fmin(brassBase * std::exp2(brassOct * b + trackOct), brassTop);
                        k.brassLp.set(std::tan(fc * piOverRate), brassK);
                        k.drive = kDriveLow + kDriveSpan * std::fmin(target, 1.0f);
                        k.invDrive = 1.0f / k.drive;
                    }
                    const uint32_t bs = exact ? step16 : static_cast<uint32_t>(bstep);
                    bstep += dstep;
                    float x = 0.5f * sawOf(k.brassPhase, bs);
                    if (has8) x += sawOf(k.brassPhase << 1, bs << 1);
                    k.brassPhase += bs;
                    // A soft drive, louder notes harder; a small signal passes at unity.
                    const float y = g * k.part[5] * dsp::fastTanh(k.drive * x) * k.invDrive;
                    busBr[i] += poly ? k.brassLp.run(y) : y;
                }
                phase += step;
            }
            if (k.own) k.ownPhase = phase;
            if (at == 0) k.level = peak;
            else k.level = std::fmax(k.level, peak);
        }

        for (int32_t i = 0; i < n; ++i) {
            // The shared brass filter's brightness (paraphonic).
            if (!poly && brassCountdown-- <= 0) {
                brassCountdown = kBrassEvery - 1;
                const float b = stepBright(sharedBright, shared.level * paraVelB, shared.level, bc);
                brassLow.set(brassBase * std::exp2(brassOct * b), kBrassRes);
            }
            const float x8 = bus8[i];
            // Each section's level, moved by the motion's balance.
            float lvAt[kSections];
            const float *lvi = lv;
            if (ramping) {
                const float t = static_cast<float>(at + i + 1) * perFrame;
                for (int s = 0; s < kSections; ++s) lvAt[s] = lv[s] * (gainFrom[s] + (gainTo[s] - gainFrom[s]) * t);
                lvi = lvAt;
            }
            // The mix, and the same weighted by each section's width.
            float m = 0.0f, w = 0.0f;
            if (lv[0] > 0.0f) {
                const float v = lvi[0] * violinLow.lowpass(violinHigh.highpass(anyParted ? x8 + 0.35f * bus4[i] + partIn[0][i] : x8 + 0.35f * bus4[i]));
                m += v;
                w += kWidth[0] * v;
                sec[0][i] = v;
            } else { violinLow.reset(); violinHigh.reset(); sec[0][i] = 0.0f; }
            if (lv[1] > 0.0f) {
                const float v = lvi[1] * violaLow.lowpass(violaHigh.highpass(anyParted ? x8 + 0.25f * bus16[i] + partIn[1][i] : x8 + 0.25f * bus16[i]));
                m += v;
                w += kWidth[1] * v;
                sec[1][i] = v;
            } else { violaLow.reset(); violaHigh.reset(); sec[1][i] = 0.0f; }
            if (lv[2] > 0.0f) {
                const float v = lvi[2] * celloLow.lowpass(celloHigh.highpass(anyParted ? bus16[i] + partIn[2][i] : bus16[i]));
                m += v;
                w += kWidth[2] * v;
                sec[2][i] = v;
            } else { celloLow.reset(); celloHigh.reset(); sec[2][i] = 0.0f; }
            if (lv[3] > 0.0f) {
                const float v = lvi[3] * bassLow.lowpass(anyParted ? bus32[i] + partIn[3][i] : bus32[i]);
                m += v;
                w += kWidth[3] * v;
                sec[3][i] = v;
            } else {
                bassLow.reset();
                sec[3][i] = 0.0f;
            }
            if (lv[4] > 0.0f) {
                const float c = choirHigh.highpass(busSq[i]);
                float f = 0.0f;
                for (int j = 0; j < 3; ++j) f += formantLevel[j] * formant[j].bandpass(c);
                const float v = lvi[4] * choirLow.lowpass(f);
                m += v;
                w += kWidth[4] * v;
                sec[4][i] = v;
            } else {
                sec[4][i] = 0.0f;
                choirHigh.reset();
                choirLow.reset();
                for (dsp::Svf &f : formant) f.reset();
            }
            if (brassOn) {
                const float v = lvi[5] * (poly ? busBr[i] : brassLow.lowpass(busBr[i]));
                m += v;
                w += kWidth[5] * v;
                sec[5][i] = v;
            } else {
                sec[5][i] = 0.0f;
            }
            if (!brassOn || poly) brassLow.reset();
            mid[i] = m;
            wide[i] = w;
            outL[i] = outR[i] = 0.0f;
            shared.step(rise, fall);
            envAt[i] = shared.level;
        }

        // The stage, which in the middle with no room leaves the mix as it is.
        {
            const float f0 = static_cast<float>(at) / static_cast<float>(frames);
            const float f1 = static_cast<float>(at + n) / static_cast<float>(frames);
            float e0[kSections], e1[kSections], c0[kSections], c1[kSections];
            for (int s = 0; s < kSections; ++s) {
                e0[s] = ensFrom[s] + (ensTo[s] - ensFrom[s]) * f0;
                e1[s] = ensFrom[s] + (ensTo[s] - ensFrom[s]) * f1;
                c0[s] = keepFrom[s] + (keepTo[s] - keepFrom[s]) * f0;
                c1[s] = keepFrom[s] + (keepTo[s] - keepFrom[s]) * f1;
            }
            bool loud = false;
            for (int s = 0; s < kSections && !loud; ++s) {
                for (int32_t i = 0; i < n && !loud; ++i) loud = std::fabs(sec[s][i]) > kSilent;
            }
            stageHeard = loud ? 0 : std::min(stageHeard + n, kHeardCap);
            runStage(sec, staged, seatGoal, e0, e1, c0, c1, reach * msToSamples, slowStep, fastStep,
                     normStart + (stageNorm - normStart) * f0, normStart + (stageNorm - normStart) * f1, mid, right, wide, n,
                     wowAt, clockL, clockR, aging);
            snapSeats = false;
        }
        if (!staged) for (int32_t i = 0; i < n; ++i) right[i] = mid[i];
        {
            bool loud = false;
            for (int32_t i = 0; i < n && !loud; ++i) {
                loud = std::fabs(mid[i]) > kSilent || std::fabs(wide[i]) > kSilent || std::fabs(right[i]) > kSilent;
            }
            lineHeard = loud ? 0 : std::min(lineHeard + n, kHeardCap);
        }
        {
            const float f0 = static_cast<float>(at) / static_cast<float>(frames);
            const float f1 = static_cast<float>(at + n) / static_cast<float>(frames);
            runPlayers(mid, right, wide, staged, handFrom + (handNow - handFrom) * f0, handFrom + (handNow - handFrom) * f1,
                       meanWidth, outL, outR, n);
        }
        lineWrite = (lineWrite + n) & lineMask;
        snapPlayers = false;

        for (int32_t i = 0; i < n; ++i) {
            float inL = outL[i], inR = outR[i];
            if (!poly) {
                inL *= envAt[i];
                inR *= envAt[i];
            }
            inL *= volume;
            inR *= volume;

            // The ensemble: the dry sound is each line's middle, and each side
            // reads two places moved by both LFOs, the right side's moved the
            // opposite way. Mirrored about the middle, the two sides comb a
            // partial by the same amount at every moment, so a narrow sound
            // like the choir doesn't swing from side to side, and their pitch
            // moves cancel in the mono sum.
            // As the stage opens the sections' own choruses take over: this
            // one's middle comes nearer by as much as theirs moves away, and
            // its reach shrinks.
            const float hand = handFrom + (handNow - handFrom) * static_cast<float>(at + i + 1) * perFrame;
            const float here = 1.0f + (centre - 1.0f) * (1.0f - hand), shrink = reach * (1.0f - hand);
            const float ensAt = ramping ? ensSharedFrom + (ensSharedTo - ensSharedFrom) * static_cast<float>(at + i + 1) * perFrame : ens;
            const float depth = ensAt * (1.0f - hand);
            line.write(inL);
            lineR.write(inR);
            const float dryL = line.read(here), dryR = lineR.read(here);
            float l = dryL, r = dryR;
            if (aging && (ens > 0.0f || ensAt > 0.0f) && shrink > 0.0f) {
                // Aged: the reads moved by the wander, and the wet sound through the bucket.
                const float wob = wowAt[i] * (1.0f - hand);
                float wetL = 0.0f, wetR = 0.0f;
                for (int j = 0; j < 2; ++j) {
                    const float third = static_cast<float>(j) / 3.0f;
                    const float move = shrink * msToSamples * (kSlowMs * lfoSin(slowPhase + third) + kFastMs * lfoSin(fastPhase + third));
                    const float w = j == 0 ? 0.65f : 0.35f;
                    wetL += w * line.read(here + move + wob);
                    wetR += w * lineR.read(here - move + wob);
                }
                wetL = runAged(agedShared, 0, ageShared, wetL, clockL[i]);
                wetR = runAged(agedShared, 1, ageShared, wetR, clockR[i]);
                l = dryL + depth * (wetL - dryL);
                r = dryR + depth * (wetR - dryR);
            } else if ((ens > 0.0f || ensAt > 0.0f) && shrink > 0.0f) {
                float wetL = 0.0f, wetR = 0.0f;
                for (int j = 0; j < 2; ++j) {
                    const float third = static_cast<float>(j) / 3.0f;
                    const float move = shrink * msToSamples * (kSlowMs * lfoSin(slowPhase + third) + kFastMs * lfoSin(fastPhase + third));
                    const float w = j == 0 ? 0.65f : 0.35f;
                    wetL += w * line.read(here + move);
                    wetR += w * lineR.read(here - move);
                }
                l = dryL + depth * (wetL - dryL);
                r = dryR + depth * (wetR - dryR);
            }
            if (hand > 0.0f && hand < 1.0f) {
                // Two choruses in a row while they hand over, each part deep: keep the level of one.
                const float keep = std::sqrt(chorusPower(ens) / (chorusPower(ens * hand) * chorusPower(depth)));
                l *= keep;
                r *= keep;
            }
            if (ramping) {
                // The motion's depth kept as loud, as far as this chorus is heard.
                const float kept = keepSharedFrom + (keepSharedTo - keepSharedFrom) * static_cast<float>(at + i + 1) * perFrame;
                const float keep = 1.0f + (kept - 1.0f) * (1.0f - hand);
                l *= keep;
                r *= keep;
            }
            slowPhase += slowStep;
            fastPhase += fastStep;
            if (slowPhase >= 1.0f) slowPhase -= 1.0f;
            if (fastPhase >= 1.0f) fastPhase -= 1.0f;

            // The phaser: four all-passes a side swept by one LFO, the right a quarter turn on.
            if (phaser > 0.0f) {
                if (phaserCountdown-- <= 0) {
                    phaserCountdown = kCoefEvery - 1;
                    for (int s = 0; s < 2; ++s) {
                        const float sweepAt = 0.5f + 0.5f * lfoSin(phaserPhase + 0.25f * static_cast<float>(s));
                        const float hz = kPhaseLow * std::pow(kPhaseHigh / kPhaseLow, sweepAt);
                        const float w = std::tan(dsp::kPi * hz / sampleRate);
                        apCoef[s] = (1.0f - w) / (1.0f + w);
                    }
                }
                float *io[2] = {&l, &r};
                for (int s = 0; s < 2; ++s) {
                    const float a = apCoef[s];
                    float x = *io[s] + feedback * apBack[s];
                    for (float &st : apState[s]) {
                        const float y = st - a * x;
                        st = dsp::undenormal(x + a * y);
                        x = y;
                    }
                    apBack[s] = x;
                    *io[s] += phaser * 0.5f * (x - *io[s]);
                }
            } else {
                for (auto &side : apState) for (float &st : side) st = 0.0f;
                apBack[0] = apBack[1] = 0.0f;
                phaserCountdown = 0;
            }
            phaserPhase += phaserStep;
            if (phaserPhase >= 1.0f) phaserPhase -= 1.0f;

            L[at + i] = l;
            R[at + i] = r;
            outPeak = std::fmax(outPeak, std::fmax(std::fabs(l), std::fabs(r)));
        }
    }
    for (int pc = 0; pc < 12; ++pc) topPhase[pc] += topStep[pc] * static_cast<uint32_t>(frames);

    // Keys let go of: paraphonic ones when their switch has closed or the
    // envelope has finished, poly ones when their own has.
    bool any = false;
    for (Key &k : keys) {
        if (!k.used) continue;
        const bool done = poly ? k.env.idle() : ((!k.gateOn && k.gate == 0.0f) || (!k.held && shared.idle()));
        if (done) k.used = false;
        any = any || k.used;
    }
    if (!any && shared.idle() && outPeak < kSilent) {
        quietSamples += frames;
        // A late player or seat may not have spoken yet.
        if (quietSamples > kSleepAfter && linesSpent()) {
            asleep = true;
            clearState();
        }
    } else {
        quietSamples = 0;
    }
    sleptLast = asleep;
    return true;
}

} // namespace acidulous::machine
