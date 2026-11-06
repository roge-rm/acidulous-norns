#include "Palm.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>
#include <sequencer/Swing.h>

namespace acidulous::machine {

using dsp::clampf;

namespace {

/** Level for a note at velocity 100, set so the bank sits with the other machines. */
constexpr float kHouse = 0.5f;
constexpr float kSilent = 2e-5f;
constexpr float kTwoPi = 6.28318530718f;
constexpr int kRetuneEvery = 16;
/** The pitch the drums' ring times are given at, Hz. */
constexpr float kRefHz = 200.0f;
/** How quickly a head that went sharp settles, seconds. */
constexpr float kDropSettles = 0.08f;
/** How much of a ringing head is left when it's struck again. */
/** How long a re-strike takes to damp what's still ringing, seconds. */
constexpr float kSettleSeconds = 0.003f;
constexpr float kRestrikeKeeps = 0.7f;

/** One mode of a head: its ratio to the note, its shape (m nodal diameters, n nodal circles), its share. */
struct ModeDef {
    float ratio;
    int m, n;
    float amp;
};

/**
 * Each drum: its head's modes, how long it rings at kRefHz and how that
 * changes with pitch and from mode to mode, its body's ring as a ratio of the
 * note (0 for none), the snares or jingles, how far a hard stroke sends it
 * sharp, the crack of a slap, and its level.
 */
struct Make {
    int count;
    ModeDef modes[Palm::kModes];
    float t60, pitchFall, modeFall;
    float body, bodyT60;
    float rattleHz, rattle;
    float drop;
    float crackHz;
    float level;
};

constexpr Make kMakes[Palm::KindCount] = {
    // Tabla: the loaded centre tunes the head to a harmonic series, with the
    // round, damped "tun" mode below it.
    {8, {{0.6f, 0, 1, 0.5f}, {1.0f, 1, 1, 1.0f}, {2.0f, 2, 1, 0.7f}, {2.0f, 0, 2, 0.5f}, {3.0f, 3, 1, 0.5f},
         {3.0f, 1, 2, 0.4f}, {4.0f, 4, 1, 0.3f}, {5.0f, 5, 1, 0.2f}},
     1.4f, 0.3f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.3f, 2500.0f, 1.0f},
    // Bayan: the big bass drum of the pair, loaded off centre.
    {6, {{1.0f, 0, 1, 1.0f}, {1.9f, 1, 1, 0.6f}, {2.8f, 2, 1, 0.4f}, {3.0f, 0, 2, 0.3f}, {3.7f, 3, 1, 0.25f},
         {4.6f, 1, 2, 0.15f}},
     0.9f, 0.3f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f, 1500.0f, 1.0f},
    // Djembe: a goatskin head, its goblet's air ringing low under a bass stroke, and the rattles.
    {10, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.7f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.6f},
          {2.918f, 1, 2, 0.4f}, {3.156f, 4, 1, 0.5f}, {3.501f, 2, 2, 0.3f}, {3.6f, 0, 3, 0.2f}, {3.652f, 5, 1, 0.4f}},
     0.35f, 0.3f, 0.3f, 0.33f, 0.3f, 4000.0f, 0.4f, 0.5f, 3500.0f, 1.0f},
    // Cajón: a thin plate on a box, its port ringing low, snares behind the plate.
    {6, {{1.0f, 0, 1, 1.0f}, {1.6f, 1, 1, 0.7f}, {2.4f, 2, 1, 0.5f}, {2.6f, 0, 2, 0.5f}, {3.4f, 3, 1, 0.4f},
         {4.1f, 1, 2, 0.3f}},
     0.2f, 0.2f, 0.6f, 0.5f, 0.2f, 3200.0f, 0.4f, 0.2f, 2000.0f, 1.1f},
    // Frame drum: a wide, thin head with nothing behind it, low and long, with jingles.
    {10, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.6f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.5f},
          {2.918f, 1, 2, 0.35f}, {3.156f, 4, 1, 0.4f}, {3.501f, 2, 2, 0.25f}, {3.6f, 0, 3, 0.2f}, {3.652f, 5, 1, 0.3f}},
     0.6f, 0.4f, 0.5f, 0.0f, 0.0f, 6800.0f, 0.5f, 0.6f, 3000.0f, 0.9f},
    // Talking drum: two heads laced together, squeezed under the arm.
    {7, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.7f}, {2.136f, 2, 1, 0.5f}, {2.296f, 0, 2, 0.4f}, {2.653f, 3, 1, 0.4f},
         {2.918f, 1, 2, 0.3f}, {3.156f, 4, 1, 0.3f}},
     0.5f, 0.3f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 1.0f},
    // Conga: a thick rawhide head on a tall staved barrel, its air ringing
    // under the head; a clear open tone and a sharp slap.
    {8, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.7f}, {2.136f, 2, 1, 0.5f}, {2.296f, 0, 2, 0.45f}, {2.653f, 3, 1, 0.4f},
         {2.918f, 1, 2, 0.3f}, {3.156f, 4, 1, 0.3f}, {3.501f, 2, 2, 0.2f}},
     0.45f, 0.3f, 0.45f, 0.45f, 0.15f, 0.0f, 0.0f, 0.4f, 3000.0f, 1.0f},
    // Bongo: a pair of small, tight heads on short open shells: high, dry
    // and quick.
    {6, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.7f}, {2.136f, 2, 1, 0.5f}, {2.296f, 0, 2, 0.4f}, {2.653f, 3, 1, 0.35f},
         {2.918f, 1, 2, 0.25f}},
     0.25f, 0.35f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.6f, 4500.0f, 1.6f},
    // Darbuka: a thin, tight head on a goblet. The middle gives a deep doum
    // from the goblet's air, the edge a bright, ringing tek.
    {8, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.7f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.6f},
         {2.918f, 1, 2, 0.45f}, {3.156f, 4, 1, 0.5f}, {3.501f, 2, 2, 0.35f}},
     0.4f, 0.3f, 0.25f, 0.3f, 0.25f, 0.0f, 0.0f, 0.3f, 5000.0f, 1.3f},
    // Riq: a small, tight frame drum with five pairs of heavy jingles that
    // ring on after the head.
    {10, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.6f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.5f},
          {2.918f, 1, 2, 0.35f}, {3.156f, 4, 1, 0.4f}, {3.501f, 2, 2, 0.25f}, {3.6f, 0, 3, 0.2f}, {3.652f, 5, 1, 0.3f}},
     0.3f, 0.4f, 0.5f, 0.0f, 0.0f, 7500.0f, 0.9f, 0.4f, 4000.0f, 1.2f},
    // Tar: a wide, thin frame drum with nothing on it, deep and long.
    {10, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.6f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.5f},
          {2.918f, 1, 2, 0.35f}, {3.156f, 4, 1, 0.4f}, {3.501f, 2, 2, 0.25f}, {3.6f, 0, 3, 0.2f}, {3.652f, 5, 1, 0.3f}},
     0.8f, 0.4f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.6f, 2500.0f, 0.9f},
    // Bendir: a frame drum with gut snares across the back of the head, a buzz under every stroke.
    {10, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.6f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.5f},
          {2.918f, 1, 2, 0.35f}, {3.156f, 4, 1, 0.4f}, {3.501f, 2, 2, 0.25f}, {3.6f, 0, 3, 0.2f}, {3.652f, 5, 1, 0.3f}},
     0.6f, 0.4f, 0.5f, 0.0f, 0.0f, 2500.0f, 0.8f, 0.6f, 2500.0f, 0.9f},
    // Kanjira: a small lizard-skin frame drum with one pair of jingles, bent up by the fingers.
    {8, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.6f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.5f},
         {2.918f, 1, 2, 0.35f}, {3.156f, 4, 1, 0.4f}, {3.501f, 2, 2, 0.25f}},
     0.45f, 0.35f, 0.5f, 0.0f, 0.0f, 6500.0f, 0.5f, 1.0f, 3500.0f, 1.2f},
    // Batá: an hourglass with a head at each end, its body ringing between them.
    {8, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.7f}, {2.136f, 2, 1, 0.5f}, {2.296f, 0, 2, 0.4f}, {2.653f, 3, 1, 0.35f},
         {2.918f, 1, 2, 0.3f}, {3.156f, 4, 1, 0.25f}, {3.501f, 2, 2, 0.2f}},
     0.4f, 0.3f, 0.5f, 0.3f, 0.2f, 0.0f, 0.0f, 0.4f, 3000.0f, 1.0f},
    // Mridangam: a loaded head tuned to a harmonic series like the tabla's,
    // but on a long barrel, with a long, clear ring.
    {8, {{0.6f, 0, 1, 0.5f}, {1.0f, 1, 1, 1.0f}, {2.0f, 2, 1, 0.7f}, {2.0f, 0, 2, 0.5f}, {3.0f, 3, 1, 0.5f},
         {3.0f, 1, 2, 0.4f}, {4.0f, 4, 1, 0.3f}, {5.0f, 5, 1, 0.2f}},
     1.8f, 0.3f, 0.2f, 0.2f, 0.3f, 0.0f, 0.0f, 0.3f, 2800.0f, 0.5f},
    // Dholak: two heads on a barrel, the bass side pasted inside so it booms and bends.
    {6, {{1.0f, 0, 1, 1.0f}, {1.9f, 1, 1, 0.6f}, {2.8f, 2, 1, 0.4f}, {3.0f, 0, 2, 0.3f}, {3.7f, 3, 1, 0.25f},
         {4.6f, 1, 2, 0.15f}},
     0.7f, 0.3f, 0.5f, 0.4f, 0.3f, 0.0f, 0.0f, 0.9f, 2500.0f, 1.0f},
    // Ashiko: a tall cone with a goatskin head, like a djembe without its rattles, its air lower.
    {10, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.8f}, {2.136f, 2, 1, 0.6f}, {2.296f, 0, 2, 0.5f}, {2.653f, 3, 1, 0.5f},
          {2.918f, 1, 2, 0.35f}, {3.156f, 4, 1, 0.4f}, {3.501f, 2, 2, 0.25f}, {3.6f, 0, 3, 0.2f}, {3.652f, 5, 1, 0.3f}},
     0.35f, 0.3f, 0.3f, 0.4f, 0.35f, 0.0f, 0.0f, 0.5f, 3200.0f, 1.0f},
    // Udu: a clay pot with a hole in its side. The air inside is the note, a
    // deep bloop that drops as it settles; the clay only knocks.
    {3, {{2.71f, 0, 1, 0.3f}, {4.15f, 1, 1, 0.2f}, {5.9f, 2, 1, 0.15f}},
     0.15f, 0.3f, 0.5f, 1.0f, 0.45f, 0.0f, 0.0f, 1.5f, 1800.0f, 1.0f},
    // Cuíca: a small head with a stick fixed inside, rubbed with a damp
    // cloth while the note is held, so the head squeaks at its own pitch.
    {6, {{1.0f, 0, 1, 1.0f}, {1.594f, 1, 1, 0.6f}, {2.136f, 2, 1, 0.4f}, {2.296f, 0, 2, 0.3f}, {2.653f, 3, 1, 0.25f},
         {2.918f, 1, 2, 0.2f}},
     0.08f, 0.2f, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3000.0f, 0.6f},
};

