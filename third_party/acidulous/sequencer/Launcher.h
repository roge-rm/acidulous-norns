#pragma once
#include <algorithm>
#include <cstdint>
#include <engine/core/Constants.h>

// Clip mode: what each rack is playing, what it's queued to play next, and
// exactly when the swap happens.
//
// The arranger plays one scene on all sixteen racks, so it needs one position.
// The launcher needs one per rack so, for example, the verse bass can play
// under the chorus drums. This only holds those sixteen positions (no racks,
// snapshot or clock), so it's plain maths over ticks and easy to test alone.
//
// Two rules decide every launch:
//
//   - a playing rack swaps at the end of its current cycle;
//   - a silent rack starts on the next multiple of the new clip's own cycle,
//     counted from the start of the transport, so an eight-bar clip always
//     starts on an eight-bar line and clips stay in phase.
//
// A fixed launch quantise overrides both with a plain grid.
//
// A clip's cycle is bars x repeat (the "x2 1b" on the scene chip). The clip
// still loops every bars, but a swap waits for the cycle and a OneShot
// re-arms on it.

namespace acidulous::seq {

class Launcher {
  public:
    // Scene ids are FNV-1a hashes of the song's scene ids, so 0 can mean
    // "nothing" and negative values can mean "stop".
    static constexpr int64_t kNone = 0;
    static constexpr int64_t kStopId = -1;
    /**
     * Cancel whatever is queued on this rack, and nothing else.
     *
     * A second tap on a queued clip would toggle it off, but if the first tap
     * has already landed by then it would queue a stop instead (e.g. on a
     * double tap to open the editor). An explicit cancel can't be misread.
     */
    static constexpr int64_t kCancelId = -2;

    void reset() {
        for (auto &s : slots) {
            s = Slot{};
        }
        changed = 0;
    }

    /** 0 launches at the end of the playing clip's cycle, otherwise a grid in ticks. */
    void setQuantise(int32_t ticks) { quantise = ticks < 0 ? 0 : ticks; }
    int32_t quantiseTicks() const { return quantise; }

    /**
     * A cell was tapped. Decided here rather than in the UI so it's based on
     * what the audio thread is actually playing, not a stale readback.
     *
     * Tapping a queued clip cancels it, tapping the playing clip queues a
     * stop, and tapping anything else queues it to start. [cycle] is the
     * tapped clip's cycle in ticks.
     */
    void request(int32_t rack, int64_t sceneId, int64_t cycle, int64_t now) {
        if (!valid(rack) || sceneId == kNone) {
            return;
        }
        Slot &s = slots[rack];
        if (s.pendingId == sceneId || (s.pendingId == kStopId && s.sceneId == sceneId)) {
            s.pendingId = kNone; // tapped twice, cancel
            s.pendingCycle = 0;
            return;
        }
        if (s.sceneId == sceneId) {
            queueAt(s, kStopId, 0, boundary(s, 0, now));
            return;
        }
        queueAt(s, sceneId, cycle, boundary(s, cycle, now));
    }

    /**
     * A scene header was pressed. Tracks with a clip in the scene play it and
     * every other track stops, all on the same tick like a scene change in
     * song mode.
     *
     * That tick is the next grid line if there's a grid, otherwise the latest
     * cycle end of the clips playing so nothing is cut short, or now if
     * nothing is playing. A track already playing this scene's clip carries on
     * (a tap on the cell would stop it).
     *
     * [cycles] is each rack's clip length in this scene, 0 where it has none.
     */
    void requestScene(int64_t sceneId, const int64_t *cycles, int32_t count, int64_t now) {
        if (sceneId == kNone) {
            return;
        }
        int64_t at = now;
        if (quantise > 0) {
            at = nextMultiple(now, quantise);
        } else if (anyPlaying()) {
            for (const auto &s : slots) {
                if (s.sceneId != kNone) at = std::max(at, boundary(s, 0, now));
            }
        }
        for (int32_t r = 0; r < kRackCount; ++r) {
            Slot &s = slots[r];
            const int64_t cycle = r < count ? cycles[r] : 0;
            if (cycle > 0 && s.sceneId == sceneId) {
                s.pendingId = kNone;
                s.pendingCycle = 0;
            } else if (cycle > 0) {
                queueAt(s, sceneId, cycle, at);
            } else if (s.sceneId != kNone) {
                queueAt(s, kStopId, 0, at);
            } else {
                s.pendingId = kNone;
                s.pendingCycle = 0;
            }
        }
    }

