#pragma once
#include "Clip.h"
#include "Swing.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

// Plays one rack's clip. Lives inside the Rack and sends into
// Rack::handleMidi(), ahead of the modifiers, so sequenced and live notes are
// treated the same.
//
// Positions are absolute transport ticks. The clip loops from the scene
// iteration's origin (pass k covers [origin + k*len, origin + (k+1)*len)) and
// pending note-offs are absolute ticks, so a note running past a loop point
// or scene boundary just ends when it ends.
//
// Audio thread only, apart from the diagnostic counters.

namespace acidulous::seq {

class ClipPlayer {
  public:
    static constexpr int kMaxPending = 64;
    static constexpr int kExprKinds = static_cast<int>(acidulous::Expr::Count);

    // Audio thread, at a block boundary (ObjectManager does this). Pending
    // note-offs are kept on purpose since those notes are still sounding.
    void setClip(const Clip *newClip) {
        clip_ = newClip;
        for (float &v : lastLane) v = -1.0f;
        lastOrigin = -1;
        // pointLaunched calls this on every launch, so a launched clip's
        // conditions start counting from that bar.
        passIndex_ = 0;
        lastBase_ = kNoBase;
        aheadEnd_ = kNoBase;
    }

    /**
     * Back to the beginning, for a render, so rendering the same song twice
     * gives the same result (like Arp::reset()).
     *
     * Not part of allNotesOff, which runs on every normal stop. Re-seeding
     * there would make a free-rolling clip repeat itself.
     */
    void reset() {
        freeSeed_ = kFreeSeed;
        lastFreePass_ = kNoBase;
        passIndex_ = 0;
        lastBase_ = kNoBase;
        aheadEnd_ = kNoBase;
    }

    /** Whether Fill trigs play. Set by the transport, read by gate(). */
    void setFill(bool on) { fill_ = on; }

    /**
     * This track's swing, set once a block like the fill.
     *
     * Applied to the clip-relative tick so a clip swings the same wherever
     * it's launched, and tracks can't swing out of phase with each other.
     */
    void setSwing(float percent, int64_t pair) { swingPercent_ = percent; swingPair_ = pair; }
    const Clip *clip() const { return clip_; }

    // Fire everything due in the absolute tick range [start, end). origin is
    // the tick the current scene iteration began at, where the clip's tick 0
    // is and where it loops from. The origin moves every iteration, so a
    // OneShot clip (first pass only) re-arms on each scene repeat.
    // Sink signature: void(uint8_t cmd, uint8_t p1, uint8_t p2).
    template <class Sink>
    void process(int64_t start, int64_t end, int64_t origin, Sink &&sink) {
        process(start, end, origin, sink, [](const uint8_t *, int32_t) {});
    }

