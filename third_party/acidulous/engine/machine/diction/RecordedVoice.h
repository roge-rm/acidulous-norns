#pragma once
#include <cstdint>
#include <engine/core/Utterance.h>
#include <memory>
#include <string>
#include <vector>

// A voice somebody recorded, in the pieces Diction sings from.
//
// Each held vowel is kept as its steady part, with a little either side so a
// grain near the edge has something to read, and its glottal pulses found, so
// it can be sung at any pitch while the throat stays the singer's own. Each
// consonant is kept with the vowel either side of it, as it was sung between
// two: the way into and out of it is the consonant as much as the middle is.
//
// Built on a worker and never changed afterwards. The audio thread only reads
// it, and a new one is swapped in.
namespace acidulous::machine::diction {

struct RecordedVoice {
    /**
     * The singer's throat at each pitch mark of a take, found by linear
     * prediction, and what's left of the take once it's taken out: the
     * folds' own sound, the source. So the built-in voice's folds can sing
     * through the singer's throat, and the singer's through the built-in
     * throat.
     */
    struct Tract {
        // Forty: at 24 the resonances spread over the whole band left only
        // two or three under 5 kHz, and an ee's dip between its first two
        // formants filled in.
        static constexpr int kOrder = 40;
        static_assert(kOrder % 4 == 0, "the source's filter sums four at a time");
        /** The source, as long as the take, with the tilt put back that the analysis took off. */
        std::vector<float> source;
        /** kOrder reflection coefficients a pitch mark, which stay stable however they're blended. */
        std::vector<float> k;
        /** The source's level at each pitch mark. */
        std::vector<float> gain;
        /**
         * The throat at each mark as line spectral frequencies, kOrder a mark
         * in radians: the sum polynomial's roots then the difference's, each
         * rising. Blended between two throats, each formant moves from one's
         * place to the other's, where blended reflection coefficients
         * smeared an ah between two voices across a kilohertz. A mark whose
         * roots couldn't all be found starts with -1. Empty unless the voice
         * was asked for them (see [fromSpec]): they took a quarter of loading
         * a voice, and only a morph uses them.
         */
        std::vector<float> lsf;
        /** Line spectral frequencies [lsf] back to reflection coefficients [k], kOrder of each. */
        static void fromLsf(const float *lsf, float *k);
    };
    /** [u]'s tract, with its line spectral frequencies if [lsf]. Worker thread. */
    static void analyseTract(const audio::Utterance &u, float sampleRate, Tract &t, bool lsf);
    /** Whether its takes' tracts get their line spectral frequencies. */
    bool wantLsf = false;

    /**
     * A voice from the lines the app sends: "V|PHONE|path|holdFrom|holdTo" a
     * held vowel, "D|PHONE|path|holdFrom|holdTo|glideFrom|glideTo" a
     * diphthong, "C|PHONE|VOWEL|path|from|to" a consonant, in frames, and
     * "LSF" on a line of its own for a voice that's morphed from or to. Its
     * takes are read and analysed on up to [threads] threads, since finding
     * a whole voice's pulses on one took most of a second here and about ten
     * on a phone. [error] gets the first take that couldn't be used. Worker
     * thread.
     */
    static std::unique_ptr<RecordedVoice> fromSpec(const std::string &spec, float sampleRate, int threads, std::string &error);

    struct Unit {
        /** The phone it was sung as. */
        uint8_t phone = 0;
        /** That phone's first three formants, from the phone table, to find the nearest unit for a sound. */
        float f[3]{};
        /** The steady part with a margin, analysed. */
        audio::Utterance sound;
        Tract tract;
        /** Where the steady part is in [sound], in frames. */
        int32_t from = 0, to = 0;
        /** Brings its level to the built-in voice's. */
        float gain = 1.0f;
        /** A diphthong's move to its second vowel, in [sound], after the steady part. 0 for a plain vowel. */
        int32_t glideFrom = 0, glideTo = 0;
    };