    /**
     * Take over a clip that's already playing, in phase, without queueing.
     *
     * Used when the grid switches to clip mode while the song plays, so the
     * current clips keep going and can be remixed live. The origin is passed
     * in so the clip carries on from where the scene was instead of
     * restarting.
     */
    void adopt(int32_t rack, int64_t sceneId, int64_t cycle, int64_t origin) {
        if (!valid(rack) || sceneId == kNone) {
            return;
        }
        Slot &s = slots[rack];
        s.sceneId = sceneId;
        s.cycle = cycle > 0 ? cycle : 1;
        s.origin = origin;
        s.pendingId = kNone;
        s.pendingCycle = 0;
        changed |= (1u << rack);
    }

    /** Clear this rack's queue but leave what it's playing alone. */
    void cancel(int32_t rack) {
        if (valid(rack)) {
            slots[rack].pendingId = kNone;
            slots[rack].pendingCycle = 0;
        }
    }

    /** Queue every playing rack to stop at its own boundary. */
    void requestStopAll(int64_t now) {
        for (auto &s : slots) {
            if (s.sceneId != kNone) {
                queueAt(s, kStopId, 0, boundary(s, 0, now));
            } else {
                s.pendingId = kNone;
                s.pendingCycle = 0;
            }
        }
    }

    /** Clear everything, queued and playing. Used by a hard stop. */
    void clearAll() {
        for (auto &s : slots) {
            s.sceneId = kNone;
            s.pendingId = kNone;
            s.cycle = 0;
            s.pendingCycle = 0;
        }
        changed = 0xffffffffu;
    }

    /**
     * Apply everything due at [tick]: cycles that have run out and launches
     * that have reached their boundary. Call it before rendering any span that
     * starts at tick so a clip starting here gets its origin here.
     */
    void applyDue(int64_t tick) {
        for (int32_t r = 0; r < kRackCount; ++r) {
            Slot &s = slots[r];
            if (s.sceneId != kNone && s.cycle > 0) {
                // A finished cycle moves the origin up. This re-arms OneShot
                // clips and restarts automation lanes.
                const int64_t past = tick - s.origin;
                if (past >= s.cycle) {
                    s.origin += (past / s.cycle) * s.cycle;
                }
            }
            if (s.pendingId != kNone && s.pendingAt <= tick) {
                if (s.pendingId == kStopId) {
                    s.sceneId = kNone;
                    s.cycle = 0;
                    s.origin = s.pendingAt;
                } else {
                    s.sceneId = s.pendingId;
                    s.cycle = std::max<int64_t>(1, s.pendingCycle);
                    // Use the boundary, not tick, so the clip keeps its phase
                    // even if a block starts past the boundary.
                    s.origin = s.pendingAt;
                }
                s.pendingId = kNone;
                s.pendingCycle = 0;
                changed |= (1u << r);
            }
        }
    }

    /**
     * The next tick after [now] where anything changes: a cycle ending or a
     * launch landing. The scheduler renders up to here and no further, which
     * makes swaps sample-accurate.
     */
    int64_t nextEvent(int64_t now) const {
        int64_t best = INT64_MAX;
        for (const Slot &s : slots) {
            if (s.sceneId != kNone && s.cycle > 0) {
                const int64_t end = s.origin + s.cycle;
                if (end > now && end < best) {
                    best = end;
                }
            }
            if (s.pendingId != kNone && s.pendingAt > now && s.pendingAt < best) {
                best = s.pendingAt;
            }
        }
        return best;
    }

