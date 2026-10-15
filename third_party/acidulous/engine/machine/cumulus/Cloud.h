#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// Cumulus's clouds: the tables a voice reads and the settings they're built
// from.
//
// Based on Paul Nasca's PADsynth idea: each harmonic goes into the spectrum
// as a band of lines with random phases, and the whole thing is inverse
// transformed into one very long table. It loops seamlessly, doesn't audibly
// repeat, and sounds like many detuned oscillators for the cost of reading
// an array.
namespace acidulous::machine::cumulus {

/** The settings the tables are built from. All used at build time. */
struct CloudSpec {
    int32_t partials = 48;
    float tilt = -9.0f;        // dB per octave of the partial series
    float odd = 0.5f;          // 0 even only, 0.5 all, 1 odd only
    float comb = 0.0f;         // depth of a scallop across the partials
    float combPeriod = 3.0f;   // its period, in partials
    float formant = 0.0f;      // vowel position, A E I O U
    float formantAmount = 0.0f;
    float bandwidth = 40.0f;   // cents, at the fundamental
    float bwScale = 1.0f;      // exponent: how the band grows up the series
    float stretch = 0.0f;      // inharmonicity: partial n sits at n^(1+stretch)
    // The B end of the morph, as offsets from the values above.
    float bTilt = 0.0f, bBandwidth = 0.0f, bStretch = 0.0f, bComb = 0.0f, bFormant = 0.0f, bOdd = 0.0f;
    uint32_t seed = 1;

    bool operator==(const CloudSpec &o) const;
};

/**
 * One playable table for a range of the keyboard at one morph position.
 *
 * Playing a table higher moves everything up, and partials past Nyquist fold
 * back as noise. So there are three key ranges, each built with only the
 * partials that still fit an octave above its base. That's all the
 * anti-aliasing.
 */
struct CloudTable {
    std::vector<float> data; // size + 1 samples; the last repeats the first
    int32_t size = 0;
    float baseHz = 130.81f;
};

struct CloudSet {
    static constexpr int kFrames = 4; // morph positions, 0 = A, last = B
    static constexpr int kZones = 3;

    CloudTable tables[kZones][kFrames];
    CloudSpec spec;
    int32_t partialsUsed[kZones] = {};
    float buildMs = 0.0f;

    /** Which zone plays [note]: C2 and below, up to C5, above. */
    static int zoneFor(int32_t note) { return note < 48 ? 0 : (note < 72 ? 1 : 2); }
    static float baseHzOf(int zone) { return zone == 0 ? 65.406f : (zone == 1 ? 261.626f : 1046.502f); }
};

/**
 * Builds a set. It's slow (tens of milliseconds) and allocates, so call it
 * on a worker, never the audio thread. The result is handed over as an
 * object, like a sample map.
 */
std::unique_ptr<CloudSet> buildCloud(const CloudSpec &spec, int32_t sampleRate);

} // namespace acidulous::machine::cumulus