    std::vector<Unit> vowels;
    /** Diphthongs as sung: the first vowel held, then the move to the second. */
    std::vector<Unit> diphthongs;

    /**
     * The recorded diphthong to sing [phone] with: itself, or for an accent's
     * version of one, the one it's a version of. The singer sang theirs in
     * their own accent. Null when there's none.
     */
    const Unit *diphthong(uint8_t phone) const;

    /**
     * Adds the diphthong [phone] from the take at [path]: held from
     * [holdFrom] to [holdTo], moving from [glideFrom] to [glideTo], in frames.
     */
    bool addDiphthong(const std::string &path, uint8_t phone, int32_t holdFrom, int32_t holdTo, int32_t glideFrom,
                      int32_t glideTo, float sampleRate, std::string &error);

    struct Join {
        /** The consonant, and the vowel it was sung between. */
        uint8_t phone = 0, carrier = 0;
        /** The vowel's formants, to find the join whose vowel is nearest a word's. */
        float f[3]{};
        /** The consonant with some of the vowel either side, analysed. */
        audio::Utterance sound;
        Tract tract;
        /** Where the consonant is in [sound], in frames. */
        int32_t from = 0, to = 0;
        /** Brings its vowels to the built-in voice's level, and the consonant with them. */
        float gain = 1.0f;
        /**
         * Brings the consonant itself down to no louder than a little under its
         * vowels, applied between [from] and [to]. Close to a phone, breath
         * made an S or a CH louder than the vowel it was sung in.
         */
        float consonantGain = 1.0f;
        /** A stop's burst in [sound], where the closure opens; 0 for anything else. */
        int32_t burst = 0;
        /**
         * Brighter than an ear hears it, so it's taken off a little at the
         * top: a sibilant centred above 8.5 kHz. A phone's mic an inch from the
         * mouth made one S sing sharp at 10.9 kHz; most sit at 6 to 8.
         */
        bool bright = false;
    };

    std::vector<Join> joins;

    /** Which take a consonant is formed from: sung between ahs, ees or oos, or the word's nearest. */
    enum class From : int32_t { Ah, Ee, Oo, Word };

    /**
     * The consonant [phone] as sung between the vowels [from] asks for, or
     * with Word, or when the voice hasn't that one, between the vowels most
     * like [vowel] in how far forward they're sung. Null if the voice has no
     * such consonant. The flap of butter is its D.
     *
     * Ah is the default because it's open and neutral, so the consonant comes through
     * clean: chosen by ear for W, R and G (between ees a W's rounding was
     * over at once, an R hardly lowered its third formant, and a G opened
     * into a y), and measured against the singer's own takes for the rest,
     * where between ees a TH kept a quarter of its hiss and a P or T lost
     * its closure.
     */
    const Join *consonant(uint8_t phone, const float vowel[3], From from = From::Ah) const;

    /**
     * Adds the consonant [phone] sung between two [carrier]s from the take at
     * [path], found at [from] to [to] in frames. Worker thread.
     */
    bool addConsonant(const std::string &path, uint8_t phone, uint8_t carrier, int32_t from, int32_t to,
                      float sampleRate, std::string &error);

    /**
     * The vowel nearest the formants [f], on a log scale, as an ear hears
     * them. The third counts too, or the a of about was sung as the er of
     * bird: the two differ only there.
     */
    const Unit *nearest(const float f[3]) const;

    /**
     * The recorded vowel to sing [phone] with: the vowel itself, or for one
     * nobody records (the a of about, an accent's shade of a vowel) the vowel
     * it's a shade of. Null when there's none, to fall back on [nearest].
     */
    const Unit *forPhone(uint8_t phone) const;

    /**
     * Adds the held vowel [phone] from the take at [path], whose steady part
     * is [holdFrom] to [holdTo] in frames. False if it couldn't be used.
     * Worker thread.
     */
    bool addVowel(const std::string &path, uint8_t phone, int32_t holdFrom, int32_t holdTo, float sampleRate,
                  std::string &error);
};

} // namespace acidulous::machine::diction
