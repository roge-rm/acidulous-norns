#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <engine/core/Mapping.h>
#include <memory>
#include <vector>

// An audio track's recordings, and where in the song each one plays.
//
// Works like FrozenSet: keyed by scene, per rack, mounted whole and swapped
// at a block boundary. The differences:
//
//   - A region is a window into a file. A take sung across four scenes is one
//     file and four regions at different offsets, so splitting at scene lines
//     loses no audio.
//   - Four lanes play at once, like a four-track, and are summed.
//
// Built on a worker and never changed. The audio thread only reads it, and a
// new one is swapped in.
namespace acidulous::audio {

/**
 * The longest take that's held in memory: two minutes. Shorter takes are
 * decoded into vectors.
 */
constexpr int32_t kResidentSeconds = 120;

/**
 * The longest take allowed: half an hour. Takes longer than
 * [kResidentSeconds] are converted once to the engine's flat format and
 * memory-mapped, so only the part being played uses RAM. See `audio::Mapping`.
 */
constexpr int32_t kMaxReelSeconds = 1800;

/** How many lanes Bias has, like a four-track. */
constexpr int32_t kReelLanes = 4;

/** How long a join between two pieces crossfades: 5 ms at 48 kHz, centred on the join. */
constexpr int32_t kJoinFadeFrames = 240;

/** How far either way a join may move to find a quiet spot: 40 ms at 48 kHz. */
constexpr int32_t kJoinSearchFrames = 1920;

/** What the loudness at a candidate join is measured over: the span its crossfade covers. */
constexpr int32_t kJoinQuietFrames = kJoinFadeFrames;

/** Candidate joins are tried this many frames apart. */
constexpr int32_t kJoinStepFrames = 4;

/**
 * Float to int16, clamped and rounded.
 *
 * A take can go above full scale (the channel strip brings it down), so this
 * clamps instead of wrapping, which would click.
 */
inline int16_t toI16(float v) {
    const float x = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<int16_t>(x * 32767.0f + (x >= 0.0f ? 0.5f : -0.5f));
}

struct Reel {
    /**
     * One decoded file, shared by every region that reads it.
     *
     * Stored as int16, and mono stays mono, so a five-minute take is 29 MB
     * instead of 115 MB as stereo float. A phone mic's noise floor is well
     * above 16 bits, so nothing audible is lost.
     *
     * Shared with `shared_ptr` so a long take split into many regions is
     * only decoded once.
     */
    struct Source {
        /**
         * Always read through these pointers, not the storage below. A source
         * is either decoded into `own` or mapped from a cache file, and `lp`
         * and `rp` point into whichever one is used.
         *
         * Planar, with mono kept as one channel. The mapped file uses the
         * same layout.
         */
        const int16_t *lp = nullptr;
        const int16_t *rp = nullptr;
        int32_t frames = 0;
        bool stereo = false;

        std::vector<int16_t> own;   // resident: the samples themselves
        std::shared_ptr<Mapping> map; // mapped: the file they live in

        /** Resident, from planar channels already in hand. */
        void hold(std::vector<int16_t> &&planes, int32_t n, bool isStereo) {
            own = std::move(planes);
            frames = n;
            stereo = isStereo;
            lp = own.data();
            rp = isStereo ? own.data() + n : own.data();
        }

        /** Mapped, from a converted cache file: left plane then right. */
        bool point(std::shared_ptr<Mapping> m, int32_t n, bool isStereo) {
            const size_t want = static_cast<size_t>(n) * (isStereo ? 2 : 1) * sizeof(int16_t);
            if (!m || !m->valid() || m->size() < want) return false;
            map = std::move(m);
            frames = n;
            stereo = isStereo;
            lp = reinterpret_cast<const int16_t *>(map->data());
            rp = isStereo ? lp + n : lp;
            return true;
        }

