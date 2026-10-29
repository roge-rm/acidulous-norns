#include "Reverb.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <engine/core/Settings.h>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

using dsp::clampf;
using dsp::fastTanh;

namespace {

inline float peakEnvelope(float &env, float in, float attack, float release) {
    const float a = std::fabs(in);
    env += (a - env) * (a > env ? attack : release);
    return env;
}

// --- room ---

// The room's comb and allpass tunings at 44.1 kHz, scaled in prepare(). The
// right channel is offset by 23 samples for width.
constexpr int kCombBase[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr int kAllpassBase[4] = {556, 441, 341, 225};
constexpr float kSizeStretchMax = 1.9f;

/** The tail's typical level, which the bit crusher quantises against. */
constexpr float kReverbNominal = 0.25f;
/**
 * How much of the shifted tail goes back into each comb. Found by testing
 * every room size. The worst case is the smallest room, since it has the most
 * headroom under unity and so gets injected hardest. 0.15 stays stable and
 * still gives a clearly audible shimmer.
 */
constexpr float kCombInject = 0.15f;

// Keeps loop states out of denormals, so the reverb renders the same
// whatever the FPU's flush-to-zero flag is set to.
inline float undenormal(float v) {
    static constexpr float kTiny = 1.0e-20f;
    return v + kTiny - kTiny;
}

// --- plate ---

/** The plate's lengths are given at this rate and scaled to the engine's. */
constexpr float kPlateRef = 29761.0f;
constexpr int kPlateDiffLen[4] = {142, 107, 379, 277};
constexpr float kPlateDiffG[4] = {0.75f, 0.75f, 0.625f, 0.625f};
// The two halves of the tank: modulated allpass, delay, allpass, delay.
constexpr int kPlateAp1[2] = {672, 908};
constexpr int kPlateD1[2] = {4453, 4217};
constexpr int kPlateAp2[2] = {1800, 2656};
constexpr int kPlateD2[2] = {3720, 3163};
constexpr float kPlateLoop = 672 + 4453 + 1800 + 3720 + 908 + 4217 + 2656 + 3163;
/** The modulated allpass and the second allpass in each half. */
constexpr float kPlateG1 = 0.7f, kPlateG2 = 0.5f;
// Output taps, a side: which half, which line (0 delay 1, 1 allpass 2,
// 2 delay 2), where and with what sign. Left reads mostly the right half and
// right the left, which is what makes the plate wide.
struct Tap { int half, line, at; float sign; };
constexpr Tap kPlateTapL[7] = {{1, 0, 266, 1}, {1, 0, 2974, 1}, {1, 1, 1913, -1}, {1, 2, 1996, 1},
                               {0, 0, 1990, -1}, {0, 1, 187, -1}, {0, 2, 1066, -1}};
constexpr Tap kPlateTapR[7] = {{0, 0, 353, 1}, {0, 0, 3627, 1}, {0, 1, 1228, -1}, {0, 2, 2673, 1},
                               {1, 0, 2111, -1}, {1, 1, 335, -1}, {1, 2, 121, -1}};
/** Size stretches the tank from this much of its length to that. */
constexpr float kPlateScaleMin = 0.65f, kPlateScaleRange = 0.7f;
constexpr float kPlateModRate[2] = {0.61f, 0.87f};
/** Into the tank and out of it: the plate at its defaults about as loud as the room. */
constexpr float kPlateIn = 0.5f, kPlateOut = 0.36f;
constexpr float kPlateInject = 0.5f;

// --- hall ---

constexpr float kHallMs[8] = {29.3f, 33.7f, 38.9f, 43.1f, 49.7f, 55.1f, 61.3f, 68.9f};
constexpr float kHallModRate[8] = {0.11f, 0.13f, 0.17f, 0.19f, 0.23f, 0.29f, 0.31f, 0.37f};
/** Lean runs four of the eight lines, spread across the lengths. */
constexpr int kHallLean[4] = {1, 3, 4, 6};
constexpr int kHallAll[8] = {0, 1, 2, 3, 4, 5, 6, 7};
// Line k takes the left input if k is even, the right if odd, with this sign.
constexpr float kHallInSign[8] = {1, 1, -1, -1, 1, 1, -1, -1};
// Two orthogonal rows, for left and right out.
constexpr float kHallRowL[8] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr float kHallRowR[8] = {1, 1, -1, -1, 1, 1, -1, -1};
constexpr float kHallDiffMs[2][2] = {{3.1f, 4.7f}, {3.7f, 5.3f}};
constexpr float kHallDiffG = 0.6f;
constexpr float kHallScaleMin = 0.5f, kHallScaleRange = 1.1f;
/** Lows last this much longer than mids. */
constexpr float kHallLowRatio = 1.5f;
constexpr float kHallLowHz = 400.0f, kHallHighHz = 3500.0f;
// Early reflections a side, before size and distance stretch them.
constexpr float kErMs[2][8] = {{7.3f, 11.9f, 17.1f, 23.3f, 29.9f, 37.1f, 44.9f, 53.3f},
                               {8.9f, 13.3f, 19.7f, 25.1f, 31.7f, 39.5f, 47.3f, 56.9f}};
constexpr float kErGain[8] = {0.9f, -0.75f, 0.64f, 0.55f, -0.46f, 0.38f, -0.31f, -0.25f};
constexpr float kHallIn = 0.5f, kHallOut = 0.15f, kHallErLevel = 0.03f;
constexpr float kHallInject = 0.4f;

// --- spring ---

/** Where the springs' chirp sits: the stretched allpasses delay most here, and nothing above it gets in. */
constexpr float kSpringFc = 4200.0f;
/** Each spring's echo period at size 0.5, and its share of the dispersion. */
constexpr float kSpringMs[3] = {38.3f, 45.1f, 53.9f};
constexpr float kSpringA[3] = {1.0f, 0.96f, 1.03f};
/** The stretched allpasses' coefficient: the higher, the more the delay gathers at the chirp's top. */
constexpr float kSpringDisp = 0.7f;
/** Size stretches the springs from this much of their length to that. */
constexpr float kSpringScaleMin = 0.75f, kSpringScaleRange = 0.5f;
/** The tank's driver passes little under this. */
constexpr float kSpringHpHz = 150.0f;
constexpr float kSpringIn = 0.5f, kSpringOut = 0.65f, kSpringInject = 0.5f;
// Knock. How far the fast envelope must jump over the slow one, the least it
// counts, and how long after one hit before the next and how long its peak is
// taken over.
constexpr float kKnockRatio = 2.0f, kKnockFloor = 0.02f, kKnockHoldS = 0.08f, kKnockCaptureS = 0.003f;
/** The thump falls from the first pitch to the second and dies away over its decay. */
constexpr float kThumpHz0 = 120.0f, kThumpHz1 = 48.0f, kThumpGlideS = 0.06f, kThumpDecayS = 0.18f;
/** How loud the thump is out of the tank and into the springs, and the burst into the springs. */
constexpr float kThumpOut = 0.35f, kThumpIn = 0.4f, kBurstIn = 0.8f, kBurstDecayS = 0.008f;

// --- early ---

constexpr float kEarlyMs[4] = {11.3f, 14.9f, 19.1f, 23.7f};
constexpr float kEarlyDiffMs[2][2] = {{4.3f, 2.9f}, {4.9f, 3.3f}};
constexpr float kEarlyDiffG = 0.65f;
constexpr float kEarlyScaleMin = 0.6f, kEarlyScaleRange = 1.0f;
constexpr float kEarlyModRate[4] = {0.13f, 0.17f, 0.23f, 0.29f};
constexpr float kEarlyInSign[4] = {1, 1, -1, -1};
/** The tank's converter: inputs are rounded to this many steps of full scale. */
constexpr float kEarlySteps = 2048.0f;
constexpr float kEarlyIn = 0.5f, kEarlyOut = 0.28f, kEarlyInject = 0.4f;
constexpr float kEarlyRateMax = 32000.0f;

// --- space ---

/** Speed of sound, m/s. */
constexpr float kSoundSpeed = 343.0f;
// The walls: absorption, how much longer the lows and how much shorter the
// highs last than the mids, and the cutoff the early reflections take.
// Tile, plaster, wood, curtains.
constexpr float kMatAlpha[4] = {0.02f, 0.06f, 0.12f, 0.55f};
constexpr float kMatLow[4] = {1.1f, 1.2f, 1.0f, 1.3f};
constexpr float kMatHigh[4] = {0.85f, 0.65f, 0.5f, 0.3f};
constexpr float kMatErHz[4] = {12000.0f, 8000.0f, 5000.0f, 2500.0f};
/** Absorption every room has besides its walls: air, openings, people. */
constexpr float kRoomAlpha = 0.04f;
/** The largest room, for sizing buffers. */
constexpr float kSpaceMaxW = 30.0f, kSpaceMaxD = 50.0f, kSpaceMaxH = 15.0f;
/** The tail's lines, as multiples of the mean free path, clamped to these times. */
constexpr float kSpaceSpread[8] = {0.71f, 0.79f, 0.89f, 0.97f, 1.07f, 1.17f, 1.29f, 1.41f};
constexpr float kSpaceMfpMin = 0.005f, kSpaceMfpMax = 0.05f;
constexpr float kSpaceEr = 0.19f, kSpaceTailIn = 0.6f, kSpaceOut = 0.15f, kSpaceInject = 0.4f;
/** How long the taps take to glide to a new room. */
constexpr float kSpaceGlideS = 0.05f;

// --- tube and string ---

/** The lines' range; a pitch outside it folds by octaves. */
constexpr float kWaveMinHz = 20.0f, kWaveMaxHz = 2000.0f;
/** Middle C, what `pitch` counts from when it isn't following. */
constexpr float kWaveBaseHz = 261.6256f;
/** The two lines, this many cents either side of the pitch. */
constexpr float kWaveDetune = 1.5f;
/** Wobble at the top swings each line's length by this much of it, about 10 cents. */
constexpr float kWaveWobble = 0.006f;
constexpr float kWaveModRate[2] = {0.37f, 0.53f};
/** The input's high-pass, so no DC goes round. */
constexpr float kWaveHpHz = 20.0f;
/** The tube's soft clip: where loud hits start to growl. */
constexpr float kTubeClip = 0.3f;
constexpr float kTubeIn = 0.5f, kTubeOut = 0.5f, kStringIn = 0.4f, kStringOut = 0.21f, kWaveInject = 0.5f;
/** How long the harmonic damp points at rings, against the fundamental. */
constexpr float kWaveHighShare = 0.2f;
/** The allpasses' coefficient at stiff 1, and the most of the period the chain may take. */
constexpr float kStiffMax = 0.85f, kStiffShare = 0.45f;

/** Phase in radians at w of the one-pole low-pass y += a (x - y). */
double lowPassPhase(double a, double w) {
    return std::arg(std::complex<double>(a) / (1.0 - (1.0 - a) * std::polar(1.0, -w)));
}

/** Phase at w of the first-order allpass (a + z^-1) / (1 + a z^-1). */
double allpassPhase(double a, double w) {
    const auto z = std::polar(1.0, -w);
    return std::arg((a + z) / (1.0 + a * z));
}

/**
 * The delay to ask a WaveTap for so it delays by `d` samples at w: the
 * first-order allpass is a little off its fraction away from DC, so the
 * fraction is moved by what it misses.
 */
float tapDelayFor(double d, double w) {
    double ask = d;
    for (int k = 0; k < 2; ++k) {
        const double m = std::floor(ask - 0.5), f = ask - m;
        const double c = (1.0 - f) / (1.0 + f);
        const double got = m - allpassPhase(c, w) / w;
        ask += d - got;
    }
    return static_cast<float>(ask);
}

// --- drumhead ---

/** The head's lowest mode at size 0.5 and tension 0.5; size moves it two octaves, tension three. */
constexpr float kDrumHz = 160.0f;
/** The mesh's lowest mode is its rate over this many times its junctions a side plus one. */
constexpr float kMeshTune = 2.0f;
/** Wobble at the top swings the mesh's rate by this much. */
constexpr float kMeshWobble = 0.008f;
constexpr float kMeshModRate = 0.41f;
// Where the input lands, where the two pickups sit and where the snares touch, across the head.
constexpr float kStrike[2][2] = {{0.35f, 0.3f}, {0.4f, 0.68f}};
constexpr float kPickup[2][2] = {{0.3f, 0.62f}, {0.66f, 0.36f}};
constexpr float kContact[3] = {0.25f, 0.5f, 0.75f};
constexpr float kDrumIn = 0.5f, kDrumOut = 1.03f, kDrumInject = 0.5f, kMeshLeanScale = 0.885f;
/** The links' low-pass at damp 1. */
constexpr float kMeshDamp = 0.012f;
/** How far the head must move before it reaches the wires, at snares 0 and 1. */
constexpr float kSnareGap0 = 0.08f, kSnareGap1 = 0.03f;
/** The buzz: how loud for how far past, its band and how fast it follows. */
constexpr float kSnareGain = 3.0f, kSnareHpHz = 1800.0f, kSnareLpHz = 7500.0f, kSnareAtkS = 0.0003f, kSnareRelS = 0.012f;

// --- thunder sheet ---

/** The sheet's lowest mode at size 0.5, and how far up its highest sits. */
constexpr float kThunderHz = 55.0f, kThunderSpan = 160.0f;
/** The bands' tops, as multiples of the lowest mode: the hum, the body, the crash and the sizzle. */
constexpr float kThBandTop[3] = {8.0f, 24.0f, 64.0f};
/** The strike reaches modes under about this with little loss. */
constexpr float kThunderStrikeHz = 250.0f;
/** How fast a band's energy spills into the next, and the most a band takes in. */
constexpr float kSpill = 16.0f, kSpillMax = 0.5f, kSpillFloor = 1e-9f;
/** How much faster a loud band loses its energy, and the most it loses a sample, at 48 kHz. */
constexpr float kDrain = 0.002f, kDrainMax = 0.0005f;
/** No mode rings past this, so nothing runs away, and one under the floor stops: 240 dB down. */
constexpr float kModeMax = 1.0f, kModeFloor = 1e-12f;
constexpr float kThunderIn = 1.0f, kThunderOut = 0.075f, kThunderInject = 0.5f;
/** Lean's half the modes, each side's sum made up to the full one's level. */
constexpr float kThunderLeanOut = 1.19f;

// --- howl ---

/** The room's three lengths against the shortest, and the shortest at size 0.5, in metres. */
constexpr float kHowlProp[3] = {1.59f, 1.26f, 1.0f};
constexpr float kHowlMetres = 2.0f;
/**
 * At this howl the loop gain at the peak is 1, so it sustains; past it the modes near the peak grow by up to
 * kHowlGrow dB a second. The loop gain for the lows stays under kHowlLowsMost.
 */
constexpr float kHowlEdge = 0.75f, kHowlGrow = 30.0f, kHowlLowsMost = 0.98f;
/** The howl's wet is soft-limited here, and lower with distance, whose gain and wider sides
 *  would take it over full scale. */
constexpr float kHowlOutCeil = 0.45f, kHowlOutMost = 0.9f;
/** The speaker's peak at size 0.5, moving with the room so the same modes sing, and the limiter's level. */
constexpr float kHowlPeakHz = 650.0f, kHowlQ = 1.2f, kHowlCeil = 0.3f;
constexpr float kHowlModRate[3] = {0.19f, 0.29f, 0.43f};
constexpr float kHowlIn = 0.3f, kHowlOut = 0.53f, kHowlInject = 0.5f;

// --- flutter ---

/** The far wall's distance a side, against the near one's: the walls aren't quite parallel. */
constexpr float kFlutterSkew = 1.012f;
/** Where the second wall's echoes fall in the round trip, a side. */
constexpr float kFlutterTap[2] = {0.37f, 0.63f};
constexpr float kFlutterModRate[2] = {0.31f, 0.47f};
constexpr float kFlutterIn = 0.4f, kFlutterOut = 0.47f, kFlutterInject = 0.5f;

/** A small noise source, the same every render. */
inline float whiteNoise(uint32_t &state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int32_t>(state)) * (1.0f / 2147483648.0f);
}

// --- shared ---

/** Modulation at default, and what wobble adds at the top, in seconds. */
constexpr float kModDepth = 0.00025f, kWobbleDepth = 0.0015f;
/** Distance at the top: the pre-delay it adds in ms, and the air loss's cutoff. */
constexpr float kDistancePreMs = 80.0f, kDistanceAirHz = 3000.0f;
/** Duck pulls fully at this envelope level and above. */
constexpr float kDuckFull = 0.2f;
/**
 * Bloom: the input level that counts as playing, the time constant it falls
 * back with, and the one it rises with at the top (full in about 2 s).
 */
constexpr float kBloomFloor = 0.003f, kBloomFall = 0.25f, kBloomRise = 0.7f;

/** An allpass on a whole-sample line: H = (z^-n - g) / (1 - g z^-n). */
inline float allpass(Reverb::Line &l, int32_t n, float g, float x) {
    const float d = l.at(n);
    const float v = undenormal(x + g * d);
    l.write(v);
    return d - g * v;
}

inline void rotate(float osc[2], const float rot[2]) {
    const float c = osc[0] * rot[0] - osc[1] * rot[1];
    const float s = osc[1] * rot[0] + osc[0] * rot[1];
    osc[0] = c;
    osc[1] = s;
}

/** Pulls a rotating phasor back onto the unit circle; once a block is plenty. */
inline void renormalise(float osc[2]) {
    const float k = 1.5f - 0.5f * (osc[0] * osc[0] + osc[1] * osc[1]);
    osc[0] *= k;
    osc[1] *= k;
}

inline float decayGain(float seconds, float rt60) { return std::pow(10.0f, -3.0f * seconds / rt60); }

} // namespace

