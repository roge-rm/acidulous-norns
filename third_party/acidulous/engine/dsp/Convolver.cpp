#include "Convolver.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace acidulous::dsp {

// Clang won't vectorise float loops for 32-bit ARM on its own, since NEON
// flushes denormals; we flush them anyway, so these loops ask for it.
#if defined(__clang__)
#define VECTORISE _Pragma("clang loop vectorize(enable)")
#else
#define VECTORISE
#endif

namespace {
// A job's phases, in order, and what a unit of each costs relative to the
// others, measured on a Cortex-A53, so the shares come out even.
enum Phase { Gather, Forward, Unpack, Mac, Pack, Inverse, Output, kPhases };
constexpr int64_t kWeight[kPhases] = {2, 3, 3, 4, 3, 3, 3};

// Bins per run of the multiply-accumulate.
constexpr int32_t kMacBins = 256;

// Calls f(slot, p, q) for slots [from, to) of a half spectrum of P + 1 bins.
// The forward transform leaves bin k and its mirror N - k at bit-reversed
// positions p and q. Slot 0 holds bin 0 alone (p = q = 0), slot P bin N/2
// alone (p = q = 1), and slot s, with b the highest power of two in s, the
// pair at s + b and 5b - 1 - s, which mirror each other within one block of
// positions. A run of slots so reads one stream forwards and one backwards.
template <typename F>
inline void forSlots(int32_t P, int64_t from, int64_t to, F &&f) {
    auto s = static_cast<int32_t>(from);
    while (s < to) {
        if (s == 0 || s == P) {
            const int32_t p = s == 0 ? 0 : 1;
            f(s, p, p);
            ++s;
            continue;
        }
        int32_t b = 1;
        while (2 * b <= s) b *= 2;
        const auto end = static_cast<int32_t>(std::min<int64_t>(to, 2 * b));
        for (; s < end; ++s) f(s, s + b, 5 * b - 1 - s);
    }
}

int32_t log2Of(int32_t v) {
    int32_t b = 0;
    while ((1 << b) < v) ++b;
    return b;
}
int32_t pow2AtLeast(int32_t v) { return 1 << log2Of(v); }
} // namespace

Convolver::Convolver(const float *left, const float *right, int32_t frames, int32_t largest)
    : frames_(std::max(frames, 0)), stereo_(right != nullptr) {
    largest = std::max(kGrid, largest);
    directTaps_ = std::min(frames_, kGrid);
    for (int32_t k = 0; k < directTaps_; ++k) {
        direct_[0][kGrid - 1 - k] = left[k];
        direct_[1][kGrid - 1 - k] = right != nullptr ? right[k] : left[k];
    }

    int32_t biggest = kGrid;
    for (int32_t size = kGrid, offset = kGrid; offset < frames_; size *= 8) {
        const bool last = size * 8 > largest;
        // The next size starts two of its partitions in, which gives its
        // work a whole partition of time to be spread over.
        const int64_t end = last ? INT64_MAX : 2LL * size * 8;
        const int64_t upTo = std::min<int64_t>(end, frames_);
        Stage s;
        s.size = size;
        s.fft = size * 2;
        s.bits = log2Of(s.fft);
        s.offset = offset;
        s.parts = static_cast<int32_t>((upTo - offset + size - 1) / size);
        s.spread = offset >= 2 * size;
        const int32_t bins = size + 1;
        s.hStride = (stereo_ ? 4 : 2) * bins;
        s.h.assign(static_cast<size_t>(s.parts) * s.hStride, 0.0f);
        s.fdl.assign(static_cast<size_t>(s.parts) * 4 * bins, 0.0f);
        s.re.assign(static_cast<size_t>(s.fft), 0.0f);
        s.im.assign(static_cast<size_t>(s.fft), 0.0f);
        s.acc.assign(static_cast<size_t>(4 * bins), 0.0f);
        s.twc.resize(static_cast<size_t>(s.fft));
        s.tws.resize(static_cast<size_t>(s.fft));
        for (int32_t len = 2; len <= s.fft; len <<= 1) {
            for (int32_t k = 0; k < len / 2; ++k) {
                const double a = -2.0 * M_PI * k / len;
                s.twc[static_cast<size_t>(len / 2 - 1 + k)] = static_cast<float>(std::cos(a));
                s.tws[static_cast<size_t>(len / 2 - 1 + k)] = static_cast<float>(std::sin(a));
            }
        }

        // Each partition's spectrum, scaled by 1/2 for the unpacking and 1/N
        // for the inverse transform, so neither costs a multiply later.
        const float scale = 0.25f / static_cast<float>(s.fft);
        for (int32_t j = 0; j < s.parts; ++j) {
            std::fill(s.re.begin(), s.re.end(), 0.0f);
            std::fill(s.im.begin(), s.im.end(), 0.0f);
            const int32_t from = offset + j * size;
            const int32_t to = std::min(frames_, from + size);
            for (int32_t t = from; t < to; ++t) {
                s.re[static_cast<size_t>(t - from)] = left[t];
                if (right != nullptr) s.im[static_cast<size_t>(t - from)] = right[t];
            }
            forward(s, 0, static_cast<int64_t>(s.fft / 2) * s.bits);
            float *h = s.h.data() + static_cast<size_t>(j) * s.hStride;
            const int32_t step = stereo_ ? 4 : 2;
            forSlots(size, 0, bins, [&](int32_t k, int32_t pk, int32_t pn) {
                const float zr = s.re[static_cast<size_t>(pk)], zi = s.im[static_cast<size_t>(pk)];
                const float cr = s.re[static_cast<size_t>(pn)], ci = -s.im[static_cast<size_t>(pn)];
                float *hk = h + static_cast<size_t>(k) * step;
                hk[0] = (zr + cr) * scale;
                hk[1] = (zi + ci) * scale;
                if (stereo_) {
                    hk[2] = (zi - ci) * scale;
                    hk[3] = -(zr - cr) * scale;
                }
            });
        }
        biggest = size;
        stages_.push_back(std::move(s));
        if (last) break;
        offset = static_cast<int32_t>(end);
    }

    const int32_t inSize = pow2AtLeast(4 * biggest);
    const int32_t outSize = pow2AtLeast(2 * biggest);
    for (int c = 0; c < 2; ++c) {
        in_[c].assign(static_cast<size_t>(inSize), 0.0f);
        out_[c].assign(static_cast<size_t>(outSize), 0.0f);
    }
    inMask_ = inSize - 1;
    outMask_ = outSize - 1;
    reset();
}

