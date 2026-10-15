#pragma once
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/machine/hammer/KeyTables.h>

// One key of an instrument, as the strings and the hammer need it.
//
// The measured part comes from the anchors in KeyTables.h, read between them
// (on a log scale where a quantity is a ratio). What a recording can't show -
// the hammer's mass and felt, where it strikes, the strings' impedance, how
// many strings a key has - comes from the piano acoustics literature as
// smooth curves over the keyboard, and the measured brightness calibrates the
// felt against them (tools/hammer_test).
namespace acidulous::machine::hammer {
using dsp::clampf;

struct KeySpec {
    int key = 60;
    float B = 2.5e-4f, stretchCents = 0.0f;
    /** T60s, seconds, as in Anchor: prompt and after for the fundamental, partials 2-4 and 5-10. */
    float prompt1 = 8.0f, after1 = 30.0f, prompt3 = 5.0f, after3 = 20.0f, prompt7 = 3.0f, after7 = 10.0f;
    float kneeDb = 35.0f, wobbleDb = 3.0f, reachHz = 1000.0f, brightF0 = 1.5f;
    /** Strings to the note. */
    int strings = 3;
    /** How far apart the strings are tuned, in coupling widths (Course::Design::unison). */
    float unison = 1.0f;
    /** How unevenly the hammer meets the strings: what starts the aftersound. */
    float uneven = 0.3f;
    /** How much the strings move across the board, against the polar knob. */
    float across = 1.0f;
    /** How long the high end rings at the least, against the grand's (Course::Design::highRing). */
    float highRing = 1.0f;
    /** A bar's first overtone (tine, reed): its ratio to the note, level against it, and T60, s. */
    float overtone = 7.1f, overtoneLevel = 0.25f, overtoneT60 = 0.3f;
    /** Its second overtone's ratio to the note (0: 2.8 times the first, as a bar fixed at one end has it). */
    float overtone2 = 0.0f;
    /** How long a bar's resonator (a tine's tonebar, a celesta's box) rings against the bar, and how much of the note it holds. */
    float resonator = 2.0f, resonatorShare = 0.5f;
    /** How much less than one B says the partials stretch high up: B / (1 + bend k) at partial k. */
    float bend = 0.0f;
    /** Where the hammer strikes, a fraction of the string from the agraffe. */
    float strike = 0.12f;
    /** The hammer: mass (kg), felt exponent, and contact time at 2 m/s on something rigid (s). */
    float mass = 8.7e-3f, exponent = 2.6f, contact = 2e-3f;
    /** The calibrated factor already in [contact]. */
    float contactAdjust = 1.0f;
    /** How loud the board's knock is under this key, against the others. */
    float knock = 1.0f;
    /** The key's output gain: what evens the keyboard out to the reference's shape. */
    float level = 1.0f;
    /** How much harder the felt is per doubling of the hammer's speed, as a power. */
    float hardening = 0.0f;
    /** The strings' wave impedance, kg/s. */
    float impedance = 2.2f;
    /** Whether it has a damper, and how fast the damper stops it (T60, s). */
    bool damper = true;
    float dampedT60 = 0.3f;
    /** Which part of the bridge it sits on: 0 bass, 1 tenor, 2 treble; and where it is across the stereo. */
    int zone = 1;
    float pan = 0.0f;
};

namespace detail {

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

/** [field] of [table] at [key], read between anchors; on a log scale if [log]. A gap takes its nearest neighbour. */
template <typename Field>
float between(const Anchor *table, int count, float key, Field field, bool log) {
    auto valid = [&](int i) { return field(table[i]) > kGap && (!log || field(table[i]) > 0.0f); };
    int lo = -1, hi = -1;
    for (int i = 0; i < count; ++i) {
        if (!valid(i)) continue;
        if (static_cast<float>(table[i].key) <= key) lo = i;
        if (static_cast<float>(table[i].key) >= key && hi < 0) hi = i;
    }
    if (lo < 0 && hi < 0) return 0.0f;
    if (lo < 0) return field(table[hi]);
    if (hi < 0 || hi == lo) return field(table[lo]);
    const float t = (key - static_cast<float>(table[lo].key)) / static_cast<float>(table[hi].key - table[lo].key);
    const float a = field(table[lo]), b = field(table[hi]);
    return log ? std::exp(lerp(std::log(a), std::log(b), t)) : lerp(a, b, t);
}

/** A factor from an Adjust table at [key], read between on a log scale. */
inline float betweenAdjust(const Adjust *table, int count, float key, float Adjust::*field) {
    if (key <= static_cast<float>(table[0].key)) return table[0].*field;
    for (int i = 1; i < count; ++i) {
        if (key <= static_cast<float>(table[i].key)) {
            const float t = (key - static_cast<float>(table[i - 1].key)) /
                            static_cast<float>(table[i].key - table[i - 1].key);
            return std::exp(lerp(std::log(table[i - 1].*field), std::log(table[i].*field), t));
        }
    }
    return table[count - 1].*field;
}

/** A straight line over the keyboard from [atA0] (key 21) to [atC8] (key 108), on a log scale if [log]. */
inline float across(float key, float atA0, float atC8, bool log) {
    const float t = clampf((key - 21.0f) / 87.0f, 0.0f, 1.0f);
    return log ? std::exp(lerp(std::log(atA0), std::log(atC8), t)) : lerp(atA0, atC8, t);
}

} // namespace detail

/** The grand's key [key] (fractional keys read between). */
inline KeySpec grandKey(float key) {
    using detail::across;
    using detail::between;
    using detail::betweenAdjust;
    constexpr int n = static_cast<int>(sizeof(kGrandA) / sizeof(kGrandA[0]));
    KeySpec s;
    s.key = static_cast<int>(std::lround(key));
    s.B = between(kGrandA, n, key, [](const Anchor &a) { return a.B; }, true);
    s.stretchCents = between(kGrandA, n, key, [](const Anchor &a) { return a.cents; }, false);
    s.prompt1 = between(kGrandA, n, key, [](const Anchor &a) { return a.prompt1; }, true);
    s.after1 = between(kGrandA, n, key, [](const Anchor &a) { return a.after1; }, true);
    s.prompt3 = between(kGrandA, n, key, [](const Anchor &a) { return a.prompt3; }, true);
    s.after3 = between(kGrandA, n, key, [](const Anchor &a) { return a.after3; }, true);
    s.prompt7 = between(kGrandA, n, key, [](const Anchor &a) { return a.prompt7; }, true);
    s.after7 = between(kGrandA, n, key, [](const Anchor &a) { return a.after7; }, true);
    // No partial 5-10 left to measure at the top: the ratios below it.
    if (key > 93.0f) {
        s.prompt7 = s.prompt3 * 0.47f;
        s.after7 = s.after3 * 0.4f;
    }
    s.kneeDb = between(kGrandA, n, key, [](const Anchor &a) { return a.kneeDb; }, false);
    s.wobbleDb = between(kGrandA, n, key, [](const Anchor &a) { return a.wobbleDb; }, false);
    s.reachHz = between(kGrandA, n, key, [](const Anchor &a) { return a.reachHz; }, true);
    s.brightF0 = between(kGrandA, n, key, [](const Anchor &a) { return a.brightF0; }, true);
    // What the model needs on top, to measure like the recordings.
    {
        constexpr int m = static_cast<int>(sizeof(kGrandAAdjust) / sizeof(kGrandAAdjust[0]));
        auto factor = [&](float Adjust::*field) {
            return betweenAdjust(kGrandAAdjust, m, key, field);
        };
        s.B *= factor(&Adjust::B);
        s.prompt1 *= factor(&Adjust::prompt1);
        s.after1 *= factor(&Adjust::after1);
        s.prompt3 *= factor(&Adjust::prompt3);
        s.after3 *= factor(&Adjust::after3);
        s.prompt7 *= factor(&Adjust::prompt7);
        s.after7 *= factor(&Adjust::after7);
        s.unison *= factor(&Adjust::unison);
        s.contactAdjust = factor(&Adjust::contact);
        s.level = factor(&Adjust::level);
        s.knock = factor(&Adjust::knock);
    }
    // Around F#6 the level calibrated against the old strike (which stuck to
    // the strings from there up) came out 3 to 12 dB over the recording's:
    // taken down 5 dB at F#6, nothing by D6 and A#6.
    if (key > 86.0f && key < 94.0f) s.level *= std::pow(10.0f, -5.0f * (1.0f - std::fabs(key - 90.0f) / 4.0f) / 20.0f);
    // With the lighter treble hammers (below) the top played hard came out
    // 4 to 10 dB over the recording from D#6 to G7: 4 dB less from A#6,
    // nothing again by C8, where it matched.
    {
        const float broad = key < 92.0f ? 0.0f : key < 94.0f ? (key - 92.0f) / 2.0f : key < 102.0f ? 1.0f
                          : clampf((108.0f - key) / 6.0f, 0.0f, 1.0f);
        s.level *= std::pow(10.0f, -4.0f * broad / 20.0f);
    }
    // A grand's lowest notes have one string, then two, then three.
    s.strings = key < 28.5f ? 1 : (key < 45.5f ? 2 : 3);
    // From the literature (hammer masses 11 to 6.5 g, felt exponents 2.3 to
    // 3, contact 4 to 0.6 ms at 2 m/s, the strike an eighth of the way along
    // and nearer the end at the top).
    // The lowest wound strings stretch less high up than their B says. In
    // the reference, the partials' spacing from 0.6 to 3 kHz is as
    // B / (1 + k/300) puts it at A0, B / (1 + k/250) at D#1 and
    // B / (1 + k/600) at A1; from C2 one B fits.
    s.bend = key < 27.0f ? detail::lerp(1.0f / 300.0f, 1.0f / 250.0f, clampf((key - 21.0f) / 6.0f, 0.0f, 1.0f))
           : key < 33.0f ? detail::lerp(1.0f / 250.0f, 1.0f / 600.0f, (key - 27.0f) / 6.0f)
           : key < 36.0f ? detail::lerp(1.0f / 600.0f, 0.0f, (key - 33.0f) / 3.0f) : 0.0f;
    // Where the hammer strikes, from the recordings' notches: an eighth of the
    // way along (0.12) up to D#4, then nearer the end, 0.09 by F#5; above
    // that, what the literature says.
    s.strike = key < 63.0f ? 0.12f
             : key < 78.0f ? detail::lerp(0.12f, 0.09f, (key - 63.0f) / 15.0f)
                           : detail::lerp(0.09f, 0.075f, clampf((key - 78.0f) / 30.0f, 0.0f, 1.0f));
    s.mass = across(key, 11.0e-3f, 6.5e-3f, true);
    // From C6 up, the hammer lighter, to a sixth by C7. Against these
    // light strings a 6.5 g hammer stayed on them for 1 ms, two to six
    // periods of the note, and a blow that long has gaps in its spectrum:
    // where each key's note fell among them was luck, and neighbouring keys
    // came out 10 to 25 dB apart, differently at every velocity. Lighter,
    // it stays on 0.4 to 0.6 ms, as a real treble hammer does, and
    // neighbours are within a few dB.
    if (key > 84.0f) s.mass /= 1.0f + 5.0f * clampf((key - 84.0f) / 12.0f, 0.0f, 1.0f);
    s.exponent = across(key, 2.3f, 3.0f, false);
    // Calibrated to how the spectrum's shape changes with velocity in the
    // reference, band by band over the first 300 ms (tools/hammer_reference,
    // bands against the note's total): soft notes keep their upper partials
    // more than a stiffening felt alone would leave them.
    s.hardening = key < 72.0f ? 0.32f : detail::lerp(0.32f, 0.2f, clampf((key - 72.0f) / 36.0f, 0.0f, 1.0f));
    s.contact = across(key, 4.0e-3f, 0.6e-3f, true) * s.contactAdjust;
    // The treble felt no softer than a blow of 1.1 periods of the note on
    // something rigid: with the light hammers there (below), softer felt
    // let the soft notes of the top octave fall 20 to 26 dB.
    s.contact = std::fmin(s.contact, 1.1f / (440.0f * std::exp2((key - 69.0f) / 12.0f)));
    // Wound bass strings are heavy: about 12 kg/s at A0, 2.2 at middle C, 1.5 at the top.
    s.impedance = key < 60.0f ? std::exp(detail::lerp(std::log(12.0f), std::log(2.2f), clampf((key - 21.0f) / 39.0f, 0.0f, 1.0f)))
                              : std::exp(detail::lerp(std::log(2.2f), std::log(1.5f), clampf((key - 60.0f) / 48.0f, 0.0f, 1.0f)));
    // No dampers on the top two octaves or so; slower ones in the bass.
    s.damper = key < 89.0f;
    s.dampedT60 = across(key, 1.2f, 0.08f, true);
    s.zone = key < 48.0f ? 0 : (key < 72.0f ? 1 : 2);
    // As the player hears it: bass on the left.
    s.pan = clampf((key - 64.0f) / 44.0f, -1.0f, 1.0f) * 0.6f;
    return s;
}

/**
 * [s], the grand's key, made into another instrument's from what [table]
 * measured of it against the grand: its own stiffness and stretch, its
 * decays and the reach of its strike as the grand's times the ratio of the
 * two recordings. The grand's calibration (what the model needs on top of a
 * measurement) carries over that way, where the other recordings are too
 * short or too few to calibrate against on their own.
 */
inline KeySpec measuredAgainstGrand(KeySpec s, const Anchor *table, int n, float key) {
    using detail::between;
    constexpr int g = static_cast<int>(sizeof(kGrandA) / sizeof(kGrandA[0]));
    auto ratio = [&](float Anchor::*field) {
        auto get = [field](const Anchor &a) { return a.*field; };
        const float mine = between(table, n, key, get, true), grand = between(kGrandA, g, key, get, true);
        return mine > 0.0f && grand > 0.0f ? mine / grand : 1.0f;
    };
    s.B *= ratio(&Anchor::B);
    // The shape of its tuning, not where the recorded instrument happened to
    // sit against A 440 (the upright was 5 cents flat, the honky-tonk 7 sharp).
    auto cents = [](const Anchor &a) { return a.cents; };
    s.stretchCents = between(table, n, key, cents, false) - between(table, n, 69.0f, cents, false);
    // A ring time the short recordings couldn't give takes the nearest
    // anchor's ratio; held within reason, as a stray fit can be anything.
    auto ring = [&](float Anchor::*field) { return clampf(ratio(field), 0.1f, 3.0f); };
    s.prompt1 *= ring(&Anchor::prompt1);
    s.after1 *= ring(&Anchor::after1);
    s.prompt3 *= ring(&Anchor::prompt3);
    s.after3 *= ring(&Anchor::after3);
    s.prompt7 *= ring(&Anchor::prompt7);
    s.after7 *= ring(&Anchor::after7);
    s.after1 = std::fmax(s.after1, s.prompt1);
    s.after3 = std::fmax(s.after3, s.prompt3);
    s.after7 = std::fmax(s.after7, s.prompt7);
    // A strike that reaches twice as high is about half as long on the strings.
    s.contact /= clampf(ratio(&Anchor::reachHz), 0.4f, 2.5f);
    return s;
}

/** The instruments, in the order of the model parameter. */
enum Model : int {
    Grand = 0, Upright, Honky, Fortepiano, ElectricGrand, Tine, Reed, Tangent, Celesta, Toy, Dulcimer, Cimbalom,
    kModels
};

/** [model]'s key [key], before size, age and the knobs. */
inline KeySpec keyFor(int model, float key) {
    using detail::across;
    using detail::lerp;
    KeySpec s = grandKey(key);
    switch (model) {
    case Upright: {
        constexpr int n = static_cast<int>(sizeof(kUprightA) / sizeof(kUprightA[0]));
        s = measuredAgainstGrand(s, kUprightA, n, key);
        // Shorter strings: one to C#1, two to E2. A lighter action, a
        // board half the size that rings the knock louder (the recording's
        // knock is 3 to 6 dB up on the grand's through the middle).
        s.strings = key < 25.5f ? 1 : (key < 40.5f ? 2 : 3);
        s.impedance *= across(key, 0.75f, 0.9f, true);
        s.mass *= 0.9f;
        s.knock *= across(key, 1.0f, 2.0f, true);
        // Its aftersound is stronger against the prompt sound than a
        // grand's (the recording's knee is 2 to 26 dB, not 5 to 43, but
        // its notes are too short to say how far). The strike reached 1.4
        // times as high as the recording's.
        s.unison *= 1.3f;
        s.across = 1.5f;
        s.contact *= 1.4f;
        // Its notes die sooner, so a phrase is 5 dB under the grand's at the
        // same peak: brought up most of the way.
        s.level *= 1.4f;
        break;
    }
    case Honky: {
        constexpr int n = static_cast<int>(sizeof(kHonkyA) / sizeof(kHonkyA[0]));
        s = measuredAgainstGrand(s, kHonkyA, n, key);
        s.strings = key < 25.5f ? 1 : (key < 40.5f ? 2 : 3);
        s.impedance *= across(key, 0.75f, 0.9f, true);
        s.knock *= 1.4f;
        s.uneven = 0.45f;
        s.across = 1.5f;
        s.highRing = 0.4f;
        // Its upper partials rang 1.3 to 2.7 times too long from C3 up, and
        // partials 2-4 from C6 up, against the recording.
        {
            const float upper = lerp(1.0f, 0.4f, clampf((key - 45.0f) / 45.0f, 0.0f, 1.0f));
            const float middle = lerp(1.0f, 0.5f, clampf((key - 76.0f) / 32.0f, 0.0f, 1.0f));
            s.prompt7 *= upper;
            s.after7 *= upper;
            s.prompt3 *= middle;
            s.after3 *= middle;
        }
        break;
    }
    case Fortepiano: {
        // No open recording to measure: from what's written about the
        // instruments of around 1800. Thin strings at low tension (less
        // stiff, lighter, quicker to die), small leather-covered hammers,
        // two strings in the bass and three above, wooden dampers that
        // don't stop a note dead, and a light case that the action knocks.
        s.B *= across(key, 0.45f, 0.6f, true);
        s.stretchCents *= 0.7f;
        const float shorter = across(key, 0.35f, 0.5f, true);
        s.prompt1 *= shorter;
        s.after1 *= shorter * 0.8f;
        s.prompt3 *= shorter;
        s.after3 *= shorter * 0.8f;
        s.prompt7 *= shorter * 0.9f;
        s.after7 *= shorter * 0.7f;
        s.strings = key < 53.5f ? 2 : 3;
        s.impedance *= 0.45f;
        s.mass *= across(key, 0.5f, 0.45f, true);
        s.exponent = across(key, 2.2f, 2.6f, false);
        s.contact *= 0.6f;
        s.hardening *= 0.5f;
        s.strike = across(key, 0.11f, 0.09f, false);
        s.damper = key < 101.0f;
        s.dampedT60 *= 3.0f;
        s.knock *= 2.5f;
        s.unison *= 1.4f;
        s.highRing = 0.4f;
        break;
    }
    case ElectricGrand: {
        constexpr int n = static_cast<int>(sizeof(kElectricGrandA) / sizeof(kElectricGrandA[0]));
        s = measuredAgainstGrand(s, kElectricGrandA, n, key);
        // Short strings with pickups under the bridge and no board to
        // speak of: little double decay (the recording's knee is 0 to 25
        // dB, the grand's 5 to 43) and hardly any knock. The grand's
        // calibration of its prompt sound doesn't carry over (it rang half
        // as long as the recording): the recording's own times, and an
        // aftersound not far behind them where it rings out too long to say.
        auto own = [&](float Anchor::*field) {
            return detail::between(kElectricGrandA, n, key, [field](const Anchor &a) { return a.*field; }, true);
        };
        s.prompt1 = own(&Anchor::prompt1);
        // The upper partials as designed rang 1.5 to 3.5 times the
        // recording's through the middle: the bridge's share of them is held
        // down (Course::kHighT60), so they ask for less.
        s.prompt3 = own(&Anchor::prompt3) * lerp(1.0f, 0.6f, clampf((key - 33.0f) / 27.0f, 0.0f, 1.0f));
        s.prompt7 = own(&Anchor::prompt7) *
                    (key < 57.0f ? lerp(0.8f, 0.33f, clampf((key - 33.0f) / 24.0f, 0.0f, 1.0f))
                                 : lerp(0.33f, 0.7f, clampf((key - 57.0f) / 31.0f, 0.0f, 1.0f)));
        s.after1 = key < 84.0f ? 1.6f * s.prompt1 : std::fmax(own(&Anchor::after1), s.prompt1);
        s.after3 = 1.6f * s.prompt3;
        s.after7 = 1.6f * s.prompt7;
        s.strings = key < 36.5f ? 1 : (key < 52.5f ? 2 : 3);
        s.impedance *= across(key, 0.5f, 0.85f, true);
        s.knock *= 0.25f;
        s.unison *= 0.6f;
        s.highRing = 0.35f;
        // The strike reached 1.2 to 1.4 times as high as the recording's from C3 up.
        s.contact *= lerp(1.0f, 1.3f, clampf((key - 36.0f) / 24.0f, 0.0f, 1.0f));
        break;
    }
    case Tine: {
        // The bar rings at its note for as long as the recording's
        // fundamental (31 s at A1, 18 at middle C, 2.4 at C7); the tonebar
        // holds most of that, the tine itself half as long. The rest of the
        // recording's partials are the pickup's.
        constexpr int n = static_cast<int>(sizeof(kTineA) / sizeof(kTineA[0]));
        const float ring = detail::between(kTineA, n, key, [](const Anchor &a) { return a.prompt1; }, true) *
                           (key > 96.0f ? std::exp2(-(key - 96.0f) / 12.0f) : 1.0f);
        s.after1 = ring;
        s.prompt1 = 0.5f * ring;
        // A bar's partials are the pickup's, all whole multiples: tuned straight.
        s.stretchCents = 0.0f;
        // The bell: the tine's second mode, about 7 times the note, gone in
        // a few tenths of a second in the bass and much less at the top.
        s.overtone = lerp(6.8f, 7.4f, clampf((key - 28.0f) / 68.0f, 0.0f, 1.0f));
        s.overtoneLevel = 0.25f;
        s.overtoneT60 = across(key, 0.6f, 0.05f, true);
        // A neoprene tip: longer on the bar than felt on a string.
        s.contact = across(key, 2.5e-3f, 0.7e-3f, true);
        s.damper = key < 89.0f;
        s.dampedT60 = across(key, 0.4f, 0.1f, true);
        s.strings = 1;
        s.knock = 0.0f;
        // Each instrument's level: its plain patch as loud as the grand's
        // to the ear (audition's loud column, the piano phrase).
        s.level = 0.56f;
        break;
    }
    case Reed: {
        // A steel reed, no tonebar: its note rings for less (the recording's
        // fits, 9 s at middle C, 2.3 at G6; its notes are cut short, so held
        // to 30 s in the bass).
        constexpr int n = static_cast<int>(sizeof(kReedA) / sizeof(kReedA[0]));
        const float ring = std::fmin(30.0f, detail::between(kReedA, n, key, [](const Anchor &a) { return a.prompt1; }, true));
        s.after1 = s.prompt1 = ring;
        s.stretchCents = 0.0f;
        // A tip weighted with solder to tune it: its second mode lower than a plain bar's.
        s.overtone = 6.3f;
        s.overtoneLevel = 0.15f;
        s.overtoneT60 = across(key, 0.25f, 0.04f, true);
        s.contact = across(key, 1.8e-3f, 0.6e-3f, true);
        s.damper = key < 100.0f;
        s.dampedT60 = across(key, 0.3f, 0.08f, true);
        s.strings = 1;
        s.knock = 0.0f;
        s.level = 0.42f;
        break;
    }
    case Tangent: {
        // A struck-and-held string: one string a key, thin and light, a hard
        // metal tangent near one end that stays on it while the key is down,
        // yarn on the other side that stops it dead when the key comes up.
        // No board: two pickups under the strings. From the literature.
        s.strings = 1;
        s.B *= 0.7f;
        s.stretchCents *= 0.5f;
        s.impedance = 1.0f;
        const float ring = across(key, 8.0f, 1.2f, true);
        s.prompt1 = s.after1 = ring;
        s.prompt3 = s.after3 = 0.6f * ring;
        s.prompt7 = s.after7 = 0.35f * ring;
        s.strike = 0.05f;
        s.mass = 3.0e-3f;
        s.exponent = 2.0f;
        s.contact = 0.15e-3f;
        s.hardening = 0.0f;
        s.damper = true;
        s.dampedT60 = 0.03f;
        s.knock = 0.0f;
        s.unison = 0.0f;
        s.uneven = 0.0f;
        s.level = 0.33f;
        s.highRing = 0.3f;
        s.bend = 0.0f;
        break;
    }
    case Celesta: {
        // Steel bars, free at both ends, struck by felt hammers, each over a
        // wooden box tuned to it that blooms with the note and lets it go
        // soon after. A bar free at both ends has its overtones at 2.76 and
        // 5.40 times the note, and they're gone in a moment. From the
        // literature: about 2.5 s at middle C, 0.6 s at the top.
        const float ring = across(key, 4.0f, 0.5f, true);
        s.prompt1 = ring;
        s.after1 = ring;
        s.resonator = 0.4f;
        s.resonatorShare = 0.5f;
        s.overtone = 2.756f;
        s.overtone2 = 5.404f;
        s.overtoneLevel = 0.12f;
        s.overtoneT60 = 0.25f * ring;
        s.stretchCents = 0.0f;
        s.contact = across(key, 1.5e-3f, 0.6e-3f, true);
        s.damper = true;
        s.dampedT60 = 0.15f;
        s.strings = 1;
        s.knock = 0.3f;
        s.level = 0.8f;
        break;
    }
    case Toy: {
        // Metal rods fixed at one end, struck by small hard hammers, in a
        // plastic box: clangy, short, never quite in tune, no dampers.
        const float ring = across(key, 4.0f, 0.8f, true);
        s.prompt1 = ring;
        s.after1 = ring;
        s.resonatorShare = 0.0f;
        s.overtone = 6.27f;
        s.overtone2 = 17.55f;
        s.overtoneLevel = 0.45f;
        s.overtoneT60 = 0.35f * ring;
        s.stretchCents = 0.0f;
        s.contact = across(key, 0.6e-3f, 0.25e-3f, true);
        s.damper = false;
        s.strings = 1;
        s.knock = 0.75f;
        s.level = 0.33f;
        break;
    }
    case Dulcimer: {
        // Courses of thin plain strings over a small trapezoid box, struck
        // with light wooden hammers, no dampers at all: everything rings
        // into everything. Unisons that never quite agree.
        s.strings = key < 52.0f ? 2 : 3;
        s.B *= 0.6f;
        s.stretchCents *= 0.5f;
        s.bend = 0.0f;
        const float ring = across(key, 1.2f, 1.6f, true);
        s.prompt1 *= ring;
        s.after1 *= ring;
        s.impedance = across(key, 3.0f, 1.2f, true);
        s.mass = 2.0e-3f;
        s.exponent = 2.2f;
        s.contact = across(key, 0.5e-3f, 0.25e-3f, true);
        s.hardening = 0.1f;
        s.strike = 0.12f;
        s.damper = false;
        s.unison *= 2.5f;
        s.uneven = 0.45f;
        s.knock *= 1.5f;
        s.level *= 0.51f;
        s.highRing = 0.7f;
        break;
    }
    case Cimbalom: {
        // A concert dulcimer: heavier strings, wound in the bass, struck
        // with hammers wrapped in cotton, and a damper pedal.
        s.strings = key < 40.0f ? 3 : 4;
        s.B *= 0.8f;
        s.stretchCents *= 0.7f;
        const float ring = across(key, 0.9f, 1.4f, true);
        s.prompt1 *= ring;
        s.after1 *= ring;
        s.impedance *= 0.6f;
        s.mass = 3.0e-3f;
        s.exponent = 2.3f;
        s.contact = across(key, 1.0e-3f, 0.4e-3f, true);
        s.hardening = 0.2f;
        s.damper = true;
        s.dampedT60 *= 2.0f;
        s.unison *= 1.6f;
        s.uneven = 0.4f;
        s.knock *= 1.2f;
        s.level *= 0.79f;
        break;
    }
    default:
        break;
    }
    return s;
}

} // namespace acidulous::machine::hammer