// --- Line ---------------------------------------------------------------------

void Reverb::Line::prepare(int32_t n) {
    size = std::max(4, n);
    buf.assign(static_cast<size_t>(size), 0.0f);
    wr = 0;
}

void Reverb::Line::clear() {
    std::fill(buf.begin(), buf.end(), 0.0f);
    wr = 0;
}

float Reverb::Line::frac(float d) const {
    if (d < 1.0f) d = 1.0f;
    if (d > static_cast<float>(size - 2)) d = static_cast<float>(size - 2);
    float f = 0.0f;
    const int32_t i0 = dsp::wrappedReadIndex(wr, d, size, f);
    const int32_t i1 = i0 + 1 >= size ? 0 : i0 + 1;
    return buf[static_cast<size_t>(i0)] * (1.0f - f) + buf[static_cast<size_t>(i1)] * f;
}

// --- Params -------------------------------------------------------------------

const ParamDef *Reverb::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"size", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"damp", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"tone", 500.0f, 18000.0f, 9000.0f, Curve::Exponential, 0, "Hz"},
        {"predelay", 0.0f, 200.0f, 10.0f, Curve::Linear, 0, "ms"},
        {"mix", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"freeze", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"gate", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"shimmer", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bits", 1.0f, 16.0f, 16.0f, Curve::Stepped, 16, ""},
        {"crush", 1.0f, 32.0f, 1.0f, Curve::Exponential, 0, ""},
        {"wobble", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        // room, plate, hall, spring, early, space, tube, string, drumhead, thunder, howl, flutter
        {"model", 0.0f, 11.0f, 0.0f, Curve::Stepped, kModels, ""},
        {"duck", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"duckrel", 50.0f, 2000.0f, 300.0f, Curve::Exponential, 0, "ms"},
        {"distance", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bloom", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // spring
        {"drip", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"knock", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        // early
        {"rate", 8000.0f, 32000.0f, 16000.0f, Curve::Exponential, 0, "Hz"},
        // space
        {"width", 2.0f, 30.0f, 9.0f, Curve::Exponential, 0, "m"},
        {"depth", 2.0f, 50.0f, 14.0f, Curve::Exponential, 0, "m"},
        {"height", 2.0f, 15.0f, 4.5f, Curve::Exponential, 0, "m"},
        {"position", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // tile, plaster, wood, curtains
        {"material", 0.0f, 3.0f, 1.0f, Curve::Stepped, 4, ""},
        // tube and string: semitones from middle C, or from the last note when following
        {"pitch", -36.0f, 24.0f, 0.0f, Curve::Linear, 0, "st"},
        {"follow", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        // tube
        {"open", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // string
        {"stiff", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"pick", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        // drumhead
        {"tension", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"snares", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // thunder
        {"metal", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // howl
        {"howl", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        // flutter: how far apart the walls are
        {"walls", 0.25f, 20.0f, 2.0f, Curve::Exponential, 0, "m"},
    };
    count = Count;
    return defs;
}

// --- Lifecycle ----------------------------------------------------------------

void Reverb::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    const float scale = sr / 44100.0f;
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < 8; ++i) {
            combs[c][i].len = static_cast<int32_t>(static_cast<float>(kCombBase[i]) * scale) + (c == 1 ? 23 : 0);
            combs[c][i].line.prepare(static_cast<int32_t>(static_cast<float>(combs[c][i].len) * kSizeStretchMax) + 8);
        }
        for (int i = 0; i < 4; ++i) {
            aps[c][i].len = static_cast<int32_t>(static_cast<float>(kAllpassBase[i]) * scale) + (c == 1 ? 23 : 0);
            aps[c][i].line.prepare(aps[c][i].len + 8);
        }
        // The pre-delay keeps its old length, since the length changes how a
        // fractional read rounds. The long one has room for distance's extra
        // pre-delay and the hall's latest reflection.
        pre[c].prepare(static_cast<int32_t>(sr * 0.21f));
        preLong[c].prepare(static_cast<int32_t>(sr * 0.5f));
        // Longer than the shimmer window, so the read point can slide without
        // catching the write head.
        shimmerLine[c].prepare(static_cast<int32_t>(sr * 0.12f));
    }

    // The plate at its largest, with the most modulation wobble can ask for.
    const float plMax = (kPlateScaleMin + kPlateScaleRange) * sr / kPlateRef;
    const auto plLen = [&](int ref) { return static_cast<int32_t>(static_cast<float>(ref) * plMax) + 4; };
    const int32_t modRoom = static_cast<int32_t>((kModDepth + kWobbleDepth) * sr) + 4;
    for (int k = 0; k < kPlateDiff; ++k) plDiff[k].prepare(plLen(kPlateDiffLen[k]));
    for (int h = 0; h < 2; ++h) {
        plAp1[h].prepare(plLen(kPlateAp1[h]) + modRoom);
        plD1[h].prepare(plLen(kPlateD1[h]));
        plAp2[h].prepare(plLen(kPlateAp2[h]));
        plD2[h].prepare(plLen(kPlateD2[h]));
        const float w = dsp::kTwoPi * kPlateModRate[h] / sr;
        plRot[h][0] = std::cos(w);
        plRot[h][1] = std::sin(w);
    }

    const float haMax = (kHallScaleMin + kHallScaleRange) * sr * 0.001f;
    for (int i = 0; i < kHallLines; ++i) {
        // The read swings up to two mod depths past the line's length.
        haLine[i].prepare(static_cast<int32_t>(kHallMs[i] * haMax) + 2 * modRoom);
        const float w = dsp::kTwoPi * kHallModRate[i] / sr;
        haRot[i][0] = std::cos(w);
        haRot[i][1] = std::sin(w);
    }
    for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < 2; ++k) {
            haLenDiff[c][k] = std::max(1, static_cast<int32_t>(kHallDiffMs[c][k] * 0.001f * sr));
            haDiff[c][k].prepare(haLenDiff[c][k] + 4);
        }
    }
    haLowCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * kHallLowHz), sr);
    haHighCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * kHallHighHz), sr);

    // Spring: K samples per stretched allpass puts the chirp's top at sr / 2K.
    spK = std::clamp(static_cast<int32_t>(std::lround(sr / (2.0f * kSpringFc))), 1, kSpringSlots);
    const float fc = sr / (2.0f * static_cast<float>(spK));
    spInCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * fc), sr);
    spHpCoeff = std::exp(-dsp::kTwoPi * kSpringHpHz / sr);
    for (int s = 0; s < kSprings; ++s) {
        const float most = kSpringMs[s] * 0.001f * sr * (kSpringScaleMin + kSpringScaleRange);
        spLine[s].prepare(static_cast<int32_t>(most) + 2 * modRoom + 4);
        const float w = dsp::kTwoPi * kHallModRate[s * 2 + 1] / sr;
        spRot[s][0] = std::cos(w);
        spRot[s][1] = std::sin(w);
    }

    // Early: sized for the top rate, the longest size and the most wobble,
    // which swings the read up to twice its depth.
    const float eaMost = kEarlyRateMax * 0.001f * (kEarlyScaleMin + kEarlyScaleRange);
    const int32_t eaWob = static_cast<int32_t>(kWobbleDepth * kEarlyRateMax) + 2;
    for (int i = 0; i < kEarlyLines; ++i) eaLine[i].prepare(static_cast<int32_t>(kEarlyMs[i] * eaMost) + 2 * eaWob + 4);
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 2; ++k) eaDiff[c][k].prepare(static_cast<int32_t>(kEarlyDiffMs[c][k] * eaMost) + 4);

    // Space: the input line holds the latest reflection of the largest room,
    // and the tail's lines the longest mean free path.
    float far = 0.0f;
    for (int nx = -3; nx <= 3; ++nx)
        for (int ny = -3; ny <= 3; ++ny)
            for (int nz = -3; nz <= 3; ++nz) {
                if (std::abs(nx) + std::abs(ny) + std::abs(nz) > 3) continue;
                const float dx = static_cast<float>(std::abs(nx) + 1) * kSpaceMaxW;
                const float dy = static_cast<float>(std::abs(ny) + 1) * kSpaceMaxD;
                const float dz = static_cast<float>(std::abs(nz) + 1) * kSpaceMaxH;
                far = std::max(far, std::sqrt(dx * dx + dy * dy + dz * dz));
            }
    // A power of two, so a still room's taps wrap with a mask.
    int32_t inSize = 1;
    while (inSize < static_cast<int32_t>(far / kSoundSpeed * sr) + 8) inSize <<= 1;
    spcIn.prepare(inSize);
    for (int i = 0; i < kSpaceLines; ++i) {
        spcLine[i].prepare(static_cast<int32_t>(kSpaceMfpMax * kSpaceSpread[i] * sr) + 2 * modRoom + 4);
        const float w = dsp::kTwoPi * kHallModRate[7 - i] / sr;
        spcRot[i][0] = std::cos(w);
        spcRot[i][1] = std::sin(w);
    }
    // Tube and string: the lowest pitch, a little under for the detune, and the most wobble.
    const int32_t wgMost = static_cast<int32_t>(sr / kWaveMinHz * (1.02f + 2.0f * kWaveWobble)) + 8;
    for (int c = 0; c < kWaveLines; ++c) {
        wgLine[c].prepare(wgMost);
        wgPick[c].prepare(wgMost / 2 + 8);
        const float w = dsp::kTwoPi * kWaveModRate[c] / sr;
        wgRot[c][0] = std::cos(w);
        wgRot[c][1] = std::sin(w);
    }
    wgHpCoeff = std::exp(-dsp::kTwoPi * kWaveHpHz / sr);
    wgWas[0] = -1.0f;
    // Drumhead.
    msRot[0] = std::cos(dsp::kTwoPi * kMeshModRate / sr);
    msRot[1] = std::sin(dsp::kTwoPi * kMeshModRate / sr);
    snHpCoeff = std::exp(-dsp::kTwoPi * kSnareHpHz / sr);
    snLpCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * std::min(kSnareLpHz, 0.4f * sr)), sr);
    msWas[0] = -1.0f;
    // Thunder sheet.
    thEnergyCoeff = dsp::onePoleCoeff(0.06f, sr);
    thWas[0] = -1.0f;
    // Howl: the longest line at the largest size and the most wobble.
    const float hwMost = 2.0f * kHowlMetres * 2.0f * kHowlProp[0] / kSoundSpeed * sr * (1.0f + 2.0f * kWaveWobble);
    for (int i = 0; i < kHowlLines; ++i) {
        hwLine[i].prepare(static_cast<int32_t>(hwMost) + 8);
        hwRot[i][0] = std::cos(dsp::kTwoPi * kHowlModRate[i] / sr);
        hwRot[i][1] = std::sin(dsp::kTwoPi * kHowlModRate[i] / sr);
    }
    hwEnvAtk = dsp::onePoleCoeff(0.002f, sr);
    hwEnvRel = dsp::onePoleCoeff(0.2f, sr);
    hwWas[0] = -1.0f;
    // Flutter: the walls at their furthest, the far one, and the most wobble.
    const float flMost = 2.0f * 20.0f / kSoundSpeed * sr * kFlutterSkew * (1.0f + 2.0f * kWaveWobble);
    for (int c = 0; c < kFlutterLines; ++c) {
        flLine[c].prepare(static_cast<int32_t>(flMost) + 8);
        flRot[c][0] = std::cos(dsp::kTwoPi * kFlutterModRate[c] / sr);
        flRot[c][1] = std::sin(dsp::kTwoPi * kFlutterModRate[c] / sr);
    }
    flWas[0] = -1.0f;
    plSizeWas = haSizeWas = spSizeWas = eaSizeWas = -1.0f;
    eaRateWas = -1.0f;
    spcGeomWas[0] = -1.0f;
    reset();
}