void Convolver::reset() {
    n_ = 0;
    std::memset(hist_, 0, sizeof(hist_));
    for (auto &o : out_) std::fill(o.begin(), o.end(), 0.0f);
    // The input ring and the spectra aren't cleared: a gather reads anything
    // from before the reset as 0, and only slots written since are summed.
    for (auto &s : stages_) {
        s.head = 0;
        s.filled = 0;
        s.busy = false;
        s.phase = 0;
        s.index = 0;
        s.done = s.total = 0;
        s.share = 0;
    }
}

void Convolver::process(const float *inL, const float *inR, float *outL, float *outR, int32_t frames) {
    for (int32_t i = 0; i < frames; ++i) {
        const float xl = inL[i], xr = inR[i];
        const int32_t w = static_cast<int32_t>(n_ & inMask_);
        in_[0][static_cast<size_t>(w)] = xl;
        in_[1][static_cast<size_t>(w)] = xr;
        const int32_t pos = static_cast<int32_t>(n_ & (kGrid - 1));
        hist_[0][pos] = hist_[0][pos + kGrid] = xl;
        hist_[1][pos] = hist_[1][pos + kGrid] = xr;
        const float *hl = hist_[0] + pos + 1, *hr = hist_[1] + pos + 1;
        float yl = 0.0f, yr = 0.0f;
        VECTORISE
        for (int32_t k = 0; k < kGrid; ++k) {
            yl += direct_[0][k] * hl[k];
            yr += direct_[1][k] * hr[k];
        }
        const auto o = static_cast<size_t>(n_ & outMask_);
        yl += out_[0][o];
        yr += out_[1][o];
        out_[0][o] = 0.0f;
        out_[1][o] = 0.0f;
        outL[i] = yl;
        outR[i] = yr;
        ++n_;
        if ((n_ & (kGrid - 1)) == 0) tick();
    }
}

void Convolver::tick() {
    for (auto &s : stages_) {
        if (s.busy) {
            const int32_t shares = s.size / kGrid;
            ++s.share;
            advance(s, s.total * s.share / shares);
            if (s.share >= shares) s.busy = false;
        }
        if (n_ % s.size == 0) {
            begin(s);
            if (s.spread) {
                s.busy = true;
            } else {
                advance(s, s.total);
            }
        }
    }
}

void Convolver::begin(Stage &s) {
    s.start = n_;
    s.head = s.head + 1 >= s.parts ? 0 : s.head + 1;
    if (s.filled < s.parts) ++s.filled;
    s.activeParts = s.filled;
    s.phase = 0;
    s.index = 0;
    s.done = 0;
    s.share = 0;
    s.total = 0;
    for (int32_t p = 0; p < kPhases; ++p) s.total += phaseUnits(s, p) * kWeight[p];
}

int64_t Convolver::phaseUnits(const Stage &s, int32_t phase) const {
    switch (phase) {
    case Gather: return s.fft;
    case Forward:
    case Inverse: return static_cast<int64_t>(s.fft / 2) * s.bits;
    case Unpack:
    case Pack: return s.size + 1;
    case Mac: return static_cast<int64_t>(s.activeParts) * (s.size + 1);
    case Output: return s.size;
    default: return 0;
    }
}

