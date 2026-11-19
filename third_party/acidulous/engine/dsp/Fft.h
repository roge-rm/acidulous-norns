#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

// A radix-2 complex FFT, iterative and in place. Used to build Cumulus's
// clouds on a worker thread, which only needs one large inverse transform.
// Abacus's borrow runs it on the audio thread a part at a time.
//
// No allocation inside transform() or forwardPart().
namespace acidulous::dsp {

class Fft {
  public:
    /** [size] must be a power of two. */
    explicit Fft(int32_t size) : n(size) {
        rev.resize(static_cast<size_t>(n));
        for (int32_t i = 0, j = 0; i < n; ++i) {
            rev[static_cast<size_t>(i)] = j;
            int32_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j |= bit;
        }
        // Twiddles for every stage, laid out end to end.
        tw.reserve(static_cast<size_t>(n));
        for (int32_t len = 2; len <= n; len <<= 1) {
            for (int32_t k = 0; k < len / 2; ++k) {
                const double a = -2.0 * M_PI * k / len;
                tw.push_back({static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))});
            }
        }
    }

    int32_t size() const { return n; }

    /**
     * In place, [re] and [im] of length size(). `inverse` conjugates and
     * scales by 1/n, so forward-then-inverse is the identity.
     */
    void transform(float *re, float *im, bool inverse) const {
        if (inverse) {
            for (int32_t i = 0; i < n; ++i) im[i] = -im[i];
        }
        for (int32_t i = 0; i < n; ++i) {
            const int32_t j = rev[static_cast<size_t>(i)];
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        size_t base = 0;
        for (int32_t len = 2; len <= n; len <<= 1) {
            const int32_t half = len / 2;
            for (int32_t i = 0; i < n; i += len) {
                for (int32_t k = 0; k < half; ++k) {
                    const Twiddle &w = tw[base + static_cast<size_t>(k)];
                    const int32_t a = i + k, b = a + half;
                    const float xr = re[b] * w.c - im[b] * w.s;
                    const float xi = re[b] * w.s + im[b] * w.c;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
            }
            base += static_cast<size_t>(half);
        }
        if (inverse) {
            const float scale = 1.0f / static_cast<float>(n);
            for (int32_t i = 0; i < n; ++i) {
                re[i] *= scale;
                im[i] *= -scale;
            }
        }
    }

    /**
     * The forward transform in [parts] calls, so it can be spread over
     * audio blocks: call with part 0 to parts - 1 in order. The same sums as
     * transform(re, im, false).
     */
    void forwardPart(float *re, float *im, int part, int parts) const {
        if (part == 0) {
            for (int32_t i = 0; i < n; ++i) {
                const int32_t j = rev[static_cast<size_t>(i)];
                if (i < j) {
                    std::swap(re[i], re[j]);
                    std::swap(im[i], im[j]);
                }
            }
        }
        int passes = 0;
        while ((2 << passes) <= n) ++passes;
        const int from = passes * part / parts, to = passes * (part + 1) / parts;
        for (int p = from; p < to; ++p) {
            const int32_t half = 1 << p, len = half * 2;
            // Each pass's twiddles start after the ones before: 1 + 2 + ... + half / 2.
            const size_t base = static_cast<size_t>(half - 1);
            for (int32_t i = 0; i < n; i += len) {
                for (int32_t k = 0; k < half; ++k) {
                    const Twiddle &w = tw[base + static_cast<size_t>(k)];
                    const int32_t a = i + k, b = a + half;
                    const float xr = re[b] * w.c - im[b] * w.s;
                    const float xi = re[b] * w.s + im[b] * w.c;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
            }
        }
    }

  private:
    struct Twiddle { float c, s; };
    int32_t n;
    std::vector<int32_t> rev;
    std::vector<Twiddle> tw;
};

} // namespace acidulous::dsp