        /** Hints to the kernel which frames a cell is about to play. */
        void willNeed(int32_t from, int32_t count) const {
            if (!map) return;
            // A stacked take's offset is negative when it starts after the region does.
            if (from < 0) {
                count += from;
                from = 0;
            }
            if (count <= 0 || from >= frames) return;
            const size_t unit = sizeof(int16_t);
            map->willNeed(static_cast<size_t>(from) * unit, static_cast<size_t>(count) * unit);
            if (stereo) {
                map->willNeed(static_cast<size_t>(frames + from) * unit,
                              static_cast<size_t>(count) * unit);
            }
        }
    };

    /** One lane of one cell: which file, which part of it, and when. */
    struct Region {
        std::shared_ptr<const Source> source;
        int32_t offset = 0;    // frames into the source where this cell starts
        int32_t frames = 0;    // how many of them are this cell's
        int32_t startTick = 0; // where in the cycle it begins, non-zero for a punch-in
        int32_t ticks = 0;     // the cycle it was recorded against
        float bpm = 120.0f;    // the tempo it was recorded at
        bool loop = false;     // wrap within the region instead of going silent
        /**
         * Fade in and out lengths, in frames.
         *
         * Equal-power, so two overlapping takes crossfade at a steady level.
         * A linear fade would dip 3 dB in the middle.
         */
        int32_t fadeIn = 0;
        int32_t fadeOut = 0;

        /**
         * The stack of takes for a comp. Each is a file and where this cell
         * starts in it, all recorded against the same cycle, so frame [at]
         * of the region is frame `offset + at` of every take. Empty for a
         * lane with one take, which is `source` and `offset` above.
         */
        struct Take {
            std::shared_ptr<const Source> source;
            int32_t offset = 0;
            /**
             * The region frames it has audio for. A take from loop recording
             * shares its file with the passes either side, so a blend must
             * not read past these. Pieces are already kept inside them.
             */
            int32_t start = INT32_MIN / 2;
            int32_t end = INT32_MAX / 2;
            /** Its level in a blend, set by settle(). */
            float gain = 1.0f;
            /**
             * Where it sits across the stereo field in a blend, set by
             * settle() from [spread]: each side's gain, 1 and 1 in the middle.
             */
            float panL = 1.0f;
            float panR = 1.0f;
            /**
             * A stretch of region frames [start, end) where a blend plays this
             * take [shift] frames later: one word lined up. Sorted, apart, and
             * shift 0 between them. A change of shift crossfades like a join.
             */
            struct Nudge {
                int32_t start = 0;
                int32_t end = 0;
                int32_t shift = 0;
            };
            std::vector<Nudge> nudges;
        };
        std::vector<Take> takes;

        /**
         * Every take plays at once instead of the pieces, as a double or a
         * choir from the stack. Each take is brought to the stack's middle
         * level and the sum is scaled by the square root of how many sound,
         * so it sits at the level of one take.
         */
        bool blend = false;
        /**
         * How far a blend's takes spread across the stereo field, 0 to 1.
         * 0 is all in the middle; 1 puts the first and last at the edges,
         * the rest evenly between.
         */
        float spread = 0.0f;

        /**
         * A stretch of the region, [start, end) in region frames, played from
         * one take. Pieces that meet crossfade over `kJoinFadeFrames` centred
         * on the join. An edge next to a gap fades the same way into silence.
         */
        struct Piece {
            int32_t start = 0;
            int32_t end = 0;
            int32_t take = 0;
            /**
             * How many frames later the take plays here: frame [at] of the
             * region reads take frame `offset + at - shift`. Negative pulls
             * it earlier. Lining up a double sets it per phrase.
             */
            int32_t shift = 0;
        };
        /** Sorted and settled (see settle()). Empty plays `source` whole. */
        std::vector<Piece> pieces;

        bool comped() const { return !pieces.empty() || blend; }

        /** Whether any take this region plays is stereo. */
        bool stereo() const {
            if (!comped()) return source != nullptr && source->stereo;
            if (blend && spread > 0.0f) return true;
            for (const Take &t : takes) {
                if (t.source != nullptr && t.source->stereo) return true;
            }
            return false;
        }

        /** Where piece [k] starts sounding, its fade-in included. */
        int32_t soundsFrom(size_t k) const {
            const Piece &p = pieces[k];
            return p.start > 0 ? p.start - kJoinFadeFrames / 2 : p.start;
        }

