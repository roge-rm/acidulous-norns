#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Cuts a recorded take into the parts Diction sings from.
//
// A take is one prompt sung on one note: a held vowel ("ee"), a diphthong
// ("eye", held on its first vowel and moving to the second at the end, as it's
// sung), or a consonant between two ahs ("ah-sah"). This finds where the
// singing is, the part of a vowel that holds steady, where a diphthong moves
// and where a consonant starts and ends, from the sound alone. A take it can't
// make sense of says why, so it can be sung again.
namespace acidulous::machine::diction {

/** What a take was asked to be. */
enum class TakeKind { Held, Glide, Between };

struct Cut {
    /** Empty when the take is usable, otherwise what's wrong with it, in a few words. */
    std::string problem;
    /** The sung part, in frames. */
    int32_t start = 0, end = 0;
    /** A held vowel's steady part, or a diphthong's first vowel's. */
    int32_t holdFrom = 0, holdTo = 0;
    /** Where a diphthong moves from its first vowel to its second. Its second runs on to [end]. */
    int32_t glideFrom = 0, glideTo = 0;
    /** A carrier's consonant, with the steady vowel before it ending and after it starting around it. */
    int32_t consonantFrom = 0, consonantTo = 0;
    /**
     * The pitch it was sung at, and how far that is from the note asked for,
     * in cents, in whichever octave it was sung: a low voice sings the note an
     * octave down.
     */
    float rootHz = 0.0f;
    float centsOff = 0.0f;
};

/**
 * Cuts [mono] at [sampleRate], a take of [kind]. [noteHz] is the note it
 * should have been sung on. [consonantNear], in frames, is where the singer
 * was shown to put the consonant, if they were: it's looked for within
 * 0.6 s of there, clear of the voice starting and stopping.
 */
Cut cutTake(const std::vector<float> &mono, float sampleRate, TakeKind kind, float noteHz, int32_t consonantNear = -1);

} // namespace acidulous::machine::diction
