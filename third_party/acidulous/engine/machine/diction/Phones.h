#pragma once
#include <cstdint>

// The sounds of English that Diction can sing, and what each is to a throat.
//
// The names are the usual dictionary ones, so a lyric's pronunciation can be
// written out by hand in brackets. A few more are accents' versions of a
// vowel: the dictionary is American, and the choice of accent swaps these in
// before the phones reach the engine.
namespace acidulous::machine::diction {

enum class Kind : uint8_t { Vowel, Diphthong, Stop, Affricate, Fricative, Aspirate, Nasal, Liquid, Glide, Flap };

/** Where a consonant is made, which decides where the formants head and what a burst sounds like. */
enum class Place : uint8_t { None, Lips, Teeth, Gum, Palate, Velum };

struct Phone {
    const char *name;
    Kind kind;
    Place place;
    bool voiced;
    /** The first three formants in hertz: a vowel's, a diphthong's start, or where a consonant takes them. */
    float f[3];
    /** A diphthong's end. The same as [f] for everything else. */
    float to[3];
    /** Hiss, as a fraction of a vowel's level: a fricative's, or a stop's burst. */
    float noise;
    /** The hiss's colour: two bands, each centre, width and share, in hertz. */
    float band[2][3];
    /** Seconds, at the middle of the consonants control. A vowel's is how long it lasts when it isn't the one held. */
    float length;
};

/** Phone codes are indexes into this table. The first one is silence, so zero means nothing. */
const Phone *phoneTable(int32_t &count);

/** The code for [name] (any case), or -1. */
int32_t phoneCode(const char *name, int32_t length);

/**
 * Codes for a space-separated list of names, written into [out]. Stress
 * digits after a vowel are ignored and unknown names skipped. Returns how
 * many were written.
 */
int32_t parsePhones(const char *text, uint8_t *out, int32_t capacity);

} // namespace acidulous::machine::diction