        /** Where piece [k] stops sounding, its fade-out included. */
        int32_t soundsTo(size_t k) const {
            const Piece &p = pieces[k];
            return p.end < frames ? p.end + kJoinFadeFrames / 2 : p.end;
        }

        /** Piece [k]'s level at [at] region frames, equal power at its edges. */
        float pieceGain(size_t k, int64_t at) const {
            const Piece &p = pieces[k];
            constexpr float kHalf = static_cast<float>(kJoinFadeFrames / 2);
            constexpr float kPer = 1.0f / static_cast<float>(kJoinFadeFrames);
            float g = 1.0f;
            if (p.start > 0) g = (static_cast<float>(at - p.start) + kHalf) * kPer;
            if (p.end < frames) {
                const float k2 = (static_cast<float>(p.end - at) + kHalf) * kPer;
                g = g < k2 ? g : k2;
            }
            if (g <= 0.0f) return 0.0f;
            if (g >= 1.0f) return 1.0f;
            return std::sin(g * 1.5707963f);
        }

        /**
         * The comp at [at] region frames, scaled to +-1, before `fadeAt`.
         *
         * Audio thread. [hint] is the piece the last read used. Reads walk
         * from it, so reading forward costs nothing and a jump costs the
         * pieces crossed. The answer doesn't depend on the hint.
         */
        void compAt(int64_t at, int32_t &hint, float &l, float &r) const {
            if (blend) {
                blendAt(at, l, r);
                return;
            }
            l = r = 0.0f;
            const int32_t n = static_cast<int32_t>(pieces.size());
            if (n == 0) return;
            int32_t k = hint < 0 ? 0 : (hint >= n ? n - 1 : hint);
            // The first piece still sounding at [at]. Pieces are at least a
            // fade long, so no more than two sound at once.
            while (k > 0 && soundsTo(static_cast<size_t>(k - 1)) > at) --k;
            while (k < n - 1 && soundsTo(static_cast<size_t>(k)) <= at) ++k;
            hint = k;
            for (int32_t j = k; j < n && j <= k + 1; ++j) {
                if (at < soundsFrom(static_cast<size_t>(j)) || at >= soundsTo(static_cast<size_t>(j))) continue;
                const Piece &p = pieces[static_cast<size_t>(j)];
                const Take &t = takes[static_cast<size_t>(p.take)];
                const int64_t s = t.offset + at - p.shift;
                if (s < 0 || s >= t.source->frames) continue;
                const float g = pieceGain(static_cast<size_t>(j), at) * (1.0f / 32768.0f);
                l += static_cast<float>(t.source->lp[static_cast<size_t>(s)]) * g;
                r += static_cast<float>(t.source->rp[static_cast<size_t>(s)]) * g;
            }
        }

        /** Take [t]'s level at [at] region frames: equal power over a fade at either end of its audio. */
        float takeEdge(const Take &t, int64_t at) const {
            constexpr float kPer = 1.0f / static_cast<float>(kJoinFadeFrames);
            if (at < t.start || at >= t.end) return 0.0f;
            float g = 1.0f;
            if (t.start > 0) g = static_cast<float>(at - t.start) * kPer;
            if (t.end < frames) {
                const float k2 = static_cast<float>(t.end - at) * kPer;
                g = g < k2 ? g : k2;
            }
            if (g >= 1.0f) return 1.0f;
            return std::sin(g * 1.5707963f);
        }

        /**
         * Every take at once at [at] region frames, scaled to +-1. Audio
         * thread: a few takes, one square root, nothing allocated.
         *
         * Divided by the square root of the summed power of the takes
         * sounding, so N takes that don't correlate sum to the level of one,
         * and a take starting or stopping moves the scale smoothly. Below
         * one take's worth it isn't raised, so a lone take still fades.
         */
        void blendAt(int64_t at, float &l, float &r) const {
            l = r = 0.0f;
            float power = 0.0f;
            for (const Take &t : takes) {
                if (t.source == nullptr) continue;
                const float w = takeEdge(t, at);
                if (w <= 0.0f) continue;
                float tl = 0.0f, tr = 0.0f;
                if (t.nudges.empty()) {
                    const int64_t s = t.offset + at;
                    if (s < 0 || s >= t.source->frames) continue;
                    tl = static_cast<float>(t.source->lp[static_cast<size_t>(s)]);
                    tr = static_cast<float>(t.source->rp[static_cast<size_t>(s)]);
                } else {
                    nudgedAt(t, at, tl, tr);
                }
                power += w * w;
                const float g = w * t.gain * (1.0f / 32768.0f);
                l += tl * g * t.panL;
                r += tr * g * t.panR;
            }
            if (power > 1.0f) {
                const float k = 1.0f / std::sqrt(power);
                l *= k;
                r *= k;
            }
        }

