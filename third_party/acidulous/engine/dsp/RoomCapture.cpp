#include "RoomCapture.h"

#include <algorithm>
#include <cmath>

#include "Fft.h"

namespace acidulous::dsp::room {

namespace {

int32_t nextPow2(int64_t n) {
    int32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/** Raised cosine from 0 to 1 as [x] goes 0..1. */
float rise(double x) { return static_cast<float>(0.5 - 0.5 * std::cos(M_PI * std::clamp(x, 0.0, 1.0))); }

} // namespace

double topHz(int32_t rate) { return std::min(20000.0, 0.45 * rate); }

std::vector<float> makeSweep(int32_t frames, int32_t rate, float level) {
    std::vector<float> x(static_cast<size_t>(std::max(frames, 0)));
    if (frames <= 0) return x;
    const double seconds = static_cast<double>(frames) / rate;
    const double span = std::log(topHz(rate) / kLowHz);
    const double k = 2.0 * M_PI * kLowHz * seconds / span;
    const double fadeIn = 0.05 * rate, fadeOut = 0.01 * rate;
    for (int32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        double v = std::sin(k * (std::exp(t * span / seconds) - 1.0));
        v *= rise(i / fadeIn) * rise((frames - 1 - i) / fadeOut);
        x[static_cast<size_t>(i)] = static_cast<float>(v * level);
    }
    return x;
}

std::vector<float> inverseSweep(int32_t frames, int32_t rate) {
    const std::vector<float> x = makeSweep(frames, rate, 1.0f);
    std::vector<float> inv(x.size());
    if (frames <= 0) return inv;
    // Reversed, so it starts at the top, and down 6 dB an octave from there:
    // the sweep spends as long on each octave, so its lows carry more energy.
    const double span = std::log(topHz(rate) / kLowHz);
    for (int32_t n = 0; n < frames; ++n) {
        inv[static_cast<size_t>(n)] = x[static_cast<size_t>(frames - 1 - n)] *
                                      static_cast<float>(std::exp(-span * n / frames));
    }
    // Scaled so the sweep through it comes out at one in the middle of the band.
    const int32_t n = nextPow2(2 * static_cast<int64_t>(frames));
    Fft fft(n);
    std::vector<float> ar(static_cast<size_t>(n)), ai(static_cast<size_t>(n)), br(static_cast<size_t>(n)),
        bi(static_cast<size_t>(n));
    std::copy(x.begin(), x.end(), ar.begin());
    std::copy(inv.begin(), inv.end(), br.begin());
    fft.transform(ar.data(), ai.data(), false);
    fft.transform(br.data(), bi.data(), false);
    const auto lo = static_cast<int32_t>(200.0 * n / rate), hi = static_cast<int32_t>(5000.0 * n / rate);
    double sum = 0.0;
    for (int32_t i = lo; i <= hi; ++i) {
        const auto s = static_cast<size_t>(i);
        sum += std::hypot(static_cast<double>(ar[s]) * br[s] - static_cast<double>(ai[s]) * bi[s],
                          static_cast<double>(ar[s]) * bi[s] + static_cast<double>(ai[s]) * br[s]);
    }
    const double mean = sum / (hi - lo + 1);
    if (mean > 0.0) {
        const auto g = static_cast<float>(1.0 / mean);
        for (float &v : inv) v *= g;
    }
    return inv;
}

Impulse deconvolve(const float *recorded, int64_t frames, const SweepSpec &spec, const std::vector<float> *inverse) {
    Impulse out;
    const int32_t rate = spec.rate;
    out.rate = rate;
    const int32_t sweep = spec.sweepFrames;
    const int32_t pass = spec.passFrames();
    if (sweep <= 0 || spec.tailFrames <= 0 || rate <= 0) {
        out.error = "no sweep";
        return out;
    }
    const auto passes = static_cast<int32_t>(std::min<int64_t>(frames / pass, spec.repeats));
    if (passes < 1) {
        out.error = "the recording is too short";
        return out;
    }

    // The passes averaged. They're the same number of frames apart in the
    // recording as in the output, so they line up without searching.
    std::vector<float> yl(static_cast<size_t>(pass)), yr(static_cast<size_t>(pass));
    float loudest = 0.0f;
    for (int32_t p = 0; p < passes; ++p) {
        const float *from = recorded + static_cast<int64_t>(p) * pass * 2;
        for (int32_t i = 0; i < pass; ++i) {
            yl[static_cast<size_t>(i)] += from[i * 2];
            yr[static_cast<size_t>(i)] += from[i * 2 + 1];
            loudest = std::max(loudest, std::max(std::fabs(from[i * 2]), std::fabs(from[i * 2 + 1])));
        }
    }
    if (!(loudest > 1e-6f)) {
        out.error = "nothing came in";
        return out;
    }
    for (int32_t i = 0; i < pass; ++i) {
        yl[static_cast<size_t>(i)] /= static_cast<float>(passes);
        yr[static_cast<size_t>(i)] /= static_cast<float>(passes);
    }

    // Both channels in one transform: left real, right imaginary. The
    // inverse is real, so they come back apart.
    const std::vector<float> made = inverse == nullptr ? inverseSweep(sweep, rate) : std::vector<float>();
    const std::vector<float> &inv = inverse == nullptr ? made : *inverse;
    const int32_t n = nextPow2(static_cast<int64_t>(pass) + static_cast<int64_t>(inv.size()));
    Fft fft(n);
    std::vector<float> ar(static_cast<size_t>(n)), ai(static_cast<size_t>(n));
    std::copy(inv.begin(), inv.end(), ar.begin());
    fft.transform(ar.data(), ai.data(), false);
    std::vector<float> hl(static_cast<size_t>(n)), hr(static_cast<size_t>(n));
    std::copy(yl.begin(), yl.end(), hl.begin());
    std::copy(yr.begin(), yr.end(), hr.begin());
    yl = {};
    yr = {};
    fft.transform(hl.data(), hr.data(), false);
    for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
        const float re = hl[i] * ar[i] - hr[i] * ai[i];
        const float im = hl[i] * ai[i] + hr[i] * ar[i];
        hl[i] = re;
        hr[i] = im;
    }
    ar = {};
    ai = {};
    fft.transform(hl.data(), hr.data(), true);

