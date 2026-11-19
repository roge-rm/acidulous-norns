#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// The sine partials of one Abacus voice and the small maths they need.
//
// Each partial is a complex rotator: a unit vector z turned by w = e^(i theta)
// every sample, heard as z's imaginary part. The arrays are laid out
// partial by partial (structure of arrays) so four partials run side by side
// in one vector register; the loops below are written for the compiler to
// vectorise. Audio thread only, no allocation.
namespace acidulous::machine::abacus {

constexpr int kMaxPartials = 64;
constexpr int kGroups = kMaxPartials / 4;

/** Four floats in one register (the compiler's vector extension, NEON or SSE). */
typedef float F4 __attribute__((vector_size(16)));
inline F4 load4(const float *p) {
    F4 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline void store4(float *p, F4 v) { std::memcpy(p, &v, sizeof(v)); }
inline F4 splat(float x) { return F4{x, x, x, x}; }
typedef int32_t I4 __attribute__((vector_size(16)));
/** [a] where [mask] is set, else [b]; [mask] from a vector compare. */
inline F4 pick(I4 mask, F4 a, F4 b) { return (F4)(((I4)a & mask) | ((I4)b & ~mask)); }
inline float sum4(F4 v) { return (v[0] + v[1]) + (v[2] + v[3]); }

/** exp2Fast four at a time, with no floor instruction (armv7 has none). */
inline F4 exp2v(F4 x) {
    x = pick(x < splat(-60.0f), splat(-60.0f), x);
    x = pick(x > splat(30.0f), splat(30.0f), x);
    // Truncation of a positive number is its floor.
    const I4 xi = __builtin_convertvector(x + splat(64.0f), I4) - 64;
    const F4 f = x - __builtin_convertvector(xi, F4);
    const F4 p = splat(1.0f) + f * (splat(0.69314718f) + f * (splat(0.24022651f) + f * (splat(0.05550411f) +
                 f * (splat(0.00961813f) + f * (splat(0.00133336f) + f * splat(0.00015404f))))));
    return p * (F4)((xi + 127) << 23);
}

/** log2 four at a time, good to about 4e-6 with no divide; anything under 2^-100 reads as -100. */
inline F4 log2v(F4 x) {
    x = pick(x < splat(7.8886e-31f), splat(7.8886e-31f), x);
    const I4 bits = (I4)x;
    F4 e = __builtin_convertvector(((bits >> 23) & 0xff) - 127, F4);
    // The mantissa in [1, 2), folded to [0.71, 1.41) so the polynomial in
    // m - 1 stays short.
    F4 m = (F4)((bits & 0x007fffff) | 0x3f800000);
    const I4 big = m > splat(1.41421356f);
    m = pick(big, m * splat(0.5f), m);
    e = pick(big, e + splat(1.0f), e);
    const F4 t = m - splat(1.0f);
    const F4 p = splat(1.4427018f) + t * (splat(-0.721208443f) + t * (splat(0.479793639f) + t * (splat(-0.36641372f) +
                 t * (splat(0.318410576f) + t * splat(-0.20686046f)))));
    return e + t * p;
}

/** rotation() four at a time. */
inline void rotationv(F4 theta, F4 &c, F4 &s) {
    const F4 h = theta * splat(0.5f);
    const F4 h2 = h * h;
    const F4 sh = h * (splat(1.0f) + h2 * (splat(-1.0f / 6.0f) + h2 * (splat(1.0f / 120.0f) + h2 * (splat(-1.0f / 5040.0f) +
                  h2 * (splat(1.0f / 362880.0f) + h2 * splat(-1.0f / 39916800.0f))))));
    const F4 ch = splat(1.0f) + h2 * (splat(-0.5f) + h2 * (splat(1.0f / 24.0f) + h2 * (splat(-1.0f / 720.0f) +
                  h2 * (splat(1.0f / 40320.0f) + h2 * (splat(-1.0f / 3628800.0f) + h2 * splat(1.0f / 479001600.0f))))));
    c = splat(1.0f) - splat(2.0f) * sh * sh;
    s = splat(2.0f) * sh * ch;
}

/** 2^x, good to about 2e-5 relative, for gains. Under 2^-60 reads as 2^-60; callers zero what they want silent. */
inline float exp2Fast(float x) {
    x = x < -60.0f ? -60.0f : (x > 30.0f ? 30.0f : x);
    const float fl = std::floor(x);
    const float f = x - fl;
    // Taylor series of e^(f ln 2) to the sixth power, f in [0, 1).
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
    const int32_t bits = (static_cast<int32_t>(fl) + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof(scale));
    return p * scale;
}

/**
 * The rotation for [theta] radians a sample, 0 to under pi: cos into [c],
 * sin into [s]. Worked from the half angle (a Taylor series, under 2e-8 out
 * up to pi / 2) and doubled, which keeps cos exact near 1 for low notes.
 */
inline void rotation(float theta, float &c, float &s) {
    const float h = theta * 0.5f;
    const float h2 = h * h;
    const float sh = h * (1.0f + h2 * (-1.0f / 6.0f + h2 * (1.0f / 120.0f + h2 * (-1.0f / 5040.0f + h2 * (1.0f / 362880.0f + h2 * (-1.0f / 39916800.0f))))));
    const float ch = 1.0f + h2 * (-0.5f + h2 * (1.0f / 24.0f + h2 * (-1.0f / 720.0f + h2 * (1.0f / 40320.0f + h2 * (-1.0f / 3628800.0f + h2 * (1.0f / 479001600.0f))))));
    c = 1.0f - 2.0f * sh * sh;
    s = 2.0f * sh * ch;
}

/** One voice's rotators and the gains they are heard at, left and right, at the end of the last block. */
struct PartialBank {
    alignas(16) float zr[kMaxPartials];
    alignas(16) float zi[kMaxPartials];
    alignas(16) float wr[kMaxPartials];
    alignas(16) float wi[kMaxPartials];
    alignas(16) float gl[kMaxPartials];
    alignas(16) float gr[kMaxPartials];

    PartialBank() { clear(); }
    /** Every rotator at phase 0 (heard as 0) and every gain 0. */
    void clear() {
        for (int n = 0; n < kMaxPartials; ++n) {
            zr[n] = 1.0f;
            zi[n] = 0.0f;
            wr[n] = 1.0f;
            wi[n] = 0.0f;
            gl[n] = gr[n] = 0.0f;
        }
    }
    /** Whether group [g] of four is heard at either end of a block heading to gains [tl], [tr]. */
    bool live(int g, const float *tl, const float *tr) const {
        const int b = g * 4;
        for (int k = 0; k < 4; ++k)
            if (gl[b + k] != 0.0f || gr[b + k] != 0.0f || tl[b + k] != 0.0f || tr[b + k] != 0.0f) return true;
        return false;
    }

    /**
     * Runs four partials from [first] (a multiple of 4), or eight when
     * [eight], for [frames] samples, their gains ramping in a straight line
     * from where they are to [tl] and [tr], reaching them on the last sample.
     * Adds lane by lane into [accL] and [accR], four floats a frame, which
     * the caller sums. Then puts the rotators back on the unit circle.
     */
    void run(int first, bool eight, const float *tl, const float *tr, float *accL, float *accR, int32_t frames) {
        const F4 inv = splat(1.0f / static_cast<float>(frames));
        const int n0 = first, n1 = first + 4;
        F4 r0 = load4(zr + n0), i0 = load4(zi + n0), c0 = load4(wr + n0), s0 = load4(wi + n0);
        F4 a0 = load4(gl + n0), b0 = load4(gr + n0);
        const F4 da0 = (load4(tl + n0) - a0) * inv, db0 = (load4(tr + n0) - b0) * inv;
        if (eight) {
            F4 r1 = load4(zr + n1), i1 = load4(zi + n1), c1 = load4(wr + n1), s1 = load4(wi + n1);
            F4 a1 = load4(gl + n1), b1 = load4(gr + n1);
            const F4 da1 = (load4(tl + n1) - a1) * inv, db1 = (load4(tr + n1) - b1) * inv;
            for (int32_t i = 0; i < frames; ++i) {
                const F4 x0 = r0 * c0 - i0 * s0, y0 = r0 * s0 + i0 * c0;
                const F4 x1 = r1 * c1 - i1 * s1, y1 = r1 * s1 + i1 * c1;
                r0 = x0;
                i0 = y0;
                r1 = x1;
                i1 = y1;
                a0 += da0;
                b0 += db0;
                a1 += da1;
                b1 += db1;
                store4(accL + i * 4, load4(accL + i * 4) + a0 * y0 + a1 * y1);
                store4(accR + i * 4, load4(accR + i * 4) + b0 * y0 + b1 * y1);
            }
            settle(n1, r1, i1, tl, tr);
        } else {
            for (int32_t i = 0; i < frames; ++i) {
                const F4 x0 = r0 * c0 - i0 * s0, y0 = r0 * s0 + i0 * c0;
                r0 = x0;
                i0 = y0;
                a0 += da0;
                b0 += db0;
                store4(accL + i * 4, load4(accL + i * 4) + a0 * y0);
                store4(accR + i * 4, load4(accR + i * 4) + b0 * y0);
            }
        }
        settle(n0, r0, i0, tl, tr);
    }

  private:
    /** Stores four rotators one Newton step toward length 1 (the drift in a block is tiny), and the gains reached. */
    void settle(int n, F4 r, F4 i, const float *tl, const float *tr) {
        const F4 m = splat(1.5f) - splat(0.5f) * (r * r + i * i);
        store4(zr + n, r * m);
        store4(zi + n, i * m);
        store4(gl + n, load4(tl + n));
        store4(gr + n, load4(tr + n));
    }
};

} // namespace acidulous::machine::abacus