        /** Take [t]'s shift at [at] region frames from its nudges. */
        static int32_t shiftAt(const Take &t, int64_t at) {
            const auto it = std::upper_bound(t.nudges.begin(), t.nudges.end(), at,
                                             [](int64_t v, const Take::Nudge &n) { return v < n.start; });
            if (it == t.nudges.begin()) return 0;
            const Take::Nudge &n = *(it - 1);
            return at < n.end ? n.shift : 0;
        }

        /** Take [t] read [shift] frames later, unscaled, 0 off its file. */
        static void readTake(const Take &t, int64_t at, int32_t shift, float &l, float &r) {
            const int64_t s = t.offset + at - shift;
            if (s < 0 || s >= t.source->frames) {
                l = r = 0.0f;
                return;
            }
            l = static_cast<float>(t.source->lp[static_cast<size_t>(s)]);
            r = static_cast<float>(t.source->rp[static_cast<size_t>(s)]);
        }

        /**
         * Take [t] at [at] through its nudges, unscaled. Where the shift
         * changes it crossfades over `kJoinFadeFrames` centred on the
         * change, equal power like a join. Settled nudges and the gaps
         * between them are at least a fade long, so one fade at a time.
         * Audio thread: two searches, nothing allocated.
         */
        static void nudgedAt(const Take &t, int64_t at, float &l, float &r) {
            constexpr int32_t kHalf = kJoinFadeFrames / 2;
            // The edge within half a fade of [at], if any.
            int64_t edge = -1;
            auto it = std::upper_bound(t.nudges.begin(), t.nudges.end(), at + kHalf,
                                       [](int64_t v, const Take::Nudge &n) { return v < n.start; });
            if (it != t.nudges.begin()) {
                const Take::Nudge &n = *(it - 1);
                if (n.end > at - kHalf && n.end <= at + kHalf) edge = n.end;
                else if (n.start > at - kHalf) edge = n.start;
            }
            if (edge < 0) {
                readTake(t, at, shiftAt(t, at), l, r);
                return;
            }
            const int32_t before = shiftAt(t, edge - 1);
            const int32_t after = shiftAt(t, edge);
            if (before == after) {
                readTake(t, at, before, l, r);
                return;
            }
            const float x = (static_cast<float>(at - edge) + static_cast<float>(kHalf)) * (1.0f / static_cast<float>(kJoinFadeFrames));
            const float gIn = x <= 0.0f ? 0.0f : x >= 1.0f ? 1.0f : std::sin(x * 1.5707963f);
            const float gOut = x <= 0.0f ? 1.0f : x >= 1.0f ? 0.0f : std::sin((1.0f - x) * 1.5707963f);
            float al = 0.0f, ar = 0.0f, bl = 0.0f, br = 0.0f;
            if (gOut > 0.0f) readTake(t, at, before, al, ar);
            if (gIn > 0.0f) readTake(t, at, after, bl, br);
            l = al * gOut + bl * gIn;
            r = ar * gOut + br * gIn;
        }

        /**
         * The lane at [at] region frames, comped or plain, scaled to +-1,
         * before `fadeAt`. Audio thread.
         */
        void soundAt(int64_t at, int32_t &hint, float &l, float &r) const {
            if (comped()) {
                compAt(at, hint, l, r);
                return;
            }
            l = r = 0.0f;
            if (source == nullptr) return;
            const int64_t s = offset + at;
            if (s < 0 || s >= source->frames) return;
            l = static_cast<float>(source->lp[static_cast<size_t>(s)]) * (1.0f / 32768.0f);
            r = static_cast<float>(source->rp[static_cast<size_t>(s)]) * (1.0f / 32768.0f);
        }

