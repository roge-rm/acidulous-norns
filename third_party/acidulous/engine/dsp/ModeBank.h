#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace acidulous::dsp {

/**
 * A bank of up to N two-pole resonators on one input, each heard at its own
 * weight in a left and a right output.
 *
 * Each mode's input gain is set from its bandwidth so white noise comes out
 * at the level it went in, whatever the decay: a longer ring is a narrower
 * band and takes less in, so a 20 s mode is no louder on a full mix than a
 * 0.2 s one.
 *
 * setMode() sets targets; process() glides every coefficient from where it
 * was to its target over a fixed 5 ms, carried across calls, so a retune or
 * a new material doesn't click however finely the block is sliced. Linear
 * steps between two stable pole pairs stay stable, as the stable (a1, a2)
 * region is convex.
 *
 * Each mode keeps its last output y and its last step v = y1 - y2, and runs
 * v += e y - c2 v + b x, y += v, with e = a1 - 1 - r^2 and c2 = 1 - r^2.
 * That is y = b x + a1 y1 - r^2 y2 with the coefficients kept as their small
 * distances from 2 and 1: near DC a1 is within a few millionths of 2, where a
 * float a1 would be off by 15 cents at 20 Hz. It also holds up under
 * -ffast-math, which may fold 2 y1 + c y1 back into (2 + c) y1.
 *
 * Not thread safe: set and process on the audio thread.
 */
template <int N>
class ModeBank {
    static_assert(N % 4 == 0, "modes are summed four at a time");

  public:
    static constexpr int kMax = N;
    /** How long a retune glides, seconds. */
    static constexpr double kGlideSeconds = 0.005;

    /**
     * Mode [k] at [hz] ringing for [t60] seconds (to -60 dB), heard at
     * [left] and [right]. A mode set silent with off() fades out instead.
     */
    void setMode(int k, double hz, double t60, float left, float right, double sampleRate) {
        const double r = std::pow(10.0, -3.0 / (std::fmax(t60, 0.002) * sampleRate));
        const double w = 6.283185307179586 * hz / sampleRate;
        const double a1 = 2.0 * r * std::cos(w), r2 = r * r;
        // White noise power out of y = b x + a1 y1 - r2 y2 is
        // b^2 (1 + r2) / ((1 - r2) ((1 + r2)^2 - a1^2)); this b makes it 1.
        const double den = (1.0 - r2) * ((1.0 + r2) * (1.0 + r2) - a1 * a1) / (1.0 + r2);
        tc1[k] = static_cast<float>(a1 - 1.0 - r2);
        tc2[k] = static_cast<float>(1.0 - r2);
        tb[k] = static_cast<float>(std::sqrt(std::fmax(den, 0.0)));
        tl[k] = left;
        tr[k] = right;
        if (k >= used) used = k + 1;
        glideFrames = std::max(1, static_cast<int>(kGlideSeconds * sampleRate + 0.5));
        retarget = true;
    }
    /** Mode [k] fades out over the glide and then stops. */
    void off(int k) {
        tb[k] = 0.0f;
        tl[k] = 0.0f;
        tr[k] = 0.0f;
        retarget = true;
    }

    /** Every coefficient at its target now, with no glide: for the first build after a clear. */
    void settle() {
        for (int k = 0; k < N; ++k) {
            c1[k] = tc1[k];
            c2[k] = tc2[k];
            b[k] = tb[k];
            l[k] = tl[k];
            r[k] = tr[k];
        }
        glideLeft = 0;
        retarget = false;
    }