    // As above, with a note's words sent to [lyric] just before its note-on.
    // Lyric signature: void(const uint8_t *phones, int32_t count).
    template <class Sink, class Lyric>
    void process(int64_t start, int64_t end, int64_t origin, Sink &&sink, Lyric &&lyric) {
        // Note-offs first so a note ending where another begins retriggers
        // cleanly. Anything overdue (tick < start) fires now.
        for (PendingOff &p : pending) {
            if (p.active && p.tick < end) {
                sink(0x80, p.pitch, 0);
                p.active = false;
                offCount.fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (clip_ == nullptr || clip_->mute || clip_->notes.empty()) {
            return;
        }
        const int64_t len = clip_->lengthTicks();
        if (len <= 0 || start < origin) {
            return;
        }

        for (int64_t k = (start - origin) / len; origin + k * len < end; ++k) {
            if (clip_->playMode == PlayMode::OneShot && k > 0) {
                break;
            }
            const int64_t base = origin + k * len;
            // Which pass of this clip this is. Counted rather than worked out
            // from base / len, which is wrong when the scene length isn't a
            // multiple of the clip's, and for a OneShot (k stays 0). A pass
            // split over two blocks counts once.
            if (base != lastBase_) {
                if (lastBase_ != kNoBase) ++passIndex_;
                lastBase_ = base;
            }
            const int64_t pass = passIndex_;
            // Free mode redraws the seed once per pass, not per block, so
            // within a pass a ratchet or Prev chain split across blocks gets
            // the same answer both times.
            if (clip_->freeRoll && pass != lastFreePass_) {
                lastFreePass_ = pass;
                freeSeed_ = freeSeed_ * 1664525u + 1013904223u;
            }

            bool prevPlayed = false; // the chain starts again every pass
            const bool words = !clip_->noteLyric.empty();
            for (size_t index = 0; index < clip_->notes.size(); ++index) {
                const ClipNote &note = clip_->notes[index];
                // Swing is monotonic, so the notes stay sorted and the break
                // below is safe.
                const int64_t t = base + swung(note.tick);
                if (t >= end) {
                    break; // notes are sorted and no sub-hit comes before its note
                }
                const int32_t rat = note.ratchet();
                // A skipped note still has a result the next note's Prev may
                // need, so gate() runs before the skip. Only clips with a Prev
                // pay for this.
                if (!clip_->hasPrevCond && rat <= 1 && t < start) {
                    continue;
                }
                const bool play = gate(note, pass, prevPlayed);
                if (note.conditional()) {
                    prevPlayed = play;
                }
                if (!play || (rat <= 1 && t < start)) {
                    continue;
                }
                // A ratchet subdivides the note within the clip. Without the
                // clamp, sub-hits past the clip end would silently vanish.
                const int32_t full = note.length > 0 ? note.length : 1;
                const int32_t span = std::min<int32_t>(full, static_cast<int32_t>(len) - note.tick);
                const int32_t step = rat > 1 ? std::max(1, span / rat) : span;
                // The note's end is swung the same as its start, so legato
                // notes still meet instead of gapping or overlapping.
                const int64_t tail = base + swung(note.tick + span);
                for (int32_t j = 0; j < rat; ++j) {
                    const int64_t h = base + swung(note.tick + static_cast<int64_t>(j) * step);
                    if (h >= end) break;
                    if (h < start) continue; // fired in an earlier block
                    const int64_t next = base + swung(note.tick + static_cast<int64_t>(j + 1) * step);
                    const int64_t off = rat > 1
                                            ? std::max<int64_t>(h + 1, std::min<int64_t>(next, tail))
                                            : tail;
                    releaseIfSounding(note.pitch, sink);
                    if (words) {
                        const uint32_t w = clip_->noteLyric[index];
                        const uint32_t first = w >> 8, count = w & 0xFFu;
                        if (count > 0 && first + count <= clip_->phones.size()) {
                            lyric(clip_->phones.data() + first, static_cast<int32_t>(count));
                        }
                    }
                    sink(0x90, note.pitch, note.velocity);
                    onCount.fetch_add(1, std::memory_order_relaxed);
                    // t, not h: the curve runs across the whole ratchet
                    // instead of restarting on each hit.
                    schedule(note, t, off, sink);
                }
            }
        }
    }

    /**
     * The words of notes due in [start, end), ahead of the playhead, to
     * [ahead](phones, count, note, velocity, tick) with the tick each is due.
     * A singer starts a word's consonants that far early. Never the same
     * stretch twice, whatever the tempo does to the window. Past
     * [origin] + [loopTicks] only while the clip lies the same way in the next
     * pass, which it does when it fits it a whole number of times; 0 for
     * no end. A note on a condition gets its words on time instead: whether
     * it plays isn't known until then.
     */
    template <class Ahead>
    void processWordsAhead(int64_t start, int64_t end, int64_t origin, int64_t loopTicks, Ahead &&ahead) {
        if (clip_ == nullptr || clip_->mute || clip_->notes.empty() || clip_->noteLyric.empty()) return;
        const int64_t len = clip_->lengthTicks();
        if (len <= 0) return;
        // Ticks only grow while playing. A clip's length behind where it had
        // got to, the transport started again or jumped back; a little behind
        // is only the tempo slowing, which shortens the window.
        if (aheadEnd_ != kNoBase && aheadEnd_ > end + len) aheadEnd_ = kNoBase;
        if (loopTicks > 0 && loopTicks % len != 0) end = std::min(end, origin + loopTicks);
        start = std::max(start, std::max(origin, aheadEnd_));
        if (end <= start) return;
        aheadEnd_ = end;
        for (int64_t k = (start - origin) / len; origin + k * len < end; ++k) {
            if (clip_->playMode == PlayMode::OneShot && k > 0) break;
            const int64_t base = origin + k * len;
            for (size_t index = 0; index < clip_->notes.size(); ++index) {
                const ClipNote &note = clip_->notes[index];
                const int64_t t = base + swung(note.tick);
                if (t >= end) break;
                if (t < start || note.conditional()) continue;
                const uint32_t w = clip_->noteLyric[index];
                const uint32_t first = w >> 8, count = w & 0xFFu;
                if (count > 0 && first + count <= clip_->phones.size()) {
                    ahead(clip_->phones.data() + first, static_cast<int32_t>(count), note.pitch, note.velocity, t);
                }
            }
        }
    }

    // Automation: before the notes, set every lane's value at the block's end
    // position. Setter signature: void(Unit, int32_t index, float value,
    // bool jump). A stepped lane jumps so a step lock is fully in place for
    // the note on that step.
    // Skips lanes the live UI has touched during this pass while recording.
    template <class Setter, class Touched>
    void processLanes(int64_t end, int64_t origin, Setter &&set, Touched &&touched) {
        if (clip_ == nullptr || clip_->lanes.empty()) return;
        const int64_t len = clip_->lengthTicks();
        if (len <= 0 || end < origin) return;
        if (origin != lastOrigin) {
            lastOrigin = origin;
            for (float &v : lastLane) v = -1.0f; // a new pass resends everything
        }
        const auto t = static_cast<int32_t>((end - origin) % len);
        const size_t n = clip_->lanes.size() < kMaxLanes ? clip_->lanes.size() : kMaxLanes;
        for (size_t i = 0; i < n; ++i) {
            const Lane &lane = clip_->lanes[i];
            if (touched(lane.unit, lane.index)) continue;
            const float v = lane.valueAt(t);
            if (v != lastLane[i]) {
                lastLane[i] = v;
                set(lane.unit, lane.index, v, !lane.linear);
            }
        }
    }
    bool originChanged(int64_t origin) const { return origin != lastOrigin; }

    /**
     * Per-note expression: each sounding note's curve values at the end of
     * this block. Only sent when the value changes, so a flat curve costs one
     * call and a glide one call a block.
     *
     * Sink signature: void(uint8_t note, int32_t kind, float value01).
     */
    template <class Sink>
    void processExpression(int64_t end, Sink &&sink) {
        if (clip_ == nullptr) return;
        for (PendingOff &p : pending) {
            // If the clip was swapped under a sounding note, its curves went
            // with the old Clip. The note keeps sounding at its last value.
            if (!p.active || !p.hasExpr || p.exprRev != clip_->rev) continue;
            const auto rel = static_cast<int32_t>(end > p.startTick ? end - p.startTick : 0);
            for (int32_t k = 0; k < kExprKinds; ++k) {
                if (p.exprCount[k] == 0) continue;
                const float v = exprValueAt(clip_->expr.data() + p.exprFirst[k], p.exprCount[k], rel);
                if (v == p.exprLast[k]) continue;
                p.exprLast[k] = v;
                sink(p.pitch, k, v);
            }
        }
    }

    // Stop: release everything that's sounding.
    template <class Sink>
    void allNotesOff(Sink &&sink) {
        for (PendingOff &p : pending) {
            if (p.active) {
                sink(0x80, p.pitch, 0);
                p.active = false;
                offCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Diagnostics, any thread. Equal after a stop means no stuck notes.
    uint32_t notesOn() const { return onCount.load(std::memory_order_relaxed); }
    uint32_t notesOff() const { return offCount.load(std::memory_order_relaxed); }

  private:
    /** Where a clip-relative tick sounds after swing. Unchanged with no swing. */
    int64_t swung(int64_t tick) const { return Swing::at(tick, swingPercent_, swingPair_); }

    float swingPercent_ = Swing::kStraight;
    int64_t swingPair_ = Swing::kSixteenths;

    struct PendingOff {
        int64_t tick = 0;
        uint8_t pitch = 0;
        bool active = false;
        // The note's expression, worked out once when it fires: its start,
        // which clip it came from, and each curve's range in that clip's expr
        // array. The ranges can't change while the note sounds.
        int64_t startTick = 0;
        int64_t exprRev = 0;
        int32_t exprFirst[kExprKinds] = {};
        int32_t exprCount[kExprKinds] = {};
        float exprLast[kExprKinds] = {};
        bool hasExpr = false;
    };

    // End the same pitch if it's already sounding so no voice is left without
    // a note-off.
    template <class Sink>
    void releaseIfSounding(uint8_t pitch, Sink &sink) {
        for (PendingOff &p : pending) {
            if (p.active && p.pitch == pitch) {
                sink(0x80, pitch, 0);
                p.active = false;
                offCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    template <class Sink>
    void schedule(const ClipNote &note, int64_t onTick, int64_t offTick, Sink &sink) {
        for (PendingOff &p : pending) {
            if (!p.active) {
                p.tick = offTick;
                p.pitch = note.pitch;
                p.active = true;
                bindExpression(p, note, onTick);
                return;
            }
        }
        // Table full (more than kMaxPending notes at once). Make it a
        // zero-length note rather than a stuck one.
        sink(0x80, note.pitch, 0);
        offCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Split the note's slice of the clip's expr array into one range per kind.
    // The slice is sorted by kind then tick, so this is one short pass.
    void bindExpression(PendingOff &p, const ClipNote &note, int64_t onTick) {
        p.startTick = onTick;
        p.exprRev = clip_ != nullptr ? clip_->rev : 0;
        p.hasExpr = false;
        for (int32_t k = 0; k < kExprKinds; ++k) {
            p.exprFirst[k] = 0;
            p.exprCount[k] = 0;
            // NaN never equals anything, so the first value is always sent.
            p.exprLast[k] = std::numeric_limits<float>::quiet_NaN();
        }
        if (clip_ == nullptr || note.exprCount <= 0) return;
        const auto total = static_cast<int32_t>(clip_->expr.size());
        if (note.exprFirst < 0 || note.exprFirst + note.exprCount > total) return; // malformed, ignore it
        for (int32_t i = 0; i < note.exprCount; ++i) {
            const int32_t kind = clip_->expr[static_cast<size_t>(note.exprFirst + i)].kind;
            if (kind < 0 || kind >= kExprKinds) continue;
            if (p.exprCount[kind] == 0) p.exprFirst[kind] = note.exprFirst + i;
            ++p.exprCount[kind];
            p.hasExpr = true;
        }
    }

    /**
     * The random roll, from the seed, the pass and the note. The note is
     * identified by (tick, pitch), not its index, so inserting a note doesn't
     * reroll every note after it. Same hash mixing as Dice.
     */
    uint32_t roll(const ClipNote &note, int64_t pass) const {
        uint32_t h = clip_->freeRoll ? freeSeed_ : static_cast<uint32_t>(clip_->seed);
        h = h * 2654435761u + static_cast<uint32_t>(pass) * 40503u + 1u;
        h += static_cast<uint32_t>(note.tick) * 2246822519u + note.pitch * 668265263u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return h;
    }

    /** Does this trig play? Condition first, then the dice. */
    bool gate(const ClipNote &note, int64_t pass, bool prevPlayed) const {
        const int32_t c = note.condition();
        if (c == static_cast<int32_t>(TrigCond::Prev) && !prevPlayed) return false;
        if (c == static_cast<int32_t>(TrigCond::NotPrev) && prevPlayed) return false;
        if (c == static_cast<int32_t>(TrigCond::Fill) && !fill_) return false;
        if (c == static_cast<int32_t>(TrigCond::NotFill) && fill_) return false;
        if (c >= static_cast<int32_t>(TrigCond::NthBase)) {
            int32_t n = 0, m = 2;
            nthOf(c, n, m);
            if (pass % m != n - 1) return false; // pass is >= 0 by construction
        }
        const int32_t chance = note.chance();
        if (chance >= 100) return true;
        if (chance <= 0) return false;
        return static_cast<int32_t>(roll(note, pass) % 100u) < chance;
    }

    static constexpr size_t kMaxLanes = 32;
    static constexpr int64_t kNoBase = std::numeric_limits<int64_t>::min();
    static constexpr uint32_t kFreeSeed = 0x9E3779B9u; // same as Arp's
    const Clip *clip_ = nullptr;
    float lastLane[kMaxLanes]{};
    int64_t lastOrigin = -1;
    int64_t passIndex_ = 0;
    int64_t lastBase_ = kNoBase;
    /** Where words ahead have been sent up to, in absolute ticks. */
    int64_t aheadEnd_ = kNoBase;
    int64_t lastFreePass_ = kNoBase;
    uint32_t freeSeed_ = kFreeSeed;
    bool fill_ = false;
    PendingOff pending[kMaxPending];
    std::atomic<uint32_t> onCount{0};
    std::atomic<uint32_t> offCount{0};
};

} // namespace acidulous::seq
