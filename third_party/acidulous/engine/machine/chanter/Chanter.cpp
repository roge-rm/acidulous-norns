#include "Chanter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>
#include <sequencer/Swing.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level at full, set so the bank sits with the other machines. */
constexpr float kPipeHouse = 1.6f, kStringHouse = 0.2f;
constexpr float kSilent = 2e-5f;
/** How hard the bag blows the chanter and the drones. */
constexpr float kChanterPush = 0.95f, kDronePush = 0.85f;
/** How quickly the bag fills and empties, seconds. */
constexpr float kFills = 0.03f, kEmpties = 0.3f;
/** A closed chanter's gap between notes, seconds. */
constexpr float kShut = 0.012f;
/** How fast a chanter's blowing pressure rises at a note's start and falls at its end, seconds. A step clicks. */
constexpr float kPushRise = 0.002f, kPushFall = 0.012f;
/** How long a sounding chanter's tube takes to move to a new fingering, seconds. A jump clicks. */
constexpr float kFingerGlide = 0.003f;
/** A coup: how much it pushes the wheel, and how quickly that passes, seconds. */
constexpr float kCoupPush = 0.8f, kCoupFalls = 0.06f;
/** How long the strings ring once the wheel stops bowing them, seconds. */
constexpr float kStringT60 = 2.5f, kStoppedT60 = 0.15f;
/**
 * The two melody strings, tuned 3 cents apart: the second plays 12 dB under
 * the first so they beat about 4 dB deep. Both are lifted so the pair holds
 * the same average power as two at full: sqrt(2 / (1 + 0.25^2)).
 */
constexpr float kMelodyFirst = 1.3719887f, kMelodySecond = 0.25f * kMelodyFirst;
/** The nudge that starts a string on the wheel. */
constexpr float kKick = 0.6f;
/** How hard the wheel pushes the strings. */
constexpr float kBowGain = 0.12f;
/** The dog's buzz at full, and the most it can ring. */
constexpr float kDogGain = 150.0f, kDogMost = 1.0f;
/** The pipes speak a little flat of the note they're tuned to; this much makes it up, cents. */
constexpr float kPipeTrim = 10.0f;
/** Drift: how far the bag's pressure wanders, and the pitch with it, cents. */
constexpr float kDriftPush = 0.06f, kDriftCents = 6.0f;

/**
 * A finite bag, in bags a second at full pressure: the drones alone, the
 * chanter on top, what the arm squeezes out once the piper stops blowing
 * (a hurdy-gurdy's wheel coasts down slower), and the piper striking in.
 */
constexpr float kDroneUse = 0.12f, kChanterUse = 0.25f, kLetGo = 0.6f, kCoast = 0.3f, kStrikeFill = 2.0f;
/** How long the piper blows hard to strike in from a soft bag, seconds. */
constexpr float kStrike = 0.5f;
/** Below this much air the pressure sags. */
constexpr float kSagFrom = 0.35f;
/** Where each drone chokes as the pressure falls, and the margin it needs to start again. */
constexpr float kChoke[Chanter::kDrones] = {0.5f, 0.38f, 0.26f};
constexpr float kChokeBack = 0.06f;
/** How far the pitch falls at full sag, semitones: the drones as they die, the chanter and the strings. */
constexpr float kDroneGroan = 2.0f, kChanterGroan = 0.8f, kStringGroan = 0.6f;
/** How quickly a drone stops or starts once the pressure crosses it, seconds, and its push follows, per sample. */
constexpr float kChokeTime = 0.08f, kDronePushTime = 0.01f;
/** Launeddas: a breath in through the nose every few seconds, and how far the cheeks let the pressure dip. */
constexpr float kCheekDip = 0.25f, kCheekTime = 0.3f;

/** Listening for the bass: the band, taken down 16 times, picked every 20 ms. */
constexpr float kBassLowHz = 30.0f, kBassHighHz = 250.0f, kEarLowHz = 300.0f, kEarEvery = 0.02f;
constexpr int kEarDown = 16;
/** Quieter than this isn't a bass note: the drones stay where they are. */
constexpr float kEarFloor = 1e-9f;

/**
 * Each kind: its drones, as semitones from the key note (A3 for A), their
 * level, the chanter's bore and reed, where its holes cut off the top, and
 * whether it's closed between notes.
 */