void Reverb::clearRoom() {
    for (auto &ch : combs) for (auto &c : ch) { c.line.clear(); c.store = 0.0f; }
    for (auto &ch : aps) for (auto &a : ch) a.line.clear();
    wobblePhase = 0.0f;
}

void Reverb::clearPlate() {
    for (auto &l : plDiff) l.clear();
    for (int h = 0; h < 2; ++h) {
        plAp1[h].clear();
        plD1[h].clear();
        plAp2[h].clear();
        plD2[h].clear();
        plDamp[h] = 0.0f;
        plCross[h] = 0.0f;
        // Out of step, so the halves never move together.
        plOsc[h][0] = h == 0 ? 1.0f : 0.0f;
        plOsc[h][1] = h == 0 ? 0.0f : 1.0f;
    }
}

void Reverb::clearHall() {
    for (int i = 0; i < kHallLines; ++i) {
        haLine[i].clear();
        haLow[i] = haHigh[i] = 0.0f;
        const float ph = dsp::kTwoPi * static_cast<float>(i) * 0.37f;
        haOsc[i][0] = std::cos(ph);
        haOsc[i][1] = std::sin(ph);
    }
    for (auto &ch : haDiff) for (auto &l : ch) l.clear();
}

void Reverb::clearSpring() {
    for (auto &st : spChain) for (auto &sl : st) for (float &v : sl) v = 0.0f;
    spIdx = 0;
    for (int s = 0; s < kSprings; ++s) {
        spLine[s].clear();
        const float ph = dsp::kTwoPi * static_cast<float>(s) * 0.29f;
        spOsc[s][0] = std::cos(ph);
        spOsc[s][1] = std::sin(ph);
    }
    for (float &v : spLp) v = 0.0f;
    spHpX = spHpY = spInLp = 0.0f;
    knFast = knSlow = thEnv = thAmp = thPhase = burstEnv = 0.0f;
    thHz = kThumpHz0;
    knHold = knCapture = 0;
    noiseState = 0x2545f491u;
}

void Reverb::clearEarly() {
    for (int i = 0; i < kEarlyLines; ++i) {
        eaLine[i].clear();
        eaLp[i] = 0.0f;
        const float ph = dsp::kTwoPi * static_cast<float>(i) * 0.23f;
        eaOsc[i][0] = std::cos(ph);
        eaOsc[i][1] = std::sin(ph);
    }
    for (auto &ch : eaDiff) for (auto &l : ch) l.clear();
    eaAcc = 0.0f;
    eaHeld[0] = eaHeld[1] = eaSum[0] = eaSum[1] = 0.0f;
    eaCount = 0;
}

void Reverb::clearSpace() {
    spcIn.clear();
    for (int i = 0; i < kSpaceLines; ++i) {
        spcLine[i].clear();
        spcLow[i] = spcHigh[i] = 0.0f;
        const float ph = dsp::kTwoPi * static_cast<float>(i) * 0.41f;
        spcOsc[i][0] = std::cos(ph);
        spcOsc[i][1] = std::sin(ph);
    }
    spcErLp[0] = spcErLp[1] = 0.0f;
    // The next block jumps to the room rather than gliding from the last one.
    spcSnap = true;
}

void Reverb::clearWave() {
    for (int c = 0; c < kWaveLines; ++c) {
        wgLine[c].clear();
        wgPick[c].clear();
        for (auto &t : wgTap[c]) t.y = 0.0f;
        for (float &v : wgChain[c]) v = 0.0f;
        wgLp[c] = wgHpX[c] = wgHpY[c] = 0.0f;
        const float ph = dsp::kTwoPi * static_cast<float>(c) * 0.31f;
        wgOsc[c][0] = std::cos(ph);
        wgOsc[c][1] = std::sin(ph);
    }
}

void Reverb::clearDrum() {
    for (auto &buf : msIn) for (auto &port : buf) for (float &v : port) v = 0.0f;
    for (auto &edge : msEdge) for (float &v : edge) v = 0.0f;
    msCur = 0;
    msAcc = 0.0f;
    msInCount = 0;
    for (int c = 0; c < 2; ++c) msInSum[c] = msPrev[c] = msNow[c] = 0.0f;
    msOsc[0] = 1.0f;
    msOsc[1] = 0.0f;
    snPast = snEnv = snHpX = snHpY = snLp = 0.0f;
    snNoise = 0x6b43a9b5u;
}

void Reverb::clearThunder() {
    for (int k = 0; k < kThModes; ++k) thRe[k] = thIm[k] = 0.0f;
    for (float &e : thEnergy) e = 0.0f;
    thNoise = 0x3c6ef372u;
}

void Reverb::clearHowl() {
    for (int i = 0; i < kHowlLines; ++i) {
        hwLine[i].clear();
        hwLp[i] = hwEnv[i] = 0.0f;
        hwBp[i][0] = hwBp[i][1] = 0.0f;
        const float ph = dsp::kTwoPi * static_cast<float>(i) * 0.33f;
        hwOsc[i][0] = std::cos(ph);
        hwOsc[i][1] = std::sin(ph);
    }
}

void Reverb::clearFlutter() {
    for (int c = 0; c < kFlutterLines; ++c) {
        flLine[c].clear();
        flLp[c] = 0.0f;
        const float ph = dsp::kTwoPi * static_cast<float>(c) * 0.27f;
        flOsc[c][0] = std::cos(ph);
        flOsc[c][1] = std::sin(ph);
    }
}

void Reverb::reset() {
    clearRoom();
    clearPlate();
    clearHall();
    clearSpring();
    clearEarly();
    clearSpace();
    clearWave();
    clearDrum();
    clearThunder();
    clearHowl();
    clearFlutter();
    heardNote = -1;
    heardHz = 440.0f;
    for (auto &p : pre) p.clear();
    for (auto &p : preLong) p.clear();
    for (auto &l : shimmerLine) l.clear();
    for (int c = 0; c < 2; ++c) {
        shimmerPhase[c] = 0.0f;
        shimmerFb[c] = 0.0f;
        shimDcX[c] = 0.0f;
        shimDcY[c] = 0.0f;
        shimLp[c] = 0.0f;
        crushAcc[c] = 0.0f;
        crushHeld[c] = 0.0f;
        airLp[c] = 0.0f;
    }
    lp[0] = lp[1] = 0.0f;
    gateEnv = 0.0f;
    gateHold = 0;
    inputEnv = 0.0f;
    duckEnv = bloomDet = bloomEnv = 0.0f;
    lastModel = -1;
    lastFull = fullQuality();
}

// --- Per block ----------------------------------------------------------------

struct Reverb::Block {
    // The room's, as it always read them.
    bool freeze;
    float stretch, fb, damp, inGain, mix, gate, preSamples, toneCoeff, envAtk, envRel, gateRel;
    int32_t holdSamples;
    float shimmer;
    int32_t bits;
    float crush, shimWin, shimDamp, wobbleInc, wobbleDepth;
    bool wobbling;
    int32_t nCombs, nAps;
    float combScale;
    // Shared knobs. `shared` is off when duck, distance and bloom are all 0,
    // and then the output is the room's to the bit.
    bool shared, bloomOn;
    float duck, duckAtk, duckRel, distance, bloom, bloomRise, bloomFall, bloomBase, bloomDetRel;
    float airCoeff, wetGain, dryGain, wetWidth, dryWidth;
    // Plate and hall.
    bool full;
    float modDepth, diffMul, erGain;
    float erAt[2][kHallEr];
    // Spring: knock, its detector and the thump's and burst's rates.
    float knock, knFastAtk, knFastRel, knSlowAtk, knSlowRel, thumpAtk, thumpDecay, thumpGlide, burstDecay;
    // Tube and string: open's two tap weights, and wobble's swing as a share of the length.
    float wgOdd, wgFull, wgWob;
    // Drumhead: wobble's swing of the mesh rate, and the snares' gap, level and envelope rates.
    float msWob, snGap, snLevel, snAtk, snRel;
    // Howl: how much of the loop goes through the speaker's peak; howl and flutter's wobble.
    float hwMix, hwWob, flWob;
};

bool Reverb::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const int32_t model = std::clamp(static_cast<int32_t>(p.get(Model) + 0.5f), 0, kModels - 1);
    const bool full = fullQuality();
    if (model != lastModel || full != lastFull) {
        // The tank coming in starts empty, or it would replay an old tail.
        // A quality switch keeps the room's tail, since its lean path reads the same lines.
        if (lastModel >= 0 && (model != lastModel || model != Room)) {
            switch (model) {
                case Room: clearRoom(); break;
                case Plate: clearPlate(); break;
                case Hall: clearHall(); break;
                case Spring: clearSpring(); break;
                case Early: clearEarly(); break;
                case Space: clearSpace(); break;
                case Drumhead: clearDrum(); break;
                case Thunder: clearThunder(); break;
                case HowlRoom: clearHowl(); break;
                case Flutter: clearFlutter(); break;
                default: clearWave(); break;
            }
        }
        lastModel = model;
        lastFull = full;
    }

    Block b{};
    b.freeze = p.get(Freeze) >= 0.5f;
    const float size = p.get(Size);
    b.stretch = 1.0f + (kSizeStretchMax - 1.0f) * size;
    b.fb = b.freeze ? 1.0f : 0.72f + 0.26f * size;
    b.damp = b.freeze ? 0.0f : p.get(Damp) * 0.6f;
    b.inGain = b.freeze ? 0.0f : 0.015f;
    b.mix = p.get(Mix);
    b.gate = p.get(Gate);
    b.distance = p.get(Distance);
    const float preMs = p.get(PreDelay) + kDistancePreMs * b.distance;
    b.preSamples = clampf(preMs * 0.001f * sr, 1.0f, sr * (0.2f + 0.08f * b.distance));
    b.toneCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * p.get(Tone)), sr);
    b.envAtk = dsp::onePoleCoeff(0.001f, sr);
    b.envRel = dsp::onePoleCoeff(0.05f, sr);
    b.gateRel = dsp::onePoleCoeff(0.006f, sr);
    b.holdSamples = static_cast<int32_t>((0.04f + b.gate * 0.5f) * sr);
    b.shimmer = p.get(Shimmer);
    b.bits = static_cast<int32_t>(p.get(Bits) + 0.5f);
    b.crush = p.get(Crush);
    const float wobble = p.get(Wobble);
    // The shimmer window. The read point slides through it to come out an
    // octave up, with two taps half a window apart for the crossfade.
    b.shimWin = sr * 0.045f;
    // The shimmer's own damping. Brighter rooms let it climb further.
    b.shimDamp = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * (1200.0f + 2400.0f * (1.0f - b.damp))), sr);
    // Combs drift by up to a couple of ms each, so the tail never settles into
    // a fixed comb pattern.
    b.wobbleInc = 0.23f / sr;
    b.wobbleDepth = wobble * sr * 0.0015f;
    b.wobbling = b.wobbleDepth > 0.0f;
    // Lean quality uses half the combs and allpasses, for a thinner but
    // cheaper tail. The sum is scaled up so a lean room isn't quieter.
    b.nCombs = full ? kCombs : kCombs / 2;
    b.nAps = full ? kAps : kAps / 2;
    b.combScale = static_cast<float>(kCombs) / static_cast<float>(b.nCombs);

    b.duck = p.get(Duck);
    b.bloom = p.get(Bloom);
    b.bloomOn = b.bloom > 0.0f;
    b.shared = b.duck > 0.0f || b.distance > 0.0f || b.bloomOn;
    b.duckAtk = dsp::onePoleCoeff(0.005f, sr);
    b.duckRel = dsp::onePoleCoeff(p.get(DuckRel) * 0.001f, sr);
    b.bloomRise = dsp::onePoleCoeff(kBloomRise * b.bloom, sr);
    b.bloomFall = dsp::onePoleCoeff(kBloomFall, sr);
    b.bloomDetRel = dsp::onePoleCoeff(0.1f, sr);
    b.bloomBase = 1.0f - 0.9f * b.bloom;
    b.airCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * 20000.0f * std::pow(kDistanceAirHz / 20000.0f, b.distance)), sr);
    b.wetGain = 1.0f + b.distance;
    b.dryGain = 1.0f - 0.5f * b.distance;
    b.wetWidth = 1.0f + 0.6f * b.distance;
    b.dryWidth = 1.0f - 0.6f * b.distance;

    b.full = full;
    b.modDepth = (kModDepth + wobble * kWobbleDepth) * sr;
    b.diffMul = 1.0f;
    b.erGain = kHallErLevel * (1.0f - 0.75f * b.distance);
    const float erScale = (0.6f + 0.8f * size) * (1.0f + 1.2f * b.distance) * 0.001f * sr;
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < kHallEr; ++k) b.erAt[c][k] = kErMs[c][k] * erScale;

    switch (model) {
        case Plate:
            updatePlate(size, p.get(Damp), b.freeze);
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { plateSample(bb, d, o); });
            for (auto &o : plOsc) renormalise(o);
            break;
        case Hall:
            updateHall(size, p.get(Damp), b.freeze);
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { hallSample(bb, d, o); });
            for (auto &o : haOsc) renormalise(o);
            break;
        case Spring:
            b.knock = p.get(Knock);
            b.knFastAtk = dsp::onePoleCoeff(0.0002f, sr);
            b.knFastRel = dsp::onePoleCoeff(0.02f, sr);
            b.knSlowAtk = dsp::onePoleCoeff(0.004f, sr);
            b.knSlowRel = dsp::onePoleCoeff(0.02f, sr);
            b.thumpAtk = dsp::onePoleCoeff(0.001f, sr);
            b.thumpDecay = std::exp(-1.0f / (kThumpDecayS * sr));
            b.thumpGlide = dsp::onePoleCoeff(kThumpGlideS, sr);
            b.burstDecay = std::exp(-1.0f / (kBurstDecayS * sr));
            updateSpring(size, p.get(Damp), p.get(Drip), b.freeze, full);
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { springSample(bb, d, o); });
            for (auto &o : spOsc) renormalise(o);
            break;
        case Early:
            updateEarly(size, p.get(Damp), p.get(Rate), b.freeze);
            // Wobble moves the lines by whole samples at the tank's rate.
            eaWobble = static_cast<int32_t>(wobble * kWobbleDepth * eaInc * sr);
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { earlySample(bb, d, o); });
            for (auto &o : eaOsc) renormalise(o);
            break;
        case Tube:
        case String: {
            const bool string = model == String;
            updateWave(string, size, p.get(Damp), b.freeze, full);
            // The odd tap is the closed pipe, half the loop with a sign flip; the full tap the open one.
            // Together the odd harmonics ring as set and the even ones for `open` of that time:
            // their loop gain is the odd ones' to the power 1 / open.
            const float open = p.get(Open);
            const float even = open > 0.0f ? std::pow(wgGLive, 1.0f / open - 1.0f) : 0.0f;
            b.wgOdd = -0.5f * (1.0f - even);
            b.wgFull = 0.5f * (1.0f + even);
            b.wgWob = wobble * kWaveWobble;
            const float pick = 0.04f + 0.46f * p.get(Pick);
            for (int c = 0; c < kWaveLines; ++c) wgPickD[c] = std::max(1.0f, wgPeriod[c] * pick * (c == 0 ? 1.0f : 0.85f));
            if (string)
                run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { stringSample(bb, d, o); });
            else
                run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { tubeSample(bb, d, o); });
            for (auto &o : wgOsc) renormalise(o);
            break;
        }
        case Drumhead: {
            updateDrum(size, p.get(Damp), b.freeze, full);
            b.msWob = wobble * kMeshWobble;
            const float snares = p.get(Snares);
            b.snGap = kSnareGap0 + (kSnareGap1 - kSnareGap0) * snares;
            b.snLevel = snares;
            b.snAtk = dsp::onePoleCoeff(kSnareAtkS, sr);
            b.snRel = dsp::onePoleCoeff(kSnareRelS, sr);
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { drumSample(bb, d, o); });
            renormalise(msOsc);
            break;
        }
        case Thunder:
            updateThunder(size, p.get(Damp), b.freeze, full);
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { thunderSample(bb, d, o); });
            break;
        case HowlRoom:
            updateHowl(size, p.get(Damp), b.freeze);
            b.hwMix = b.freeze ? 0.0f : p.get(Howl);
            b.hwWob = wobble * kWaveWobble;
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { howlSample(bb, d, o); });
            for (auto &o : hwOsc) renormalise(o);
            break;
        case Flutter:
            updateFlutter(size, p.get(Damp), b.freeze);
            b.flWob = wobble * kWaveWobble;
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { flutterSample(bb, d, o); });
            for (auto &o : flOsc) renormalise(o);
            break;
        case Space:
            updateSpace(p.get(Damp), b.freeze, full, frames);
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { spaceSample(bb, d, o); });
            for (auto &o : spcOsc) renormalise(o);
            break;
        default:
            run(L, R, frames, stereoIn, b, [this](const Block &bb, const float *d, float *o) { roomSample(bb, d, o); });
            break;
    }
    return true;
}

