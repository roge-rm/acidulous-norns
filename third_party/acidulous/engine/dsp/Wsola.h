#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

// Changes the length of audio without changing its pitch.
//
// The PSOLA in `engine/core/Utterance.h` (used by Molt) is better for voices,
// but it needs glottal pulses and a slow analysis pass. Audio tracks can hold
// anything, so they use WSOLA, which works on any material.
//
// WSOLA lays the output down in overlapping hops. For each hop it searches the
// source near the nominal position for the window that best lines up with
// what's already been written. Without the search, overlap-add at a random
// phase cancels frequencies and warbles.
//
// Reads int16 directly, since that's how a reel holds a take
// (`engine/core/Reel.h`).
namespace acidulous::dsp {

/**
 * Converts a sample type to float.
 *
 * A reel is int16. A frozen clip is stereo float, because it's pre-fader and
 * can go above full scale, so converting it to int16 would clip it.
 */
template <class Sample> struct SampleScale;
template <> struct SampleScale<int16_t> {
    static float of(int16_t v) { return static_cast<float>(v) * (1.0f / 32768.0f); }
};
template <> struct SampleScale<float> {
    static float of(float v) { return v; }
};

/**
 * The search runs once on the sum of all [Channels] and the offset is used
 * for every channel. Two separate mono stretchers would pick different
 * offsets and the stereo image would wander.
 */
template <class Sample, int32_t Channels> class Stretcher {
  public:
    /**
     * The window, hop and maximum search distance.
     *
     * A 30 ms window holds several periods of a low voice for the correlation
     * to lock onto, while keeping transient smearing small. The search goes up
     * to half a hop either way, a full period of anything above 70 Hz.
     */
    static constexpr int32_t kWindow = 1440; // 30 ms at 48k
    static constexpr int32_t kHop = kWindow / 2;
    static constexpr int32_t kSearch = kHop / 2;

    /** The Hann window, built once and shared by every stretcher. */
    static const float *hann() {
        static const std::vector<float> w = [] {
            std::vector<float> v(static_cast<size_t>(kWindow));
            for (int32_t i = 0; i < kWindow; ++i) {
                // Overlapping halves sum to one, so a rate of 1 with no search
                // gives back the input.
                v[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(i) /
                                                                  static_cast<float>(kWindow));
            }
            return v;
        }();
        return w.data();
    }

    void prepare() {
        window = hann();
        for (auto &ch : out) ch.resize(static_cast<size_t>(kWindow + kHop));
        reset();
    }

    /** Start again at [from] frames into the source. */
    void seek(int64_t from) {
        reset();
        readPos = static_cast<double>(from);
        anchor = from;
    }

    void reset() {
        for (auto &ch : out) {
            for (auto &v : ch) v = 0.0f;
        }
        have = 0;
        taken = 0;
        readPos = 0.0;
        anchor = 0;
        primed = false;
        // Clear the search too, or `stepSearch` would correlate against the
        // zeroed `out`.
        searchLag = 1;
        searchReach = 0;
        searchLags = 0;
        searchCredit = 0.0f;
    }

    /** How far into the source the next output sample comes from. */
    int64_t sourcePosition() const { return static_cast<int64_t>(readPos); }

    /**
     * Reads [first, last) as a loop, wrapping past `last` back to `first`.
     * Windows at the seam join the end onto the start like any other hops,
     * so a loop doesn't lose its tail. The source must be longer than two
     * windows.
     */
    void setLoop(bool on) { looping = on; }

    /**
     * Fills [n] output frames from [src], which holds [first, last) of a take.
     *
     * [rate] is how fast the source is used up: 1.0 is real time, 2.0 plays it
     * in half the time at the same pitch. Returns the frames written, fewer
     * than [n] if the source ran out.
     */
    int32_t fill(float *const dst[Channels], const Sample *const src[Channels], int64_t first, int64_t last,
                 int32_t n, float rate) {
        if (window == nullptr || last - first < (looping ? kWindow * 2 : kWindow)) return 0;
        for (int32_t ch = 0; ch < Channels; ++ch) {
            if (src[ch] == nullptr) return 0;
        }
        int32_t made = 0;
        while (made < n) {
            if (taken >= have) {
                if (!lay(src, first, last, rate)) break;
            }
            const int32_t k = have - taken < n - made ? have - taken : n - made;
            for (int32_t ch = 0; ch < Channels; ++ch) {
                for (int32_t i = 0; i < k; ++i) {
                    dst[ch][made + i] = out[ch][static_cast<size_t>(taken + i)];
                }
            }
            made += k;
            taken += k;
            // Do part of the next hop's search, in proportion to what was just
            // output, so it's done by the time the hop is due.
            stepSearch(src, first, last, k);
        }
        return made;
    }

    /** The mono version, used by reels. */
    int32_t fill(float *dst, int32_t n, const Sample *src, int64_t first, int64_t last, float rate) {
        static_assert(Channels == 1, "a stereo stretch needs both channels, or the image wanders");
        float *dsts[1] = {dst};
        const Sample *srcs[1] = {src};
        return fill(dsts, srcs, first, last, n, rate);
    }

  private:
    /**
     * One hop: find where the source joins best, overlap-add it, and shift.
     *
     * The first hop has nothing to join onto, so it goes at the nominal
     * position and searching starts from the second. Call `seek` at a cycle
     * boundary instead of nudging the position, or the search compares
     * against unrelated audio.
     */
    bool lay(const Sample *const src[Channels], int64_t first, int64_t last, float rate) {
        // Shift the overlap down: the tail becomes the head of the next window.
        for (int32_t ch = 0; ch < Channels; ++ch) {
            for (int32_t i = 0; i < kWindow - kHop; ++i) {
                out[ch][static_cast<size_t>(i)] = out[ch][static_cast<size_t>(i + kHop)];
            }
            for (int32_t i = kWindow - kHop; i < kWindow + kHop; ++i) out[ch][static_cast<size_t>(i)] = 0.0f;
        }
        have = kHop;
        taken = 0;

        // The search chooses which samples are copied but never moves readPos.
        // Feeding the offset back into it would let offsets pile up and play
        // at the wrong rate.
        int64_t want = static_cast<int64_t>(readPos);
        if (primed) {
            // Finish whatever's left of the search. Usually nothing, since it
            // was spread over the blocks since the last hop.
            finishSearch(src, first, last);
            want = searchBest;
        }
        if (looping) {
            want = wrapped(want, first, last);
            for (int32_t ch = 0; ch < Channels; ++ch) {
                for (int32_t i = 0; i < kWindow; ++i) {
                    out[ch][static_cast<size_t>(i)] +=
                        SampleScale<Sample>::of(src[ch][wrapped(want + i, first, last)]) * window[i];
                }
            }
        } else {
            if (want < first) want = first;
            if (want + kWindow > last) return false;
            for (int32_t ch = 0; ch < Channels; ++ch) {
                for (int32_t i = 0; i < kWindow; ++i) {
                    out[ch][static_cast<size_t>(i)] += SampleScale<Sample>::of(src[ch][want + i]) * window[i];
                }
            }
        }
        // The output advances by a hop and the source by a hop times the rate.
        // That's the stretch.
        readPos += static_cast<double>(kHop) * static_cast<double>(rate);
        if (looping && readPos >= static_cast<double>(last)) readPos -= static_cast<double>(last - first);
        primed = true;
        // Start the next hop's search now, since its position and the audio it
        // joins onto are both known.
        beginSearch(rate);
        return true;
    }

    /**
     * How far the search needs to look, based on the rate.
     *
     * After the first hop the previous one is already aligned, so the search
     * only has to cover the slip of one hop, `(r - 1) * kHop`, not a whole
     * period. At rates near 1 that's far cheaper. It's capped at [kSearch].
     */
    int32_t searchFor(float rate) const {
        const float slip = std::fabs(rate - 1.0f) * static_cast<float>(kHop);
        const int32_t want = static_cast<int32_t>(slip * 2.0f) + 32;
        return want > kSearch ? kSearch : want;
    }

    /**
     * Starts a search that's spread over the blocks between two hops, so the
     * cost is flat instead of a spike every hop (which several racks in
     * lockstep would stack into one block).
     *
     * It can start as soon as the previous hop is laid. It joins onto the
     * second half of that window, which the next shift brings to the front of
     * `out` (hence the `kHop` offset in scoreLags). Nothing writes `out`
     * between hops.
     */
    void beginSearch(float rate) {
        searchWant = static_cast<int64_t>(readPos);
        searchBest = searchWant;
        searchBestScore = -1e30f;
        searchReach = searchFor(rate);
        searchLag = -searchReach;
        searchCredit = 0.0f;
        // Lags step by four from -reach to +reach, so there are reach / 2 + 1.
        searchLags = searchReach / 2 + 1;
    }

    /** Runs the share of the search that [frames] of output pays for. */
    void stepSearch(const Sample *const src[Channels], int64_t first, int64_t last, int32_t frames) {
        if (searchLag > searchReach) return; // done
        searchCredit += static_cast<float>(searchLags) * static_cast<float>(frames) / static_cast<float>(kHop);
        int32_t lags = static_cast<int32_t>(searchCredit);
        if (lags <= 0) return;
        searchCredit -= static_cast<float>(lags);
        scoreLags(src, first, last, lags);
    }

    /** Runs whatever is left of the search, when the hop is due. */
    void finishSearch(const Sample *const src[Channels], int64_t first, int64_t last) {
        scoreLags(src, first, last, searchLags * 2 + 2);
    }

    void scoreLags(const Sample *const src[Channels], int64_t first, int64_t last, int32_t lags) {
        const int32_t overlap = kWindow - kHop;
        for (int32_t n = 0; n < lags && searchLag <= searchReach; ++n, searchLag += 4) {
            const int64_t at = looping ? wrapped(searchWant + searchLag, first, last) : searchWant + searchLag;
            if (!looping && (at < first || at + kWindow > last)) continue;
            float score = 0.0f;
            // Correlated on the sum of the channels, every fourth sample (four
            // samples is well under an audible phase error). Scale doesn't
            // matter for picking a winner, so the source isn't converted.
            for (int32_t i = 0; i < overlap; i += 4) {
                float written = 0.0f, coming = 0.0f;
                for (int32_t ch = 0; ch < Channels; ++ch) {
                    written += out[ch][static_cast<size_t>(kHop + i)];
                    coming += static_cast<float>(src[ch][looping ? wrapped(at + i, first, last) : at + i]);
                }
                score += written * coming;
            }
            if (score > searchBestScore) {
                searchBestScore = score;
                searchBest = at;
            }
        }
    }

    /** [i] wrapped into [first, last). It's never more than one length outside. */
    static int64_t wrapped(int64_t i, int64_t first, int64_t last) {
        if (i >= last) return i - (last - first);
        if (i < first) return i + (last - first);
        return i;
    }

    bool looping = false;
    const float *window = nullptr;
    std::vector<float> out[Channels];
    int32_t have = 0, taken = 0;
    double readPos = 0.0;
    int64_t anchor = 0;
    bool primed = false;
    int64_t searchWant = 0;
    int64_t searchBest = 0;
    float searchBestScore = -1e30f;
    int32_t searchLag = 1;
    int32_t searchReach = 0;
    int32_t searchLags = 0;
    float searchCredit = 0.0f;
};

/** A reel lane: one channel of int16, used by Bias. */
using Wsola = Stretcher<int16_t, 1>;

/**
 * A frozen clip: stereo float, stretched to follow a tempo it wasn't rendered
 * at, e.g. during a tempo ramp. Much cheaper than falling back to the machine.
 */
using StereoStretch = Stretcher<float, 2>;

} // namespace acidulous::dsp
