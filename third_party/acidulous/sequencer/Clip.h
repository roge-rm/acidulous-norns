#pragma once
#include <cstdint>
#include <engine/core/Constants.h>
#include <engine/core/Expression.h>
#include <engine/core/Messages.h>
#include <vector>

// One track's data for one scene.
//
// Built off the audio thread, handed over through the constructor queue, and
// never changed after that. An edit makes a new Clip and the old one goes to
// the destructor queue, so the audio thread can read notes without a lock.

namespace acidulous::seq {

/**
 * Trig conditions. There's room for 64 (six bits). The five below plus every
 * "Nth of every M" up to M = 8 is 40, which wouldn't fit in five bits.
 */
enum class TrigCond : uint8_t {
    Always = 0,
    Prev = 1,    // only if the previous conditional trig in this pass played
    NotPrev = 2, // only if it didn't
    Fill = 3,    // only while the fill control is held
    NotFill = 4, // only while it isn't
    NthBase = 5, // and up, see nthCond
};

/**
 * The code for "the Nth of every M passes", packed triangularly.
 *
 * M = 2 takes two codes, M = 3 takes three and so on, so everything before M
 * is 2 + 3 + ... + (M-1), which is (M-1)M/2 - 1. No table needed.
 *
 * Don't drop the minus one. Without it the M = 3 codes overlap the M = 2
 * codes and 1:3 silently plays as 2:2.
 */
inline int32_t nthCond(int32_t n, int32_t m) {
    return static_cast<int32_t>(TrigCond::NthBase) + (m - 1) * m / 2 - 1 + (n - 1);
}

/** The inverse of nthCond: gives back N and M. */
inline void nthOf(int32_t cond, int32_t &n, int32_t &m) {
    int32_t off = cond - static_cast<int32_t>(TrigCond::NthBase);
    m = 2;
    while (off >= m) { off -= m; ++m; }
    n = off + 1;
}

/**
 * Chance, condition and ratchet packed into 16 bits (7 + 6 + 3). That fits in
 * the padding after velocity in ClipNote, so notes stay 20 bytes. Three
 * separate bytes would grow every note by a fifth.
 */
inline uint16_t packTrig(int32_t chance, int32_t cond, int32_t ratchet) {
    const auto c = static_cast<uint32_t>(chance < 0 ? 0 : (chance > 100 ? 100 : chance));
    const auto d = static_cast<uint32_t>(cond) & 0x3Fu;
    const auto r = static_cast<uint32_t>((ratchet < 1 ? 1 : (ratchet > 8 ? 8 : ratchet)) - 1);
    return static_cast<uint16_t>(c | (d << 7) | (r << 13));
}

struct ClipNote {
    int32_t tick;     // offset from clip start
    int32_t length;   // in ticks; ≥ 1
    uint8_t pitch;    // MIDI note number
    uint8_t velocity; // 1..127
    /**
     * The default is a normal note: 100% chance, no condition, one hit. A
     * zeroed trig would mean 0% chance (silence), so don't memset notes.
     * ClipNote n{} picks up this default.
     */
    static constexpr uint16_t kTrigDefault = 100; // 100%, Always, one hit
    uint16_t trig = kTrigDefault;

    int32_t chance() const { return trig & 0x7F; }               // 0..100
    int32_t condition() const { return (trig >> 7) & 0x3F; }     // a TrigCond
    int32_t ratchet() const { return ((trig >> 13) & 7) + 1; }   // 1..8
    /** Whether this note is in the Prev chain, i.e. it has a chance or condition. */
    bool conditional() const { return condition() != 0 || chance() < 100; }