    // --- read by the scheduler and the UI ---------------------------------------

    bool playing(int32_t rack) const { return valid(rack) && slots[rack].sceneId != kNone; }
    int64_t sceneId(int32_t rack) const { return valid(rack) ? slots[rack].sceneId : kNone; }
    int64_t pendingId(int32_t rack) const { return valid(rack) ? slots[rack].pendingId : kNone; }
    int64_t origin(int32_t rack) const { return valid(rack) ? slots[rack].origin : 0; }
    int64_t cycle(int32_t rack) const { return valid(rack) ? slots[rack].cycle : 0; }

    bool anyPlaying() const {
        for (const Slot &s : slots) {
            if (s.sceneId != kNone) {
                return true;
            }
        }
        return false;
    }
    bool anyPending() const {
        for (const Slot &s : slots) {
            if (s.pendingId != kNone) {
                return true;
            }
        }
        return false;
    }

    /** Which racks changed clip since this was last asked. One bit per rack. */
    uint32_t takeChanged() {
        const uint32_t c = changed;
        changed = 0;
        return c;
    }

    /** A snapshot arrived and this rack's scene went away. */
    void dropRack(int32_t rack) {
        if (!valid(rack)) {
            return;
        }
        slots[rack].sceneId = kNone;
        slots[rack].cycle = 0;
        slots[rack].pendingId = kNone;
        changed |= (1u << rack);
    }
    /** A snapshot arrived and this rack's clip changed length. */
    void reshape(int32_t rack, int64_t cycle) {
        if (valid(rack) && slots[rack].sceneId != kNone) {
            slots[rack].cycle = std::max<int64_t>(1, cycle);
        }
    }

  private:
    struct Slot {
        int64_t sceneId = kNone;   // what's playing, by stable scene id
        int64_t pendingId = kNone; // what's queued, or kStopId
        int64_t pendingAt = 0;     // the tick it lands on, set when queued
        int64_t pendingCycle = 0;
        int64_t origin = 0; // absolute tick this rack's cycle began
        int64_t cycle = 0;  // bars x repeat x ticksPerBar
    };

    static bool valid(int32_t rack) { return rack >= 0 && rack < kRackCount; }

    static int64_t nextMultiple(int64_t now, int64_t m) {
        if (m <= 0) {
            return now;
        }
        if (now <= 0) {
            return 0;
        }
        return ((now + m - 1) / m) * m;
    }

    /** When a change asked for at [now] should land on this slot. */
    int64_t boundary(const Slot &s, int64_t incoming, int64_t now) const {
        if (quantise > 0) {
            return nextMultiple(now, quantise);
        }
        if (s.sceneId != kNone && s.cycle > 0) {
            // The end of the current cycle. Never now, or a tap exactly on a
            // boundary would skip a whole cycle.
            const int64_t past = std::max<int64_t>(0, now - s.origin);
            return s.origin + (past / s.cycle + 1) * s.cycle;
        }
        // Silent while other racks play: the next multiple of this clip's
        // length from the transport's zero, so clips stay in phase.
        //
        // Silent with nothing playing at all: start now. There's no phase to
        // keep, and waiting for an unheard grid feels like the tap was ignored
        // (the transport is still running after stopping all clips).
        if (!anyPlaying()) return now;
        return incoming > 0 ? nextMultiple(now, incoming) : now;
    }

    static void queueAt(Slot &s, int64_t id, int64_t cycle, int64_t at) {
        s.pendingId = id;
        s.pendingCycle = cycle;
        s.pendingAt = at;
    }

    Slot slots[kRackCount];
    int32_t quantise = 0;
    uint32_t changed = 0;
};

} // namespace acidulous::seq