    /** Writes the bank's output for [frames] of [in] to [outL] and [outR]. */
    void process(const float *in, float *outL, float *outR, int32_t frames) {
        if (frames <= 0) return;
        const int n = (used + 3) & ~3;
        if (retarget) {
            // New targets: glide to them from wherever the last glide had got to.
            retarget = false;
            bool moving = false;
            for (int k = 0; k < n && !moving; ++k)
                moving = c1[k] != tc1[k] || c2[k] != tc2[k] || b[k] != tb[k] || l[k] != tl[k] || r[k] != tr[k];
            glideLeft = moving ? glideFrames : 0;
            const float inv = 1.0f / static_cast<float>(glideFrames);
            for (int k = 0; k < n; ++k) {
                dc1[k] = (tc1[k] - c1[k]) * inv;
                dc2[k] = (tc2[k] - c2[k]) * inv;
                db[k] = (tb[k] - b[k]) * inv;
                dl[k] = (tl[k] - l[k]) * inv;
                dr[k] = (tr[k] - r[k]) * inv;
            }
        }
        int32_t i = 0;
        if (glideLeft > 0) {
            const int32_t g = std::min<int32_t>(frames, glideLeft);
            for (; i < g; ++i) {
                for (int k = 0; k < n; ++k) {
                    c1[k] += dc1[k];
                    c2[k] += dc2[k];
                    b[k] += db[k];
                    l[k] += dl[k];
                    r[k] += dr[k];
                }
                frame(in[i], outL[i], outR[i], n);
            }
            glideLeft -= g;
            // Land on the targets exactly, whatever the rounding on the way.
            if (glideLeft == 0) {
                for (int k = 0; k < n; ++k) {
                    c1[k] = tc1[k];
                    c2[k] = tc2[k];
                    b[k] = tb[k];
                    l[k] = tl[k];
                    r[k] = tr[k];
                }
            }
        }
        for (; i < frames; ++i) frame(in[i], outL[i], outR[i], n);
        // Tails that have gone under hearing are zeroed, so they never reach
        // denormals where a thread has no flush-to-zero (the web build), and
        // modes switched off and faded stop costing anything.
        int top = 0;
        for (int k = 0; k < n; ++k) {
            if (std::fabs(y[k]) + std::fabs(v[k]) < 1e-15f) y[k] = v[k] = 0.0f;
            const bool silent = b[k] == 0.0f && l[k] == 0.0f && r[k] == 0.0f;
            if (silent) y[k] = v[k] = 0.0f;
            else top = k + 1;
        }
        // A mode still gliding in from silence keeps its place.
        if (glideLeft > 0) top = std::max(top, used);
        used = top;
    }

    /** True when nothing rings and nothing is fading: the output is zero for a zero input. */
    bool still() const {
        for (int k = 0; k < used; ++k)
            if (y[k] != 0.0f || v[k] != 0.0f) return false;
        return true;
    }

    /** Silence and every coefficient zeroed, as constructed. */
    void clear() { *this = ModeBank(); }

  private:
    void frame(float x, float &outL, float &outR, int n) {
        float sl[4] = {0.0f, 0.0f, 0.0f, 0.0f}, sr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int k = 0; k < n; ++k) {
            v[k] += c1[k] * y[k] - c2[k] * v[k] + b[k] * x;
            y[k] += v[k];
            sl[k & 3] += l[k] * y[k];
            sr[k & 3] += r[k] * y[k];
        }
        outL = (sl[0] + sl[1]) + (sl[2] + sl[3]);
        outR = (sr[0] + sr[1]) + (sr[2] + sr[3]);
    }

    // Coefficients now, where they're going and each frame's step on the way,
    // one array each so the mode loop runs down them in step.
    float c1[N] = {}, c2[N] = {}, b[N] = {}, l[N] = {}, r[N] = {};
    float tc1[N] = {}, tc2[N] = {}, tb[N] = {}, tl[N] = {}, tr[N] = {};
    float dc1[N] = {}, dc2[N] = {}, db[N] = {}, dl[N] = {}, dr[N] = {};
    float y[N] = {}, v[N] = {}; // each mode's last output and its last step
    int used = 0;        // modes from here up are silent
    int glideFrames = 240;
    int glideLeft = 0;   // frames of the glide still to run
    bool retarget = false;
};

} // namespace acidulous::dsp
