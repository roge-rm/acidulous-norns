#include "Palm.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/machine/Voices.h>

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

} // namespace

Palm::Palm() { initParams(); }

const ParamDef *Palm::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
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
    };
    count = Count;
    return defs;
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
        v.quietBlocks = 0;
    }
    bend = wheel = channelPressure_ = 0.0f;
    retuneCountdown = 0;
    noise = 0x68e31da4u;
    clock = 0;
    quietSamples = 0;
    asleep = true;
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
    const float f = noteHz(v.pitch);
    const float where = clampf(s.where + clampf(targetOf(Position), 0.0f, 1.0f) - 0.5f, 0.0f, 1.0f);
    const float damp = 1.0f - 0.9f * clampf(targetOf(Damp), 0.0f, 1.0f);
    const float t60 = clampf(k.t60 * std::pow(4.0f, targetOf(Decay) - 0.5f) * std::pow(kRefHz / f, k.pitchFall), 0.01f, 10.0f) * damp;
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
        m.amp = 1.5f * clampf(targetOf(Body), 0.0f, 1.0f) * reach;
        m.t60 = k.bodyT60 * std::pow(4.0f, targetOf(Decay) - 0.5f) * damp * s.allDamp;
    }
    for (int i = 0; i < v.modeCount; ++i) v.modes[i].r = poleFor(v.modes[i].t60, sampleRate);
    v.crackTone.set(v.stroke == Rim ? k.crackHz * 0.5f : k.crackHz, 0.4f);
    v.rattleTone.set(k.rattleHz > 0.0f ? k.rattleHz : 4000.0f, kind == Frame ? 0.75f : 0.4f);
    v.builtPitch = -1000.0f;
    retune(v);
}

void Palm::retune(Voice &v) {
    const float pitch = v.pitch + bend * paramOf(BendRange) + v.noteBend + v.drop + v.squeezed;
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

void Palm::strike(Voice &v, float velocity) {
    const Make &k = kMakes[v.kind];
    // Over about 3 ms, not at once: a step in a ringing mode is a click. Only
    // when something's ringing, so a fresh note isn't kept waiting.
    bool ringing = false;
    for (int m = 0; m < v.modeCount; ++m) ringing = ringing || std::fabs(v.modes[m].y1) > 1e-6f;
    v.settleLeft = ringing ? std::max(1, static_cast<int32_t>(kSettleSeconds * sampleRate)) : 0;
    v.settle = ringing ? std::pow(kRestrikeKeeps, 1.0f / static_cast<float>(v.settleLeft)) : 1.0f;
    v.stroke = strokeFor(v, velocity);
    v.velocity = velocity;
    build(v);
    const StrokeDef &s = kStrokes[v.stroke];
    // The hand's contact: softer and longer for a palm, and a harder stroke
    // leaves sooner. Never longer than half the head's period.
    const float hand = clampf(targetOf(Hand), 0.0f, 1.0f);
    float seconds = s.contact * std::pow(3.0f, 0.5f - hand) * (1.3f - 0.6f * velocity);
    seconds = std::fmin(seconds, 0.5f / noteHz(v.pitch));
    const int len = std::clamp(static_cast<int>(seconds * sampleRate), 2, static_cast<int>(v.strike.size()));
    const float peak = 3.14159265f / (2.0f * static_cast<float>(len));
    for (int i = 0; i < len; ++i) {
        v.strike[static_cast<size_t>(i)] = peak * std::sin(3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(len));
    }
    v.strikeLength = len;
    v.strikeAt = 0;
    v.crack = s.crack * velocity;
    v.drop = clampf(targetOf(Drop), 0.0f, 1.0f) * k.drop * velocity;
}

void Palm::noteOn(uint8_t note, uint8_t velocity) {
    const bool kit = steppedTargetOf(Kit) != 0;
    const int pad = static_cast<int>(note) - kBaseNote;
    if (kit && (pad < 0 || pad >= kPads)) return;
    const int kind = std::clamp(steppedTargetOf(kit ? padParam(pad, PadModel) : static_cast<int32_t>(Model)), 0, KindCount - 1);
    const Make &k = kMakes[kind];
    Voice *v = voiceFor(note);
    const bool fresh = !v->used || v->note != note || v->kind != kind;
    v->note = note;
    v->kind = kind;
    v->pad = kit ? pad : -1;
    const float played = kit ? static_cast<float>(steppedTargetOf(padParam(pad, PadNote))) : static_cast<float>(note);
    v->pitch = played + 12.0f * static_cast<float>(steppedTargetOf(Octave)) + targetOf(Tune) / 100.0f;
    v->held = true;
    v->noteBend = 0.0f;
    v->pressure = -1.0f;
    v->press = 0.0f;
    v->builtPress = -1.0f;
    const float vel = static_cast<float>(velocity) / 127.0f;
    v->gain = kHouse * k.level * velocityGain(vel, targetOf(VelocityAmount));
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
    strike(*v, vel);
    const float spread = clampf(targetOf(Spread), 0.0f, 1.0f);
    v->pan = spread * clampf((v->pitch - 55.0f) / 24.0f, -1.0f, 1.0f);
    const float roll = targetOf(Roll);
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
    for (Voice &v : voices) v.held = false;
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

bool Palm::render(float *L, float *R, int32_t frames) {
    params_.tick();
    for (int32_t i = 0; i < frames; ++i) L[i] = R[i] = 0.0f;
    if (asleep) return true;

    const float squeeze = paramOf(Squeeze);
    const float rattleAmount = clampf(paramOf(Rattle), 0.0f, 1.0f);
    const float roll = paramOf(Roll);
    const float volume = paramOf(Volume);
    const float crackFall = std::exp(-1.0f / (0.004f * sampleRate));
    const float dropFall = std::exp(-static_cast<float>(kRetuneEvery) / (kDropSettles * sampleRate));
    const float shakeFall = std::exp(-1.0f / (0.02f * sampleRate));
    bool any = false;

    for (Voice &v : voices) {
        if (!v.used) continue;
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
                retune(v);
                if (v.kind == Cuica) v.rubStep = noteHz(v.pitch + bend * paramOf(BendRange) + v.noteBend + v.squeezed) / sampleRate;
            }
            if (v.held && roll > 0.0f && --v.rollLeft <= 0) {
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
        const bool keeps = v.held && roll > 0.0f;
        if (peak < kSilent && v.strikeAt >= v.strikeLength && !keeps) {
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