struct Make {
    int drones;
    float droneAt[Chanter::kDrones];
    float droneCents[Chanter::kDrones];
    float droneLevel;
    bool cylinder;
    float lattice;
    float reed;
    bool closed;
    float level;
    /** How loud a second chanter plays the key held before the last, against the first; 0 for none. */
    float second = 0.0f;
};
constexpr Make kMakes[Chanter::KindCount] = {
    // Highland pipes: two tenor drones on the key and a bass an octave down, a loud, bright chanter.
    {3, {0.0f, 0.0f, -12.0f}, {0.0f, 3.0f, -2.0f}, 0.5f, false, 3000.0f, 0.7f, false, 1.0f},
    // Smallpipes: a quiet, closed chanter, drones on the key, the fifth and the octave below.
    {3, {0.0f, 7.0f, -12.0f}, {0.0f, 2.0f, -2.0f}, 0.4f, true, 2200.0f, 0.5f, true, 1.0f},
    // Gaita: one bass drone and a conical chanter, sweeter.
    {1, {-12.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.55f, false, 2200.0f, 0.55f, false, 1.0f},
    // Hurdy-gurdy: bourdon an octave down and mouche a fourth below the key.
    {2, {-12.0f, -5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f, false, 0.0f, 0.0f, false, 1.0f},
    // Uilleann pipes: blown from bellows, a sweet, quiet conical chanter
    // played closed, drones on the key and one and two octaves below. A key
    // held under the melody sounds on the regulators, quieter.
    {3, {0.0f, -12.0f, -24.0f}, {0.0f, 2.0f, -2.0f}, 0.35f, false, 2600.0f, 0.45f, true, 0.8f, 0.45f},
    // Gaida: a goatskin bag, one deep drone two octaves down, a bright,
    // reedy chanter, open between notes for the fast ornaments.
    {1, {-24.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f, true, 3500.0f, 0.75f, false, 0.9f},
    // Cornemuse: the French bagpipe, a big and a small drone, a conical
    // chanter played half closed.
    {2, {0.0f, -12.0f, 0.0f}, {0.0f, 3.0f, 0.0f}, 0.45f, false, 2400.0f, 0.6f, true, 0.9f},
    // Musette de cour: small and courtly, a soft, narrow chanter, its
    // drones quiet in a short barrel.
    {2, {0.0f, -12.0f, 0.0f}, {0.0f, 2.0f, 0.0f}, 0.3f, true, 1800.0f, 0.4f, true, 0.7f},
    // Säckpipa: the Swedish pipe, one drone an octave down, a narrow
    // cylinder played closed.
    {1, {-12.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.45f, true, 2000.0f, 0.5f, true, 0.8f},
    // Dudy: the Czech pipe, one long drone two octaves down, loud, and a
    // cylindrical chanter.
    {1, {-24.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.6f, true, 1800.0f, 0.6f, false, 0.9f},
    // Zampogna: two conical chanters, one for each hand, and drones on the key
    // and the octave below.
    {2, {0.0f, -12.0f, 0.0f}, {0.0f, 2.0f, 0.0f}, 0.4f, false, 2600.0f, 0.65f, false, 0.8f, 0.8f},
    // Tulum: no drone, two cylindrical chanters side by side.
    {0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, true, 3000.0f, 0.7f, false, 0.9f, 0.9f},
    // Launeddas: three cane pipes with single reeds, blown without a bag by
    // breathing in through the nose: a drone, and two melody pipes.
    {1, {-12.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.5f, true, 2800.0f, 0.6f, false, 0.9f, 0.8f},
};

/**
 * What a chanter needs added to speak in tune, cents, by kind and by the
 * note, from E3 (52) every 4 semitones, measured across keys. The reed and
 * the holes pull the pitch: flat at the bottom, sharp at the top. The
 * hurdy-gurdy has none.
 */
constexpr int kTrimPoints = 12;
constexpr float kTrimFrom = 52.0f, kTrimStep = 4.0f;
constexpr float kChanterTrim[Chanter::KindCount][kTrimPoints] = {
    {-3.2f, 1.5f, 6.2f, 7.0f, 6.6f, 2.9f, -0.1f, -2.6f, -8.5f, -13.9f, -18.0f, -22.3f}, // highland
    {-3.4f, 0.8f, 5.6f, 7.3f, 4.2f, -0.5f, -7.5f, -16.2f, -23.3f, -28.1f, -33.2f, -34.8f}, // smallpipes
    {-3.6f, 0.8f, 4.9f, 4.2f, 2.6f, -2.2f, -6.1f, -8.8f, -16.7f, -23.3f, -22.2f, -25.6f}, // gaita
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // hurdy-gurdy
    {-3.9f, 0.4f, 4.2f, 3.7f, 1.8f, -2.9f, -7.2f, -10.2f, -17.7f, -24.1f, -24.7f, -28.7f}, // uilleann
    {-2.5f, 2.2f, 7.7f, 11.5f, 11.9f, 10.5f, 7.2f, -0.6f, -5.5f, -15.9f, -22.9f, -26.1f}, // gaida
    {-3.4f, 1.1f, 5.4f, 5.3f, 4.1f, -0.5f, -4.2f, -6.8f, -14.1f, -20.4f, -21.0f, -24.5f}, // cornemuse
    {-4.0f, -0.3f, 3.9f, 4.3f, -0.9f, -7.4f, -15.7f, -22.6f, -30.5f, -32.6f, -39.1f, -41.3f}, // musette
    {-3.4f, 0.6f, 5.4f, 6.8f, 3.0f, -2.3f, -9.9f, -17.9f, -24.8f, -28.3f, -33.2f, -34.8f}, // säckpipa
    {-3.0f, 1.3f, 5.9f, 7.1f, 3.1f, -2.5f, -10.2f, -17.4f, -23.4f, -25.4f, -29.4f, -30.6f}, // dudy
    {-3.3f, 1.4f, 5.8f, 5.9f, 5.1f, 0.9f, -2.4f, -4.8f, -11.8f, -17.7f, -20.0f, -23.4f}, // zampogna
    {-2.7f, 1.8f, 7.4f, 10.7f, 10.3f, 8.3f, 4.0f, -4.6f, -10.7f, -19.6f, -25.5f, -27.5f}, // tulum
    {-2.9f, 1.5f, 6.8f, 9.6f, 8.4f, 5.5f, 0.5f, -8.5f, -15.1f, -22.8f, -29.1f, -30.6f}, // launeddas
};
/**
 * Above note 92 the trim goes by the semitone, from 92 to 108: the top notes
 * are uneven from one to the next, so the coarse table can't follow them.
 * Its first column is the coarse table's value at 92.
 */
constexpr int kTopPoints = 17;
constexpr float kTopFrom = 92.0f;
constexpr float kTopTrim[Chanter::KindCount][kTopPoints] = {
    {-18.0f, -20.9f, -21.6f, -22.0f, -22.9f, -23.3f, -23.8f, -24.9f, -24.8f, -25.2f, -26.4f, -26.7f, -28.0f, -27.0f, -28.4f, -31.0f, -28.5f}, // highland
    {-33.2f, -35.9f, -37.3f, -38.8f, -40.1f, -40.4f, -42.6f, -43.5f, -46.7f, -46.9f, -50.3f, -55.0f, -55.6f, -55.8f, -53.5f, -53.0f, 19.6f}, // smallpipes
    {-22.2f, -23.7f, -24.3f, -25.0f, -26.6f, -26.7f, -27.4f, -29.1f, -28.6f, -29.0f, -29.5f, -30.7f, -32.6f, -32.5f, -33.8f, -33.3f, -29.1f}, // gaita
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // hurdy-gurdy
    {-24.7f, -26.1f, -26.7f, -27.8f, -29.9f, -29.7f, -30.7f, -32.9f, -32.3f, -32.9f, -33.4f, -34.9f, -34.4f, -31.1f, -29.2f, -28.4f, -23.0f}, // uilleann
    {-22.9f, -24.4f, -25.5f, -27.6f, -29.1f, -29.3f, -32.0f, -31.3f, -33.3f, -33.8f, -34.4f, -36.8f, -39.0f, -38.9f, -41.1f, -44.8f, -2.8f}, // gaida
    {-21.0f, -22.7f, -23.3f, -23.9f, -25.2f, -25.4f, -26.0f, -27.4f, -27.2f, -27.6f, -29.2f, -29.1f, -30.8f, -30.3f, -31.8f, -32.9f, -31.1f}, // cornemuse
    {-39.1f, -42.7f, -42.5f, -45.5f, -46.3f, -48.8f, -51.1f, -53.0f, -57.8f, -58.3f, -58.5f, -58.9f, -51.9f, -45.0f, -41.5f, -41.0f, 36.8f}, // musette
    {-33.2f, -35.9f, -37.3f, -38.8f, -40.1f, -40.4f, -42.6f, -43.5f, -46.7f, -46.9f, -50.3f, -55.0f, -55.6f, -55.8f, -53.5f, -53.0f, 19.6f}, // säckpipa
    {-29.4f, -31.0f, -32.7f, -33.4f, -34.8f, -34.9f, -37.2f, -37.3f, -40.2f, -40.0f, -41.5f, -46.4f, -46.9f, -46.2f, -49.2f, -55.2f, 1.6f}, // dudy
    {-20.0f, -21.9f, -22.4f, -22.9f, -24.0f, -24.3f, -24.8f, -26.0f, -25.9f, -26.3f, -27.7f, -27.7f, -29.3f, -28.5f, -29.9f, -31.1f, -30.0f}, // zampogna
    {-25.5f, -27.3f, -28.9f, -29.6f, -31.2f, -30.9f, -33.4f, -33.0f, -35.3f, -35.5f, -36.4f, -38.8f, -41.2f, -41.2f, -43.9f, -47.7f, -1.4f}, // tulum
    {-29.1f, -30.9f, -32.7f, -33.4f, -34.8f, -34.9f, -37.2f, -37.3f, -40.2f, -40.0f, -41.5f, -46.4f, -46.9f, -46.2f, -49.2f, -55.2f, 1.6f}, // launeddas
};
/**
 * The bell's share of the wave sent back up the bore. It fades out over the
 * four semitones above 92, and over the four from 24 to 28 semitones above
 * the tube's lowest note [tube]: with nearly every hole open the bore below
 * them hardly answers, and its echo pulled each high note sharp or flat by
 * where it fell against the tube, so by key as well as by note.
 */
float bellBack(float note, float tube) {
    const float fromTop = (kTopFrom + 4.0f - note) / 4.0f;
    const float fromTube = (tube + 28.0f - note) / 4.0f;
    return 0.9f * clampf(std::min(fromTop, fromTube), 0.0f, 1.0f);
}
/** A harder reed plays flatter: cents added per unit of the reed knob past the middle. */
constexpr float kReedTrim[Chanter::KindCount] = {8.1f, 17.8f, 11.1f, 0, 17.4f, 9.1f, 10.0f, 23.0f, 16.7f, 11.0f, 8.6f, 10.9f, 13.8f};

float chanterTrim(int kind, float note, float reed) {
    if (note > kTopFrom) {
        const float t = clampf(note - kTopFrom, 0.0f, static_cast<float>(kTopPoints - 1));
        const int i = std::min(static_cast<int>(t), kTopPoints - 2);
        const float f = t - static_cast<float>(i);
        const float *row = kTopTrim[kind];
        return row[i] + (row[i + 1] - row[i]) * f + kReedTrim[kind] * (reed - 0.5f);
    }
    const float at = clampf((note - kTrimFrom) / kTrimStep, 0.0f, static_cast<float>(kTrimPoints - 1));
    const int i = std::min(static_cast<int>(at), kTrimPoints - 2);
    const float f = at - static_cast<float>(i);
    const float *row = kChanterTrim[kind];
    return row[i] + (row[i + 1] - row[i]) * f + kReedTrim[kind] * (reed - 0.5f);
}

constexpr float kCoupsPerBeat[4] = {0.0f, 1.0f, 2.0f, 4.0f};

/**
 * What the hurdy-gurdy's strings need added to speak in tune, cents, by the
 * crank (rows, 0 to 1 by 0.2) and the rosin (columns, 0 to 1 by 0.05). A
 * string held longer on the wheel each swing runs flat, the more as the rosin
 * grips harder and the slower the wheel turns, down to about 25 cents.
 */
constexpr int kRosinCranks = 6, kRosinSteps = 21;
constexpr float kRosinTrim[kRosinCranks][kRosinSteps] = {
    {1.3f, 1.4f, 1.4f, 1.3f, 1.2f, 1.1f, 0.8f, -0.7f, -0.1f, 0.5f, 1.8f, 5.6f, 10.5f, 16.4f, 22.3f, 23.1f, 23.6f, 23.9f, 24.4f, 25.2f, 25.9f}, // crank 0.0
    {-0.1f, 0.8f, 0.5f, 0.5f, 0.0f, 0.3f, 0.6f, 0.5f, 1.2f, 1.6f, 4.2f, 5.1f, 7.9f, 11.7f, 16.1f, 19.3f, 21.7f, 23.1f, 23.7f, 24.6f, 25.3f}, // crank 0.2
    {0.2f, 0.9f, 0.5f, 0.4f, 0.0f, 0.2f, 1.1f, 1.0f, 0.9f, 1.9f, 2.9f, 4.2f, 5.7f, 8.5f, 11.5f, 14.6f, 18.8f, 21.5f, 22.8f, 23.9f, 24.7f}, // crank 0.4
    {0.8f, 1.6f, 0.9f, 0.6f, 0.0f, 0.1f, 0.9f, 0.7f, 0.4f, 1.2f, 1.6f, 4.1f, 4.5f, 6.6f, 8.4f, 11.9f, 14.9f, 18.9f, 21.4f, 23.0f, 24.2f}, // crank 0.6
    {1.6f, 1.9f, 1.3f, 1.2f, 0.7f, 0.1f, 0.2f, 1.0f, 0.8f, 0.6f, 1.4f, 1.8f, 4.4f, 5.1f, 7.0f, 9.4f, 12.1f, 16.0f, 19.6f, 21.9f, 23.6f}, // crank 0.8
    {1.8f, 2.1f, 2.3f, 1.8f, 1.7f, 1.0f, 0.8f, 1.0f, 1.1f, 0.9f, 0.7f, 1.6f, 2.1f, 4.3f, 5.1f, 7.1f, 10.7f, 13.6f, 17.7f, 20.7f, 22.9f}, // crank 1.0
};

float rosinTrim(float rosin, float crank) {
    const float x = clampf(rosin, 0.0f, 1.0f) * static_cast<float>(kRosinSteps - 1);
    const float y = clampf(crank, 0.0f, 1.0f) * static_cast<float>(kRosinCranks - 1);
    const int i = std::min(static_cast<int>(x), kRosinSteps - 2), j = std::min(static_cast<int>(y), kRosinCranks - 2);
    const float fx = x - static_cast<float>(i), fy = y - static_cast<float>(j);
    const float a = kRosinTrim[j][i] + (kRosinTrim[j][i + 1] - kRosinTrim[j][i]) * fx;
    const float b = kRosinTrim[j + 1][i] + (kRosinTrim[j + 1][i + 1] - kRosinTrim[j + 1][i]) * fx;
    return a + (b - a) * fy;
}

} // namespace

Chanter::Chanter() { initParams(); }

const ParamDef *Chanter::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        {"model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""}, // highland, smallpipes, gaita, hurdy-gurdy, uilleann, gaida, cornemuse, musette, säckpipa, dudy, zampogna, tulum, launeddas
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        // The key the drones are tuned to, C to B.
        {"key", 0.0f, 11.0f, 9.0f, Curve::Stepped, 12, ""},
        {"drones", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // How long the drones go on after the last key's let go, seconds.
        {"bag", 0.0f, 8.0f, 1.5f, Curve::Linear, 0, "s"},
        {"reed", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // A quick high note before each one; 0 for none.
        {"grace", 0.0f, 60.0f, 0.0f, Curve::Linear, 0, "ms"},
        {"drift", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"air", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        // How fast the hurdy-gurdy's wheel turns.
        {"wheel", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"rosin", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"dog", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // How fast the wheel must turn before the dog buzzes.
        {"threshold", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"coup", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""}, // off, 1, 2 or 4 a beat
        {"velocity", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
    };
    static ParamDef defs[Count];
    static char names[kMacros * MacroParams][12];
    static bool built = false;
    if (!built) {
        for (int32_t i = 0; i < MacroBase; ++i) defs[i] = first[i];
        auto put = [&](int32_t i, const char *n, float mn, float mx, float df, acidulous::Curve c, int32_t steps) {
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
        defs[CoupBeats] = ParamDef{"coupbeats", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""}; // every beat, 2 and 4
        defs[Sidechain] = ParamDef{"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""};
        defs[Follow] = ParamDef{"follow", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        defs[Glide] = ParamDef{"glide", 0.0f, 2.0f, 0.5f, Curve::Linear, 0, "s"};
        defs[Wind] = ParamDef{"wind", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""};
        defs[Sag] = ParamDef{"sag", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""};
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Chanter::Count];
    DestMap() {
        using P = Chanter;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {P::Drones, P::McDrones}, {P::Bag, P::McBag}, {P::Reed, P::McReed}, {P::Grace, P::McGrace},
            {P::Drift, P::McDrift}, {P::Air, P::McAir}, {P::Wheel, P::McWheel}, {P::Rosin, P::McRosin},
            {P::Dog, P::McDog}, {P::Threshold, P::McThreshold}, {P::VelocityAmount, P::McVelocity}, {P::Volume, P::McVolume},
            {P::Follow, P::McFollow}, {P::Wind, P::McWind}, {P::Sag, P::McSag},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Chanter::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : (goal ? macGoal_ : mac_)[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Chanter::updateMacroGoals() {
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

void Chanter::updateMacros() {
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

float Chanter::notePitch(int note) const {
    // The pitch macro counts in octaves here, so a route of half matches the octave knob at +1.
    return static_cast<float>(note) + 12.0f * (static_cast<float>(steppedTargetOf(Octave)) + 2.0f * macGoal_[McPitch]) +
           targetOf(Tune) / 100.0f;
}

void Chanter::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    chanter.prepare(sampleRate);
    second.prepare(sampleRate);
    chanter.setLengthGlide(kFingerGlide);
    second.setLengthGlide(kFingerGlide);
    chanter.setTongueGlide(kFingerGlide);
    second.setTongueGlide(kFingerGlide);
    for (timber::Pipe &d : drones) d.prepare(sampleRate);
    for (Bowed &b : melody) {
        b.wave.prepare(sampleRate);
        b.wave.setLengthGlide(kFingerGlide);
    }
    for (Bowed &b : gurdyDrones) b.wave.prepare(sampleRate);
    trompette.wave.prepare(sampleRate);
    dogTone.setSampleRate(sampleRate);
    dogTone.set(2200.0f, 0.55f);
    box.setSampleRate(sampleRate);
    box.set(280.0f, 0.4f);
    ear.prepare(sampleRate, kEarDown, kEarLowHz);
    earBuilt = false;
    reset();
}

void Chanter::reset() {
    chanter.clear();
    second.clear();
    secondLifted = false;
    chanterPushNow = secondPushNow = 0.0f;
    for (timber::Pipe &d : drones) d.clear();
    for (Bowed *b : {&melody[0], &melody[1], &gurdyDrones[0], &gurdyDrones[1], &trompette}) {
        b->wave.clear();
        b->dc = 0.0f;
        b->kickLeft = 0;
    }
    heldCount = 0;
    velocity = 0.8f;
    pitch = gracePitch = 60.0f;
    graceLeft = shutLeft = 0;
    sounding = lifted = false;
    kickMelody = kickDrones = false;
    bag = 0.0f;
    sinceRelease = 1e9f;
    driftNow = driftAim = 0.0f;
    driftLeft = 0;
    bpm = 120.0f;
    beatAt = 0.0;
    emptyFor = 1e9f;
    coupNow = dogNow = 0.0f;
    bend = wheel = channelPressure_ = 0.0f;
    notePress = -1.0f;
    noteBend_ = 0.0f;
    retuneCountdown = 0;
    updateMacros();
    noteLevel = velocityGain(velocity, goalOf(VelocityAmount));
    dogTone.reset();
    dogLast = 0.0f;
    box.reset();
    dcIn = dcOut = 0.0f;
    noise = 0x3b9aca07u;
    quietSamples = 0;
    asleep = true;
    store = 1.0f;
    strikeLeft = cheekAt = 0.0f;
    dronesPlaced = followed = false;
    for (int d = 0; d <= kDrones; ++d) droneNow[d] = droneAim[d] = moveFrom[d] = moveAim[d] = moveT[d] = placedAim[d] = 0.0f;
    for (int d = 0; d < kDrones; ++d) {
        open[d] = 1.0f;
        dronePushNow[d] = 0.0f;
        choked[d] = false;
    }
    ear.reset();
    earBuilt = false;
    earUntil = 0;
    tuningSeenEqual = true;
    tuningGen = 0;
    earGen = 0xffffffffu;
    root = pending = -1;
    pendingFor = 0.0f;
    placedRoot = -2;
    placedKind = placedKey = -1;
    placedTune = 1e9f;
    placedGen = 0xffffffffu;
}

void Chanter::onBlock(int64_t tickStart, int64_t tickEnd, float tempo) {
    bpm = tempo > 1.0f ? tempo : 120.0f;
    blockTicks = std::max<int64_t>(0, tickEnd - tickStart);
}

void Chanter::onBar(int64_t tickInBar, int32_t ticksPerBar, float swing, int64_t swingPair) {
    barTick = tickInBar;
    barTicks = ticksPerBar;
    swingPercent = swing;
    swingPairTicks = swingPair;
}

void Chanter::startNote(int note, float vel, bool legato) {
    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const float keyNote = 57.0f + static_cast<float>(std::clamp(steppedTargetOf(Key), 0, 11) - 9);
    pitch = notePitch(note);
    velocity = vel;
    noteLevel = velocityGain(vel, goalOf(VelocityAmount));
    noteBend_ = 0.0f;
    notePress = -1.0f;
    // There's no tonguing a bagpipe: notes are set apart by a grace note, a
    // quick high G (for A) before each, or a step above for the top notes.
    const float grace = goalOf(Grace);
    if (grace > 0.0f && kind != Gurdy) {
        const float highG = keyNote + 22.0f;
        gracePitch = pitch < highG - 0.5f ? highG : pitch + 2.0f;
        graceLeft = static_cast<int32_t>(grace * 0.001f * sampleRate);
    } else {
        graceLeft = 0;
    }
    // A closed chanter stops between notes.
    if (legato && kMakes[kind].closed) shutLeft = static_cast<int32_t>(kShut * sampleRate);
    if (!legato) lifted = false;
    // The wheel catches a string at once; a nudge starts it, where the
    // rosin alone would take a moment to.
    if (kind == Gurdy) {
        kickMelody = true;
        if (bag < 0.05f) kickDrones = true;
    }
}

void Chanter::noteOn(uint8_t note, uint8_t vel) {
    const bool legato = heldCount > 0;
    // From a let-go bag the piper blows it up hard before the note speaks.
    if (!legato && sinceRelease >= goalOf(Bag) && store < 0.6f) strikeLeft = kStrike;
    for (int i = 0; i < heldCount; ++i) {
        if (held[i] == note) {
            for (int j = i; j + 1 < heldCount; ++j) {
                held[j] = held[j + 1];
                heldPitch[j] = heldPitch[j + 1];
            }
            --heldCount;
            break;
        }
    }
    if (heldCount >= kHeld) {
        for (int j = 0; j + 1 < heldCount; ++j) {
            held[j] = held[j + 1];
            heldPitch[j] = heldPitch[j + 1];
        }
        --heldCount;
    }
    held[heldCount] = note;
    updateMacroGoals(); // a note reads where the knobs are going, so renders repeat
    heldPitch[heldCount++] = notePitch(note);
    startNote(note, static_cast<float>(vel) / 127.0f, legato);
    asleep = false;
    quietSamples = 0;
}

void Chanter::noteOff(uint8_t note) {
    const int before = heldNote();
    for (int i = 0; i < heldCount; ++i) {
        if (held[i] == note) {
            for (int j = i; j + 1 < heldCount; ++j) {
                held[j] = held[j + 1];
                heldPitch[j] = heldPitch[j + 1];
            }
            --heldCount;
            break;
        }
    }
    if (heldCount == 0) {
        sinceRelease = 0.0f;
        graceLeft = 0;
    } else if (heldNote() != before) {
        // Back to a key still held, without a grace note.
        pitch = heldPitch[heldCount - 1];
        graceLeft = 0;
    }
}

void Chanter::allNotesOff() {
    if (heldCount > 0) sinceRelease = 0.0f;
    heldCount = 0;
    graceLeft = 0;
}

void Chanter::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel squeezes the bag, or pushes the wheel.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Chanter::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Chanter::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Chanter::noteBend(uint8_t note, float semitones) {
    if (heldCount > 0 && heldNote() == note) noteBend_ = semitones;
}

void Chanter::notePressure(uint8_t note, uint8_t value) {
    if (heldCount > 0 && heldNote() == note) notePress = static_cast<float>(value) / 127.0f;
}

float Chanter::bow(Bowed &s, float speed, float grip) {
    // Rosin's friction, as Filament's bow: it grips while the wheel and the
    // string move together and lets go when the string slips. The curve
    // falls away both sides, or the wheel would only add energy.
    // A nudge: half a period's push, the way the wheel first catches the string.
    float nudge = 0.0f;
    if (s.kickLeft > 0) {
        nudge = kKick * std::sin(3.14159265f * static_cast<float>(s.kickLeft) / static_cast<float>(s.kickLength));
        --s.kickLeft;
    }
    if (speed <= 0.0f) return s.wave.step(nudge);
    // The wheel's speed is kept just past where the rosin lets go, where a
    // faster wheel drives the string harder rather than skating over it.
    const float width = 0.05f + 0.25f * (1.0f - grip);
    const float relative = width * speed - s.wave.velocity();
    const float force = relative / (width + relative * relative / width);
    // A little grit from the rosin starts the string going.
    float excite = (force * (0.3f + 0.7f * grip) + white() * 0.02f) * kBowGain;
    // The steady part of the push isn't a wave.
    s.dc += (excite - s.dc) * 0.002f;
    excite -= s.dc;
    return s.wave.step(excite + nudge);
}

int Chanter::nearestKey(float hz) const {
    const float want = std::log2(std::fmax(hz, 1.0f));
    const int guess = std::clamp(static_cast<int>(std::lround(69.0f + 12.0f * (want - std::log2(440.0f)))), 0, 127);
    if (tuningTable() == nullptr) return guess;
    int best = guess;
    float bestOff = 1e9f;
    for (int k = std::max(0, guess - 48); k <= std::min(127, guess + 48); ++k) {
        const float off = std::fabs(std::log2(noteHz(static_cast<float>(k))) - want);
        if (off < bestOff) {
            bestOff = off;
            best = k;
        }
    }
    return best;
}

void Chanter::checkTuning() {
    const float *t = tuningTable();
    if (t == nullptr ? tuningSeenEqual : (!tuningSeenEqual && std::equal(t, t + 128, tuningSeen))) return;
    tuningSeenEqual = t == nullptr;
    if (t != nullptr) std::copy(t, t + 128, tuningSeen);
    ++tuningGen;
}

void Chanter::tuneEar() {
    earGen = tuningGen;
    earBuilt = true;
    // A band on every key of the tuning from 30 to 250 Hz. A tuning with
    // more keys there than bands takes every second or third key, so the
    // bands still reach 250 Hz.
    int first = 0, last = -1;
    while (first < 127 && noteHz(static_cast<float>(first)) < kBassLowHz) ++first;
    for (int key = first; key <= 127 && noteHz(static_cast<float>(key)) <= kBassHighHz; ++key) last = key;
    const int keys = last - first + 1;
    const int stride = keys > kEarBands ? (keys + kEarBands - 1) / kEarBands : 1;
    int n = 0;
    for (int key = first; key <= last && n < kEarBands; key += stride) {
        earKey[n] = key;
        earHz[n] = noteHz(static_cast<float>(key));
        ear.setBand(n, earHz[n]);
        ++n;
    }
    ear.setCount(n);
    root = pending = -1;
    pendingFor = 0.0f;
}

void Chanter::hear(const float *song, int32_t frames, float follow) {
    checkTuning();
    if (!earBuilt || earGen != tuningGen) tuneEar();
    if (ear.count < 2) return;
    const float downRate = sampleRate / static_cast<float>(kEarDown);
    // A quicker follow remembers less: from 0.6 s down to 0.12.
    const float memory = 0.12f + 0.5f * (1.0f - follow);
    const float keep = 1.0f - std::exp(-1.0f / (memory * downRate));
    const int every = std::max(1, static_cast<int>(kEarEvery * downRate));
    for (int32_t i = 0; i < frames; ++i) {
        if (!ear.step(song[i], keep)) continue;
        if (--earUntil <= 0) {
            earUntil = every;
            chooseBass(follow, static_cast<float>(every) / downRate);
        }
    }
    ear.settle();
}

void Chanter::chooseBass(float follow, float seconds) {
    const float *e = ear.heard;
    const int n = ear.count;
    auto peak = [&](int b) { return (b == 0 || e[b] >= e[b - 1]) && (b == n - 1 || e[b] >= e[b + 1]); };
    float loudest = 0.0f;
    int best = -1;
    for (int b = 0; b < n; ++b) {
        if (e[b] > loudest && peak(b)) {
            loudest = e[b];
            best = b;
        }
    }
    // Silence: the drones stay where they are.
    if (best < 0 || loudest < kEarFloor) {
        pending = -1;
        pendingFor = 0.0f;
        return;
    }
    // A loud harmonic over a quieter fundamental: the note is the one an
    // octave or a twelfth down, if it's there.
    for (int b = 0; b < best; ++b) {
        if (e[b] < 0.2f * loudest || !peak(b)) continue;
        const float r = std::log2(earHz[b] / earHz[best]);
        if (std::fabs(r + 1.0f) < 0.04f || std::fabs(r + 1.585f) < 0.04f) {
            best = b;
            break;
        }
    }
    const int key = earKey[best];
    auto sameClass = [&](int a, int b) {
        const float c = std::fmod(std::fabs(1200.0f * std::log2(noteHz(static_cast<float>(a)) / noteHz(static_cast<float>(b)))), 1200.0f);
        return c < 30.0f || c > 1170.0f;
    };
    if (root >= 0 && sameClass(key, root)) {
        pending = -1;
        pendingFor = 0.0f;
        return;
    }
    // A new note has to hold before the drones go to it, so passing notes
    // don't drag them: from 1.25 s with follow low to 50 ms at the top.
    if (pending >= 0 && sameClass(key, pending)) {
        pendingFor += seconds;
    } else {
        pending = key;
        pendingFor = 0.0f;
    }
    const float hold = 0.05f + 1.2f * (1.0f - follow) * (1.0f - follow);
    if (pendingFor >= hold) {
        root = pending;
        pending = -1;
        pendingFor = 0.0f;
    }
}

void Chanter::placeDrones(int kind, float tune) {
    const Make &k = kMakes[kind];
    const int keyStep = std::clamp(steppedTargetOf(Key), 0, 11);
    if (root == placedRoot && kind == placedKind && keyStep == placedKey && tune == placedTune && tuningGen == placedGen) return;
    placedRoot = root;
    placedKind = kind;
    placedKey = keyStep;
    placedTune = tune;
    placedGen = tuningGen;
    // The bass's note in the octave nearest the key's, so the drones keep
    // their register: the key's note was A3 for A.
    const float keyHz = noteHz(static_cast<float>(57 + keyStep - 9));
    const float rootHz = noteHz(static_cast<float>(root));
    const float place = rootHz * std::pow(2.0f, std::round(std::log2(keyHz / rootHz)));
    const bool equal = tuningTable() == nullptr;
    const int rootKey = equal ? root + 12 * static_cast<int>(std::lround(std::log2(place / rootHz))) : nearestKey(place);
    for (int d = 0; d < k.drones; ++d) {
        const int at = equal ? rootKey + static_cast<int>(std::lround(k.droneAt[d]))
                             : nearestKey(noteHz(static_cast<float>(rootKey)) * std::pow(2.0f, k.droneAt[d] / 12.0f));
        placedAim[d] = static_cast<float>(at) + tune;
    }
    placedAim[kDrones] = static_cast<float>(rootKey) + tune;
}

bool Chanter::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    const int kind = std::clamp(steppedTargetOf(Model), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    // The song's low end, heard even while asleep so the drones start on its note.
    const float follow = clampf(goalOf(Follow), 0.0f, 1.0f);
    const bool listening = key_ != nullptr && sidechainRack() >= 0 && follow > 0.0f && k.drones > 0;
    if (listening) hear(key_, frames, follow);
    if (asleep) return true;

    const float keyNote = 57.0f + static_cast<float>(std::clamp(steppedTargetOf(Key), 0, 11) - 9) + targetOf(Tune) / 100.0f;
    const float dt = static_cast<float>(frames) / sampleRate;
    const float volume = paramOf(Volume);
    const float droneLevel = clampf(paramOf(Drones), 0.0f, 1.0f) * k.droneLevel;
    const float press = std::fmax(notePress >= 0.0f ? notePress : channelPressure_, wheel);
    const float air = clampf(paramOf(Air), 0.0f, 1.0f);

    // The bag fills while a key's held and keeps the drones going after;
    // the wheel likewise turns on.
    const bool playing = heldCount > 0;
    if (!playing) sinceRelease += dt;
    const float wind = clampf(paramOf(Wind), 0.0f, 1.0f);
    const float sag = clampf(paramOf(Sag), 0.0f, 1.0f);
    // A finite bag (or the launeddas' breaths); at wind 1 the bag is as it
    // always was. Sag only bends the pitch.
    const bool finite = wind < 1.0f && kind != Launeddas;
    const bool shaped = wind < 1.0f;
    if (!shaped) {
        const float aim = playing || sinceRelease < paramOf(Bag) ? 1.0f : 0.0f;
        bag += (aim - bag) * (1.0f - std::exp(-dt / (aim > bag ? kFills : kEmpties)));
        if (bag < 1e-4f && aim == 0.0f) bag = 0.0f;
    } else {
        // The piper blows while playing and for `bag` seconds after.
        const bool piping = playing || sinceRelease < paramOf(Bag);
        float aim = piping ? 1.0f : 0.0f;
        float fallTime = kEmpties;
        if (finite) {
            // The bag holds so much air. The pipes use it as they sound, the
            // wind puts back its share of what they'd use playing, and once
            // the piper stops blowing the arm squeezes the rest out through
            // the drones. The pressure holds until the bag's soft, then sags.
            // On the hurdy-gurdy it's the wheel: the arm keeps it turning and
            // let go, it coasts down.
            const float ratio = wind / (1.0f - wind);
            const float use = (kDroneUse + (playing ? kChanterUse * (1.0f + 0.5f * press) : 0.0f)) * bag;
            float fill = piping ? ratio * (kDroneUse + kChanterUse) : 0.0f;
            if (strikeLeft > 0.0f && piping) fill += kStrikeFill;
            // Never quite nothing, so a let-go bag does empty.
            const float out = piping ? 0.0f : (kind == Gurdy ? kCoast : kLetGo) * std::fmax(bag, 0.2f);
            store = clampf(store + (fill - use - out) * dt, 0.0f, 1.0f);
            const float x = clampf(store / kSagFrom, 0.0f, 1.0f);
            aim = x * (2.0f - x);
            fallTime = kind == Gurdy ? 0.15f : 0.05f;
        }
        if (!finite && playing) {
            // No bag: circular breathing. A breath in through the nose every
            // 1.5 to 7.5 s, and the cheeks don't quite hold the pressure.
            cheekAt += dt;
            const float every = 1.5f + 6.0f * wind;
            if (cheekAt >= every) cheekAt -= every;
            if (cheekAt < kCheekTime) aim *= 1.0f - kCheekDip * std::sin(3.14159265f * cheekAt / kCheekTime);
        }
        bag += (aim - bag) * (1.0f - std::exp(-dt / (aim > bag ? kFills : fallTime)));
        if (bag < 1e-4f && aim == 0.0f) bag = 0.0f;
    }
    strikeLeft = std::fmax(0.0f, strikeLeft - dt);

    // The drones' home on the key, and where the song's bass sends them.
    float home[kDrones + 1];
    for (int d = 0; d < kDrones; ++d) home[d] = keyNote + k.droneAt[d];
    home[kDrones] = keyNote;
    if (listening && root >= 0) placeDrones(kind, targetOf(Tune) / 100.0f);
    for (int d = 0; d <= kDrones; ++d) droneAim[d] = listening && root >= 0 ? placedAim[d] : home[d];
    if (listening) followed = true;
    const bool gliding = listening || followed;
    const bool dronesQuiet = kind == Gurdy ? bag <= 0.0f : bag <= 0.05f;
    const float glideTime = goalOf(Glide);
    float dip[kDrones] = {};
    bool arrived = true;
    for (int d = 0; d <= kDrones; ++d) {
        if (!gliding || dronesQuiet || !dronesPlaced || glideTime <= 0.0f) {
            droneNow[d] = moveFrom[d] = moveAim[d] = droneAim[d];
            moveT[d] = 0.0f;
        } else if (kind == Gurdy) {
            // A peg turned: an even move over the glide time, easing in and out.
            if (droneAim[d] != moveAim[d]) {
                moveFrom[d] = droneNow[d];
                moveAim[d] = droneAim[d];
                moveT[d] = 0.0f;
            }
            moveT[d] += dt;
            const float f = clampf(moveT[d] / glideTime, 0.0f, 1.0f);
            droneNow[d] = f >= 1.0f ? droneAim[d] : moveFrom[d] + (droneAim[d] - moveFrom[d]) * f * f * (3.0f - 2.0f * f);
        } else {
            // A reed pulled to a new note: quick at first, settling in, and
            // the pipe's push dips while it's on its way.
            droneNow[d] += (droneAim[d] - droneNow[d]) * (1.0f - std::exp(-3.0f * dt / glideTime));
            if (std::fabs(droneAim[d] - droneNow[d]) < 1e-3f) droneNow[d] = droneAim[d];
            if (d < kDrones) dip[d] = 0.2f * std::fmin(1.0f, std::fabs(droneAim[d] - droneNow[d]));
        }
        if (droneNow[d] != droneAim[d]) arrived = false;
    }
    dronesPlaced = true;
    if (!listening && arrived) followed = false;

    // Drift: the bag's pressure wanders as the arm squeezes it.
    driftLeft -= frames;
    if (driftLeft <= 0) {
        driftAim = white();
        driftLeft = static_cast<int32_t>((0.25f + 0.2f * (white() + 1.0f)) * sampleRate);
    }
    driftNow += (driftAim - driftNow) * (1.0f - std::exp(-dt / 0.2f));
    const float drift = clampf(paramOf(Drift), 0.0f, 1.0f) * driftNow;

    const float melodyPitch = pitch + bend * paramOf(BendRange) + noteBend_;
    const float level = noteLevel;
    bool any = bag > 0.0f || playing;

    if (kind != Gurdy) {
        // --- bagpipes -----------------------------------------------------
        const float push = (1.0f + kDriftPush * drift + 0.1f * press) * bag;
        const float cents = drift * kDriftCents;
        bool chanterOn = playing && bag > 0.3f;
        // As the pressure sags the pipes go flat, and each drone chokes as
        // it passes that drone's point, falling further as it dies.
        // Only a finite bag chokes them; sag sets how far the pitch falls.
        float chanterGroan = 0.0f, droneGroan[kDrones] = {};
        bool reopened[kDrones] = {};
        if (finite) {
            const float chokeCoef = 1.0f - std::exp(-dt / kChokeTime);
            for (int d = 0; d < k.drones; ++d) {
                if (!choked[d] && bag < kChoke[d] && sounding) {
                    choked[d] = true;
                } else if (choked[d] && bag > kChoke[d] + kChokeBack) {
                    choked[d] = false;
                    reopened[d] = true;
                }
                open[d] += ((choked[d] ? 0.0f : 1.0f) - open[d]) * chokeCoef;
            }
        } else {
            for (int d = 0; d < kDrones; ++d) {
                choked[d] = false;
                open[d] = 1.0f;
            }
        }
        if (sag > 0.0f) {
            chanterGroan = -sag * kChanterGroan * (1.0f - bag);
            for (int d = 0; d < k.drones; ++d) {
                const float q = clampf((1.0f - bag) / (1.0f - kChoke[d]), 0.0f, 1.0f);
                droneGroan[d] = -sag * kDroneGroan * (q * q + 0.5f * (1.0f - open[d]));
            }
        }
        if (shutLeft > 0) {
            chanter.setTongue(1.0f);
            shutLeft -= frames;
        } else {
            chanter.setTongue(0.0f);
        }
        const float now = graceLeft > 0 ? gracePitch : melodyPitch;
        if (graceLeft > 0) graceLeft -= frames;
        const float reedKnob = paramOf(Reed);
        chanter.setNote(noteHz(now + (cents + kPipeTrim + chanterTrim(kind, now, reedKnob)) * 0.01f + chanterGroan));
        chanter.setShape(k.cylinder, 1);
        chanter.setTube(noteHz(keyNote + 10.0f));
        chanter.setLattice(k.lattice, 0.0f, 0.4f);
        chanter.setBelow(1.0f);
        chanter.setFork(0.2f);
        chanter.setReed(k.cylinder ? 0 : 1, clampf(k.reed + (paramOf(Reed) - 0.5f) * 0.6f, 0.05f, 1.0f), 0.45f);
        chanter.setBell(bellBack(now, keyNote + 10.0f), 0.4f);
        chanter.setLoss(0.999f);
        // The reed is solved for the bag's pressure even between notes: solving it
        // for none steps its table as a note ends, which clicks.
        chanter.setPressure(kChanterPush * push);
        chanter.setDrive(1.0f);
        chanter.tune();
        if (chanterOn && !lifted) {
            chanter.tune();
            chanter.lift();
            lifted = true;
        }
        if (!chanterOn) lifted = false;
        // The other hand: the key held before the last, on the second chanter.
        const bool secondOn = chanterOn && k.second > 0.0f && heldCount >= 2;
        if (k.second > 0.0f) {
            const float other = heldPitch[std::max(heldCount - 2, 0)];
            second.setTongue(0.0f);
            const float otherNow = other + bend * paramOf(BendRange);
            second.setNote(noteHz(otherNow + (cents + kPipeTrim + chanterTrim(kind, otherNow, reedKnob)) * 0.01f + chanterGroan));
            second.setShape(k.cylinder, 1);
            second.setTube(noteHz(keyNote + 10.0f));
            second.setLattice(k.lattice, 0.0f, 0.4f);
            second.setBelow(1.0f);
            second.setFork(0.2f);
            second.setReed(k.cylinder ? 0 : 1, clampf(k.reed + (paramOf(Reed) - 0.5f) * 0.6f, 0.05f, 1.0f), 0.45f);
            second.setBell(bellBack(otherNow, keyNote + 10.0f), 0.4f);
            second.setLoss(0.999f);
            second.setPressure(kChanterPush * push);
            second.setDrive(1.0f);
            second.tune();
            if (secondOn && !secondLifted) {
                second.tune();
                second.lift();
                secondLifted = true;
            }
            if (!secondOn) secondLifted = false;
        }
        const float secondPush = secondOn ? kChanterPush * push : 0.0f;
        const float secondGain = k.level * k.second * level;
        for (int d = 0; d < k.drones; ++d) {
            timber::Pipe &p = drones[d];
            const float hz = noteHz(droneNow[d] + (k.droneCents[d] + cents + kPipeTrim) * 0.01f + droneGroan[d]);
            p.setLengthGlide(gliding ? 0.01f : 0.0f);
            p.setNote(hz);
            p.setShape(true, 1);
            p.setTube(hz);
            p.setLattice(6000.0f, 0.0f, 0.0f);
            p.setBelow(1.0f);
            p.setFork(0.0f);
            p.setReed(0, 0.5f, 0.55f);
            p.setBell(0.9f, 0.6f);
            p.setLoss(0.999f);
            p.setPressure(kDronePush * push);
            p.setDrive(1.0f);
            const bool speaking = bag > 0.05f;
            p.tune();
            if ((speaking && !sounding) || reopened[d]) {
                p.tune();
                p.lift();
            }
        }
        // Each drone's push, followed per sample when it can choke or dip.
        const bool eachPush = shaped || gliding;
        float dronePushAim[kDrones] = {};
        for (int d = 0; d < k.drones; ++d) dronePushAim[d] = kDronePush * push * (finite ? open[d] : 1.0f) * (1.0f - dip[d]);
        const float pushCoef = 1.0f - std::exp(-1.0f / (kDronePushTime * sampleRate));
        sounding = bag > 0.05f;
        const float chanterPush = chanterOn ? kChanterPush * push : 0.0f;
        const float rise = 1.0f - std::exp(-1.0f / (kPushRise * sampleRate));
        const float fall = 1.0f - std::exp(-1.0f / (kPushFall * sampleRate));
        for (int32_t i = 0; i < frames; ++i) {
            chanterPushNow += (chanterPush - chanterPushNow) * (chanterPush > chanterPushNow ? rise : fall);
            secondPushNow += (secondPush - secondPushNow) * (secondPush > secondPushNow ? rise : fall);
            float y = chanter.step(chanterPushNow, white() * air * 0.35f * chanterPushNow) * k.level * level;
            if (k.second > 0.0f) y += second.step(secondPushNow, white() * air * 0.35f * secondPushNow) * secondGain;
            float drone = 0.0f;
            for (int d = 0; d < k.drones; ++d) {
                float dp = kDronePush * push;
                if (eachPush) {
                    dronePushNow[d] += (dronePushAim[d] - dronePushNow[d]) * pushCoef;
                    dp = dronePushNow[d];
                }
                drone += drones[d].step(dp, white() * air * 0.2f * dp);
            }
            y += drone * droneLevel;
            y *= kPipeHouse * volume;
            const float hp = y - dcIn + (1.0f - 6.2831853f * 15.0f / sampleRate) * dcOut;
            dcIn = y;
            dcOut = hp;
            L[i] = R[i] = hp;
        }
        if (!eachPush) {
            for (int d = 0; d < k.drones; ++d) dronePushNow[d] = kDronePush * push;
        }
    } else {
        // --- hurdy-gurdy --------------------------------------------------
        // Coups: the wheel pushed on the beat, so the dog buzzes in time.
        const int perBeat = static_cast<int>(kCoupsPerBeat[std::clamp(steppedTargetOf(Coup), 0, 3)]);
        // On 2 and 4, only the coups in the bar's even beats: 2 and 4 in 4/4,
        // 2 in 3/4.
        const bool evenOnly = steppedTargetOf(CoupBeats) == 1;
        if (songRolling && barTicks > 0) {
            // From the song: a coup where its tick, swung like the track's
            // notes, falls in this block. Swing pairs fit inside a beat, so
            // the beats themselves don't move.
            const int64_t step = kPPQN / std::max(1, perBeat);
            const int64_t from = barTick, to = barTick + blockTicks;
            if (perBeat > 0 && bag > 0.0f && to > from) {
                for (int64_t c = std::max<int64_t>(0, (from - kPPQN) / step * step); c < to + kPPQN; c += step) {
                    const int64_t at = seq::Swing::at(c, swingPercent, swingPairTicks);
                    const bool even = ((c % barTicks) / kPPQN & 1) == 1;
                    if (at >= from && at < to && (!evenOnly || even)) coupNow = kCoupPush;
                }
            }
            beatAt = 0.0;
        } else {
            // Stopped: beats counted from the first note, carried across gaps
            // shorter than a beat so short notes don't each start at beat 1.
            const double before = beatAt;
            beatAt += static_cast<double>(dt * bpm / 60.0f);
            const bool even = (static_cast<int64_t>(std::floor(beatAt)) & 1) == 1;
            if (perBeat > 0 && bag > 0.0f && (!evenOnly || even) &&
                std::floor(beatAt * perBeat) != std::floor(before * perBeat))
                coupNow = kCoupPush;
        }
        emptyFor = !playing && bag <= 0.0f ? emptyFor + dt : 0.0f;
        if (emptyFor > 60.0f / bpm) beatAt = 0.0;
        coupNow *= std::exp(-dt / kCoupFalls);
        const float turning = clampf(paramOf(Wheel), 0.0f, 1.0f) * bag;
        const float speed = turning * (1.0f + coupNow + press);
        const float grip = clampf(paramOf(Rosin), 0.0f, 1.0f);
        // The dog lifts once the wheel's fast enough, as a player's wrist
        // jerks it.
        const float dogAim = clampf((speed - paramOf(Threshold)) * 6.0f, 0.0f, 1.0f);
        dogNow += (dogAim - dogNow) * (1.0f - std::exp(-dt / 0.01f));
        const float dog = clampf(paramOf(Dog), 0.0f, 1.0f);
        // The wheel slows to a stop as the player stops cranking, and once
        // it's below where the rosin lets go, the strings stop speaking.
        const float bowSpeed = (1.2f + 2.0f * clampf(paramOf(Wheel), 0.0f, 1.0f) * (1.0f + coupNow + press)) * bag;
        const float tone = 0.55f + 0.3f * grip;
        const float rosinCents = rosinTrim(grip, clampf(paramOf(Wheel), 0.0f, 1.0f));
        // A wheel slowing down pulls every string flat.
        const float stringGroan = sag > 0.0f ? -sag * kStringGroan * (1.0f - bag) : 0.0f;
        auto tuneString = [&](Bowed &s, float note, bool bowing) {
            s.hz = noteHz(note + drift * kDriftCents * 0.002f);
            s.wave.setFrequency(noteHz(note + (drift * kDriftCents * 0.2f + rosinCents) * 0.01f));
            const float t60 = bowing ? kStringT60 : kStoppedT60;
            s.wave.setDamping(std::pow(10.0f, -3.0f / (t60 * s.hz)), tone);
        };
        tuneString(melody[0], melodyPitch + stringGroan, playing);
        tuneString(melody[1], melodyPitch + 0.03f + stringGroan, playing);
        for (int d = 0; d < k.drones; ++d) {
            gurdyDrones[d].wave.setLengthGlide(gliding ? 0.01f : 0.0f);
            tuneString(gurdyDrones[d], droneNow[d] + stringGroan, bag > 0.0f);
        }
        trompette.wave.setLengthGlide(gliding ? 0.01f : 0.0f);
        tuneString(trompette, droneNow[kDrones] + stringGroan, bag > 0.0f);
        const bool wheelOn = bag > 0.05f;
        auto kick = [&](Bowed &b) {
            b.kickLength = std::max(2, static_cast<int>(0.5f * sampleRate / std::fmax(b.hz, 20.0f)));
            b.kickLeft = b.kickLength;
        };
        if (kickMelody) {
            for (Bowed &m : melody) kick(m);
            kickMelody = false;
        }
        if (kickDrones) {
            for (int d = 0; d < k.drones; ++d) kick(gurdyDrones[d]);
            kick(trompette);
            kickDrones = false;
        }
        for (int32_t i = 0; i < frames; ++i) {
            const float melodySpeed = playing && wheelOn ? bowSpeed + 1e-3f : 0.0f;
            float y = (kMelodyFirst * bow(melody[0], melodySpeed, grip) +
                       kMelodySecond * bow(melody[1], melodySpeed, grip)) * level;
            const float droneSpeed = wheelOn ? bowSpeed + 1e-3f : 0.0f;
            float drone = 0.0f;
            for (int d = 0; d < k.drones; ++d) drone += bow(gurdyDrones[d], droneSpeed, grip);
            const float t = bow(trompette, droneSpeed, grip);
            // The dog: lifted, it strikes the soundboard on every swing of
            // the trompette, and that rings: the buzz.
            float buzz = 0.0f;
            if (dogNow > 0.0f && dog > 0.0f) {
                const float past = t - 0.01f;
                const float c = past > 0.0f ? past * past / (past + 0.01f) : 0.0f;
                buzz = dog * dogNow * kDogMost * std::tanh(kDogGain / kDogMost * dogTone.step(c - dogLast).bp);
                dogLast = c;
            }
            y += (drone + 0.5f * t) * droneLevel + buzz;
            y += 0.6f * box.step(y).bp;
            y *= kStringHouse * volume;
            const float hp = y - dcIn + (1.0f - 6.2831853f * 15.0f / sampleRate) * dcOut;
            dcIn = y;
            dcOut = hp;
            L[i] = R[i] = hp;
        }
        any = any || melody[0].wave.level() > kSilent || trompette.wave.level() > kSilent;
    }

    float peak = 0.0f;
    for (int32_t i = 0; i < frames; ++i) peak = std::fmax(peak, std::fabs(L[i]));
    if (!any && peak < kSilent) {
        quietSamples += frames;
        if (quietSamples > 8192) asleep = true;
    } else {
        quietSamples = 0;
    }
    return false;
}

} // namespace acidulous::machine