/**
 * The strokes: where on the head (0 the middle, 1 the edge), how long the
 * hand stays on it, how much it damps the low modes and all of them by
 * staying there, and how much crack.
 */
struct StrokeDef {
    float where;
    float contact;
    float lowDamp, allDamp;
    float crack;
};
constexpr StrokeDef kStrokes[Palm::ByVelocity] = {
    {0.75f, 0.0006f, 1.0f, 1.0f, 0.1f},   // open: the fingers flat near the edge, lifted at once
    {0.85f, 0.00025f, 0.15f, 1.0f, 0.7f}, // slap: the fingertips cracking, the hand left on the middle
    {0.6f, 0.0012f, 1.0f, 0.15f, 0.05f},  // muted: the hand pressed down and left there
    {0.05f, 0.004f, 1.0f, 1.0f, 0.0f},    // bass: the palm in the middle
    {0.97f, 0.0003f, 0.3f, 1.0f, 0.5f},   // rim: the fingers on the edge and the shell
};

/** A flam's grace stroke: its velocity and level against the stroke's. */
constexpr float kGrace = 0.6f;

/** How hard a cuíca's stick drives the head, against a stroke's impulse. */
constexpr float kRub = 0.02f;

float poleFor(float t60, float sampleRate) { return std::pow(10.0f, -3.0f / (std::fmax(t60, 0.003f) * sampleRate)); }

/**
 * How much a stroke at [r] (0 the middle, 1 the edge) sets a mode ringing.
 * Modes with nodal diameters have a node in the middle and come up towards
 * the edge; each nodal circle moves where the head swings most. The edge
 * itself is held, so nothing quite reaches it.
 */
float shapeAt(int m, int n, float r) {
    const float rr = clampf(r, 0.0f, 0.95f);
    const float across = m == 0 ? 1.0f : std::pow(std::sin(1.5707963f * std::fmin(rr / 0.8f, 1.0f)), static_cast<float>(m));
    const float along = std::fabs(std::cos(1.5707963f * rr * static_cast<float>(2 * n - 1) * 0.9f));
    return across * (0.15f + 0.85f * along);
}