    // The linear response starts at the sweep's length less one, plus the
    // latency. Up to a second of latency is looked for, or half the tail.
    const int32_t zero = sweep - 1;
    const int32_t end = pass; // past here the sweep no longer covers the response
    const int32_t lo = std::max(0, zero - rate / 500);
    const int32_t hi = std::min(end - 1, zero + std::min(rate, spec.tailFrames / 2));
    auto mag = [&](int32_t i) {
        return std::max(std::fabs(hl[static_cast<size_t>(i)]), std::fabs(hr[static_cast<size_t>(i)]));
    };
    int32_t peakAt = lo;
    float peak = 0.0f;
    for (int32_t i = lo; i <= hi; ++i) {
        if (mag(i) > peak) {
            peak = mag(i);
            peakAt = i;
        }
    }
    if (!(peak > 0.0f)) {
        out.error = "nothing came in";
        return out;
    }
    // The direct sound: the first thing at half the peak in the 5 ms before
    // it, in case a reflection is louder than the direct path.
    int32_t direct = peakAt;
    for (int32_t i = std::max(lo, peakAt - rate / 200); i < peakAt; ++i) {
        if (mag(i) >= 0.5f * peak) {
            direct = i;
            break;
        }
    }
    const int32_t pre = rate * 25 / 10000; // 2.5 ms kept before it
    const int32_t start = std::max(0, direct - pre);
    if (end - start < rate / 20) {
        out.error = "the recording is too short";
        return out;
    }
    out.latencyFrames = direct - zero;
    out.directFrame = direct - start;

    const auto len = static_cast<size_t>(end - start);
    out.left.assign(hl.begin() + start, hl.begin() + end);
    out.right.assign(hr.begin() + start, hr.begin() + end);
    hl = {};
    hr = {};
    // Faded in over the first half of what's kept before the direct sound.
    const int32_t fadeIn = std::max(1, out.directFrame / 2);
    for (int32_t i = 0; i < fadeIn; ++i) {
        out.left[static_cast<size_t>(i)] *= rise(static_cast<double>(i) / fadeIn);
        out.right[static_cast<size_t>(i)] *= rise(static_cast<double>(i) / fadeIn);
    }
    // One microphone, or two the same: mono.
    double both = 0.0, apart = 0.0;
    for (size_t i = 0; i < len; ++i) {
        const double a = out.left[i], b = out.right[i];
        both += a * a + b * b;
        apart += (a - b) * (a - b);
    }
    if (apart <= both * 1e-4) {
        for (size_t i = 0; i < len; ++i) out.left[i] = 0.5f * (out.left[i] + out.right[i]);
        out.right.clear();
    }

