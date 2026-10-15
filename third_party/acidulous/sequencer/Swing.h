#pragma once
#include <cstdint>
#include <engine/core/Constants.h>

// Swing: where a tick lands when the beat isn't even.
//
// Pushing every note on an odd sixteenth later by a fixed amount breaks on
// played-in material: a note a tick before the odd sixteenth stays put, a
// note on it jumps past it, and the two swap order.
//
// So time itself is bent instead. Each pair of subdivisions maps onto itself
// with its midpoint moved later, linearly on either side. The map is
// continuous and monotonic, and at 50% it changes nothing, so notes keep
// their order and spacing.
//
// It's applied to clip-relative ticks, so a clip swings the same wherever
// it's launched.
namespace acidulous::seq {

struct Swing {
    /** Straight. The default, and changes nothing. */
    static constexpr float kStraight = 50.0f;
    /** The most swing: the midpoint three quarters of the way through the pair. */
    static constexpr float kMax = 75.0f;
    /** Triplet swing, two thirds of the way through the pair. */
    static constexpr float kTriplet = 66.667f;

    /** A pair of sixteenths. The default unit, and what grooveboxes use. */
    static constexpr int64_t kSixteenths = kPPQN / 2;
    /** A pair of eighths, for a jazz or shuffle feel. */
    static constexpr int64_t kEighths = kPPQN;

    static bool straight(float percent) { return percent <= kStraight + 0.01f; }

    /**
     * Where [tick] sounds, given [percent] swing over pairs of [pair] ticks.
     *
     * Both halves use integer maths so the result is rounded in one place,
     * and a note and its note-off can't disagree about where the beat is.
     */
    static int64_t at(int64_t tick, float percent, int64_t pair) {
        if (straight(percent) || pair < 2 || tick < 0) return tick;
        const int64_t mid = midpoint(percent, pair);
        const int64_t half = pair / 2;
        const int64_t base = tick - tick % pair;
        const int64_t pos = tick % pair;
        const int64_t out = pos < half
                                ? pos * mid / half
                                : mid + (pos - half) * (pair - mid) / (pair - half);
        return base + out;
    }

    /**
     * The reverse: the straight tick that would sound at [swung].
     *
     * A part recorded while swing is on arrives in swung time. Songs store
     * straight time, so this converts it back before it's saved. Otherwise
     * it would be swung twice on playback.
     */
    static int64_t from(int64_t swung, float percent, int64_t pair) {
        if (straight(percent) || pair < 2 || swung < 0) return swung;
        const int64_t mid = midpoint(percent, pair);
        const int64_t half = pair / 2;
        const int64_t base = swung - swung % pair;
        const int64_t pos = swung % pair;
        const int64_t out = pos < mid
                                ? pos * half / mid
                                : half + (pos - mid) * (pair - half) / (pair - mid);
        return base + out;
    }

  private:
    /**
     * Where the middle of the pair goes, in ticks, kept away from both ends.
     *
     * A midpoint at either end would squash one half into a single instant
     * and make the inverse divide by zero. At sixteenths the pair is 120
     * ticks, so the guard is a twentieth of that and no setting reaches it.
     */
    static int64_t midpoint(float percent, int64_t pair) {
        const float clamped = percent < kStraight ? kStraight : (percent > kMax ? kMax : percent);
        int64_t mid = static_cast<int64_t>(static_cast<float>(pair) * clamped * 0.01f + 0.5f);
        const int64_t guard = pair / 20 + 1;
        if (mid < guard) mid = guard;
        if (mid > pair - guard) mid = pair - guard;
        return mid;
    }
};

} // namespace acidulous::seq