/**
 * A cycle's pattern: one bar of [steps] strokes ([perBeat] to a beat), the
 * bar it plays, and two fills for a cycle's last bar.
 *
 *   .  rest              O o  open, loud or soft
 *   S s  slap            M m  muted
 *   B b  bass            R r  rim
 *   L l  open, on the low drum
 *   P    open, then the second hand presses the head up
 *   D    open with a bass stroke on the low drum under it
 *   X    the second hand laid on the head, stopping it
 */
struct CycleDef {
    int steps, perBeat;
    const char *bar, *fills[2];
};
constexpr CycleDef kCycles[Palm::CycleCount] = {
    {16, 4, "................", {"................", "................"}},
    // Heel and toe, a slap on two, open tones on four.
    {16, 4, "b.m.S.m.b.m.O.L.", {"S.S.S.SSO.OLL.L.", "SmSmS.S.O.OOL.L."}},
    // A djembe's accompaniment, the slaps in pairs.
    {16, 4, "B.mSS.OOB.mSS.O.", {"SSS.SSS.SSS.B.B.", "S.SSO.OOS.SSB.BB"}},
    // Doum doum tek doum tek, with soft kas between.
    {16, 4, "B.B.r.R.B.r.R.r.", {"B.B.RrRrB.RrRrRr", "RrRrB.RrRrB.R.R."}},
    // Doum tek tek doum tek.
    {16, 4, "B.R.r.R.B.r.R.r.", {"B.R.RrRrB.RrRrRR", "B.RrR.RrB.RrRRRR"}},
    // Eight beats of open, pressed, rim and muted strokes, a phrase thrice to end.
    {16, 4, "D.P.R.M.R.m.D.R.", {"DRM.DRM.DRM.D...", "RrRrD.RrRrD.D.D."}},
    // Twelve in a bar, in threes.
    {12, 3, "B.mO.mB.mO.S", {"S.SS.SS.SB.B", "SSSOOOSSSB.B"}},
    // Bass on one and three, slaps on two and four.
    {16, 4, "B...S.mmB.B.S.m.", {"SmSmS.SmB.B.SSSS", "B.B.S.SSB.SSS.S."}},
    // Open and pressed, the head stopped between.
    {16, 4, "O..P..O.O.P.X...", {"OPOPOP..OPOPP.P.", "P.P.P.OOP.P.PPPP"}},
};

/** Bars in a cycle, by the bars param's step. */
constexpr int kCycleBars[4] = {1, 2, 4, 8};
/** A soft stroke's level against a loud one's. */
constexpr float kGhost = 0.38f;
/** How far a low tone sits under the note with one drum held, semitones. */
constexpr float kLowTone = -5.0f;
/** How quickly the second hand presses, seconds. */
constexpr float kPressTakes = 0.07f;

/** A number from a bar and step that's the same every time it's asked for. */
uint32_t mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u + (a << 6) + (a >> 2));
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}
float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/**
 * The pattern step played at step [i] of a bar [n] steps long, for a pattern
 * of [steps] with [perBeat] to a beat, or -1 - the step for a filler stroke.
 * A bar of another length keeps the pattern's start and its last half (at
 * most), so the closing strokes stay at the bar's end; a shorter bar loses
 * the middle, a longer one repeats the beat before the end as filler.
 */
int stepFor(int i, int n, int steps, int perBeat) {
    if (n == steps) return i;
    const int tail = std::min(n, steps) / 2 / perBeat * perBeat;
    if (i >= n - tail) return steps - (n - i);
    if (i < steps - tail) return i;
    const int before = steps - tail - perBeat;
    return -1 - (before + (i - (steps - tail)) % perBeat);
}

} // namespace

Palm::Palm() { initParams(); }

