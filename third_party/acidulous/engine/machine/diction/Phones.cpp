#include "Phones.h"

#include <cctype>
#include <cstring>

namespace acidulous::machine::diction {

namespace {

// The colours of hiss. Two bands each: centre, width, share. F's is flat and
// low; TH's, made at the teeth, higher and brighter, like a soft S. Too alike,
// birthday was sung birfday.
#define QUIET {{0, 0, 0}, {0, 0, 0}}
#define LIPS {{1200, 1500, 1}, {3000, 3000, 0.3f}}
#define LIPTEETH {{2000, 3000, 0.7f}, {5000, 5000, 1}}
#define TEETH {{6500, 3000, 1}, {4000, 3000, 0.35f}}
#define GUM_BURST {{4000, 2000, 1}, {6500, 2500, 0.3f}}
#define GUM_HISS {{5800, 2000, 1}, {7800, 2500, 0.3f}}
#define PALATE {{2800, 900, 1}, {4500, 1800, 0.6f}}
#define VELUM {{1900, 700, 1}, {3500, 1200, 0.3f}}

#define V(name, f1, f2, f3) {name, Kind::Vowel, Place::None, true, {f1, f2, f3}, {f1, f2, f3}, 0, QUIET, 0.12f}
#define D(name, f1, f2, f3, t1, t2, t3) {name, Kind::Diphthong, Place::None, true, {f1, f2, f3}, {t1, t2, t3}, 0, QUIET, 0.16f}
#define C(name, kind, place, voiced, f1, f2, f3, noise, band, length) \
    {name, Kind::kind, Place::place, voiced, {f1, f2, f3}, {f1, f2, f3}, noise, band, length}

// Formants are an adult voice's, pitched between a man's and a woman's like
// the voice itself; the formant control moves them from there.
const Phone kPhones[] = {
    {"", Kind::Vowel, Place::None, false, {500, 1500, 2500}, {500, 1500, 2500}, 0, QUIET, 0},

    // The vowels.
    V("IY", 270, 2290, 3010), // beet
    V("IH", 390, 1990, 2550), // bit
    V("EH", 530, 1840, 2480), // bet
    V("AE", 660, 1720, 2410), // bat
    V("AA", 730, 1090, 2440), // father
    V("AO", 570, 840, 2410),  // caught, where it isn't cot's
    V("AH", 640, 1190, 2390), // but
    V("UH", 440, 1020, 2240), // book
    V("UW", 300, 870, 2240),  // boot
    V("ER", 490, 1350, 1690), // bird
    V("AX", 500, 1400, 2450), // the a in about: the vowel an unstressed syllable falls to
    D("EY", 480, 2020, 2600, 330, 2200, 2800), // bait
    D("AY", 710, 1150, 2450, 380, 2050, 2650), // bite
    D("AW", 710, 1250, 2450, 420, 950, 2300),  // bout
    D("OY", 550, 850, 2400, 380, 2000, 2600),  // boy
    D("OW", 500, 910, 2460, 380, 870, 2300),   // boat

    // Accents' versions. Canadian: bit, bet and bat a little lower and further
    // back; cot and caught one rounded vowel; bite and bout starting higher
    // before a voiceless consonant.
    V("IHC", 430, 1880, 2550),
    V("EHC", 600, 1720, 2480),
    V("AEC", 760, 1550, 2420),
    V("OC", 680, 960, 2420),
    V("OR", 540, 860, 2400), // the vowel of sore, before an R in every accent
    D("AYC", 600, 1350, 2450, 380, 2050, 2650),
    D("AWC", 580, 1350, 2420, 420, 950, 2300),
    // The prairies: bout raised a little less, bait and boat glide less, and
    // bag and egg rise towards bait.
    D("AWP", 630, 1250, 2430, 420, 950, 2300),
    D("EYP", 460, 2050, 2600, 400, 2120, 2700),
    D("OWP", 480, 900, 2430, 430, 880, 2350),
    D("EG", 470, 2050, 2650, 390, 2150, 2700),
    // American: bat rises and glides before M and N, as in man.
    D("AEN", 520, 1980, 2600, 640, 1700, 2450),

    // Stops: closed, then a burst. Their formants are where a vowel next to
    // them heads, which is how an ear tells a B from a D from a G.
    C("P", Stop, Lips, false, 200, 900, 2100, 0.1f, LIPS, 0.06f),
    C("B", Stop, Lips, true, 200, 900, 2100, 0.08f, LIPS, 0.05f),
    C("T", Stop, Gum, false, 200, 1700, 2600, 0.14f, GUM_BURST, 0.06f),
    C("D", Stop, Gum, true, 200, 1700, 2600, 0.12f, GUM_BURST, 0.05f),
    C("K", Stop, Velum, false, 200, 1900, 2200, 0.12f, VELUM, 0.065f),
    C("G", Stop, Velum, true, 200, 1900, 2200, 0.1f, VELUM, 0.055f),
    C("DX", Flap, Gum, true, 250, 1700, 2600, 0, QUIET, 0.022f), // the T in butter
    C("CH", Affricate, Palate, false, 250, 1900, 2500, 0.18f, PALATE, 0.11f),
    C("JH", Affricate, Palate, true, 250, 1900, 2500, 0.1f, PALATE, 0.09f),

    C("F", Fricative, Lips, false, 300, 800, 2100, 0.07f, LIPTEETH, 0.09f),
    C("V", Fricative, Lips, true, 300, 800, 2100, 0.04f, LIPTEETH, 0.07f),
    C("TH", Fricative, Teeth, false, 300, 1800, 2700, 0.035f, TEETH, 0.08f),
    C("DH", Fricative, Teeth, true, 300, 1800, 2700, 0.02f, TEETH, 0.05f),
    C("S", Fricative, Gum, false, 300, 1700, 2600, 0.14f, GUM_HISS, 0.08f),
    C("Z", Fricative, Gum, true, 300, 1700, 2600, 0.08f, GUM_HISS, 0.08f),
    C("SH", Fricative, Palate, false, 300, 1900, 2500, 0.16f, PALATE, 0.08f),
    C("ZH", Fricative, Palate, true, 300, 1900, 2500, 0.08f, PALATE, 0.08f),
    C("HH", Aspirate, None, false, 500, 1500, 2500, 0, QUIET, 0.07f),

    C("M", Nasal, Lips, true, 250, 1100, 2200, 0, QUIET, 0.07f),
    C("N", Nasal, Gum, true, 250, 1700, 2600, 0, QUIET, 0.07f),
    C("NG", Nasal, Velum, true, 250, 2000, 2400, 0, QUIET, 0.08f),
    C("L", Liquid, Gum, true, 380, 1250, 2900, 0, QUIET, 0.06f),
    C("R", Liquid, Palate, true, 350, 1060, 1380, 0, QUIET, 0.06f),
    C("W", Glide, Lips, true, 300, 700, 2200, 0, QUIET, 0.05f),
    C("Y", Glide, Palate, true, 260, 2200, 3000, 0, QUIET, 0.05f),
};

#undef V
#undef D
#undef C

constexpr int32_t kCount = static_cast<int32_t>(sizeof(kPhones) / sizeof(kPhones[0]));
static_assert(kCount < 256, "phone codes are a byte");

} // namespace

const Phone *phoneTable(int32_t &count) {
    count = kCount;
    return kPhones;
}

int32_t phoneCode(const char *name, int32_t length) {
    if (length <= 0) return -1;
    for (int32_t i = 1; i < kCount; ++i) {
        const char *p = kPhones[i].name;
        if (static_cast<int32_t>(std::strlen(p)) != length) continue;
        int32_t k = 0;
        while (k < length && std::toupper(static_cast<unsigned char>(name[k])) == p[k]) ++k;
        if (k == length) return i;
    }
    return -1;
}

int32_t parsePhones(const char *text, uint8_t *out, int32_t capacity) {
    int32_t n = 0;
    const char *s = text;
    while (*s != '\0' && n < capacity) {
        while (*s == ' ' || *s == '\t') ++s;
        const char *start = s;
        while (*s != '\0' && *s != ' ' && *s != '\t') ++s;
        auto length = static_cast<int32_t>(s - start);
        // Stress marks: AH0, EY1. Sung, every vowel gets its note.
        while (length > 0 && std::isdigit(static_cast<unsigned char>(start[length - 1]))) --length;
        const int32_t code = phoneCode(start, length);
        if (code > 0) out[n++] = static_cast<uint8_t>(code);
    }
    return n;
}

} // namespace acidulous::machine::diction
