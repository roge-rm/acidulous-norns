#pragma once
#include <cstdint>
#include <engine/machine/draw/FreeReed.h>
#include <engine/machine/draw/PipeReed.h>

// What makes each kind of free reed instrument itself: its reeds, how the
// player's breath or the bellows feed them, and what the sound passes
// through on its way out. Shared by Draw and by tools/draw_tune, which works
// out DrawTuning.h from the reeds here.
namespace acidulous::machine::draw {

enum Kind : int32_t {
    Diatonic = 0, Chromatic, TremoloHarp, OctaveHarp,
    Accordion, Bandoneon, Concertina, Melodica, Harmonium,
    Sheng, Sho, Khaen, PitchPipe,
    kKinds
};

/** The reed makes there are; a kind uses one. DrawTuning.h has a table for each. */
enum Make : int32_t { HarpReed = 0, AccordionReed, MelodicaReed, HarmoniumReed, ConcertinaReed, kMakes };

struct KindVoice {
    int32_t make;
    /** How freely the breath or bellows feed a reed's cell, m^3/s a Pa. */
    float supply;
    /** The pressure a reed is blown at by default, Pa, and the most. */
    float pressure, most;
    /** The body: a low-pass it radiates through (Hz) and a resonance on top: Hz, width Hz, lift. */
    float lowPass, bodyHz, bodyWidth, bodyLift;
    /** Level, so every kind sits at the same loudness. */
    float level;
    /** How unsteady the breath or bellows are: the pressure's slow wander, as a share of it. */
    float wander;
    /** The register a note sounds when the register knob is on auto (kRegisters): an accordion's is two 8' reeds. */
    int32_t stops;
};

/**
 * One reed of a note: its octave against the note (-1 a 16' reed, 0 an 8',
 * +1 a 4'), and which way the detune knob moves it (an 8' tuned apart for
 * musette).
 */
struct Rank {
    int8_t octave, apart;
};

/** A register: the reeds a note sounds, up to five, as an accordion's treble switches them. */
struct Register {
    int32_t count;
    Rank ranks[5];
};

/** Index 0 is auto, the kind's own. Named by footage: 16' sounds an octave down, 4' an octave up. */
inline constexpr int kRegisterCount = 13;
inline constexpr Register kRegisters[kRegisterCount] = {
    {1, {{0, 0}}},                                  // auto (stands in for the kind's own)
    {1, {{0, 0}}},                                  // 8'
    {2, {{0, 0}, {0, 1}}},                          // 8' 8'
    {3, {{0, -1}, {0, 0}, {0, 1}}},                 // 8' 8' 8'
    {1, {{-1, 0}}},                                 // 16'
    {1, {{1, 0}}},                                  // 4'
    {2, {{-1, 0}, {0, 0}}},                         // 16' 8'
    {2, {{0, 0}, {1, 0}}},                          // 8' 4'
    {2, {{-1, 0}, {1, 0}}},                         // 16' 4'
    {3, {{-1, 0}, {0, 0}, {1, 0}}},                 // 16' 8' 4'
    {3, {{0, 0}, {0, 1}, {1, 0}}},                  // 8' 8' 4'
    {4, {{-1, 0}, {0, 0}, {0, 1}, {1, 0}}},         // 16' 8' 8' 4'
    {5, {{-1, 0}, {0, -1}, {0, 0}, {0, 1}, {1, 0}}}, // 16' 8' 8' 8' 4'
};

// A harmonica's reed is Millot & Baumann's (hole 4 of a G harp). Accordion
// reeds are longer and sit in a block; a melodica's are in a closed body
// blown through a tube. A harmonium's sit in cells under a steady wind; a
// concertina's are small. Kinds not modelled yet borrow the nearest.
inline constexpr ReedMake kMakes_[kMakes] = {
    // harmonica
    {12.95e-3f, 2.1e-3f, 2.0000e-03f, 2.0000e-05f, 5.5738e+01f, 2.1120e+00f, 700.0f, 95.0f, 2.3438e+00f, 8.0000e-01f, 1.6000e-02f, 30e-6f, 1.0000e+00f, 7.0400e-01f, 0.022f},
    // accordion: longer, a little stiffer, set for bellows pressure, a bigger cell
    {18.0e-3f, 2.6e-3f, 2.0000e-03f, 5.5000e-05f, 55.0f, 2.4640e+00f, 500.0f, 1.7600e+02f, 3.0000e+00f, 3.1818e-01f, 14e-3f, 40e-6f, 1.0000e+00f, 5.0000e-01f, 0.008f},
    // melodica: harmonica-sized reeds in a body
    {13.5e-3f, 2.2e-3f, 1.9800e-03f, 3.1250e-05f, 50.0f, 1.32f, 650.0f, 90.0f, 1.8750e+00f, 0.45f, 1.9200e-02f, 30e-6f, 9.6000e-01f, 2.5000e-01f, 0.03f},
    // harmonium: long reeds in cells under the wind chest, set for its steady wind
    {18.0e-3f, 2.6e-3f, 2.0000e-03f, 5.5000e-05f, 55.0f, 4.5000e+00f, 6.5000e+02f, 1.7600e+02f, 3.0000e+00f, 3.1818e-01f, 14e-3f, 40e-6f, 1.0000e+00f, 5.0000e-01f, 0.008f},
    // concertina: small reeds in a short chamber
    {1.4000e-02f, 2.2000e-03f, 2.0000e-03f, 5.5000e-05f, 55.0f, 2.4640e+00f, 500.0f, 1.7600e+02f, 3.0000e+00f, 2.5000e-01f, 14e-3f, 40e-6f, 1.0000e+00f, 5.0000e-01f, 0.008f},
};

inline constexpr KindVoice kKindVoices[kKinds] = {// make         supply  pressure most    lowPass   bodyHz    bodyWidth lift  level wander stops
    {HarpReed, 5e-6f, 600.0f, 1100.0f, 16000.0f, 5000.0f, 1800.0f, 1.2000e+00f, 0.85f, 0.12f, 1},  // diatonic
    {HarpReed,      5e-6f,  600.0f, 1100.0f, 16000.0f, 5000.0f, 1800.0f, 0.6f, 0.97f, 0.06f, 1},  // chromatic
    {HarpReed,      5e-6f,  600.0f, 1100.0f, 16000.0f, 5000.0f, 1800.0f, 0.6f, 0.97f, 0.06f, 1},  // tremolo harp
    {HarpReed,      5e-6f,  600.0f, 1100.0f, 16000.0f, 5000.0f, 1800.0f, 0.6f, 0.97f, 0.06f, 1},  // octave harp
    {AccordionReed, 2e-5f, 450.0f, 900.0f, 2.8000e+03f, 2.1120e+03f, 900.0f, 2.5000e-01f, 0.7f, 0.05f, 2},  // accordion
    {AccordionReed, 2e-5f, 450.0f, 900.0f, 7.0000e+02f, 2.1120e+03f, 900.0f, 4.0000e-01f, 0.8f, 0.05f, 6},  // bandoneon
    {ConcertinaReed, 2e-5f, 450.0f, 900.0f, 4.5000e+03f, 2.8000e+03f, 1200.0f, 3.0000e-01f, 0.9f, 0.05f, 1},  // concertina
    {MelodicaReed,  4e-6f,  550.0f, 1000.0f, 2600.0f,  1000.0f, 1200.0f, 0.8f, 0.82f, 0.06f, 1},  // melodica
    {HarmoniumReed, 2e-5f, 900.0f, 1600.0f, 2.0000e+03f, 2.1120e+03f, 900.0f, 4.0000e-01f, 0.33f, 0.05f, 7},  // harmonium
    {HarpReed,      5e-6f,  600.0f, 1100.0f, 12000.0f, 5000.0f, 1800.0f, 0.0f, 0.24f, 0.06f, 1},  // sheng
    {HarpReed,      5e-6f,  600.0f, 1100.0f, 12000.0f, 5000.0f, 1800.0f, 0.0f, 0.19f, 0.06f, 1},  // sho
    {HarpReed,      5e-6f,  600.0f, 1100.0f, 12000.0f, 5000.0f, 1800.0f, 0.0f, 0.32f, 0.06f, 1},  // khaen
    {HarpReed,      5e-6f,  600.0f, 1100.0f, 16000.0f, 5000.0f, 1800.0f, 0.6f, 0.75f, 0.06f, 1},  // pitch pipe
};

/** The pipes of the sheng, shō and khaen, in that order: bore, reflection, corner, reed heard, stray. */
inline constexpr PipeMake kPipeMakes[3] = {
    {8e-3f, 0.99f, 6.0f, 0.05f, 0.4f},  // sheng
    {7e-3f, 0.995f, 5.0f, 0.02f, 0.3f}, // shō: narrow and pure
    {9e-3f, 0.97f, 8.0f, 0.4f, 0.5f},   // khaen: its reeds heard in the wind chest too
};

/**
 * The shō's fifteen pipes, MIDI notes (Pythagorean, from A), and its eleven
 * chords (aitake), each up to six of them, 0 past the last.
 */
inline constexpr int kShoAitake = 11;
inline constexpr int kAitake[kShoAitake][6] = {
    {69, 76, 81, 83, 88, 90}, // kotsu (A)
    {71, 74, 76, 81, 83, 90}, // ichi (B)
    {73, 74, 76, 80, 81, 83}, // ku (C#)
    {74, 76, 81, 83, 88, 90}, // bo (D)
    {76, 81, 83, 86, 88, 90}, // otsu (E)
    {78, 80, 81, 83, 86, 90}, // ge (F#)
    {78, 79, 81, 83, 86, 88}, // ju (G)
    {79, 81, 83, 86, 88, 0},  // ju, sojo
    {80, 81, 83, 84, 86, 90}, // bi (G#)
    {81, 83, 86, 88, 90, 0},  // gyo (A, high)
    {81, 83, 84, 86, 88, 90}, // hi (C)
};
/** The aitake each pitch class plays (C up to B), -1 for none; A is kotsu below the high A, gyo from it. */
inline constexpr int kAitakeFor[12] = {10, 2, 3, -1, 4, -1, 5, 6, 8, 0, -1, 1};
/** Pythagorean tuning from A, cents against equal temperament, C up to B. */
inline constexpr float kPythagorean[12] = {-5.9f, 7.8f, -2.0f, 11.7f, 2.0f, -7.8f, 5.9f, -3.9f, 9.8f, 0.0f, -9.8f, 3.9f};

} // namespace acidulous::machine::draw