// --- The shared loop ----------------------------------------------------------

template <typename Tank>
void Reverb::run(float *L, float *R, int32_t frames, bool stereoIn, const Block &b0, Tank &&tank) {
    Block b = b0;
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        pre[0].write(inL);
        pre[1].write(inR);
        preLong[0].write(inL);
        preLong[1].write(inR);
        float dry[2];
        if (b.distance > 0.0f) {
            dry[0] = preLong[0].read(b.preSamples);
            dry[1] = preLong[1].read(b.preSamples);
        } else {
            dry[0] = pre[0].read(b.preSamples);
            dry[1] = pre[1].read(b.preSamples);
        }
        // Bloom: while there's input the tank's feed rises from thin to full,
        // and it falls back once the input stops.
        if (b.bloomOn) {
            const float det = peakEnvelope(bloomDet, 0.5f * (std::fabs(inL) + std::fabs(inR)), b.envAtk, b.bloomDetRel);
            bloomDet = undenormal(bloomDet);
            bloomEnv += ((det > kBloomFloor ? 1.0f : 0.0f) - bloomEnv) * (det > kBloomFloor ? b.bloomRise : b.bloomFall);
            bloomEnv = undenormal(bloomEnv);
            const float g = b.bloomBase + (1.0f - b.bloomBase) * bloomEnv;
            dry[0] *= g;
            dry[1] *= g;
            b.diffMul = 1.0f - 0.7f * b.bloom * (1.0f - bloomEnv);
        }
        float out[2];
        tank(b, dry, out);
        // Gate: lets the tail through for a hold time after each hit, then
        // cuts it. The classic gated drum room.
        float g = 1.0f;
        if (b.gate > 0.0f) {
            const float env = peakEnvelope(inputEnv, (std::fabs(inL) + std::fabs(inR)), b.envAtk, b.envRel);
            if (env > 0.05f) { gateHold = b.holdSamples; gateEnv = 1.0f; }
            if (gateHold > 0) --gateHold; else gateEnv += (0.0f - gateEnv) * b.gateRel;
            g = gateEnv;
        }
        if (!b.shared) {
            L[i] = inL + out[0] * b.mix * g;
            R[i] = inR + out[1] * b.mix * g;
            continue;
        }
        // Duck: the dry's envelope pulls the tail down.
        const float env = peakEnvelope(duckEnv, 0.5f * (std::fabs(inL) + std::fabs(inR)), b.duckAtk, b.duckRel);
        duckEnv = undenormal(duckEnv);
        const float dg = b.wetGain * (1.0f - b.duck * std::min(1.0f, env / kDuckFull));
        float w0 = out[0] * dg, w1 = out[1] * dg;
        float d0 = inL, d1 = inR;
        if (b.distance > 0.0f) {
            // Air loss on the tail, a wider tail and a narrower, quieter dry.
            airLp[0] = undenormal(airLp[0] + (w0 - airLp[0]) * b.airCoeff);
            airLp[1] = undenormal(airLp[1] + (w1 - airLp[1]) * b.airCoeff);
            const float wm = 0.5f * (airLp[0] + airLp[1]), ws = 0.5f * (airLp[0] - airLp[1]) * b.wetWidth;
            w0 = wm + ws;
            w1 = wm - ws;
            const float dm = 0.5f * (inL + inR), ds = 0.5f * (inL - inR) * b.dryWidth;
            d0 = (dm + ds) * b.dryGain;
            d1 = (dm - ds) * b.dryGain;
        }
        L[i] = d0 + w0 * b.mix * g;
        R[i] = d1 + w1 * b.mix * g;
    }
}

// --- Shared tail extras ---------------------------------------------------------

float Reverb::lofi(int c, float t, const Block &b) {
    // Lo-fi tail: quantised and sample-held after the tank and before the
    // mix, so the dry signal isn't affected.
    if (b.bits < 16) {
        const float step = kReverbNominal / static_cast<float>(1 << b.bits);
        t = std::round(t / step) * step;
    }
    if (b.crush > 1.001f) {
        crushAcc[c] += 1.0f / b.crush;
        if (crushAcc[c] >= 1.0f) { crushAcc[c] -= 1.0f; crushHeld[c] = t; }
        t = crushHeld[c];
    }
    return t;
}

void Reverb::shimmerStep(int c, float t, const Block &b) {
    // Shimmer: the tail read back an octave up, for the tank to take in.
    if (b.shimmer > 0.0f) {
        shimmerLine[c].write(t);
        // The delay shrinks by one sample per sample, so the read point moves
        // at twice the rate and the tail comes back an octave up. Adding
        // instead would hold the read point still.
        shimmerPhase[c] -= 1.0f / b.shimWin;
        while (shimmerPhase[c] < 0.0f) shimmerPhase[c] += 1.0f;
        float ph2 = shimmerPhase[c] + 0.5f;
        if (ph2 >= 1.0f) ph2 -= 1.0f;
        const float g1 = std::sin(shimmerPhase[c] * 3.14159265f);
        const float g2 = std::sin(ph2 * 3.14159265f);
        const float shifted = shimmerLine[c].read(shimmerPhase[c] * b.shimWin + 2.0f) * g1 +
                              shimmerLine[c].read(ph2 * b.shimWin + 2.0f) * g2;
        // Remove DC before it goes round again. The shifter passes DC
        // unchanged, so it would build up in the loop. A one-pole blocker at
        // about 5 Hz stops it.
        const float dc = shifted - shimDcX[c] + 0.9995f * shimDcY[c];
        shimDcX[c] = shifted;
        shimDcY[c] = dc;
        // Low-pass it every time round. Each pass moves the tail up an
        // octave, so without damping it would climb forever. NaNs are dropped
        // here too, since a feedback path would keep them.
        const float bounded = std::isfinite(dc) ? clampf(dc, -2.0f, 2.0f) : 0.0f;
        shimLp[c] += (bounded - shimLp[c]) * b.shimDamp;
        // Soft-limit the feedback. No fixed injection amount is safe for every
        // size, so the limiter is what keeps the loop bounded, like a shimmer
        // pedal.
        shimmerFb[c] = undenormal(fastTanh(shimLp[c] * 1.6f) * 0.55f);
    } else {
        shimmerFb[c] = 0.0f;
    }
}

// --- Room ---------------------------------------------------------------------

void Reverb::roomSample(const Block &b, const float dry[2], float out[2]) {
    for (int c = 0; c < 2; ++c) {
        const float x = dry[c] * b.inGain;
        float acc = 0.0f;
        int combIndex = 0;
        for (int32_t ci = 0; ci < b.nCombs; ++ci) {
            Comb &comb = combs[c][ci];
            // Each comb wobbles on its own phase. Without the offset they'd
            // all lengthen together, which is a pitch bend. Only computed
            // when wobble is on, since it's sixteen sines a sample.
            float drift = 0.0f;
            if (b.wobbling) {
                const float ph = wobblePhase + static_cast<float>(combIndex) * 0.125f + (c == 1 ? 0.5f : 0.0f);
                drift = b.wobbleDepth * std::sin((ph - std::floor(ph)) * dsp::kTwoPi);
            }
            const float y = comb.line.read(static_cast<float>(comb.len) * b.stretch + drift);
            comb.store = undenormal(y * (1.0f - b.damp) + comb.store * b.damp);
            // The shimmer uses only the headroom left under unity (1 - fb),
            // or the loop gain goes above one and blows up. So a frozen
            // reverb (fb = 1) can't shimmer. kCombInject accounts for the
            // shimmer going into every comb.
            comb.line.write(x + comb.store * b.fb + shimmerFb[c] * b.shimmer * (1.0f - b.fb) * kCombInject);
            acc += y;
            ++combIndex;
        }
        for (int32_t ai = 0; ai < b.nAps; ++ai) {
            Allpass &ap = aps[c][ai];
            const float y = ap.line.read(static_cast<float>(ap.len));
            ap.line.write(acc + y * 0.5f);
            acc = y - acc;
        }
        acc *= b.combScale;
        lp[c] += (acc - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
    wobblePhase += b.wobbleInc;
    if (wobblePhase >= 1.0f) wobblePhase -= 1.0f;
}

// --- Plate --------------------------------------------------------------------

void Reverb::updatePlate(float size, float damp, bool freeze) {
    if (size == plSizeWas && damp == plDampWas && freeze == plFreezeWas) return;
    plSizeWas = size;
    plDampWas = damp;
    plFreezeWas = freeze;
    plScale = (kPlateScaleMin + kPlateScaleRange * size) * sr / kPlateRef;
    const auto len = [&](int ref) { return std::max(1, static_cast<int32_t>(static_cast<float>(ref) * plScale)); };
    for (int k = 0; k < kPlateDiff; ++k) plLenDiff[k] = len(kPlateDiffLen[k]);
    for (int h = 0; h < 2; ++h) {
        plLenAp1[h] = static_cast<float>(kPlateAp1[h]) * plScale;
        plLenD1[h] = len(kPlateD1[h]);
        plLenAp2[h] = len(kPlateAp2[h]);
        plLenD2[h] = len(kPlateD2[h]);
    }
    for (int k = 0; k < 7; ++k) {
        plTapL[k] = len(kPlateTapL[k].at);
        plTapR[k] = len(kPlateTapR[k].at);
    }
    // The decay is set as a time, 0.4 s to 8 s, and spread over the four
    // gain stages a trip round the figure eight passes.
    const float rt = 0.4f * std::pow(20.0f, size);
    const float loopSeconds = kPlateLoop * plScale / sr;
    plGain = freeze ? 1.0f : decayGain(loopSeconds * 0.25f, rt);
    // Damping: 14 kHz down to 1.5 kHz in the loop.
    const float hz = 14000.0f * std::pow(1500.0f / 14000.0f, damp);
    plDampCoeff = freeze ? 1.0f : dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * hz), sr);
}