void Convolver::advance(Stage &s, int64_t target) {
    while (s.done < target && s.phase < kPhases) {
        const int64_t left = phaseUnits(s, s.phase) - s.index;
        if (left <= 0) {
            ++s.phase;
            s.index = 0;
            continue;
        }
        const int64_t w = kWeight[s.phase];
        const int64_t n = std::min(left, (target - s.done + w - 1) / w);
        runPhase(s, s.phase, s.index, s.index + n);
        s.index += n;
        s.done += n * w;
    }
}

namespace {
// One run of butterflies at k0..k1 of a group, the halves at a and c.
inline void difRun(float *__restrict ar, float *__restrict ai, float *__restrict cr, float *__restrict ci,
                   const float *__restrict wc, const float *__restrict ws, int32_t k0, int32_t k1) {
    VECTORISE
    for (int32_t k = k0; k < k1; ++k) {
        const float xr = ar[k], xi = ai[k], yr = cr[k], yi = ci[k];
        const float dr = xr - yr, di = xi - yi;
        ar[k] = xr + yr;
        ai[k] = xi + yi;
        cr[k] = dr * wc[k] - di * ws[k];
        ci[k] = dr * ws[k] + di * wc[k];
    }
}
inline void ditRun(float *__restrict ar, float *__restrict ai, float *__restrict cr, float *__restrict ci,
                   const float *__restrict wc, const float *__restrict ws, int32_t k0, int32_t k1) {
    VECTORISE
    for (int32_t k = k0; k < k1; ++k) {
        const float yr = cr[k] * wc[k] - ci[k] * ws[k];
        const float yi = cr[k] * ws[k] + ci[k] * wc[k];
        const float xr = ar[k], xi = ai[k];
        cr[k] = xr - yr;
        ci[k] = xi - yi;
        ar[k] = xr + yr;
        ai[k] = xi + yi;
    }
}

// Butterflies [b, endB) of one pass, a group at a time so the inner loop
// runs over contiguous memory. [shift] is log2 of [half].
template <bool kDif>
inline void pass(float *re, float *im, const float *twc, const float *tws, int32_t half, int32_t shift, int32_t b,
                 int32_t endB) {
    const float *wc = twc + half - 1, *ws = tws + half - 1;
    if (half == 1) {
        // Span 2, a twiddle of 1: the pairs run on without groups.
        for (int32_t g = b; g < endB; ++g) {
            const float xr = re[2 * g], xi = im[2 * g], yr = re[2 * g + 1], yi = im[2 * g + 1];
            re[2 * g] = xr + yr;
            im[2 * g] = xi + yi;
            re[2 * g + 1] = xr - yr;
            im[2 * g + 1] = xi - yi;
        }
        return;
    }
    while (b < endB) {
        const int32_t k0 = b & (half - 1);
        const int32_t k1 = std::min(half, k0 + (endB - b));
        const int32_t base = (b >> shift) << (shift + 1);
        float *ar = re + base, *ai = im + base;
        if (kDif) {
            difRun(ar, ai, ar + half, ai + half, wc, ws, k0, k1);
        } else {
            ditRun(ar, ai, ar + half, ai + half, wc, ws, k0, k1);
        }
        b += k1 - k0;
    }
}

// Partition [j]'s product into the accumulators. [acc] and [x] hold left re,
// left im, right re, right im for each bin in turn; [h] the same, or only the
// left pair for a mono impulse.
template <bool kFirst, bool kStereo>
inline void macRun(float *__restrict acc, const float *__restrict x, const float *__restrict h, int32_t k0,
                   int32_t k1) {
    constexpr int32_t hs = kStereo ? 4 : 2, hr = kStereo ? 2 : 0;
    VECTORISE
    for (int32_t k = k0; k < k1; ++k) {
        const float ar = x[4 * k], ai = x[4 * k + 1], br = x[4 * k + 2], bi = x[4 * k + 3];
        const float hlr = h[hs * k], hli = h[hs * k + 1], hrr = h[hs * k + hr], hri = h[hs * k + hr + 1];
        const float pl = ar * hlr - ai * hli;
        const float ql = ar * hli + ai * hlr;
        const float pr = br * hrr - bi * hri;
        const float qr = br * hri + bi * hrr;
        if (kFirst) {
            acc[4 * k] = pl;
            acc[4 * k + 1] = ql;
            acc[4 * k + 2] = pr;
            acc[4 * k + 3] = qr;
        } else {
            acc[4 * k] += pl;
            acc[4 * k + 1] += ql;
            acc[4 * k + 2] += pr;
            acc[4 * k + 3] += qr;
        }
    }
}
} // namespace