        /**
         * Tidies the pieces and moves each join to a quiet spot. Run once
         * when the reel is built, never on the audio thread.
         *
         * Pieces are clamped to the region and sorted, a later piece cuts
         * short the one it overlaps, neighbours from the same take merge, and
         * pieces shorter than a fade are dropped. One piece over the whole
         * region becomes a plain region of its take, so it plays exactly as
         * a lane with one take does. Each edge inside the region then moves
         * up to `kJoinSearchFrames` either way, to where the takes either
         * side of it are quietest.
         */
        void settle() {
            if (blend) {
                settleBlend();
                return;
            }
            if (pieces.empty()) return;
            if (takes.empty()) takes.push_back({source, offset});
            std::vector<Piece> kept;
            for (Piece p : pieces) {
                if (p.take < 0 || p.take >= static_cast<int32_t>(takes.size())) continue;
                if (takes[static_cast<size_t>(p.take)].source == nullptr) continue;
                p.start = std::max(p.start, 0);
                p.end = std::min(p.end, frames);
                if (p.end > p.start) kept.push_back(p);
            }
            std::stable_sort(kept.begin(), kept.end(),
                             [](const Piece &a, const Piece &b) { return a.start < b.start; });
            std::vector<Piece> out;
            for (const Piece &p : kept) {
                if (!out.empty() && out.back().end > p.start) out.back().end = p.start;
                if (!out.empty() && out.back().end - out.back().start < kJoinFadeFrames) out.pop_back();
                if (!out.empty() && out.back().take == p.take && out.back().shift == p.shift &&
                    out.back().end == p.start) {
                    out.back().end = p.end;
                    continue;
                }
                out.push_back(p);
            }
            if (!out.empty() && out.back().end - out.back().start < kJoinFadeFrames) out.pop_back();
            pieces = std::move(out);

            if (pieces.size() == 1 && pieces[0].start == 0 && pieces[0].end == frames && pieces[0].shift == 0) {
                const Take &t = takes[static_cast<size_t>(pieces[0].take)];
                source = t.source;
                offset = t.offset;
                pieces.clear();
                takes.clear();
                return;
            }
            if (pieces.empty()) {
                // Nothing left to play, which is silence, not take one.
                frames = 0;
                takes.clear();
                return;
            }

            const int32_t n = static_cast<int32_t>(pieces.size());
            for (int32_t k = 0; k < n; ++k) {
                Piece &p = pieces[static_cast<size_t>(k)];
                const bool joined = k + 1 < n && pieces[static_cast<size_t>(k + 1)].start == p.end;
                // The start, unless the piece before already moved it as its join.
                const bool startJoined = k > 0 && pieces[static_cast<size_t>(k - 1)].end == p.start;
                if (p.start > 0 && !startJoined) {
                    const int32_t lo = k > 0 ? pieces[static_cast<size_t>(k - 1)].end + kJoinFadeFrames : kJoinFadeFrames / 2;
                    const int32_t hi = p.end - kJoinFadeFrames;
                    p.start = quietest(p.start, lo, hi, &p, nullptr);
                }
                // A join between words of one take lined up was put just
                // before a word's hit on purpose, so it stays.
                if (joined && pieces[static_cast<size_t>(k + 1)].take == p.take) continue;
                if (p.end < frames) {
                    const int32_t lo = p.start + kJoinFadeFrames;
                    int32_t hi = frames - kJoinFadeFrames / 2;
                    const Piece *other = nullptr;
                    if (k + 1 < n) {
                        const Piece &q = pieces[static_cast<size_t>(k + 1)];
                        hi = joined ? q.end - kJoinFadeFrames : q.start - kJoinFadeFrames;
                        if (joined) other = &q;
                    }
                    p.end = quietest(p.end, lo, hi, &p, other);
                    if (joined) pieces[static_cast<size_t>(k + 1)].start = p.end;
                }
            }
        }