    // This note's slice of Clip::expr, possibly empty. A shared array rather
    // than three vectors per note, because the note table is walked every
    // block and most notes have no expression. Three empty vectors would add
    // 72 bytes a note.
    int32_t exprFirst = 0;
    int32_t exprCount = 0;
};

// If this fails the note table grew. Make sure that's on purpose.
static_assert(sizeof(ClipNote) == 20, "ClipNote must stay twenty bytes");

// One point on one of a note's three expression curves. Ticks are relative
// to the note's start so the expression moves with the note when it's moved,
// quantised or copied.
struct ExprPoint {
    int32_t tick;
    int32_t kind; // an acidulous::Expr
    float value;  // 0..1, see engine/core/Expression.h
};

// A curve's value at tick: flat before the first point and after the last,
// interpolated in between. Same as Lane::valueAt but over a plain range,
// since a note's curves live inside one shared array.
inline float exprValueAt(const ExprPoint *points, int32_t count, int32_t tick) {
    if (count <= 0) return 0.0f;
    if (tick <= points[0].tick) return points[0].value;
    if (tick >= points[count - 1].tick) return points[count - 1].value;
    int32_t lo = 0, hi = count - 1;
    while (hi - lo > 1) {
        const int32_t mid = (lo + hi) / 2;
        if (points[mid].tick <= tick) lo = mid; else hi = mid;
    }
    const ExprPoint &a = points[lo], &b = points[hi];
    if (b.tick == a.tick) return a.value;
    const float f = static_cast<float>(tick - a.tick) / static_cast<float>(b.tick - a.tick);
    return a.value + (b.value - a.value) * f;
}

enum class PlayMode : uint8_t { Loop, OneShot };

struct LanePoint {
    int32_t tick;
    float value; // normalised 0..1, the engine's parameter domain
};

// One parameter's automation over the clip. Names are resolved to a unit and
// index when the snapshot is built, so playback is a plain lookup.
struct Lane {
    Unit unit = Unit::Machine;
    int32_t index = 0;
    bool linear = true;
    std::vector<LanePoint> points; // sorted by tick

    float valueAt(int32_t tick) const {
        if (points.empty()) return 0.0f;
        if (tick <= points.front().tick) return points.front().value;
        if (tick >= points.back().tick) return points.back().value;
        // binary search for the last point at or before tick
        size_t lo = 0, hi = points.size() - 1;
        while (hi - lo > 1) {
            const size_t mid = (lo + hi) / 2;
            if (points[mid].tick <= tick) lo = mid; else hi = mid;
        }
        const LanePoint &a = points[lo], &b = points[hi];
        if (!linear || b.tick == a.tick) return a.value;
        const float f = static_cast<float>(tick - a.tick) / static_cast<float>(b.tick - a.tick);
        return a.value + (b.value - a.value) * f;
    }
};

struct Clip {
    // Revision of the Kotlin-side clip this was built from. The builder reuses
    // a Clip across snapshots when the rev matches, so editing one clip only
    // rebuilds that clip.
    int64_t rev = 0;
    int32_t bars = 1;
    int32_t ticksPerBar = 4 * kPPQN; // from the scene's signature, set when the snapshot is built
    PlayMode playMode = PlayMode::Loop;
    bool mute = false;
    /**
     * Randomness for chance and conditions.
     *
     * With the same [seed] a clip plays the same every time, so exports are
     * repeatable. Change it for a different variation. It's saved with the
     * song and is separate from rev, which changes on every edit.
     *
     * [freeRoll] redraws the seed once per pass so the clip varies as it
     * plays. Within a pass the result is still deterministic, so ratchets and
     * Prev chains work across block boundaries in both modes.
     */
    int32_t seed = 0;
    bool freeRoll = false;
    /**
     * Some note in here uses Prev or NotPrev. Set when the clip is built so
     * only these clips pay for tracking the chain across skipped notes.
     */
    bool hasPrevCond = false;
    std::vector<ClipNote> notes; // must be sorted by tick before hand-over
    std::vector<Lane> lanes;
    // Every note's curves end to end. Each note owns [exprFirst, +exprCount),
    // sorted by kind and then tick, so a note-on finds its three curves in one
    // pass over its own points.
    std::vector<ExprPoint> expr;
    // The words the notes sing, for a singer: every note's phone codes end to
    // end, and for each note (in the notes' order) where its own start and
    // how many, packed as first << 8 | count. Both empty when no note has
    // words, which is nearly every clip, so they cost nothing there.
    std::vector<uint8_t> phones;
    std::vector<uint32_t> noteLyric;

    int32_t lengthTicks() const { return bars * ticksPerBar; }
};

} // namespace acidulous::seq