void Convolver::forward(Stage &s, int64_t from, int64_t to) {
    const int32_t halfN = s.fft / 2;
    int64_t u = from;
    while (u < to) {
        const auto p = static_cast<int32_t>(u / halfN);
        const auto b = static_cast<int32_t>(u % halfN);
        const auto endB = static_cast<int32_t>(std::min<int64_t>(halfN, b + (to - u)));
        // Spans from N down to 2.
        const int32_t shift = s.bits - 1 - p;
        pass<true>(s.re.data(), s.im.data(), s.twc.data(), s.tws.data(), 1 << shift, shift, b, endB);
        u += endB - b;
    }
}

void Convolver::inverse(Stage &s, int64_t from, int64_t to) {
    const int32_t halfN = s.fft / 2;
    int64_t u = from;
    while (u < to) {
        const auto p = static_cast<int32_t>(u / halfN);
        const auto b = static_cast<int32_t>(u % halfN);
        const auto endB = static_cast<int32_t>(std::min<int64_t>(halfN, b + (to - u)));
        // Spans from 2 up to N.
        pass<false>(s.re.data(), s.im.data(), s.twc.data(), s.tws.data(), 1 << p, p, b, endB);
        u += endB - b;
    }
}

void Convolver::runPhase(Stage &s, int32_t phase, int64_t from, int64_t to) {
    const int32_t P = s.size, bins = P + 1;
    float *re = s.re.data(), *im = s.im.data();
    switch (phase) {
    case Gather: {
        const int64_t first = s.start - 2LL * P;
        for (int64_t u = from; u < to; ++u) {
            const int64_t t = first + u;
            const bool before = t < 0; // before the reset
            const auto r = static_cast<size_t>(t & inMask_);
            re[u] = before ? 0.0f : in_[0][r];
            im[u] = before ? 0.0f : in_[1][r];
        }
        break;
    }
    case Forward:
        forward(s, from, to);
        break;
    case Inverse:
        inverse(s, from, to);
        break;
    case Unpack: {
        // Two real spectra out of one complex one: A is 2X(left), B 2X(right).
        float *slot = s.fdl.data() + static_cast<size_t>(s.head) * 4 * bins;
        forSlots(P, from, to, [=](int32_t k, int32_t pk, int32_t pn) {
            const float zr = re[pk], zi = im[pk], cr = re[pn], ci = -im[pn];
            float *sk = slot + 4 * k;
            sk[0] = zr + cr;
            sk[1] = zi + ci;
            sk[2] = zi - ci;
            sk[3] = -(zr - cr);
        });
        break;
    }
    case Mac: {
        // In runs of kMacBins bins, every partition for one run before the
        // next, so the run's accumulators stay in cache.
        const int64_t parts = s.activeParts;
        int64_t u = from;
        while (u < to) {
            const auto run = static_cast<int32_t>(u / (parts * kMacBins));
            const int32_t k0 = run * kMacBins;
            const int32_t width = std::min(kMacBins, bins - k0);
            const int64_t r = u - parts * k0;
            const auto j = static_cast<int32_t>(r / width);
            int32_t k = k0 + static_cast<int32_t>(r % width);
            const int32_t endK = static_cast<int32_t>(std::min<int64_t>(k0 + width, k + (to - u)));
            u += endK - k;
            int32_t slotIndex = s.head - j;
            if (slotIndex < 0) slotIndex += s.parts;
            const float *x = s.fdl.data() + static_cast<size_t>(slotIndex) * 4 * bins;
            const float *h = s.h.data() + static_cast<size_t>(j) * s.hStride;
            float *acc = s.acc.data();
            if (j == 0) {
                if (stereo_) macRun<true, true>(acc, x, h, k, endK);
                else macRun<true, false>(acc, x, h, k, endK);
            } else {
                if (stereo_) macRun<false, true>(acc, x, h, k, endK);
                else macRun<false, false>(acc, x, h, k, endK);
            }
        }
        break;
    }
    case Pack: {
        // conj(L + iR) on the way back, so the forward transform inverts it.
        const float *acc = s.acc.data();
        forSlots(P, from, to, [=](int32_t k, int32_t pk, int32_t pn) {
            const float *ak = acc + 4 * k;
            const float ar = ak[0], ai = ak[1], br = ak[2], bi = ak[3];
            re[pk] = ar - bi;
            im[pk] = -ai - br;
            if (pn != pk) {
                re[pn] = ar + bi;
                im[pn] = ai - br;
            }
        });
        break;
    }
    case Output: {
        const int64_t first = s.start - P + s.offset;
        for (int64_t u = from; u < to; ++u) {
            const auto o = static_cast<size_t>((first + u) & outMask_);
            out_[0][o] += re[P + u];
            out_[1][o] -= im[P + u];
        }
        break;
    }
    default:
        break;
    }
}

} // namespace acidulous::dsp