      private:
        /**
         * A blend: every take with a source, each brought to the middle
         * level of the stack, at most 12 dB either way. The level is the
         * RMS over the take's own audio, so a take that's mostly silence
         * still counts the singing.
         */
        void settleBlend() {
            if (takes.empty()) takes.push_back({source, offset});
            if (takes.size() < 2) {
                // Not a blend after all: the pieces of a stack this small
                // could name takes that aren't there, so it plays plain.
                blend = false;
                takes.clear();
                pieces.clear();
                return;
            }
            pieces.clear();
            std::vector<double> levels;
            for (Take &t : takes) {
                if (t.source == nullptr) continue;
                const int32_t a = std::max(t.start, 0);
                const int32_t b = std::min(t.end, frames);
                double e = 0.0;
                int64_t n = 0;
                for (int32_t i = a; i < b; ++i) {
                    const int64_t s = static_cast<int64_t>(t.offset) + i;
                    if (s < 0 || s >= t.source->frames) continue;
                    const double l = t.source->lp[static_cast<size_t>(s)];
                    const double r = t.source->rp[static_cast<size_t>(s)];
                    e += 0.5 * (l * l + r * r);
                    ++n;
                }
                const double rms = n > 0 ? std::sqrt(e / static_cast<double>(n)) : 0.0;
                t.gain = static_cast<float>(rms);
                if (rms > 0.0) levels.push_back(rms);
            }
            settleSpread();
            if (levels.empty()) return;
            std::sort(levels.begin(), levels.end());
            const double middle = levels[levels.size() / 2];
            for (Take &t : takes) {
                const double rms = t.gain;
                t.gain = rms > 0.0 ? static_cast<float>(std::clamp(middle / rms, 0.25, 4.0)) : 1.0f;
            }
        }

        /**
         * Each take's place across the stereo field, and its nudges tidied:
         * sorted, inside the region, overlaps cut, and anything shorter
         * than a fade (a nudge, or a gap between two) folded into its
         * neighbour so a fade never meets another.
         *
         * Equal power, scaled so the middle is 1 and 1: a take keeps its
         * power wherever it sits, so the blend keeps its level.
         */
        void settleSpread() {
            const float sp = spread < 0.0f ? 0.0f : (spread > 1.0f ? 1.0f : spread);
            int32_t count = 0;
            for (const Take &t : takes) count += t.source != nullptr ? 1 : 0;
            int32_t i = 0;
            for (Take &t : takes) {
                t.panL = t.panR = 1.0f;
                if (t.source != nullptr && sp > 0.0f && count > 1) {
                    const float p = sp * (2.0f * static_cast<float>(i) / static_cast<float>(count - 1) - 1.0f);
                    const float th = (p + 1.0f) * 0.78539816f;
                    t.panL = 1.41421356f * std::cos(th);
                    t.panR = 1.41421356f * std::sin(th);
                }
                if (t.source != nullptr) ++i;

                std::vector<Take::Nudge> ns;
                for (Take::Nudge n : t.nudges) {
                    n.start = std::max(n.start, 0);
                    n.end = std::min(n.end, frames);
                    if (n.end > n.start) ns.push_back(n);
                }
                std::stable_sort(ns.begin(), ns.end(),
                                 [](const Take::Nudge &a, const Take::Nudge &b) { return a.start < b.start; });
                std::vector<Take::Nudge> out;
                for (Take::Nudge n : ns) {
                    if (!out.empty() && out.back().end > n.start) n.start = out.back().end;
                    if (n.end - n.start < kJoinFadeFrames) continue;
                    if (!out.empty() && n.start - out.back().end < kJoinFadeFrames) n.start = out.back().end;
                    if (!out.empty() && out.back().end == n.start && out.back().shift == n.shift) {
                        out.back().end = n.end;
                        continue;
                    }
                    out.push_back(n);
                }
                t.nudges = std::move(out);
            }
        }

