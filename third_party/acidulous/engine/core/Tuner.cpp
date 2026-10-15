#include "Tuner.h"

#include <algorithm>
#include <cmath>

#include <engine/dsp/Biquad.h>

namespace acidulous::audio {
namespace {

/**
 * Running sums of x squared. energy[k] is the energy of the first k samples,
 * so the energy of any run is one subtraction. This makes normalisation
 * free per lag and roughly triples the speed.
 */
std::vector<double> energies(const float *x, int32_t n) {
    std::vector<double> e(static_cast<size_t>(n) + 1, 0.0);
    for (int32_t i = 0; i < n; ++i) e[static_cast<size_t>(i) + 1] = e[static_cast<size_t>(i)] + static_cast<double>(x[i]) * x[i];
    return e;
}

/**
 * Normalised autocorrelation at one lag.
 *
 * Normalised by the energy of the two halves being compared, not the whole
 * window, so a decaying note (like a plucked string) scores as well as a
 * steady one.
 */
float correlate(const float *x, const std::vector<double> &e, int32_t n, int32_t lag, int32_t span = 0) {
    int32_t m = n - lag;
    // Score every lag over the same number of samples ([span]) so the scores
    // can be compared. Otherwise short lags get flattered and a low note can
    // be read far too high.
    if (span > 0 && m > span) m = span;
    if (m <= 0) return 0.0f;
    // Float in the loop so the compiler can vectorise it. Precision is
    // plenty for a few thousand terms of a signal that peaks at one.
    float num = 0.0f;
    for (int32_t i = 0; i < m; ++i) num += x[i] * x[i + lag];
    const double a = e[static_cast<size_t>(m)];
    const double b = e[static_cast<size_t>(lag + m)] - e[static_cast<size_t>(lag)];
    const double den = std::sqrt(a * b);
    return den > 1e-20 ? static_cast<float>(static_cast<double>(num) / den) : 0.0f;
}

/** The vertex of the parabola through three points, as an offset in [-1, 1]. */
float vertex(float left, float mid, float right) {
    const float d = left - 2.0f * mid + right;
    if (std::fabs(d) < 1e-12f) return 0.0f;
    const float off = 0.5f * (left - right) / d;
    return off < -1.0f ? -1.0f : (off > 1.0f ? 1.0f : off);
}

} // namespace

float PitchFinder::find(const float *mono, int32_t frames, float sampleRate, float *clarity) {
    if (clarity != nullptr) *clarity = 0.0f;
    if (mono == nullptr || frames < 2048 || sampleRate < 8000.0f) return 0.0f;

    // Bail out on near-silence first, since room noise can correlate well
    // enough to name a note.
    double energy = 0.0;
    for (int32_t i = 0; i < frames; ++i) energy += static_cast<double>(mono[i]) * mono[i];
    if (std::sqrt(energy / frames) < 1.0e-4f) return 0.0f; // about -80 dBFS

    // --- coarse: decimated, searching the whole range ------------------------------
    const int32_t decimate = std::max(1, static_cast<int32_t>(sampleRate / kCoarseRate + 0.5f));
    const float coarseRate = sampleRate / static_cast<float>(decimate);
    const int32_t minLag = std::max(2, static_cast<int32_t>(coarseRate / kMaxHz));
    const int32_t maxLag = static_cast<int32_t>(coarseRate / kMinHz) + 1;

    // Three periods of the lowest note is enough to pick the octave, and a
    // shorter window keeps the coarse search cheap.
    const int32_t cn = std::min(frames / decimate, maxLag * 3);
    if (cn < 3 * maxLag || cn < 64) return 0.0f;
    const int32_t from = frames - cn * decimate; // use the newest audio

    // A box average as the decimation filter. It's a poor low-pass, but
    // nothing it lets through can be mistaken for periods this long.
    std::vector<float> c(static_cast<size_t>(cn));
    for (int32_t i = 0; i < cn; ++i) {
        float sum = 0.0f;
        for (int32_t k = 0; k < decimate; ++k) sum += mono[from + i * decimate + k];
        c[static_cast<size_t>(i)] = sum / static_cast<float>(decimate);
    }

    const auto coarseEnergy = energies(c.data(), cn);
    // Every lag uses the span the longest lag can manage.
    const int32_t coarseSpan = cn - maxLag;
    std::vector<float> corr(static_cast<size_t>(maxLag + 2), 0.0f);
    float best = 0.0f;
    for (int32_t lag = minLag; lag <= maxLag; ++lag) {
        const float v = correlate(c.data(), coarseEnergy, cn, lag, coarseSpan);
        corr[static_cast<size_t>(lag)] = v;
        best = std::max(best, v);
    }
    if (best < kClarity) return 0.0f;

    // The shortest lag that's a local peak within the margin of the best.
    // Taking the best one can read a bass an octave low.
    const float wins = best * kOctaveMargin;
    int32_t chosen = -1;
    for (int32_t lag = minLag + 1; lag < maxLag; ++lag) {
        const float v = corr[static_cast<size_t>(lag)];
        if (v >= wins && v >= corr[static_cast<size_t>(lag - 1)] && v >= corr[static_cast<size_t>(lag + 1)]) {
            chosen = lag;
            break;
        }
    }
    if (chosen < 0) return 0.0f;

    // --- fine: just the fundamental, at the full rate ------------------------------
    //
    // Low-pass around the coarse result first. A real string's upper partials
    // are slightly sharp, and correlating the whole signal reads a bright note
    // about three cents high.
    const float roughHz = coarseRate / static_cast<float>(chosen);
    const int32_t centre = chosen * decimate;

    // Sixteen periods is plenty, and much cheaper than the whole buffer for a
    // high note.
    int32_t fineFrames = std::min(frames, std::max(centre * 16, 8192));
    if (fineFrames < centre * 4) return 0.0f;

    // Four poles. Two leave the second harmonic loud enough to make the
    // reading wander by a couple of cents.
    dsp::Biquad lp[2];
    const float corner = std::min(roughHz * 1.3f, sampleRate * 0.4f);
    lp[0].lowpass(corner, 0.541f, sampleRate);
    lp[1].lowpass(corner, 1.307f, sampleRate);
    std::vector<float> f(static_cast<size_t>(fineFrames));
    const int32_t fineFrom = frames - fineFrames;
    for (int32_t i = 0; i < fineFrames; ++i) {
        f[static_cast<size_t>(i)] = lp[1].process(lp[0].process(mono[fineFrom + i]));
    }
    // Skip the first two periods while the filter settles.
    const int32_t skip = std::min(fineFrames / 4, centre * 2);
    fineFrames -= skip;

    const int32_t span = decimate + 2;
    const int32_t lo = std::max(2, centre - span);
    const int32_t hi = std::min(fineFrames / 2 - 1, centre + span);
    if (hi <= lo) return 0.0f;

    const auto fineEnergy = energies(f.data() + skip, fineFrames);
    float bestV = -2.0f;
    int32_t bestLag = lo;
    std::vector<float> fineCorr(static_cast<size_t>(hi - lo + 1));
    for (int32_t lag = lo; lag <= hi; ++lag) {
        const float v = correlate(f.data() + skip, fineEnergy, fineFrames, lag);
        fineCorr[static_cast<size_t>(lag - lo)] = v;
        if (v > bestV) { bestV = v; bestLag = lag; }
    }
    if (bestV < kClarity) return 0.0f;
    if (clarity != nullptr) *clarity = bestV;

    float lag = static_cast<float>(bestLag);
    if (bestLag > lo && bestLag < hi) {
        const size_t i = static_cast<size_t>(bestLag - lo);
        lag += vertex(fineCorr[i - 1], fineCorr[i], fineCorr[i + 1]);
    }
    return lag > 0.0f ? sampleRate / lag : 0.0f;
}

void Tuner::push(const float *interleaved, int32_t frames) {
    if (!enabled.load(std::memory_order_acquire) || interleaved == nullptr || frames <= 0) return;
    const int64_t at = writeIndex.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < frames; ++i) {
        const size_t slot = static_cast<size_t>((at + i) % kWindow);
        ring[slot] = 0.5f * (interleaved[static_cast<size_t>(i) * 2] + interleaved[static_cast<size_t>(i) * 2 + 1]);
    }
    writeIndex.store(at + frames, std::memory_order_release);
}

float Tuner::analyse(float sampleRate) {
    if (!enabled.load(std::memory_order_acquire)) return 0.0f;
    if (window.size() != static_cast<size_t>(kWindow)) window.assign(static_cast<size_t>(kWindow), 0.0f);

    // Copy the newest window out, oldest first, and try again if the writer
    // overtook us while copying. A torn window has a jump in it, which would
    // show up as a false pitch.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int64_t before = writeIndex.load(std::memory_order_acquire);
        if (before < kWindow) return 0.0f; // not a window's worth yet
        const int64_t from = before - kWindow;
        for (int32_t i = 0; i < kWindow; ++i) {
            window[static_cast<size_t>(i)] = ring[static_cast<size_t>((from + i) % kWindow)];
        }
        const int64_t after = writeIndex.load(std::memory_order_acquire);
        if (after - before < kWindow) {
            const float hz = PitchFinder::find(window.data(), kWindow, sampleRate);
            hzOut.store(hz, std::memory_order_relaxed);
            return hz;
        }
    }
    return hzOut.load(std::memory_order_relaxed);
}

} // namespace acidulous::audio