    // The noise floor is the last half second, in 10 ms windows; the tail
    // ends where 50 ms of it first comes within 3 dB of that.
    const int32_t w = std::max(1, rate / 100);
    const auto windows = static_cast<int32_t>(len / static_cast<size_t>(w));
    std::vector<double> e(static_cast<size_t>(std::max(windows, 1)));
    for (int32_t k = 0; k < windows; ++k) {
        double s = 0.0;
        for (int32_t i = k * w; i < (k + 1) * w; ++i) {
            const double a = out.left[static_cast<size_t>(i)];
            const double b = out.stereo() ? out.right[static_cast<size_t>(i)] : a;
            s += a * a + b * b;
        }
        e[static_cast<size_t>(k)] = s;
    }
    const int32_t quiet = std::clamp(windows / 5, 1, 50);
    double noise = 0.0;
    for (int32_t k = std::max(0, windows - quiet); k < windows; ++k) noise += e[static_cast<size_t>(k)];
    noise = std::max(noise / quiet, 1e-30);
    const double top = *std::max_element(e.begin(), e.end());
    out.snrDb = static_cast<float>(10.0 * std::log10(std::max(top, 1e-30) / noise));
    out.autoFrames = static_cast<int32_t>(len);
    const int32_t from = out.directFrame / w + 2;
    for (int32_t k = from; k + 5 <= windows; ++k) {
        double s = 0.0;
        for (int32_t j = 0; j < 5; ++j) s += e[static_cast<size_t>(k + j)];
        if (s / 5.0 <= 2.0 * noise) {
            out.autoFrames = std::min(static_cast<int32_t>(len), (k + 3) * w);
            break;
        }
    }
    out.autoFrames = std::max(out.autoFrames, std::min(static_cast<int32_t>(len), rate / 20));
    return out;
}

void finish(const Impulse &impulse, int32_t frames, std::vector<float> &left, std::vector<float> &right) {
    const int32_t n = std::clamp(frames, 1, std::max(1, impulse.frames()));
    left.assign(impulse.left.begin(), impulse.left.begin() + std::min(n, impulse.frames()));
    left.resize(static_cast<size_t>(n), 0.0f);
    right.clear();
    if (impulse.stereo()) {
        right.assign(impulse.right.begin(), impulse.right.begin() + std::min(n, impulse.frames()));
        right.resize(static_cast<size_t>(n), 0.0f);
    }
    const int32_t ms = impulse.rate / 1000;
    const int32_t fade = std::min(n, std::clamp(n * 15 / 100, 20 * ms, 200 * ms));
    for (int32_t i = 0; i < fade; ++i) {
        const float g = rise(static_cast<double>(i + 1) / (fade + 1));
        left[static_cast<size_t>(n - 1 - i)] *= g;
        if (!right.empty()) right[static_cast<size_t>(n - 1 - i)] *= g;
    }
    float peak = 0.0f;
    for (const float v : left) peak = std::max(peak, std::fabs(v));
    for (const float v : right) peak = std::max(peak, std::fabs(v));
    if (peak > 0.0f) {
        const float g = 0.9f / peak;
        for (float &v : left) v *= g;
        for (float &v : right) v *= g;
    }
}

std::vector<float> picture(const Impulse &impulse, int32_t columns) {
    std::vector<float> out(static_cast<size_t>(std::max(columns, 0)), -90.0f);
    const int32_t n = impulse.frames();
    if (n <= 0 || columns <= 0) return out;
    float top = 0.0f;
    for (int32_t c = 0; c < columns; ++c) {
        const int32_t a = static_cast<int32_t>(static_cast<int64_t>(n) * c / columns);
        const int32_t b = std::max(a + 1, static_cast<int32_t>(static_cast<int64_t>(n) * (c + 1) / columns));
        float m = 0.0f;
        for (int32_t i = a; i < b && i < n; ++i) {
            m = std::max(m, std::fabs(impulse.left[static_cast<size_t>(i)]));
            if (impulse.stereo()) m = std::max(m, std::fabs(impulse.right[static_cast<size_t>(i)]));
        }
        out[static_cast<size_t>(c)] = m;
        top = std::max(top, m);
    }
    for (float &v : out) v = top > 0.0f && v > 0.0f ? std::max(-90.0f, 20.0f * std::log10(v / top)) : -90.0f;
    return out;
}

} // namespace acidulous::dsp::room