void Reverb::plateSample(const Block &b, const float dry[2], float out[2]) {
    const float inGain = b.freeze ? 0.0f : kPlateIn;
    float x = (dry[0] + dry[1]) * 0.5f * inGain;
    // Lean runs two of the four diffusers.
    for (int k = 0; k < kPlateDiff; k += b.full ? 1 : 2) x = allpass(plDiff[k], plLenDiff[k], kPlateDiffG[k] * b.diffMul, x);

    const float inject = b.shimmer * (1.0f - plGain) * kPlateInject;
    float cross[2];
    for (int h = 0; h < 2; ++h) {
        const float in = x + plCross[h ^ 1] + shimmerFb[h] * inject;
        rotate(plOsc[h], plRot[h]);
        const float d = plAp1[h].frac(plLenAp1[h] + b.modDepth * plOsc[h][1]);
        const float v = undenormal(in - kPlateG1 * d);
        plAp1[h].write(v);
        const float a = d + kPlateG1 * v;
        const float t = plD1[h].at(plLenD1[h]);
        plD1[h].write(a);
        plDamp[h] = undenormal(plDamp[h] + (t - plDamp[h]) * plDampCoeff);
        const float bb = allpass(plAp2[h], plLenAp2[h], kPlateG2, plDamp[h] * plGain);
        const float e = plD2[h].at(plLenD2[h]);
        plD2[h].write(bb);
        cross[h] = undenormal(e * plGain);
    }
    plCross[0] = cross[0];
    plCross[1] = cross[1];

    const auto tap = [&](const Tap &t, int32_t at) {
        const Line &l = t.line == 0 ? plD1[t.half] : (t.line == 1 ? plAp2[t.half] : plD2[t.half]);
        return t.sign * l.at(at);
    };
    // Lean reads four taps a side.
    const int taps = b.full ? 7 : 4;
    float sum[2] = {0.0f, 0.0f};
    for (int k = 0; k < taps; ++k) {
        sum[0] += tap(kPlateTapL[k], plTapL[k]);
        sum[1] += tap(kPlateTapR[k], plTapR[k]);
    }
    const float outScale = kPlateOut * (b.full ? 1.0f : 1.27f);
    for (int c = 0; c < 2; ++c) {
        lp[c] += (sum[c] * outScale - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Hall ---------------------------------------------------------------------

void Reverb::updateHall(float size, float damp, bool freeze) {
    if (size == haSizeWas && damp == haDampWas && freeze == haFreezeWas) return;
    haSizeWas = size;
    haDampWas = damp;
    haFreezeWas = freeze;
    haScale = kHallScaleMin + kHallScaleRange * size;
    // 0.8 s to 12 s. Lows last longer and damp shortens the highs.
    const float rt = 0.8f * std::pow(15.0f, size);
    const float rtHigh = rt * std::max(0.08f, 1.0f - 0.85f * damp);
    for (int i = 0; i < kHallLines; ++i) {
        haLen[i] = kHallMs[i] * 0.001f * haScale * sr;
        const float s = haLen[i] / sr;
        haGMid[i] = freeze ? 1.0f : decayGain(s, rt);
        haGLow[i] = freeze ? 1.0f : decayGain(s, rt * kHallLowRatio);
        haGHigh[i] = freeze ? 1.0f : decayGain(s, rtHigh);
    }
}

void Reverb::hallSample(const Block &b, const float dry[2], float out[2]) {
    const float inGain = b.freeze ? 0.0f : kHallIn;
    float in[2];
    for (int c = 0; c < 2; ++c) {
        float x = dry[c] * inGain;
        for (int k = 0; k < 2; ++k) x = allpass(haDiff[c][k], haLenDiff[c][k], kHallDiffG * b.diffMul, x);
        in[c] = x;
    }

    const int n = b.full ? kHallLines : 4;
    const int *act = b.full ? kHallAll : kHallLean;
    float y[kHallLines], z[kHallLines];
    float sum = 0.0f;
    for (int k = 0; k < n; ++k) {
        const int i = act[k];
        rotate(haOsc[i], haRot[i]);
        y[k] = haLine[i].frac(haLen[i] + b.modDepth * (1.0f + haOsc[i][1]));
        // Three bands in one: highs at gHigh, mids at gMid, lows at gLow.
        haLow[i] = undenormal(haLow[i] + (y[k] - haLow[i]) * haLowCoeff);
        haHigh[i] = undenormal(haHigh[i] + (y[k] - haHigh[i]) * haHighCoeff);
        z[k] = haGHigh[i] * y[k] + (haGMid[i] - haGHigh[i]) * haHigh[i] + (haGLow[i] - haGMid[i]) * haLow[i];
        sum += z[k];
    }
    // Householder: every line gets itself less 2/n of the sum.
    const float h = sum * (2.0f / static_cast<float>(n));
    const float inject = b.shimmer * kHallInject;
    float o[2] = {0.0f, 0.0f};
    for (int k = 0; k < n; ++k) {
        const int i = act[k];
        const int c = k & 1;
        const float headroom = 1.0f - haGMid[i];
        haLine[i].write(undenormal(z[k] - h + in[c] * kHallInSign[k] + shimmerFb[c] * inject * headroom));
        o[0] += kHallRowL[k] * y[k];
        o[1] += kHallRowR[k] * y[k];
    }
    const float outScale = kHallOut * (b.full ? 1.0f : 1.41f);

    // Early reflections, from the pre-delayed dry.
    float er[2] = {0.0f, 0.0f};
    for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < kHallEr; ++k) er[c] += kErGain[k] * preLong[c].read(b.preSamples + b.erAt[c][k]);
    }

    for (int c = 0; c < 2; ++c) {
        const float wet = o[c] * outScale + er[c] * b.erGain * (b.freeze ? 0.0f : 1.0f);
        lp[c] += (wet - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Spring -------------------------------------------------------------------

void Reverb::updateSpring(float size, float damp, float drip, bool freeze, bool full) {
    if (size == spSizeWas && damp == spDampWas && drip == spDripWas && freeze == spFreezeWas && full == spFullWas) return;
    spSizeWas = size;
    spDampWas = damp;
    spDripWas = drip;
    spFreezeWas = freeze;
    spFullWas = full;
    // Drip is how many stages the springs run through: the chirp spreads with
    // the count. Lean runs half as many.
    const int32_t stages = 8 + static_cast<int32_t>(std::lround(drip * static_cast<float>(kSpringStages - 8)));
    const int32_t was = spStages;
    spStages = full ? stages : std::max(4, stages / 2);
    // Stages coming back in start empty rather than with what they last held.
    for (int32_t st = was; st < spStages; ++st)
        for (auto &sl : spChain[st]) for (float &v : sl) v = 0.0f;
    // 0.7 s to 4.9 s.
    const float rt = 0.7f * std::pow(7.0f, size);
    const float scale = kSpringScaleMin + kSpringScaleRange * size;
    for (int s = 0; s < kSprings; ++s) {
        spA[s] = kSpringDisp * kSpringA[s];
        spLen[s] = kSpringMs[s] * 0.001f * sr * scale;
        spG[s] = freeze ? 1.0f : decayGain(spLen[s] / sr, rt);
    }
    spA[3] = 0.0f;
    spG[3] = 0.0f;
    // Damping in the loop: the chirp's top down to half of it.
    const float fc = sr / (2.0f * static_cast<float>(spK));
    const float hz = fc * std::pow(0.5f, damp);
    spLpCoeff = freeze ? 1.0f : dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * hz), sr);
}

void Reverb::springSample(const Block &b, const float dry[2], float out[2]) {
    const float mono = 0.5f * (dry[0] + dry[1]);

    // Knock: a hit that jumps well over the recent level shakes the tank. The
    // strength is the hit's peak over its first few ms.
    peakEnvelope(knFast, mono, b.knFastAtk, b.knFastRel);
    peakEnvelope(knSlow, mono, b.knSlowAtk, b.knSlowRel);
    knFast = undenormal(knFast);
    knSlow = undenormal(knSlow);
    if (knHold > 0) --knHold;
    else if (b.knock > 0.0f && knFast > kKnockRatio * knSlow + kKnockFloor) {
        knHold = static_cast<int32_t>(kKnockHoldS * sr);
        knCapture = static_cast<int32_t>(kKnockCaptureS * sr);
        if (thAmp < 0.01f) {
            thPhase = 0.0f;
            thHz = kThumpHz0;
        }
    }
    if (knCapture > 0) {
        --knCapture;
        const float strength = std::min(1.0f, 2.0f * knFast) * b.knock;
        thEnv = std::max(thEnv, strength);
        burstEnv = std::max(burstEnv, strength);
    }
    float thump = 0.0f, burst = 0.0f;
    if (thEnv > 1.0e-5f || thAmp > 1.0e-5f) {
        thAmp += (thEnv - thAmp) * b.thumpAtk;
        thEnv *= b.thumpDecay;
        thHz += (kThumpHz1 - thHz) * b.thumpGlide;
        thPhase += thHz / sr;
        if (thPhase >= 1.0f) thPhase -= 1.0f;
        thump = std::sin(thPhase * dsp::kTwoPi) * thAmp;
    } else {
        thEnv = thAmp = 0.0f;
    }
    if (burstEnv > 1.0e-5f) {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        burst = (static_cast<float>(noiseState & 0xffffff) / 8388608.0f - 1.0f) * burstEnv;
        burstEnv *= b.burstDecay;
    } else {
        burstEnv = 0.0f;
    }

    // Into the tank: the driver's high-pass, then nothing over the chirp's top.
    const float inGain = b.freeze ? 0.0f : 1.0f;
    const float hp = spHpCoeff * (spHpY + mono - spHpX);
    spHpX = mono;
    spHpY = undenormal(hp);
    spInLp = undenormal(spInLp + (hp - spInLp) * spInCoeff);
    const float x = inGain * (spInLp * kSpringIn + burst * kBurstIn + thump * kThumpIn);

    float o[4] = {0.0f, 0.0f, 0.0f, 0.0f}, v[4] = {0.0f, 0.0f, 0.0f, 0.0f}, a[4];
    for (int s = 0; s < kSprings; ++s) {
        rotate(spOsc[s], spRot[s]);
        o[s] = spLine[s].frac(spLen[s] + b.modDepth * (1.0f + spOsc[s][1]));
        const float inject = shimmerFb[s & 1] * b.shimmer * (1.0f - spG[s]) * kSpringInject;
        v[s] = x + o[s] * spG[s] + inject;
    }
    for (int l = 0; l < 4; ++l) a[l] = spA[l] * b.diffMul;
    // The chain: each stage is (a + z^-K) / (1 + a z^-K), its state K slots deep.
    for (int32_t st = 0; st < spStages; ++st) {
        float *slot = spChain[st][spIdx];
        for (int l = 0; l < 4; ++l) {
            const float old = slot[l];
            const float w = undenormal(v[l] - a[l] * old);
            slot[l] = w;
            v[l] = a[l] * w + old;
        }
    }
    if (++spIdx >= spK) spIdx = 0;
    for (int s = 0; s < kSprings; ++s) {
        spLp[s] = undenormal(spLp[s] + (v[s] - spLp[s]) * spLpCoeff);
        spLine[s].write(spLp[s]);
    }

    const float outScale = kSpringOut;
    const float wet[2] = {o[0] + 0.6f * o[2], o[1] - 0.6f * o[2]};
    for (int c = 0; c < 2; ++c) {
        const float t0 = wet[c] * outScale + thump * kThumpOut * inGain;
        lp[c] += (t0 - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Early digital --------------------------------------------------------------

void Reverb::updateEarly(float size, float damp, float rate, bool freeze) {
    if (size == eaSizeWas && damp == eaDampWas && rate == eaRateWas && freeze == eaFreezeWas) return;
    eaSizeWas = size;
    eaDampWas = damp;
    eaRateWas = rate;
    eaFreezeWas = freeze;
    const float r = std::min(rate, std::min(sr, kEarlyRateMax));
    eaInc = r / sr;
    const float scale = (kEarlyScaleMin + kEarlyScaleRange * size) * 0.001f * r;
    // 0.4 s to 4.8 s.
    const float rt = 0.4f * std::pow(12.0f, size);
    for (int i = 0; i < kEarlyLines; ++i) {
        eaLen[i] = std::max(2, static_cast<int32_t>(kEarlyMs[i] * scale));
        eaG[i] = freeze ? 1.0f : decayGain(static_cast<float>(eaLen[i]) / r, rt);
        const float w = dsp::kTwoPi * kEarlyModRate[i] / r;
        eaRot[i][0] = std::cos(w);
        eaRot[i][1] = std::sin(w);
    }
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 2; ++k) eaLenDiff[c][k] = std::max(1, static_cast<int32_t>(kEarlyDiffMs[c][k] * 0.001f * r));
    // Damping at the tank's rate: near its top down to 1 kHz.
    const float top = 0.4f * r;
    const float hz = top * std::pow(1000.0f / top, damp);
    eaDampCoeff = freeze ? 1.0f : dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * hz), r);
}

void Reverb::earlySample(const Block &b, const float dry[2], float out[2]) {
    eaSum[0] += dry[0];
    eaSum[1] += dry[1];
    ++eaCount;
    eaAcc += eaInc;
    if (eaAcc >= 1.0f) {
        eaAcc -= 1.0f;
        // One tick of the tank. The input is the plain average of the samples
        // since the last tick, rounded like a cheap converter.
        const float inGain = b.freeze ? 0.0f : kEarlyIn / static_cast<float>(eaCount);
        float in[2];
        for (int c = 0; c < 2; ++c) {
            float x = std::round(eaSum[c] * inGain * kEarlySteps) / kEarlySteps;
            eaSum[c] = 0.0f;
            for (int k = 0; k < (b.full ? 2 : 1); ++k) x = allpass(eaDiff[c][k], eaLenDiff[c][k], kEarlyDiffG * b.diffMul, x);
            in[c] = x;
        }
        float y[kEarlyLines], z[kEarlyLines], sum = 0.0f;
        for (int i = 0; i < kEarlyLines; ++i) {
            int32_t d = eaLen[i];
            if (eaWobble > 0) {
                rotate(eaOsc[i], eaRot[i]);
                d += static_cast<int32_t>(static_cast<float>(eaWobble) * (1.0f + eaOsc[i][1]));
            }
            y[i] = eaLine[i].at(std::min(d, eaLine[i].size));
            eaLp[i] = undenormal(eaLp[i] + (y[i] - eaLp[i]) * eaDampCoeff);
            z[i] = eaLp[i] * eaG[i];
            sum += z[i];
        }
        const float h = sum * 0.5f;
        for (int i = 0; i < kEarlyLines; ++i) {
            const int c = i & 1;
            const float inject = shimmerFb[c] * b.shimmer * (1.0f - eaG[i]) * kEarlyInject;
            eaLine[i].write(undenormal(z[i] - h + in[c] * kEarlyInSign[i] + inject));
        }
        // Held until the next tick, with no smoothing.
        eaHeld[0] = (y[0] - y[1] + y[2] - y[3]) * kEarlyOut;
        eaHeld[1] = (y[0] + y[1] - y[2] - y[3]) * kEarlyOut;
        eaCount = 0;
    }
    for (int c = 0; c < 2; ++c) {
        lp[c] += (eaHeld[c] - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Space --------------------------------------------------------------------

void Reverb::updateSpace(float damp, bool freeze, bool full, int32_t frames) {
    const auto &p = params_;
    const float W = p.get(Width), D = p.get(Depth), H = p.get(Height), pos = p.get(Position);
    const int32_t mat = std::clamp(static_cast<int32_t>(p.get(Material) + 0.5f), 0, 3);
    const float geom[5] = {W, D, H, pos, static_cast<float>(mat)};
    if (!std::equal(geom, geom + 5, spcGeomWas) || full != spcFullWas) {
        std::copy(geom, geom + 5, spcGeomWas);
        spcFullWas = full;
        // The source near the front, off centre; you stand on the centre line,
        // `position` of the way from it to the back wall.
        const float src[3] = {0.4f * W, std::clamp(0.15f * D, 0.3f, 3.0f), std::min(1.5f, 0.5f * H)};
        const float ly = src[1] + (D - 0.3f - src[1]) * (0.1f + 0.9f * pos);
        const float beta = std::sqrt(1.0f - kMatAlpha[mat]);
        const float len[3] = {W, D, H};
        // Images in order of their reflections: 6 first, 18 second, 38 third.
        float energy[2] = {0.0f, 0.0f}, energyLean[2] = {0.0f, 0.0f};
        for (int e = 0; e < 2; ++e) {
            const float ear[3] = {0.5f * W + (e == 0 ? -0.09f : 0.09f), ly, std::min(1.7f, 0.6f * H)};
            const float side = e == 0 ? -1.0f : 1.0f;
            float r0 = 0.0f;
            for (int a = 0; a < 3; ++a) r0 += (src[a] - ear[a]) * (src[a] - ear[a]);
            r0 = std::max(0.5f, std::sqrt(r0));
            int k = 0;
            for (int order = 1; order <= 3; ++order) {
                for (int nx = -order; nx <= order; ++nx) {
                    for (int ny = -order; ny <= order; ++ny) {
                        const int rest = order - std::abs(nx) - std::abs(ny);
                        if (rest < 0) continue;
                        for (int nz = -rest; nz <= rest; nz += rest == 0 ? 1 : 2 * rest) {
                            const int n[3] = {nx, ny, nz};
                            float d2 = 0.0f, dx = 0.0f;
                            for (int a = 0; a < 3; ++a) {
                                // Even images keep the source's side, odd ones mirror it.
                                const float img = static_cast<float>(n[a]) * len[a] + ((n[a] & 1) ? len[a] - src[a] : src[a]);
                                const float dd = img - ear[a];
                                d2 += dd * dd;
                                if (a == 0) dx = dd;
                            }
                            const float r = std::sqrt(d2);
                            // Facing forward: an ear hears its own side louder.
                            const float facing = 0.6f + 0.4f * side * dx / std::max(r, 0.01f);
                            const float g = std::pow(beta, static_cast<float>(order)) * r0 / r * facing * kSpaceEr;
                            tapDTo[e][k] = std::min(std::max(0.0f, (r - r0) / kSoundSpeed * sr) + 1.0f,
                                                    static_cast<float>(spcIn.size - 2));
                            tapGTo[e][k] = g;
                            energy[e] += g * g;
                            if (order < 3) energyLean[e] += g * g;
                            ++k;
                        }
                    }
                }
            }
            // Lean plays the first two orders, as loud as all three.
            spcTaps = full ? kSpaceTaps : 24;
            if (!full) {
                const float comp = std::sqrt(energy[e] / std::max(energyLean[e], 1.0e-12f));
                for (int j = 0; j < 24; ++j) tapGTo[e][j] *= comp;
            }
        }
        // The tail: its decay from the volume, the surface and what the
        // walls and the room take, with lines around the mean free path.
        const float V = W * D * H, S = 2.0f * (W * D + W * H + D * H);
        const float alpha = kMatAlpha[mat] + kRoomAlpha;
        spcRt = std::clamp(0.161f * V / (-S * std::log(1.0f - alpha)), 0.12f, 15.0f);
        spcRtLow = spcRt * kMatLow[mat];
        spcRtHigh = spcRt * kMatHigh[mat];
        const float mfp = std::clamp(4.0f * V / S / kSoundSpeed, kSpaceMfpMin, kSpaceMfpMax);
        for (int i = 0; i < kSpaceLines; ++i) spcLenTo[i] = mfp * kSpaceSpread[i] * sr;
        spcErCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * kMatErHz[mat]), sr);
    }

    // Glide towards the room: two one-poles per block, so a move starts and
    // ends slowly, and a line within the block.
    const float f = static_cast<float>(std::max(1, frames));
    const float k = spcSnap ? 1.0f : 1.0f - std::exp(-f / (kSpaceGlideS * sr));
    spcMoving = false;
    // Only the taps this quality reads; a quality switch snaps the rest.
    const int32_t glideTaps = spcSnap ? kSpaceTaps : spcTaps;
    for (int e = 0; e < 2; ++e) {
        for (int j = 0; j < glideTaps; ++j) {
            // Close enough is there, so a still room skips the glide.
            if (spcSnap || (std::fabs(tapDTo[e][j] - tapDAt[e][j]) < 1.0e-3 && std::fabs(tapGTo[e][j] - tapGAt[e][j]) < 1.0e-6)) {
                tapD[e][j] = tapDTo[e][j];
                tapG[e][j] = tapGTo[e][j];
                tapDMid[e][j] = tapDAt[e][j] = tapDTo[e][j];
                tapGAt[e][j] = tapGTo[e][j];
                tapDStep[e][j] = tapGStep[e][j] = 0.0f;
                continue;
            }
            spcMoving = true;
            tapD[e][j] = static_cast<float>(tapDAt[e][j]);
            tapG[e][j] = static_cast<float>(tapGAt[e][j]);
            tapDMid[e][j] += (tapDTo[e][j] - tapDMid[e][j]) * k;
            const double dNext = tapDAt[e][j] + (tapDMid[e][j] - tapDAt[e][j]) * k;
            const double gNext = tapGAt[e][j] + (tapGTo[e][j] - tapGAt[e][j]) * k;
            tapDStep[e][j] = static_cast<float>((dNext - tapDAt[e][j]) / f);
            tapGStep[e][j] = static_cast<float>((gNext - tapGAt[e][j]) / f);
            tapDAt[e][j] = dNext;
            tapGAt[e][j] = gNext;
        }
    }
    if (!spcMoving) {
        for (int e = 0; e < 2; ++e) {
            for (int j = 0; j < kSpaceTaps; ++j) {
                tapO[e][j] = static_cast<int32_t>(tapD[e][j]);
                tapF[e][j] = tapD[e][j] - static_cast<float>(tapO[e][j]);
            }
        }
    }
    const float rtHigh = spcRtHigh * std::max(0.08f, 1.0f - 0.85f * damp);
    for (int i = 0; i < kSpaceLines; ++i) {
        if (spcSnap) spcLen[i] = spcLenMid[i] = spcLenTo[i];
        spcLenMid[i] += (spcLenTo[i] - spcLenMid[i]) * k;
        spcLenStep[i] = (spcLenMid[i] - spcLen[i]) * k / f;
        // The decay for where the line will be at the end of the block.
        const float s = (spcLen[i] + spcLenStep[i] * f) / sr;
        spcGMid[i] = freeze ? 1.0f : decayGain(s, spcRt);
        spcGLow[i] = freeze ? 1.0f : decayGain(s, spcRtLow);
        spcGHigh[i] = freeze ? 1.0f : decayGain(s, rtHigh);
    }
    spcSnap = false;
}

void Reverb::spaceSample(const Block &b, const float dry[2], float out[2]) {
    spcIn.write(0.5f * (dry[0] + dry[1]));
    // The taps read the line directly: every delay is at least 1 and well
    // inside it (see updateSpace), so only the wrap needs care.
    const float *buf = spcIn.buf.data();
    const int32_t size = spcIn.size;
    const float wr = static_cast<float>(spcIn.wr), fsize = static_cast<float>(size);
    const auto read = [&](float d) {
        float pos = wr - d;
        if (pos < 0.0f) pos += fsize;
        if (!(pos < fsize)) pos = 0.0f;
        const auto i0 = static_cast<int32_t>(pos);
        const float fr = pos - static_cast<float>(i0);
        const int32_t i1 = i0 + 1 >= size ? 0 : i0 + 1;
        return buf[i0] + (buf[i1] - buf[i0]) * fr;
    };
    float er[2];
    for (int e = 0; e < 2; ++e) {
        float acc = 0.0f;
        if (spcMoving) {
            for (int32_t j = 0; j < spcTaps; ++j) {
                tapD[e][j] += tapDStep[e][j];
                tapG[e][j] += tapGStep[e][j];
                acc += tapG[e][j] * read(tapD[e][j]);
            }
        } else {
            // Still: whole and fractional parts worked out once a block.
            const int32_t w = spcIn.wr, mask = size - 1;
            for (int32_t j = 0; j < spcTaps; ++j) {
                const int32_t o = tapO[e][j];
                acc += tapG[e][j] * (buf[(w - o) & mask] + (buf[(w - o - 1) & mask] - buf[(w - o) & mask]) * tapF[e][j]);
            }
        }
        spcErLp[e] = undenormal(spcErLp[e] + (acc - spcErLp[e]) * spcErCoeff);
        er[e] = spcErLp[e];
    }
    // The reflections feed the tail, through the hall's kind of network.
    const float inGain = b.freeze ? 0.0f : kSpaceTailIn;
    const int n = b.full ? kSpaceLines : 4;
    const int *act = b.full ? kHallAll : kHallLean;
    float y[kSpaceLines], z[kSpaceLines];
    float sum = 0.0f;
    for (int k = 0; k < n; ++k) {
        const int i = act[k];
        spcLen[i] += spcLenStep[i];
        rotate(spcOsc[i], spcRot[i]);
        y[k] = spcLine[i].frac(spcLen[i] + b.modDepth * (1.0f + spcOsc[i][1]));
        spcLow[i] = undenormal(spcLow[i] + (y[k] - spcLow[i]) * haLowCoeff);
        spcHigh[i] = undenormal(spcHigh[i] + (y[k] - spcHigh[i]) * haHighCoeff);
        z[k] = spcGHigh[i] * y[k] + (spcGMid[i] - spcGHigh[i]) * spcHigh[i] + (spcGLow[i] - spcGMid[i]) * spcLow[i];
        sum += z[k];
    }
    // The lean lines still glide, so they're in place if quality comes back.
    if (!b.full) for (int i : {0, 2, 5, 7}) spcLen[i] += spcLenStep[i];
    const float h = sum * (2.0f / static_cast<float>(n));
    const float inject = b.shimmer * kSpaceInject;
    float o[2] = {0.0f, 0.0f};
    for (int k = 0; k < n; ++k) {
        const int i = act[k];
        const int c = k & 1;
        const float headroom = 1.0f - spcGMid[i];
        spcLine[i].write(undenormal(z[k] - h + er[c] * inGain * kHallInSign[k] + shimmerFb[c] * inject * headroom));
        o[0] += kHallRowL[k] * y[k];
        o[1] += kHallRowR[k] * y[k];
    }
    const float outScale = kSpaceOut * (b.full ? 1.0f : 1.41f);
    const float erLevel = b.freeze ? 0.0f : 1.0f - 0.85f * b.distance;
    for (int c = 0; c < 2; ++c) {
        const float wet = o[c] * outScale + er[c] * erLevel;
        lp[c] += (wet - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Tube and string -----------------------------------------------------------

float Reverb::WaveTap::read(const Line &l, float d) {
    d = clampf(d, 1.5f, static_cast<float>(l.size - 2));
    const auto m = static_cast<int32_t>(d - 0.5f);
    const float f = d - static_cast<float>(m);
    const float c = (1.0f - f) / (1.0f + f);
    y = undenormal(c * l.at(m) + l.at(m + 1) - c * y);
    return y;
}

void Reverb::updateWave(bool string, float size, float damp, bool freeze, bool full) {
    const auto &p = params_;
    const bool follow = p.get(Follow) >= 0.5f && heardNote >= 0;
    const float pitch = p.get(Pitch), stiff = string ? p.get(Stiff) : 0.0f;
    const float key[8] = {pitch, follow ? heardHz : -1.0f, size, damp, stiff,
                          freeze ? 1.0f : 0.0f, full ? 1.0f : 0.0f, string ? 1.0f : 0.0f};
    if (std::equal(key, key + 8, wgWas)) return;
    std::copy(key, key + 8, wgWas);

    float hz = (follow ? heardHz : kWaveBaseHz) *
               std::pow(2.0f, pitch / 12.0f);
    while (hz > kWaveMaxHz) hz *= 0.5f;
    while (hz < kWaveMinHz) hz *= 2.0f;
    wgHz = hz;
    // Tube 0.25 s to 6 s, string 0.4 s to 12 s.
    const float rt = string ? 0.4f * std::pow(30.0f, size) : 0.25f * std::pow(24.0f, size);
    // Brightness: the harmonic this many up rings for kWaveHighShare of the ring, so the
    // loop's low-pass takes that much more off it each trip (the tube's odd trip is half a period).
    const float harm = string ? 40.0f * std::pow(3.0f / 40.0f, damp) : 30.0f * std::pow(3.0f / 30.0f, damp);
    const double ref = dsp::kTwoPi * std::min(hz * harm, 0.45f * sr) / sr;
    const float trip = (string ? 1.0f : 0.5f) / hz;
    const double m = decayGain(trip, rt * kWaveHighShare) / decayGain(trip, rt);
    // The one-pole's 1 - a for a gain of m at ref.
    const double cr = std::cos(ref), k = 1.0 - m * m * cr, q = 1.0 - m * m;
    const double pole = (k - std::sqrt(std::max(0.0, k * k - q * q))) / q;
    wgLpCoeff = freeze ? 1.0f : static_cast<float>(1.0 - pole);

    const int lines = full ? kWaveLines : 1;
    double fHz[kWaveLines];
    for (int c = 0; c < lines; ++c) fHz[c] = full ? hz * std::pow(2.0, (c == 0 ? -kWaveDetune : kWaveDetune) / 1200.0) : hz;
    // Stiff: the chain's allpasses, eased off until it takes no more than its share of the loop.
    wgA = 0.0f;
    if (string) {
        const double wTop = dsp::kTwoPi * fHz[lines - 1] / sr;
        const double most = kStiffShare * sr / fHz[lines - 1];
        const auto chainDelay = [&](double a) { return -kWaveStages * allpassPhase(a, wTop) / wTop; };
        double a = -kStiffMax * stiff;
        if (chainDelay(a) > most) {
            double lo = a, hi = 0.0;
            for (int k = 0; k < 30; ++k) {
                const double mid = 0.5 * (lo + hi);
                (chainDelay(mid) > most ? lo : hi) = mid;
            }
            a = hi;
        }
        wgA = static_cast<float>(a);
    }
    for (int c = 0; c < lines; ++c) {
        const double w = dsp::kTwoPi * fHz[c] / sr, period = sr / fHz[c];
        // The low-pass and the chain delay the loop too; the taps are that much shorter.
        double phase = lowPassPhase(wgLpCoeff, w);
        if (string) phase += kWaveStages * allpassPhase(wgA, w);
        wgPeriod[c] = static_cast<float>(period);
        wgD[c][1] = tapDelayFor(period + phase / w, w);
        wgD[c][0] = tapDelayFor(0.5 * period + phase / w, w);
        wgG[c][1] = freeze ? 1.0f : decayGain(static_cast<float>(period) / sr, rt);
        wgG[c][0] = freeze ? 1.0f : decayGain(static_cast<float>(0.5 * period) / sr, rt);
    }
    // Open's even harmonics follow the ring as it is unfrozen, so freeze doesn't open the tube.
    wgGLive = decayGain(1.0f / hz, rt);
    if (!full) {
        wgPeriod[1] = wgPeriod[0];
        wgD[1][0] = wgD[0][0];
        wgD[1][1] = wgD[0][1];
        wgG[1][0] = wgG[0][0];
        wgG[1][1] = wgG[0][1];
    }
}

void Reverb::tubeSample(const Block &b, const float dry[2], float out[2]) {
    const int lines = b.full ? kWaveLines : 1;
    const float inGain = b.freeze ? 0.0f : kTubeIn;
    float y[kWaveLines];
    for (int c = 0; c < lines; ++c) {
        const float in = b.full ? 0.7f * dry[c] + 0.3f * dry[c ^ 1] : 0.5f * (dry[0] + dry[1]);
        const float hp = wgHpCoeff * (wgHpY[c] + in - wgHpX[c]);
        wgHpX[c] = in;
        wgHpY[c] = undenormal(hp);
        float off = 0.0f;
        if (b.wgWob > 0.0f) {
            rotate(wgOsc[c], wgRot[c]);
            off = b.wgWob * wgPeriod[c] * wgOsc[c][1];
        }
        const float odd = wgTap[c][0].read(wgLine[c], wgD[c][0] + 0.5f * off);
        const float all = wgTap[c][1].read(wgLine[c], wgD[c][1] + off);
        const float fb = b.wgOdd * wgG[c][0] * odd + b.wgFull * wgG[c][1] * all;
        wgLp[c] = undenormal(wgLp[c] + (fb - wgLp[c]) * wgLpCoeff);
        const float x = hp * inGain + shimmerFb[c] * b.shimmer * (1.0f - wgG[c][1]) * kWaveInject;
        // The soft clip: quiet rings pass, loud ones growl.
        // Frozen, it gets the string's ceiling instead, since the growl would wear the held ring away.
        const float v = wgLp[c] + x;
        wgLine[c].write(b.freeze ? dsp::feedbackCeiling(v) : kTubeClip * fastTanh(v * (1.0f / kTubeClip)));
        // Out from a period back: a sum of the taps would cancel the even harmonics half open.
        y[c] = all;
    }
    if (lines == 1) y[1] = y[0];
    for (int c = 0; c < 2; ++c) {
        // Bloom thins the ring out too, since the clip evens out a thinner feed.
        lp[c] += (y[c] * kTubeOut * b.diffMul - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

void Reverb::stringSample(const Block &b, const float dry[2], float out[2]) {
    const int lines = b.full ? kWaveLines : 1;
    const float inGain = b.freeze ? 0.0f : kStringIn;
    const float a = wgA;
    float y[kWaveLines];
    for (int c = 0; c < lines; ++c) {
        const float in = b.full ? 0.7f * dry[c] + 0.3f * dry[c ^ 1] : 0.5f * (dry[0] + dry[1]);
        const float hp = wgHpCoeff * (wgHpY[c] + in - wgHpX[c]);
        wgHpX[c] = in;
        wgHpY[c] = undenormal(hp);
        // Pick: the input lands part way along, so the harmonics with a node there are missing.
        wgPick[c].write(hp);
        const float picked = hp - wgPick[c].frac(wgPickD[c]);
        float off = 0.0f;
        if (b.wgWob > 0.0f) {
            rotate(wgOsc[c], wgRot[c]);
            off = b.wgWob * wgPeriod[c] * wgOsc[c][1];
        }
        const float t = wgTap[c][1].read(wgLine[c], wgD[c][1] + off);
        wgLp[c] = undenormal(wgLp[c] + (t * wgG[c][1] - wgLp[c]) * wgLpCoeff);
        float v = wgLp[c];
        // Stiffness: each stage delays the lows more than the highs, so the overtones come round sooner.
        for (int k = 0; k < kWaveStages; ++k) {
            const float s = wgChain[c][k];
            const float w = undenormal(v - a * s);
            wgChain[c][k] = w;
            v = a * w + s;
        }
        const float x = picked * inGain + shimmerFb[c] * b.shimmer * (1.0f - wgG[c][1]) * kWaveInject;
        // A ceiling for a note held in tune with a long ring.
        wgLine[c].write(dsp::feedbackCeiling(v + x));
        y[c] = t;
    }
    if (lines == 1) y[1] = y[0];
    for (int c = 0; c < 2; ++c) {
        lp[c] += (y[c] * kStringOut * b.diffMul - lp[c]) * b.toneCoeff;
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Drumhead -------------------------------------------------------------------

void Reverb::updateDrum(float size, float damp, bool freeze, bool full) {
    const float tension = params_.get(Tension);
    const float key[5] = {size, tension, damp, freeze ? 1.0f : 0.0f, full ? 1.0f : 0.0f};
    if (std::equal(key, key + 5, msWas)) return;
    std::copy(key, key + 5, msWas);
    msN = full ? kMeshFull : kMeshLean;
    const auto at = [&](const float rc[2]) {
        const auto i = std::clamp(static_cast<int32_t>(rc[0] * static_cast<float>(msN)), 0, msN - 1);
        const auto j = std::clamp(static_cast<int32_t>(rc[1] * static_cast<float>(msN)), 0, msN - 1);
        return i * msN + j;
    };
    for (int c = 0; c < 2; ++c) {
        msStrikeAt[c] = at(kStrike[c]);
        msPickAt[c] = at(kPickup[c]);
    }
    for (int k = 0; k < 3; ++k) {
        const float rc[2] = {0.5f, kContact[k]};
        msTouchAt[k] = at(rc);
    }
    // The mesh's rate puts its lowest mode on the pitch, but it can't tick faster than the engine.
    const float per = kMeshTune * static_cast<float>(msN + 1);
    const float hz = std::min(kDrumHz * std::pow(4.0f, 0.5f - size) * std::pow(2.0f, 3.0f * (tension - 0.5f)),
                              sr / (per * (1.0f + kMeshWobble)));
    const float rate = hz * per;
    msInc = rate / sr;
    // 0.25 s to 4 s.
    const float rt = 0.25f * std::pow(16.0f, size);
    msLoss = freeze ? 1.0f : decayGain(1.0f / rate, rt);
    msLoop = freeze ? 0.0f : 1.0f - decayGain(1.0f / hz, rt);
    // Damp: the rim takes the highs.
    msEdgeCoeff = freeze ? 1.0f : 1.0f - 0.6f * damp * damp;
    // And every link a little, or the mesh's modes near a quarter of its rate, which barely move, never reach the rim.
    msLinkDamp = freeze ? 0.0f : kMeshDamp * damp * damp;
    // The lean mesh's modes are fewer and each louder.
    msScale = full ? 1.0f : kMeshLeanScale;
}

void Reverb::drumTick(const Block &b) {
    const int32_t n = msN;
    const float inv = msInCount > 0 ? 1.0f / static_cast<float>(msInCount) : 0.0f;
    const float x[2] = {msInSum[0] * inv, msInSum[1] * inv};
    msInSum[0] = msInSum[1] = 0.0f;
    msInCount = 0;
    const float *inN = msIn[msCur][0], *inE = msIn[msCur][1], *inS = msIn[msCur][2], *inW = msIn[msCur][3];
    float *toN = msIn[msCur ^ 1][0], *toE = msIn[msCur ^ 1][1], *toS = msIn[msCur ^ 1][2], *toW = msIn[msCur ^ 1][3];
    const float loss = msLoss, ec = msEdgeCoeff, a = msLinkDamp;
    const auto edge = [ec](float &e, float o) {
        e = undenormal(e + (o - e) * ec);
        return -e;
    };
    // Each junction: the waves in, averaged, are its motion; each wave out is that less the wave
    // that came in on the same side. Out over the rim comes back inverted through the rim's low-pass.
    for (int32_t i = 0; i < n; ++i) {
        for (int32_t j = 0; j < n; ++j) {
            const int32_t idx = i * n + j;
            float v = 0.5f * (inN[idx] + inE[idx] + inS[idx] + inW[idx]);
            if (idx == msStrikeAt[0]) v += x[0];
            if (idx == msStrikeAt[1]) v += x[1];
            v = undenormal(v);
            const float oN = loss * (v - inN[idx]), oE = loss * (v - inE[idx]);
            const float oS = loss * (v - inS[idx]), oW = loss * (v - inW[idx]);
            // A link's low-pass: what it carries now, eased towards what it carried last tick.
            if (i == 0) toN[idx] = edge(msEdge[0][j], oN); else toS[idx - n] = oN + a * (inS[idx - n] - oN);
            if (j == n - 1) toE[idx] = edge(msEdge[1][i], oE); else toW[idx + 1] = oE + a * (inW[idx + 1] - oE);
            if (i == n - 1) toS[idx] = edge(msEdge[2][j], oS); else toN[idx + n] = oS + a * (inN[idx + n] - oS);
            if (j == 0) toW[idx] = edge(msEdge[3][i], oW); else toE[idx - 1] = oW + a * (inE[idx - 1] - oW);
        }
    }
    // The pickups and the snares read the junctions as this tick moved them.
    const auto motion = [&](int32_t idx) {
        float v = 0.5f * (inN[idx] + inE[idx] + inS[idx] + inW[idx]);
        if (idx == msStrikeAt[0]) v += x[0];
        if (idx == msStrikeAt[1]) v += x[1];
        return v;
    };
    for (int c = 0; c < 2; ++c) {
        msPrev[c] = msNow[c];
        msNow[c] = motion(msPickAt[c]);
    }
    float touch = 0.0f;
    for (const int32_t idx : msTouchAt) touch = std::max(touch, std::fabs(motion(idx)));
    snPast = std::max(0.0f, touch - b.snGap);
    msCur ^= 1;
}

void Reverb::drumSample(const Block &b, const float dry[2], float out[2]) {
    const float inGain = b.freeze ? 0.0f : kDrumIn;
    for (int c = 0; c < 2; ++c) msInSum[c] += dry[c] * inGain + shimmerFb[c] * b.shimmer * msLoop * kDrumInject;
    ++msInCount;
    // The mesh ticks at its own rate; the input is averaged over a tick and the output drawn between ticks.
    float inc = msInc;
    if (b.msWob > 0.0f) {
        rotate(msOsc, msRot);
        inc *= 1.0f + b.msWob * msOsc[1];
    }
    msAcc += inc;
    while (msAcc >= 1.0f) {
        msAcc -= 1.0f;
        drumTick(b);
    }
    // Snares: noise in the wires' band, as loud as the head went past them, so it buzzes with the head's motion.
    float buzz = 0.0f;
    if (b.snLevel > 0.0f) {
        const float target = snPast * kSnareGain;
        snEnv = undenormal(snEnv + (target - snEnv) * (target > snEnv ? b.snAtk : b.snRel));
        const float nz = whiteNoise(snNoise);
        const float hp = snHpCoeff * (snHpY + nz - snHpX);
        snHpX = nz;
        snHpY = undenormal(hp);
        snLp = undenormal(snLp + (hp - snLp) * snLpCoeff);
        buzz = snLp * snEnv * b.snLevel;
    }
    for (int c = 0; c < 2; ++c) {
        const float y = (msPrev[c] + (msNow[c] - msPrev[c]) * msAcc) * msScale * kDrumOut + buzz;
        lp[c] += (y * b.diffMul - lp[c]) * b.toneCoeff;
        lp[c] = undenormal(lp[c]);
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Thunder sheet ---------------------------------------------------------------

void Reverb::updateThunder(float size, float damp, bool freeze, bool full) {
    const float metal = params_.get(Metal);
    const float key[5] = {size, damp, metal, freeze ? 1.0f : 0.0f, full ? 1.0f : 0.0f};
    if (std::equal(key, key + 5, thWas)) return;
    std::copy(key, key + 5, thWas);
    const float lo = kThunderHz * std::pow(4.0f, 0.5f - size);
    // 0.6 s to 9 s at the bottom; damp shortens the modes up the range, per kHz.
    const float rt = 0.6f * std::pow(15.0f, size);
    const float perKHz = 0.05f + 2.0f * damp * damp;
    // The modes spread from lo to kThunderSpan times it, sparse at the bottom and crowded at the top.
    const float power = std::log(kThunderSpan) / std::log(static_cast<float>(kThModes));
    // A thin sheet takes more of the strike's top.
    const float strike = kThunderStrikeHz * (1.0f + 2.0f * metal);
    for (int k = 0; k < kThModes; ++k) {
        const float kf = static_cast<float>(k);
        const float jitter = 1.0f + 0.03f * std::sin(kf * 16.7993f);
        const float hz = lo * std::pow(kf + 1.0f, power) * jitter;
        const float ratio = hz / lo;
        thBand[k] = ratio < kThBandTop[0] ? 0 : ratio < kThBandTop[1] ? 1 : ratio < kThBandTop[2] ? 2 : 3;
        if (hz > 0.45f * sr) {
            thCos[k] = thSin[k] = thIn[k] = thNz[k] = 0.0f;
            continue;
        }
        const float r = decayGain(1.0f / sr, rt / (1.0f + perKHz * hz * 0.001f));
        // Fed in at sqrt(1 - r^2), so every mode rings at about the input's level whatever its decay.
        const float norm = std::sqrt(1.0f - r * r);
        const float w = dsp::kTwoPi * hz / sr, held = freeze ? 1.0f : r;
        thCos[k] = held * std::cos(w);
        thSin[k] = held * std::sin(w);
        thIn[k] = kThunderIn * norm / std::sqrt(1.0f + (hz / strike) * (hz / strike));
        thNz[k] = norm;
        // Each side hears the modes at its own strengths and signs.
        const float u = 0.5f + 0.5f * std::sin(kf * 7.31f), v = 0.5f + 0.5f * std::sin(kf * 3.17f + 1.0f);
        thOutL[k] = (k & 1 ? -1.0f : 1.0f) * (0.6f + 0.4f * u);
        thOutR[k] = (k % 3 == 0 ? -1.0f : 1.0f) * (0.6f + 0.4f * v);
    }
    thSpill = freeze ? 0.0f : kSpill * metal;
    // The drain is per sample, so it's scaled to the rate.
    thDrain = freeze ? 0.0f : kDrain * metal * 48000.0f / sr;
    thDrainMax = kDrainMax * 48000.0f / sr;
    thLoop = freeze ? 0.0f : 1.0f - decayGain(1.0f / lo, rt);
    thStep = full ? 1 : 2;
    thScale = full ? 1.0f : std::sqrt(2.0f);
    thOutScale = full ? 1.0f : kThunderLeanOut;
}

void Reverb::thunderSample(const Block &b, const float dry[2], float out[2]) {
    const float x = (b.freeze ? 0.0f : 0.5f * (dry[0] + dry[1])) +
                    0.5f * (shimmerFb[0] + shimmerFb[1]) * b.shimmer * thLoop * kThunderInject;
    // The louder a band rings the more noise it spills into the one above, and the faster it loses its own.
    float spill[kThBands], keep[kThBands];
    for (int band = 0; band < kThBands; ++band) {
        // A spill too small to lift a mode over its floor is dropped, or its square underflows.
        const float amount = band == 0 ? 0.0f : std::min(kSpillMax, thSpill * thEnergy[band - 1]);
        spill[band] = amount < kSpillFloor ? 0.0f : amount * whiteNoise(thNoise);
        keep[band] = 1.0f - std::min(thDrainMax, thDrain * thEnergy[band]);
    }
    float l = 0.0f, r = 0.0f, sum[kThBands]{};
    for (int k = 0; k < kThModes; k += thStep) {
        const int band = thBand[k];
        const float c = thCos[k] * keep[band], s = thSin[k] * keep[band];
        float re = c * thRe[k] - s * thIm[k] + x * thIn[k] + spill[band] * thNz[k];
        float im = s * thRe[k] + c * thIm[k];
        const float m2 = re * re + im * im;
        if (m2 > kModeMax * kModeMax) {
            const float g = kModeMax / std::sqrt(m2);
            re *= g;
            im *= g;
        } else if (m2 < kModeFloor * kModeFloor) {
            // A rotating mode this quiet rounds into a cycle that never dies, so it stops here.
            re = im = 0.0f;
        }
        thRe[k] = re;
        thIm[k] = im;
        sum[band] += re;
        l += thOutL[k] * re;
        r += thOutR[k] * re;
    }
    for (int band = 0; band < kThBands; ++band) {
        // Flushed, so e * e can't underflow where the FPU doesn't flush for us.
        float e = sum[band] * thScale;
        if (std::fabs(e) < 1e-15f) e = 0.0f;
        const float energy = thEnergy[band] + (e * e - thEnergy[band]) * thEnergyCoeff;
        thEnergy[band] = energy < 1e-30f ? 0.0f : energy;
    }
    const float y[2] = {l * thOutScale * kThunderOut, r * thOutScale * kThunderOut};
    for (int c = 0; c < 2; ++c) {
        lp[c] += (y[c] * b.diffMul - lp[c]) * b.toneCoeff;
        lp[c] = undenormal(lp[c]);
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Howl -----------------------------------------------------------------------

void Reverb::updateHowl(float size, float damp, bool freeze) {
    const float howl = params_.get(Howl);
    const float key[4] = {size, damp, howl, freeze ? 1.0f : 0.0f};
    if (std::equal(key, key + 4, hwWas)) return;
    std::copy(key, key + 4, hwWas);
    // Half to twice the middle size.
    const float metres = kHowlMetres * std::pow(2.0f, 2.0f * size - 1.0f);
    // The peak moves with the room, so the same harmonics sing and size retunes them.
    const float w0 = dsp::kTwoPi * std::min(kHowlPeakHz * kHowlMetres / metres, 0.4f * sr) / sr;
    const float alpha = std::sin(w0) / (2.0f * kHowlQ);
    hwB0 = alpha / (1.0f + alpha);
    hwA1 = -2.0f * std::cos(w0) / (1.0f + alpha);
    hwA2 = (1.0f - alpha) / (1.0f + alpha);
    // 0.3 s to 3.6 s with no howl.
    const float rt = 0.3f * std::pow(12.0f, size);
    hwLpCoeff = freeze ? 1.0f : dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * 12000.0f * std::pow(1500.0f / 12000.0f, damp)), sr);
    // The low-pass's loss at the peak is made up, so the loop gain there is what howl asks for.
    // The lows, which pass outside the peak at (1 - howl), are kept under 1 while doing it.
    const float atPeak = std::abs(std::complex<float>(hwLpCoeff) / (1.0f - (1.0f - hwLpCoeff) * std::polar(1.0f, -w0)));
    const float lowsMost = howl < 1.0f ? kHowlLowsMost / (1.0f - howl) : 1e9f;
    for (int i = 0; i < kHowlLines; ++i) {
        // A round trip across the room on this axis.
        hwD[i] = 2.0f * metres * kHowlProp[i] / kSoundSpeed * sr;
        const float sec = hwD[i] / sr;
        // Up to the edge howl stretches the ring towards forever; past it the loop grows.
        const float db = howl <= kHowlEdge ? -60.0f * sec / rt * (1.0f - howl / kHowlEdge)
                                           : kHowlGrow * sec * (howl - kHowlEdge) / (1.0f - kHowlEdge);
        hwG[i] = freeze ? 1.0f : std::min(std::pow(10.0f, db / 20.0f) / atPeak, lowsMost);
    }
    hwLoop = freeze ? 0.0f : 1.0f - decayGain(hwD[kHowlLines - 1] / sr, rt);
}

void Reverb::howlSample(const Block &b, const float dry[2], float out[2]) {
    static constexpr float kInSign[kHowlLines] = {1.0f, -1.0f, 1.0f};
    static constexpr float kOutL[kHowlLines] = {1.0f, 1.0f, -1.0f};
    static constexpr float kOutR[kHowlLines] = {1.0f, -1.0f, -1.0f};
    const float in = (b.freeze ? 0.0f : 0.5f * (dry[0] + dry[1]) * kHowlIn) +
                     0.5f * (shimmerFb[0] + shimmerFb[1]) * b.shimmer * hwLoop * kHowlInject;
    const float m = b.hwMix;
    float y[2] = {0.0f, 0.0f};
    for (int i = 0; i < kHowlLines; ++i) {
        float d = hwD[i];
        if (b.hwWob > 0.0f) {
            rotate(hwOsc[i], hwRot[i]);
            d += b.hwWob * hwD[i] * hwOsc[i][1];
        }
        const float t = hwLine[i].frac(d);
        // The speaker's peak: the more howl, the more of the loop goes through it. It runs
        // with no howl too, so it's in step when howl comes back.
        const float bp = hwB0 * t + hwBp[i][0];
        hwBp[i][0] = undenormal(-hwA1 * bp + hwBp[i][1]);
        hwBp[i][1] = undenormal(-hwB0 * t - hwA2 * bp);
        const float v0 = (1.0f - m) * t + m * bp;
        hwLp[i] = undenormal(hwLp[i] + (v0 - hwLp[i]) * hwLpCoeff);
        float v = hwLp[i] * hwG[i];
        // The limiter holds a singing mode at its level.
        hwEnv[i] = undenormal(peakEnvelope(hwEnv[i], v, hwEnvAtk, hwEnvRel));
        if (hwEnv[i] > kHowlCeil) v *= kHowlCeil / hwEnv[i];
        hwLine[i].write(dsp::feedbackCeiling(v + in * kInSign[i]));
        y[0] += kOutL[i] * t;
        // The second microphone hears each line from another place along it.
        if (b.full) y[1] += kOutR[i] * hwLine[i].frac(0.61f * d);
    }
    if (!b.full) y[1] = y[0];
    const float ceil = std::min(kHowlOutCeil, kHowlOutMost / (b.wetGain * b.wetWidth));
    for (int c = 0; c < 2; ++c) {
        // A tail under 1e-15 is flushed, or its square in the tanh underflows.
        float w = y[c] * kHowlOut * b.diffMul;
        if (std::fabs(w) < 1e-15f) w = 0.0f;
        lp[c] += (ceil * fastTanh(w / ceil) - lp[c]) * b.toneCoeff;
        lp[c] = undenormal(lp[c]);
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

// --- Flutter --------------------------------------------------------------------

void Reverb::updateFlutter(float size, float damp, bool freeze) {
    const float walls = params_.get(Walls);
    const float key[4] = {size, damp, walls, freeze ? 1.0f : 0.0f};
    if (std::equal(key, key + 4, flWas)) return;
    std::copy(key, key + 4, flWas);
    // 0.15 s to 3 s.
    const float rt = 0.15f * std::pow(20.0f, size);
    const float cut = std::min(16000.0f * std::pow(1500.0f / 16000.0f, damp), 0.45f * sr);
    flLpCoeff = freeze ? 1.0f : dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * cut), sr);
    // The low-pass delays the lows by this much, so the line is that much shorter and the pitch holds.
    const float lag = (1.0f - flLpCoeff) / flLpCoeff;
    for (int c = 0; c < kFlutterLines; ++c)
        flD[c] = std::max(2.0f, 2.0f * walls * (c == 0 ? 1.0f : kFlutterSkew) / kSoundSpeed * sr - lag);
    flG = freeze ? 1.0f : decayGain(2.0f * walls / kSoundSpeed, rt);
    flLoop = freeze ? 0.0f : 1.0f - flG;
}

void Reverb::flutterSample(const Block &b, const float dry[2], float out[2]) {
    const int lines = b.full ? kFlutterLines : 1;
    const float inGain = b.freeze ? 0.0f : kFlutterIn;
    float y[2], first = 0.0f, firstD = 0.0f;
    for (int c = 0; c < lines; ++c) {
        const float in = b.full ? dry[c] : 0.5f * (dry[0] + dry[1]);
        float d = flD[c];
        if (b.flWob > 0.0f) {
            rotate(flOsc[c], flRot[c]);
            d += b.flWob * flD[c] * flOsc[c][1];
        }
        const float t = flLine[c].frac(d);
        flLp[c] = undenormal(flLp[c] + (t - flLp[c]) * flLpCoeff);
        flLine[c].write(dsp::feedbackCeiling(flLp[c] * flG + in * inGain + shimmerFb[c] * b.shimmer * flLoop * kFlutterInject));
        // The other wall's echoes fall part way through each round trip.
        y[c] = t + 0.7f * flLine[c].frac(kFlutterTap[c] * d);
        first = t;
        firstD = d;
    }
    if (lines == 1) y[1] = first + 0.7f * flLine[0].frac(kFlutterTap[1] * firstD);
    for (int c = 0; c < 2; ++c) {
        lp[c] += (y[c] * kFlutterOut * b.diffMul - lp[c]) * b.toneCoeff;
        lp[c] = undenormal(lp[c]);
        const float t = lofi(c, lp[c], b);
        shimmerStep(c, t, b);
        out[c] = t;
    }
}

} // namespace acidulous::effect
