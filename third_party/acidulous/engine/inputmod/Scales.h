#pragma once
#include <cstdint>

// Scale and chord tables. The 33 scales come from ScaleInKey (MIT), in the
// same order and grouping. Kotlin keeps the same names in the same order for
// the panel (ui/ModifiersPanel.kt).
namespace acidulous::music {

struct ScaleDef {
    const char *name;
    int8_t count;
    int8_t intervals[8];
};

constexpr int kScaleCount = 33;
constexpr ScaleDef kScales[kScaleCount] = {
    {"Ionian (Major)", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"Dorian", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"Phrygian", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"Lydian", 7, {0, 2, 4, 6, 7, 9, 11}},
    {"Mixolydian", 7, {0, 2, 4, 5, 7, 9, 10}},
    {"Aeolian (Minor)", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"Locrian", 7, {0, 1, 3, 5, 6, 8, 10}},
    {"Harmonic Minor", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"Melodic Minor", 7, {0, 2, 3, 5, 7, 9, 11}},
    {"Major Pentatonic", 5, {0, 2, 4, 7, 9}},
    {"Minor Pentatonic", 5, {0, 3, 5, 7, 10}},
    {"Major Blues", 6, {0, 2, 3, 4, 7, 9}},
    {"Minor Blues", 6, {0, 3, 5, 6, 7, 10}},
    {"Egyptian", 5, {0, 2, 5, 7, 10}},
    {"Hungarian Minor", 7, {0, 2, 3, 6, 7, 8, 11}},
    {"Byzantine", 7, {0, 1, 4, 5, 7, 8, 11}},
    {"Persian", 7, {0, 1, 4, 5, 6, 8, 11}},
    {"Hirajoshi", 5, {0, 2, 3, 7, 8}},
    {"In Sen", 5, {0, 1, 5, 7, 10}},
    {"Iwato", 5, {0, 1, 5, 6, 10}},
    {"Enigmatic", 7, {0, 1, 4, 6, 8, 10, 11}},
    {"Phrygian Dominant", 7, {0, 1, 4, 5, 7, 8, 10}},
    {"Neapolitan Minor", 7, {0, 1, 3, 5, 7, 8, 11}},
    {"Neapolitan Major", 7, {0, 1, 3, 5, 7, 9, 11}},
    {"Altered", 7, {0, 1, 3, 4, 6, 8, 10}},
    {"Lydian Dominant", 7, {0, 2, 4, 6, 7, 9, 10}},
    {"Lydian Augmented", 7, {0, 2, 4, 6, 8, 9, 11}},
    {"Locrian nat2", 7, {0, 2, 3, 5, 6, 8, 10}},
    {"Bebop Dominant", 8, {0, 2, 4, 5, 7, 9, 10, 11}},
    {"Bebop Major", 8, {0, 2, 4, 5, 7, 8, 9, 11}},
    {"Whole Tone", 6, {0, 2, 4, 6, 8, 10}},
    {"Dim Whole-Half", 8, {0, 2, 3, 5, 6, 8, 9, 11}},
    {"Dim Half-Whole", 8, {0, 1, 3, 4, 6, 7, 9, 10}},
};

constexpr const char *kKeyNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

struct ChordDef {
    const char *name;
    int8_t count;
    int8_t intervals[6];
};

constexpr int kChordCount = 25;
constexpr ChordDef kChords[kChordCount] = {
    {"maj", 3, {0, 4, 7}},        {"min", 3, {0, 3, 7}},        {"dim", 3, {0, 3, 6}},
    {"aug", 3, {0, 4, 8}},        {"sus2", 3, {0, 2, 7}},       {"sus4", 3, {0, 5, 7}},
    {"5", 2, {0, 7}},             {"6", 4, {0, 4, 7, 9}},       {"m6", 4, {0, 3, 7, 9}},
    {"7", 4, {0, 4, 7, 10}},      {"maj7", 4, {0, 4, 7, 11}},   {"m7", 4, {0, 3, 7, 10}},
    {"m7b5", 4, {0, 3, 6, 10}},   {"dim7", 4, {0, 3, 6, 9}},    {"mMaj7", 4, {0, 3, 7, 11}},
    {"7sus4", 4, {0, 5, 7, 10}},  {"add9", 4, {0, 4, 7, 14}},   {"madd9", 4, {0, 3, 7, 14}},
    {"9", 5, {0, 4, 7, 10, 14}},  {"maj9", 5, {0, 4, 7, 11, 14}}, {"m9", 5, {0, 3, 7, 10, 14}},
    {"11", 6, {0, 4, 7, 10, 14, 17}}, {"13", 6, {0, 4, 7, 10, 14, 21}},
    {"oct", 2, {0, 12}},          {"5+oct", 3, {0, 7, 12}},
};

inline int floorMod(int a, int n) { const int m = a % n; return m < 0 ? m + n : m; }
inline int floorDiv(int a, int n) { return (a - floorMod(a, n)) / n; }

// The k-th scale degree above the root as semitones, wrapping octaves (k may be negative).
inline int degreeInterval(const ScaleDef &s, int k) {
    return s.intervals[floorMod(k, s.count)] + 12 * floorDiv(k, s.count);
}

// Index of the scale degree at or below a pitch-class offset from the root.
inline int degreeAtOrBelow(const ScaleDef &s, int pc) {
    int best = 0;
    for (int i = 0; i < s.count; ++i) if (s.intervals[i] <= pc) best = i;
    return best;
}

} // namespace acidulous::music