const ParamDef *Palm::paramDefs(int32_t &count) const {
    static const ParamDef first[MacroBase] = {
        {"model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""}, // tabla, bayan, djembe, cajon, frame, talking, conga, bongo, darbuka, riq, tar, bendir, kanjira, bata, mridangam, dholak, ashiko, udu, cuica
        {"tune", -100.0f, 100.0f, 0.0f, Curve::Linear, 0, "cents"},
        {"stroke", 0.0f, static_cast<float>(StrokeCount - 1), 0.0f, Curve::Stepped, StrokeCount, ""}, // open, slap, muted, bass, rim, by velocity
        // Moves the stroke towards the middle or the edge.
        {"position", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // Soft fingertips and palms to hard, quick fingers.
        {"hand", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"decay", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // The other hand resting on the head.
        {"damp", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        // How sharp a hard stroke sends the head before it settles.
        {"drop", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        // How far pressure (or the mod wheel) bends the head up, semitones.
        {"squeeze", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"rattle", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"body", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        // Held notes struck again this many times a second; 0 strikes once.
        {"roll", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, "Hz"},
        {"spread", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"voices", 1.0f, static_cast<float>(kVoices), static_cast<float>(kVoices), Curve::Stepped, kVoices, ""},
        {"velocity", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"bend", 0.0f, 12.0f, 2.0f, Curve::Linear, 0, "st"},
        {"octave", -2.0f, 2.0f, 0.0f, Curve::Stepped, 5, ""},
        {"volume", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        // Kit mode, and the default kit: djembe, cajon, tabla, bayan, frame and talking drum.
        {"kit", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"p01_model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""},
        {"p01_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 3.0f, Curve::Stepped, StrokeCount, ""},
        {"p01_note", 24.0f, 96.0f, 43.0f, Curve::Stepped, 73, ""},
        {"p01_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p02_model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""},
        {"p02_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 0.0f, Curve::Stepped, StrokeCount, ""},
        {"p02_note", 24.0f, 96.0f, 52.0f, Curve::Stepped, 73, ""},
        {"p02_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p03_model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""},
        {"p03_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 1.0f, Curve::Stepped, StrokeCount, ""},
        {"p03_note", 24.0f, 96.0f, 57.0f, Curve::Stepped, 73, ""},
        {"p03_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p04_model", 0.0f, static_cast<float>(KindCount - 1), 2.0f, Curve::Stepped, KindCount, ""},
        {"p04_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 2.0f, Curve::Stepped, StrokeCount, ""},
        {"p04_note", 24.0f, 96.0f, 50.0f, Curve::Stepped, 73, ""},
        {"p04_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p05_model", 0.0f, static_cast<float>(KindCount - 1), 3.0f, Curve::Stepped, KindCount, ""},
        {"p05_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 3.0f, Curve::Stepped, StrokeCount, ""},
        {"p05_note", 24.0f, 96.0f, 40.0f, Curve::Stepped, 73, ""},
        {"p05_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p06_model", 0.0f, static_cast<float>(KindCount - 1), 3.0f, Curve::Stepped, KindCount, ""},
        {"p06_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 1.0f, Curve::Stepped, StrokeCount, ""},
        {"p06_note", 24.0f, 96.0f, 52.0f, Curve::Stepped, 73, ""},
        {"p06_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p07_model", 0.0f, static_cast<float>(KindCount - 1), 3.0f, Curve::Stepped, KindCount, ""},
        {"p07_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 4.0f, Curve::Stepped, StrokeCount, ""},
        {"p07_note", 24.0f, 96.0f, 57.0f, Curve::Stepped, 73, ""},
        {"p07_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p08_model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""},
        {"p08_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 0.0f, Curve::Stepped, StrokeCount, ""},
        {"p08_note", 24.0f, 96.0f, 62.0f, Curve::Stepped, 73, ""},
        {"p08_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p09_model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""},
        {"p09_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 4.0f, Curve::Stepped, StrokeCount, ""},
        {"p09_note", 24.0f, 96.0f, 69.0f, Curve::Stepped, 73, ""},
        {"p09_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p10_model", 0.0f, static_cast<float>(KindCount - 1), 0.0f, Curve::Stepped, KindCount, ""},
        {"p10_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 2.0f, Curve::Stepped, StrokeCount, ""},
        {"p10_note", 24.0f, 96.0f, 64.0f, Curve::Stepped, 73, ""},
        {"p10_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p11_model", 0.0f, static_cast<float>(KindCount - 1), 1.0f, Curve::Stepped, KindCount, ""},
        {"p11_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 0.0f, Curve::Stepped, StrokeCount, ""},
        {"p11_note", 24.0f, 96.0f, 43.0f, Curve::Stepped, 73, ""},
        {"p11_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p12_model", 0.0f, static_cast<float>(KindCount - 1), 1.0f, Curve::Stepped, KindCount, ""},
        {"p12_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 1.0f, Curve::Stepped, StrokeCount, ""},
        {"p12_note", 24.0f, 96.0f, 48.0f, Curve::Stepped, 73, ""},
        {"p12_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p13_model", 0.0f, static_cast<float>(KindCount - 1), 4.0f, Curve::Stepped, KindCount, ""},
        {"p13_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 0.0f, Curve::Stepped, StrokeCount, ""},
        {"p13_note", 24.0f, 96.0f, 45.0f, Curve::Stepped, 73, ""},
        {"p13_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p14_model", 0.0f, static_cast<float>(KindCount - 1), 4.0f, Curve::Stepped, KindCount, ""},
        {"p14_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 4.0f, Curve::Stepped, StrokeCount, ""},
        {"p14_note", 24.0f, 96.0f, 52.0f, Curve::Stepped, 73, ""},
        {"p14_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p15_model", 0.0f, static_cast<float>(KindCount - 1), 5.0f, Curve::Stepped, KindCount, ""},
        {"p15_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 0.0f, Curve::Stepped, StrokeCount, ""},
        {"p15_note", 24.0f, 96.0f, 55.0f, Curve::Stepped, 73, ""},
        {"p15_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"p16_model", 0.0f, static_cast<float>(KindCount - 1), 5.0f, Curve::Stepped, KindCount, ""},
        {"p16_stroke", 0.0f, static_cast<float>(StrokeCount - 1), 2.0f, Curve::Stepped, StrokeCount, ""},
        {"p16_note", 24.0f, 96.0f, 57.0f, Curve::Stepped, 73, ""},
        {"p16_level", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"flam", 0.0f, 60.0f, 0.0f, Curve::Linear, 0, "ms"},
    };
    static const ParamDef last[Count - Cycle] = {
        {"cycle", 0.0f, static_cast<float>(CycleCount - 1), 0.0f, Curve::Stepped, CycleCount, ""}, // off, tumbao, kuku, baladi, maqsum, theka, 6/8, groove, talking
        {"bars", 0.0f, 3.0f, 2.0f, Curve::Stepped, 4, ""}, // 1, 2, 4 or 8 bars a cycle
        {"feel", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"press", 0.0f, 12.0f, 3.0f, Curve::Linear, 0, "st"},
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
        for (int32_t i = Cycle; i < Count; ++i) defs[i] = last[i - Cycle];
        built = true;
    }
    count = Count;
    return defs;
}

namespace {
// The macro destination each param answers to, or -1 for none.
struct DestMap {
    int8_t of[Palm::Count];
    DestMap() {
        using P = Palm;
        for (auto &d : of) d = -1;
        const int32_t pairs[][2] = {
            {P::Position, P::McPosition}, {P::Hand, P::McHand}, {P::Decay, P::McDecay}, {P::Damp, P::McDamp},
            {P::Drop, P::McDrop}, {P::Squeeze, P::McSqueeze}, {P::Rattle, P::McRattle}, {P::Body, P::McBody},
            {P::Roll, P::McRoll}, {P::Spread, P::McSpread}, {P::VelocityAmount, P::McVelocity},
            {P::Volume, P::McVolume}, {P::Flam, P::McFlam},
            {P::Feel, P::McFeel}, {P::Press, P::McPress},
        };
        for (const auto &pr : pairs) of[pr[0]] = static_cast<int8_t>(pr[1]);
    }
};
const DestMap &destMap() {
    static const DestMap m;
    return m;
}
} // namespace

float Palm::val(int32_t p, bool goal) const {
    const float v = read(p, goal);
    const int d = destMap().of[p];
    const float off = d < 0 ? 0.0f : (goal ? macGoal_ : mac_)[d];
    if (off == 0.0f) return v;
    const ParamDef &def = params_.def(p);
    return def.map(def.unmap(v) + off);
}

void Palm::updateMacroGoals() {
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

void Palm::updateMacros() {
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

void Palm::prepare(int32_t rate) {
    sampleRate = static_cast<float>(rate);
    for (Voice &v : voices) {
        v.strike.assign(static_cast<size_t>(sampleRate * 0.012f) + 4, 0.0f);
        v.crackTone.setSampleRate(sampleRate);
        v.rattleTone.setSampleRate(sampleRate);
    }
    reset();
}

void Palm::reset() {
    for (Voice &v : voices) {
        v.settleLeft = 0;
        for (Mode &m : v.modes) m.y1 = m.y2 = 0.0f;
        v.used = v.held = false;
        v.strikeLength = v.strikeAt = 0;
        v.drop = 0.0f;
        v.crack = v.crackLast = v.shake = 0.0f;
        v.crackTone.reset();
        v.rattleTone.reset();
        v.noteBend = 0.0f;
        v.pressure = -1.0f;
        v.squeezed = 0.0f;
        v.press = 0.0f;
        v.builtPress = -1.0f;
        v.builtPitch = -1000.0f;
        v.level = 0.0f;
        v.rubPhase = v.rubStep = 0.0f;
        v.flamLeft = 0;
        v.flamVelocity = 0.0f;
        v.macAt = v.macNow = 0.0f;
        v.quietBlocks = 0;
        v.noteVelocity = 0.8f;
        v.hitCount = 0;
        v.shift = v.hand2 = v.hand2Aim = 0.0f;
    }
    for (float &m : mac_) m = 0.0f;
    for (float &m : macGoal_) m = 0.0f;
    bend = wheel = channelPressure_ = 0.0f;
    retuneCountdown = 0;
    noise = 0x68e31da4u;
    clock = 0;
    quietSamples = 0;
    asleep = true;
    bpm = 120.0f;
    songRolling = false;
    songTick = barTick = blockTicks = 0;
    barTicks = 0;
    swingPercent = 50.0f;
    swingPairTicks = 0;
    freeTick = 0.0;
    countLeft = 0.0;
}

void Palm::onBlock(int64_t tickStart, int64_t tickEnd, float tempo) {
    bpm = tempo > 1.0f ? tempo : 120.0f;
    blockTicks = std::max<int64_t>(0, tickEnd - tickStart);
}

void Palm::onTransport(bool rolling, int64_t tickStart) {
    if (songRolling && !rolling) {
        // Stopped with a key down: carry on from where the song was, and drop
        // strokes still waiting to land.
        if (barTicks > 0) freeTick = static_cast<double>(floorDiv(songTick - barTick, barTicks) * barTicks + barTick + blockTicks);
        for (Voice &v : voices) v.hitCount = 0;
    }
    songRolling = rolling;
    songTick = tickStart;
}

void Palm::onBar(int64_t tickInBar, int32_t ticksPerBar, float swing, int64_t swingPair) {
    barTick = tickInBar;
    barTicks = ticksPerBar;
    swingPercent = swing;
    swingPairTicks = swingPair;
}

int Palm::activeVoices() const {
    int n = 0;
    for (const Voice &v : voices) n += v.used ? 1 : 0;
    return n;
}

Palm::Voice *Palm::voiceFor(uint8_t note) {
    const int cap = std::clamp(steppedTargetOf(Voices), 1, kVoices);
    for (int i = 0; i < cap; ++i) {
        if (voices[i].used && voices[i].note == note) return &voices[i];
    }
    for (int i = 0; i < cap; ++i) {
        if (!voices[i].used) return &voices[i];
    }
    Voice *pick = &voices[0];
    for (int i = 0; i < cap; ++i) {
        if (voices[i].age < pick->age) pick = &voices[i];
    }
    return pick;
}

int Palm::strokeFor(const Voice &v, float velocity) const {
    const int which = v.pad >= 0 ? padParam(v.pad, PadStroke) : static_cast<int32_t>(Stroke);
    const int stroke = std::clamp(steppedTargetOf(which), 0, StrokeCount - 1);
    if (stroke != ByVelocity) return stroke;
    // Soft notes are muted, the middle open, and the hardest slapped.
    if (velocity < 0.35f) return Muted;
    if (velocity < 0.8f) return Open;
    return Slap;
}

void Palm::build(Voice &v) {
    const int kind = v.kind;
    const Make &k = kMakes[kind];
    const StrokeDef &s = kStrokes[v.stroke];
    const float f = noteHz(v.pitch + v.shift);
    const float where = clampf(s.where + clampf(goalOf(Position), 0.0f, 1.0f) - 0.5f, 0.0f, 1.0f);
    const float damp = 1.0f - 0.9f * clampf(goalOf(Damp), 0.0f, 1.0f);
    const float t60 = clampf(k.t60 * std::pow(4.0f, goalOf(Decay) - 0.5f) * std::pow(kRefHz / f, k.pitchFall), 0.01f, 10.0f) * damp;
    v.modeCount = 0;
    for (int i = 0; i < k.count; ++i) {
        const ModeDef &d = k.modes[i];
        if (f * d.ratio > sampleRate * 0.45f) continue;
        Mode &m = v.modes[v.modeCount++];
        m.ratio = d.ratio;
        m.amp = d.amp * shapeAt(d.m, d.n, where);
        // A hand left on the head damps what it touches: a slap's hand on
        // the middle takes the low modes, a muted stroke all of them.
        float keep = s.allDamp;
        if (d.ratio < 2.0f) keep *= s.lowDamp;
        // The tabla's lowest mode is the damped one.
        if (kind == Tabla && i == 0) keep *= 0.25f;
        m.t60 = t60 * std::pow(1.0f / d.ratio, k.modeFall) * keep;
    }
    // The body: air in a goblet or a box, rung by a stroke near the middle.
    if (k.body > 0.0f && f * k.body > 20.0f) {
        Mode &m = v.modes[v.modeCount++];
        m.ratio = k.body;
        // The udu's air rings however the pot is struck; anything else's body
        // answers a stroke near the middle.
        const float reach = v.kind == Udu ? 1.0f : (1.0f - where) * (1.0f - where);
        m.amp = 1.5f * clampf(goalOf(Body), 0.0f, 1.0f) * reach;
        m.t60 = k.bodyT60 * std::pow(4.0f, goalOf(Decay) - 0.5f) * damp * s.allDamp;
    }
    for (int i = 0; i < v.modeCount; ++i) v.modes[i].r = poleFor(v.modes[i].t60, sampleRate);
    v.crackTone.set(v.stroke == Rim ? k.crackHz * 0.5f : k.crackHz, 0.4f);
    v.rattleTone.set(k.rattleHz > 0.0f ? k.rattleHz : 4000.0f, kind == Frame ? 0.75f : 0.4f);
    v.builtPitch = -1000.0f;
    retune(v);
}

void Palm::retune(Voice &v) {
    const float pitch = pitchNow(v) + v.drop + v.squeezed + v.hand2;
    if (std::fabs(pitch - v.builtPitch) < 0.002f && v.press == v.builtPress) return;
    v.builtPitch = pitch;
    // A finger sliding onto the head (MPE slide) damps it as it rings.
    const float pressDamp = 1.0f - 0.97f * v.press;
    const bool pressChanged = v.press != v.builtPress;
    v.builtPress = v.press;
    const float f = noteHz(pitch);
    for (int i = 0; i < v.modeCount; ++i) {
        Mode &m = v.modes[i];
        if (pressChanged) m.r = poleFor(m.t60 * pressDamp, sampleRate);
        const float w = kTwoPi * std::fmin(f * m.ratio, sampleRate * 0.49f) / sampleRate;
        m.a1 = 2.0f * m.r * std::cos(w);
        m.a2 = -m.r * m.r;
        m.b0 = m.amp * std::sin(w);
    }
}

void Palm::strike(Voice &v, float velocity, int stroke) {
    const Make &k = kMakes[v.kind];
    // Over about 3 ms, not at once: a step in a ringing mode is a click. Only
    // when something's ringing, so a fresh note isn't kept waiting.
    bool ringing = false;
    for (int m = 0; m < v.modeCount; ++m) ringing = ringing || std::fabs(v.modes[m].y1) > 1e-6f;
    v.settleLeft = ringing ? std::max(1, static_cast<int32_t>(kSettleSeconds * sampleRate)) : 0;
    v.settle = ringing ? std::pow(kRestrikeKeeps, 1.0f / static_cast<float>(v.settleLeft)) : 1.0f;
    v.stroke = stroke >= 0 ? stroke : strokeFor(v, velocity);
    v.velocity = velocity;
    build(v);
    const StrokeDef &s = kStrokes[v.stroke];
    // The hand's contact: softer and longer for a palm, and a harder stroke
    // leaves sooner. Never longer than half the head's period.
    const float hand = clampf(goalOf(Hand), 0.0f, 1.0f);
    float seconds = s.contact * std::pow(3.0f, 0.5f - hand) * (1.3f - 0.6f * velocity);
    seconds = std::fmin(seconds, 0.5f / noteHz(v.pitch + v.shift));
    const int len = std::clamp(static_cast<int>(seconds * sampleRate), 2, static_cast<int>(v.strike.size()));
    const float peak = 3.14159265f / (2.0f * static_cast<float>(len));
    for (int i = 0; i < len; ++i) {
        v.strike[static_cast<size_t>(i)] = peak * std::sin(3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(len));
    }
    v.strikeLength = len;
    v.strikeAt = 0;
    v.crack = s.crack * velocity;
    v.drop = clampf(goalOf(Drop), 0.0f, 1.0f) * k.drop * velocity;
}

void Palm::noteOn(uint8_t note, uint8_t velocity) {
    updateMacroGoals(); // a note reads where the knobs are going, so renders repeat
    const bool kit = steppedTargetOf(Kit) != 0;
    const int pad = static_cast<int>(note) - kBaseNote;
    if (kit && (pad < 0 || pad >= kPads)) return;
    const int kind = std::clamp(steppedTargetOf(kit ? padParam(pad, PadModel) : static_cast<int32_t>(Model)), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    Voice *v = voiceFor(note);
    // A cycle starts its bar with the first held note while the song is stopped.
    const bool cycling = steppedTargetOf(Cycle) != CycleOff;
    if (cycling) {
        // A held voice being taken for another note still counts.
        bool others = false;
        for (const Voice &u : voices) others = others || ((&u != v || u.note != note) && u.used && u.held);
        if (!others) freeTick = 0.0;
    }
    v->hitCount = 0;
    const bool fresh = !v->used || v->note != note || v->kind != kind;
    // A drum still ringing keeps its low tone and press until the cycle's next stroke.
    if (fresh || !cycling) v->shift = v->hand2 = v->hand2Aim = 0.0f;
    v->note = note;
    v->kind = kind;
    v->pad = kit ? pad : -1;
    const float played = kit ? static_cast<float>(steppedTargetOf(padParam(pad, PadNote))) : static_cast<float>(note);
    v->pitch = played + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    // The pitch macro where it's going is in the note; a move after it bends it.
    v->macAt = v->macNow = macGoal_[McPitch];
    if (v->macAt != 0.0f) v->pitch += v->macAt * 24.0f;
    v->held = true;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    v->press = 0.0f;
    v->builtPress = -1.0f;
    const float vel = static_cast<float>(velocity) / 127.0f;
    v->noteVelocity = vel;
    v->gain = kHouse * k.level * velocityGain(vel, goalOf(VelocityAmount));
    // A pad's level is 0.75 for as loud as the drum played on its own.
    if (kit) v->gain *= clampf(targetOf(padParam(pad, PadLevel)), 0.0f, 1.0f) / 0.75f;
    if (fresh) {
        for (Mode &m : v->modes) m.y1 = m.y2 = 0.0f;
        v->crackTone.reset();
        v->rattleTone.reset();
        v->shake = 0.0f;
        v->squeezed = 0.0f;
        v->rubPhase = 0.0f;
    }
    const float flam = goalOf(Flam);
    if (cycling) {
        // The cycle plays the strokes, from its next step. A fresh drum is
        // built now so a cuíca rubbed before then drives its own modes.
        v->flamLeft = 0;
        if (fresh) {
            v->stroke = Open;
            build(*v);
        }
    } else if (flam > 0.0f) {
        // The grace stroke now, softer and quieter; the stroke itself in render.
        strike(*v, kGrace * vel);
        for (int32_t i = 0; i < v->strikeLength; ++i) v->strike[static_cast<size_t>(i)] *= kGrace;
        // The stroke waits for the grace's contact pulse to finish, and rolls
        // keep the stroke's velocity.
        v->velocity = vel;
        v->flamLeft = std::max({1, static_cast<int32_t>(flam * 0.001f * sampleRate), v->strikeLength + v->settleLeft});
        v->flamVelocity = vel;
    } else {
        v->flamLeft = 0;
        strike(*v, vel);
    }
    const float spread = clampf(goalOf(Spread), 0.0f, 1.0f);
    v->pan = spread * clampf((v->pitch - 55.0f) / 24.0f, -1.0f, 1.0f);
    const float roll = goalOf(Roll);
    v->rollLeft = roll > 0.0f ? static_cast<int32_t>(sampleRate / roll) : 0;
    v->used = true;
    v->quietBlocks = 0;
    v->age = ++clock;
    asleep = false;
    quietSamples = 0;
}

void Palm::noteOff(uint8_t note) {
    // A struck head rings on; letting go only stops rolls and the hand's pressure.
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.held = false;
    }
}

void Palm::allNotesOff() {
    for (Voice &v : voices) {
        v.held = false;
        v.hitCount = 0;
    }
}

void Palm::controlChange(uint8_t cc, uint8_t value) {
    // The mod wheel squeezes the head, as pressure does.
    if (cc == 1) wheel = static_cast<float>(value) / 127.0f;
}

void Palm::pitchBend(int16_t value14) { bend = static_cast<float>(value14) / 8192.0f; }

void Palm::channelPressure(uint8_t value) { channelPressure_ = static_cast<float>(value) / 127.0f; }

void Palm::noteBend(uint8_t note, float semitones) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.noteBend = semitones;
    }
}

void Palm::notePressure(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.pressure = static_cast<float>(value) / 127.0f;
    }
}

void Palm::noteTimbre(uint8_t note, uint8_t value) {
    for (Voice &v : voices) {
        if (v.used && v.held && v.note == note) v.press = static_cast<float>(value) / 127.0f;
    }
}

void Palm::cycleStep(int32_t frames) {
    const int kind = std::clamp(steppedTargetOf(Cycle), 0, CycleCount - 1);
    const CycleDef &c = kCycles[kind];
    // Where the block is: the song's bar while it rolls, or else bars of the
    // song's length on the free clock, which a count-in sets so its downbeat
    // is the song's. Stopped, the clock is fractional so strokes land on
    // their own samples.
    int64_t bar = 0, barLen = 0;
    double from = 0.0, to = 0.0;
    if (songRolling && barTicks > 0) {
        barLen = barTicks;
        from = static_cast<double>(barTick);
        to = static_cast<double>(barTick + blockTicks);
        bar = floorDiv(songTick - barTick, barLen);
    } else {
        barLen = barTicks > 0 ? barTicks : 4 * kPPQN;
        if (countLeft > 0.0) freeTick = -countLeft;
        const double adv = static_cast<double>(frames) * static_cast<double>(bpm) * kPPQN / (60.0 * static_cast<double>(sampleRate));
        bar = floorDiv(static_cast<int64_t>(std::floor(freeTick)), barLen);
        from = freeTick - static_cast<double>(bar * barLen);
        to = from + adv;
        freeTick += adv;
    }
    Voice *lo = nullptr, *hi = nullptr;
    int held = 0;
    for (Voice &v : voices) {
        if (!v.used || !v.held) continue;
        ++held;
        if (lo == nullptr || v.pitch < lo->pitch) lo = &v;
        if (hi == nullptr || v.pitch >= hi->pitch) hi = &v;
    }
    if (held == 0 || to <= from || kind == CycleOff) return;

    const int bars = kCycleBars[std::clamp(steppedTargetOf(Bars), 0, 3)];
    const float feel = clampf(goalOf(Feel), 0.0f, 1.0f);
    const int64_t stepTicks = kPPQN / c.perBeat;
    // Sixteenths swing like the track's notes; threes are already uneven.
    const bool swung = c.perBeat == 4;
    // The soft stroke a player adds between: the pattern's own, or a muted tip.
    char ghost = 'm';
    for (int i = 0; i < c.steps; ++i) {
        if (c.bar[i] == 'r' || c.bar[i] == 'm' || c.bar[i] == 'o') {
            ghost = c.bar[i];
            break;
        }
    }
    const double span = to - from;
    for (int64_t b = 0; static_cast<double>(b * barLen) < to; ++b) {
        const int64_t barNo = bar + b;
        const auto barSeed = mix(static_cast<uint32_t>(barNo), 0x5a17u);
        const int inCycle = static_cast<int>(barNo - floorDiv(barNo, bars) * bars);
        const bool fill = bars > 1 && inCycle == bars - 1;
        const int pick = feel > 0.0f && unit(mix(static_cast<uint32_t>(floorDiv(barNo, bars)), 0xf111u)) < 0.5f ? 1 : 0;
        // How this bar breathes: as written, busier, sparer, or with tones and slaps swapped.
        const uint32_t mood = feel > 0.0f && !fill ? mix(barSeed, 77u) & 3u : 0u;
        const float lean = feel * 0.004f * unit(mix(barSeed, 9u));
        for (int64_t st = 0; st * stepTicks < barLen; ++st) {
            const int64_t cIn = st * stepTicks;
            const auto at = static_cast<double>(b * barLen + (swung ? seq::Swing::at(cIn, swingPercent, swingPairTicks) : cIn));
            if (at < from || at >= to) continue;
            const int n = static_cast<int>((barLen + stepTicks - 1) / stepTicks);
            const int at0 = stepFor(static_cast<int>(st), n, c.steps, c.perBeat);
            const bool filler = at0 < 0;
            const int idx = filler ? -1 - at0 : at0;
            char ch = fill ? c.fills[pick][idx] : c.bar[idx];
            // Filler beats are played soft.
            if (filler) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const uint32_t h = mix(barSeed, static_cast<uint32_t>(idx + 1));
            const float u1 = unit(h), u3 = unit(mix(h, 2u)), u4 = unit(mix(h, 3u));
            const bool onBeat = idx % c.perBeat == 0;
            if (!fill && feel > 0.0f) {
                const float add = feel * (mood == 1 ? 0.45f : mood == 2 ? 0.04f : 0.16f) * (onBeat ? 0.3f : 1.0f);
                const float drop = feel * (mood == 2 ? 0.6f : 0.15f);
                if (ch == '.' && u1 < add) ch = ghost;
                else if (std::islower(static_cast<unsigned char>(ch)) && u1 < drop) ch = '.';
                else if (mood == 3 && !onBeat && u1 < 0.4f * feel) ch = ch == 'O' ? 'S' : ch == 'S' ? 'O' : ch;
            }
            if (ch == '.') continue;
            const bool loud = std::isupper(static_cast<unsigned char>(ch)) != 0;
            float vm = loud ? 1.0f : kGhost;
            vm *= idx == 0 ? 1.05f : onBeat ? 1.0f : 0.86f;
            // The cycle builds a little to its fill, and the fill to the next bar.
            if (bars > 1) vm *= 0.92f + 0.08f * static_cast<float>(inCycle) / static_cast<float>(bars - 1);
            if (fill) vm *= 0.85f + 0.3f * static_cast<float>(idx) / static_cast<float>(c.steps);
            vm *= 1.0f + 0.16f * feel * (2.0f * u3 - 1.0f);
            vm = clampf(vm, 0.05f, 1.25f);
            // A hand lands a hair behind, slaps a little more.
            float late = feel * 0.006f * u4 + lean;
            if (ch == 'S' || ch == 's') late += 0.002f * feel;
            const int32_t wait = static_cast<int32_t>((at - from) / span * frames + 0.5) +
                                 static_cast<int32_t>(late * sampleRate);
            auto queue = [&](Voice *v, int stroke, float shift, bool press) {
                if (v == nullptr || v->hitCount >= kHits) return;
                Hit &q = v->hits[v->hitCount++];
                q.wait = wait;
                q.velocity = clampf(v->noteVelocity * vm, 0.03f, 1.0f);
                q.level = vm;
                q.shift = shift;
                q.stroke = static_cast<int8_t>(stroke);
                q.press = press;
            };
            const bool two = held > 1;
            switch (std::toupper(static_cast<unsigned char>(ch))) {
            case 'O': queue(hi, Open, 0.0f, false); break;
            case 'S': queue(hi, Slap, 0.0f, false); break;
            case 'M': queue(hi, Muted, 0.0f, false); break;
            case 'R': queue(hi, Rim, 0.0f, false); break;
            case 'B': queue(lo, Bass, 0.0f, false); break;
            case 'L': queue(lo, Open, two ? 0.0f : kLowTone, false); break;
            case 'P': queue(lo, Open, 0.0f, true); break;
            case 'D':
                queue(hi, Open, 0.0f, false);
                if (two) queue(lo, Bass, 0.0f, false);
                break;
            case 'X':
                for (Voice &v : voices) {
                    if (v.used && v.held) queue(&v, -1, 0.0f, false);
                }
                break;
            default: break;
            }
        }
    }
}

void Palm::land(Voice &v, const Hit &h) {
    if (h.stroke < 0) {
        // The second hand laid flat on the head stops it over about 12 ms.
        const auto len = std::max<int32_t>(1, static_cast<int32_t>(0.012f * sampleRate));
        v.settleLeft = len;
        v.settle = std::pow(0.02f, 1.0f / static_cast<float>(len));
        v.hand2Aim = 0.0f;
        return;
    }
    v.shift = h.shift;
    v.hand2 = 0.0f;
    v.hand2Aim = h.press ? clampf(goalOf(Press), 0.0f, 12.0f) : 0.0f;
    strike(v, h.velocity, h.stroke);
    for (int32_t i = 0; i < v.strikeLength; ++i) v.strike[static_cast<size_t>(i)] *= h.level;
}

bool Palm::render(float *L, float *R, int32_t frames) {
    params_.tick();
    updateMacros();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;
    const bool cycling = steppedTargetOf(Cycle) != CycleOff;
    if (cycling) cycleStep(frames);

    const float squeeze = paramOf(Squeeze);
    const float rattleAmount = clampf(paramOf(Rattle), 0.0f, 1.0f);
    const float roll = paramOf(Roll);
    const float volume = paramOf(Volume);
    const float crackFall = std::exp(-1.0f / (0.004f * sampleRate));
    const float dropFall = std::exp(-static_cast<float>(kRetuneEvery) / (kDropSettles * sampleRate));
    const float shakeFall = std::exp(-1.0f / (0.02f * sampleRate));
    const float pressRate = 1.0f - std::exp(-static_cast<float>(kRetuneEvery) / (kPressTakes * sampleRate));
    bool any = false;

    for (Voice &v : voices) {
        if (!v.used) continue;
        v.macNow = mac_[McPitch];
        const float rattle = rattleAmount * kMakes[v.kind].rattle;
        float peak = 0.0f;
        const float pl = std::sqrt(0.5f * (1.0f - v.pan)), pr = std::sqrt(0.5f * (1.0f + v.pan));
        int countdown = retuneCountdown;
        for (int32_t i = 0; i < frames; ++i) {
            if (countdown-- <= 0) {
                countdown = kRetuneEvery;
                v.drop *= dropFall;
                const float press = v.held ? (v.pressure >= 0.0f ? v.pressure : channelPressure_) : 0.0f;
                const float aim = squeeze * std::fmax(press, wheel);
                v.squeezed += (aim - v.squeezed) * 0.08f;
                // The second hand pressing the head, or letting go.
                if (v.hand2 != v.hand2Aim) {
                    v.hand2 += (v.hand2Aim - v.hand2) * pressRate;
                    if (std::fabs(v.hand2Aim - v.hand2) < 1e-3f) v.hand2 = v.hand2Aim;
                }
                retune(v);
                if (v.kind == Cuica) v.rubStep = noteHz(pitchNow(v) + v.squeezed) / sampleRate;
            }
            for (int k = 0; k < v.hitCount;) {
                if (v.hits[k].wait <= 0) {
                    const Hit h = v.hits[k];
                    v.hits[k] = v.hits[--v.hitCount];
                    land(v, h);
                } else {
                    --v.hits[k].wait;
                    ++k;
                }
            }
            if (v.flamLeft > 0 && --v.flamLeft == 0) {
                strike(v, v.flamVelocity);
                // The flam's stroke takes the place of a roll stroke.
                if (roll > 0.0f) v.rollLeft = static_cast<int32_t>(sampleRate / roll);
            }
            if (v.held && roll > 0.0f && !cycling && v.flamLeft == 0 && --v.rollLeft <= 0) {
                strike(v, clampf(v.velocity * (0.8f + 0.15f * white()), 0.05f, 1.0f));
                v.rollLeft = static_cast<int32_t>(sampleRate / roll * (1.0f + 0.08f * white()));
            }
            float x = 0.0f;
            // A re-struck note waits for what was ringing to be damped, then lands whole.
            if (v.settleLeft == 0 && v.strikeAt < v.strikeLength) x = v.strike[static_cast<size_t>(v.strikeAt++)] * v.gain;
            // The cuíca's stick sticks and slips once a cycle, so the head is
            // driven at its own note for as long as it's held.
            if (v.kind == Cuica && v.held) {
                v.rubPhase += v.rubStep;
                if (v.rubPhase >= 1.0f) v.rubPhase -= 1.0f;
                x += v.gain * kRub * ((v.rubPhase < 0.15f ? 1.0f : -0.18f) + 0.15f * white());
            }
            float y = 0.0f;
            if (v.settleLeft > 0) {
                --v.settleLeft;
                for (int m = 0; m < v.modeCount; ++m) {
                    v.modes[m].y1 *= v.settle;
                    v.modes[m].y2 *= v.settle;
                }
            }
            for (int m = 0; m < v.modeCount; ++m) {
                Mode &md = v.modes[m];
                const float out = md.b0 * x + md.a1 * md.y1 + md.a2 * md.y2;
                md.y2 = md.y1;
                md.y1 = out;
                y += out;
            }
            // The crack of the hand, or the knock on the shell.
            if (v.crack > 1e-5f) {
                const float n = v.crackTone.step(white() * v.crack * v.gain).bp * 0.8f;
                y += n - v.crackLast;
                v.crackLast = n;
                v.crack *= crackFall;
            }
            // Snares and jingles, shaken by the head.
            if (rattle > 0.0f) {
                v.shake = std::fmax(std::fabs(y), v.shake * shakeFall);
                y += rattle * v.rattleTone.step(white() * v.shake).bp * 0.4f;
            }
            y *= volume;
            L[i] += y * pl;
            R[i] += y * pr;
            peak = std::fmax(peak, std::fabs(y));
        }
        v.level = peak;
        const bool keeps = (v.held && (roll > 0.0f || cycling)) || v.hitCount > 0;
        if (peak < kSilent && v.strikeAt >= v.strikeLength && v.flamLeft == 0 && !keeps) {
            if (++v.quietBlocks > 4) v.used = false;
        } else {
            v.quietBlocks = 0;
        }
        any = any || v.used;
    }
    retuneCountdown -= frames;
    while (retuneCountdown < 0) retuneCountdown += kRetuneEvery;

    if (!any) {
        quietSamples += frames;
        if (quietSamples > 8192) asleep = true;
    } else {
        quietSamples = 0;
    }
    return true;
}

} // namespace acidulous::machine