        /** The energy of piece [p]'s take over the quiet window centred on [at]. */
        double energyNear(const Piece *p, int32_t at) const {
            if (p == nullptr) return 0.0;
            const Take &k = takes[static_cast<size_t>(p->take)];
            double e = 0.0;
            for (int32_t i = at - kJoinQuietFrames / 2; i < at + kJoinQuietFrames / 2; ++i) {
                const int64_t s = static_cast<int64_t>(k.offset) + i - p->shift;
                if (s < 0 || s >= k.source->frames) continue;
                const double l = k.source->lp[static_cast<size_t>(s)];
                const double r = k.source->rp[static_cast<size_t>(s)];
                e += l * l + r * r;
            }
            return e;
        }

        /**
         * The quietest point for a join near [at], within [lo, hi], summed
         * over pieces [a] and [b] (null for none). The nearest wins a tie, so
         * a join in silence stays where it was put.
         */
        int32_t quietest(int32_t at, int32_t lo, int32_t hi, const Piece *a, const Piece *b) const {
            lo = std::max(lo, at - kJoinSearchFrames);
            hi = std::min(hi, at + kJoinSearchFrames);
            if (lo > hi) return at;
            int32_t best = std::clamp(at, lo, hi);
            double bestE = energyNear(a, best) + energyNear(b, best);
            for (int32_t d = kJoinStepFrames; d <= kJoinSearchFrames; d += kJoinStepFrames) {
                for (const int32_t c : {at - d, at + d}) {
                    if (c < lo || c > hi) continue;
                    const double e = energyNear(a, c) + energyNear(b, c);
                    if (e < bestE) {
                        bestE = e;
                        best = c;
                    }
                }
            }
            return best;
        }

      public:
        /** The envelope at [at] frames into the region: 0..1. */
        float fadeAt(int64_t at) const {
            float g = 1.0f;
            if (fadeIn > 0 && at < fadeIn) g = static_cast<float>(at) / static_cast<float>(fadeIn);
            if (fadeOut > 0 && at > frames - fadeOut) {
                const float k = static_cast<float>(frames - at) / static_cast<float>(fadeOut);
                g = g < k ? g : k;
            }
            if (g <= 0.0f) return 0.0f;
            if (g >= 1.0f) return 1.0f;
            // Equal power: the squares of this and its mirror sum to one.
            return std::sin(g * 1.5707963f);
        }
    };

    /** One cell: the lanes that have anything on them. */
    struct Cell {
        int64_t sceneId = 0;
        Region lanes[kReelLanes];
        bool has(int32_t lane) const {
            return lane >= 0 && lane < kReelLanes && lanes[lane].source != nullptr &&
                   lanes[lane].frames > 0;
        }
    };

    std::vector<Cell> cells;

    /** Audio thread. Only a handful of cells, so a linear scan is fine. */
    const Cell *find(int64_t sceneId) const {
        if (sceneId == 0) return nullptr;
        for (const Cell &c : cells) {
            if (c.sceneId == sceneId) return &c;
        }
        return nullptr;
    }
};

/** How long listening through fades in and out at each end of its loop: 10 ms at 48 kHz. */
constexpr int32_t kListenEdgeFrames = 480;

/**
 * Listening through a phrase: one lane played over and over on a loop of
 * [from, to), with a different take over the phrase on each pass. Each pass
 * is the whole lane as it would be with that take kept, settled like any
 * comp, so its joins sound as they will after keeping it.
 *
 * Built on a worker, mounted whole and never changed, like a Reel. Its
 * sources are the reel's own, so it costs no decoding.
 */
struct Listen {
    int32_t lane = 0;
    int32_t from = 0; // region frames: the lead-in starts here
    int32_t to = 0;   // and the phrase ends here
    std::vector<Reel::Region> passes; // in the order they play

    int32_t length() const { return to - from; }
    bool playable() const { return length() > 0 && !passes.empty(); }
};

/**
 * A read position that runs free between blocks and only resyncs when it has
 * drifted more than 256 frames. Recomputing it from the tick every block
 * would step it by a sample or two each time, which clicks. Same idea as
 * `Rack::syncFrozen`.
 */
struct FrameCursor {
    int64_t at = -1;

    void invalidate() { at = -1; }

    void anchor(int64_t target) {
        if (at < 0 || std::llabs(target - at) > 256) at = target;
    }
};

} // namespace acidulous::audio
