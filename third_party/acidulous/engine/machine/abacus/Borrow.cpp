#include "Borrow.h"
#include <algorithm>
#include <cmath>

namespace acidulous::machine::abacus {

namespace {
constexpr float kPi = 3.14159265358979f;
/** The cepstrum is cut at 3 ms, with a half-Hann taper. */
constexpr float kLifterSeconds = 0.003f;
/** Magnitudes under the frame's loudest by this much (-100 dB) are floored, so silent bins don't drag the curve. */
constexpr float kMagFloor = 1e-5f;
/** Under this rms a window is silent. */
constexpr float kQuiet = 1e-6f;
/** How sure the pitch follower must be before the harmonics are read. */
constexpr float kSure = 0.6f;
/** The envelope's loudest point is looked for from here up; under it is what the high-pass left of the rumble. */
constexpr float kEnvFromHz = 80.0f;
} // namespace

float envPointsPerOctave() {
    static const float ppo = static_cast<float>(kEnvPoints - 1) / std::log2(kEnvTopHz / kEnvLowHz);
    return ppo;
}

Analyser::Analyser()
    : win(kFftSize), re(kFftSize / 2), im(kFftSize / 2), mag(kBins), logMag(kBins), twr(kFftSize / 2), twi(kFftSize / 2),
      pointBin(kEnvPoints), pointFrac(kEnvPoints) {
    double power = 0.0;
    for (int i = 0; i < kFftSize; ++i) {
        win[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * kPi * (static_cast<float>(i) + 0.5f) / kFftSize);
        power += static_cast<double>(win[static_cast<size_t>(i)]) * win[static_cast<size_t>(i)];
    }
    winPower = static_cast<float>(power);
    // The twiddles that put the half-size transform's two halves together.
    for (int k = 0; k < kFftSize / 2; ++k) {
        const double a = -2.0 * 3.14159265358979323846 * k / kFftSize;
        twr[static_cast<size_t>(k)] = static_cast<float>(std::cos(a));
        twi[static_cast<size_t>(k)] = static_cast<float>(std::sin(a));
    }
    kernel.reserve(kBins);
    prepare(48000.0f);
}

void Analyser::prepare(float sr) {
    sampleRate = sr;
    // The lifter: the cepstrum kept under kLifterSeconds with a half-Hann
    // taper. In the spectrum that is a convolution with the lifter's own
    // transform, kernel[d] = (1/N) sum over n of w(n) cos(2 pi n d / N), kept
    // out to where it falls under 1e-3 of its centre (47 bins either side).
    const int cut = std::max(2, static_cast<int>(std::lround(kLifterSeconds * sr)));
    std::vector<double> k(kBins);
    for (int d = 0; d < kBins; ++d) {
        double sum = 1.0;
        for (int n = 1; n < cut; ++n)
            sum += 2.0 * (0.5 + 0.5 * std::cos(3.14159265358979323846 * n / cut)) *
                   std::cos(2.0 * 3.14159265358979323846 * n * d / kFftSize);
        k[static_cast<size_t>(d)] = sum / kFftSize;
    }
    int reach = 1;
    for (int d = 1; d < kBins; ++d)
        if (std::fabs(k[static_cast<size_t>(d)]) > 1e-3 * k[0]) reach = d;
    kernel.assign(k.begin(), k.begin() + reach + 1);

    // Where each envelope point reads: the bin under it and how far on.
    const float binHz = sr / static_cast<float>(kFftSize);
    const float ppo = envPointsPerOctave();
    firstLoud = kEnvPoints;
    for (int j = 0; j < kEnvPoints; ++j) {
        const float hz = kEnvLowHz * std::exp2(static_cast<float>(j) / ppo);
        const float pos = std::min(hz / binHz, static_cast<float>(kBins - 1) - 1e-3f);
        pointBin[static_cast<size_t>(j)] = static_cast<int>(pos);
        pointFrac[static_cast<size_t>(j)] = pos - static_cast<float>(pointBin[static_cast<size_t>(j)]);
        if (hz >= kEnvFromHz) firstLoud = std::min(firstLoud, j);
    }
}

void Analyser::reset() { stage = 0; }

void Analyser::blank(Frame &f) {
    for (float &e : f.env) e = 0.0f;
    for (int n = 0; n < kMaxPartials; ++n) f.harm[n] = -std::log2(static_cast<float>(n + 1));
    f.level = 0.0f;
}

void Analyser::frame(const float *x, float pitchHz, float sure, float levelRef, Frame &out) {
    begin(x, pitchHz, sure, levelRef, out);
    while (!step()) {}
    out = work;
}

void Analyser::begin(const float *x, float pitchHz, float sure, float levelRef, const Frame &from, bool envelope) {
    src = x;
    wantEnv = envelope;
    pitch = pitchHz;
    sureness = sure;
    ref = levelRef;
    work = from;
    stage = 1;
}

bool Analyser::step() {
    constexpr int kHalf = kFftSize / 2;
    switch (stage) {
    case 1: {
        // The window's samples in pairs as complex numbers, for a transform of half the size.
        double sum = 0.0;
        for (int i = 0; i < kHalf; ++i) {
            const float a = src[2 * i] * win[static_cast<size_t>(2 * i)];
            const float b = src[2 * i + 1] * win[static_cast<size_t>(2 * i + 1)];
            re[static_cast<size_t>(i)] = a;
            im[static_cast<size_t>(i)] = b;
            sum += static_cast<double>(a) * a + static_cast<double>(b) * b;
        }
        const float rms = static_cast<float>(std::sqrt(sum / winPower));
        work.level = rms / ref;
        if (rms < kQuiet) {
            work.level = 0.0f;
            stage = 0;
            return true;
        }
        break;
    }
    case 2: fft.forwardPart(re.data(), im.data(), 0, 2); break;
    case 3: fft.forwardPart(re.data(), im.data(), 1, 2); break;
    case 4: {
        // Pulled apart into the even and odd samples' spectra and put back
        // together as the real window's: X[k] = E[k] + W^k O[k].
        top = 0.0f;
        for (int k = 0; k <= kHalf; ++k) {
            const int a = k % kHalf, b = (kHalf - k) % kHalf;
            const float zr = re[static_cast<size_t>(a)], zi = im[static_cast<size_t>(a)];
            const float cr = re[static_cast<size_t>(b)], ci = -im[static_cast<size_t>(b)];
            const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);
            // (z - c) / 2i
            const float orr = 0.5f * (zi - ci), oi = -0.5f * (zr - cr);
            const float wr = k < kHalf ? twr[static_cast<size_t>(k)] : -1.0f, wi = k < kHalf ? twi[static_cast<size_t>(k)] : 0.0f;
            const float xr = er + wr * orr - wi * oi, xi = ei + wr * oi + wi * orr;
            // Magnitudes for the envelope; the harmonics alone keep powers and
            // take the root of only the loudest near each, which comes out the same.
            const float p = xr * xr + xi * xi;
            mag[static_cast<size_t>(k)] = wantEnv ? std::sqrt(p) : p;
            top = std::max(top, mag[static_cast<size_t>(k)]);
        }
        const float binHz = sampleRate / static_cast<float>(kFftSize);

        // The harmonics, when the pitch is known: the loudest bin near each multiple.
        if (sureness >= kSure && pitch > 0.0f) {
            const int reach = std::clamp(static_cast<int>(0.3f * pitch / binHz), 1, 3);
            float most = 0.0f;
            for (int n = 0; n < kMaxPartials; ++n) {
                const float at = pitch * static_cast<float>(n + 1) / binHz;
                float m = 0.0f;
                if (at < static_cast<float>(kBins - 1 - reach)) {
                    const int c = static_cast<int>(std::lround(at));
                    for (int k = std::max(0, c - reach); k <= c + reach; ++k) m = std::max(m, mag[static_cast<size_t>(k)]);
                    if (!wantEnv) m = std::sqrt(m);
                }
                work.harm[n] = m;
                most = std::max(most, m);
            }
            const float inv = 1.0f / most;
            for (float &h : work.harm) h = h > 0.0f ? std::max(kShapeFloor, std::log2(h * inv)) : kShapeFloor;
        }
        // Without the envelope the last two steps are empty, so a frame lands on the same block either way.
        if (!wantEnv) break;
        const F4 floor = splat(top * kMagFloor);
        for (int k = 0; k < kBins - 1; k += 4) {
            const F4 m = load4(mag.data() + k);
            store4(logMag.data() + k, log2v(pick(m < floor, floor, m)));
        }
        logMag[kBins - 1] = std::log2(std::max(mag[kBins - 1], top * kMagFloor));
        loudest = -1e30f;
        lastK = -1;
        sk0 = sk1 = 0.0f;
        break;
    }
    // The low points share bins and cost less, so the first step takes more of them.
    case 5:
        if (wantEnv) envelope(0, kEnvPoints * 5 / 8);
        break;
    case 6:
        if (wantEnv) {
            envelope(kEnvPoints * 5 / 8, kEnvPoints);
            for (float &e : work.env) e = std::clamp(e - loudest, kShapeFloor, 0.0f);
        }
        stage = 0;
        return true;
    default: stage = 0; return true;
    }
    ++stage;
    return false;
}

void Analyser::envelope(int from, int to) {
    // The log spectrum smoothed by the lifter's kernel, at the two bins
    // either side of each point. The spectrum of a real window is even about
    // 0 and about N/2, so the kernel reflects at both ends.
    const int reach = static_cast<int>(kernel.size()) - 1;
    const float *lm = logMag.data();
    const float *ker = kernel.data();
    auto smooth = [&](int k) {
        float s = ker[0] * lm[k];
        if (k - reach >= 0 && k + reach <= kBins - 1) {
            for (int d = 1; d <= reach; ++d) s += ker[d] * (lm[k - d] + lm[k + d]);
            return s;
        }
        for (int d = 1; d <= reach; ++d) {
            int lo = k - d, hi = k + d;
            if (lo < 0) lo = -lo;
            if (hi > kBins - 1) hi = 2 * (kBins - 1) - hi;
            s += kernel[static_cast<size_t>(d)] * (logMag[static_cast<size_t>(lo)] + logMag[static_cast<size_t>(hi)]);
        }
        return s;
    };
    for (int j = from; j < to; ++j) {
        const int k = pointBin[static_cast<size_t>(j)];
        const float f = pointFrac[static_cast<size_t>(j)];
        // Low points share bins, so a bin's smoothing is kept for the next point.
        if (k != lastK) {
            sk0 = k == lastK + 1 ? sk1 : smooth(k);
            sk1 = smooth(k + 1);
            lastK = k;
        }
        const float l = sk0 + (sk1 - sk0) * f;
        work.env[j] = l;
        if (j >= firstLoud) loudest = std::max(loudest, l);
    }
}

void LiveSource::prepare(float sr) {
    analyser.prepare(sr);
    rumble.prepare(sr);
    follow.prepare(sr);
    reset();
}

void LiveSource::reset() {
    analyser.reset();
    rumble.reset();
    follow.reset();
    for (float &s : ring) s = 0.0f;
    write = 0;
    fresh = 0;
    frames = 0;
    stale = true;
    Analyser::blank(frame);
}

void LiveSource::push(const float *x, int n, bool pitch, bool look) {
    for (int at = 0; at < n; at += kFftSize) {
        const int m = std::min(kFftSize, n - at);
        for (int i = 0; i < m; ++i) {
            const float y = rumble.process(x[at + i]);
            hp[i] = y;
            ring[write] = y;
            write = (write + 1) & (kFftSize - 1);
        }
        // The follower takes the filtered sound too.
        if (pitch) follow.push(hp, m);
    }
    harmOnly = pitch;
    fresh = std::min(fresh + n, kFftSize);
    if (pitch && look) follow.updateWide(0.25f);
}

void LiveSource::advance(int steps) {
    // A frame still under way when the next hop is in is finished first.
    if (stale || (fresh >= kHop && analyser.busy())) steps = Analyser::kSteps + 1;
    for (; steps > 0; --steps) {
        if (!analyser.busy()) {
            if (fresh < kHop) return;
            for (int i = 0; i < kFftSize; ++i) window[i] = ring[(write + i) & (kFftSize - 1)];
            analyser.begin(window, follow.pitch(), follow.sureness(), kTrackRef, frame, !harmOnly);
            fresh = 0;
        }
        if (analyser.step()) {
            frame = analyser.result();
            ++frames;
            stale = false;
        }
    }
}

std::unique_ptr<Spectra> Spectra::analyse(const float *mono, int32_t count, float sr) {
    auto s = std::make_unique<Spectra>();
    const auto n = static_cast<int32_t>(std::min<double>(count, std::floor(kFileSeconds * sr)));
    s->hopSeconds = static_cast<float>(kHop) / sr;
    s->seconds = static_cast<float>(n) / sr;
    std::vector<float> x(static_cast<size_t>(n));
    Rumble rumble;
    rumble.prepare(sr);
    for (int32_t i = 0; i < n; ++i) x[static_cast<size_t>(i)] = rumble.process(mono[i]);

    Analyser analyser;
    analyser.prepare(sr);
    dsp::PitchFollow follow;
    follow.prepare(sr);
    const int32_t count0 = std::max<int32_t>(1, (n + kHop - 1) / kHop);
    s->frames.resize(static_cast<size_t>(count0));
    std::vector<float> window(kFftSize);
    Frame last{};
    Analyser::blank(last);
    int32_t pushed = 0;
    for (int32_t k = 0; k < count0; ++k) {
        const int32_t centre = k * kHop;
        // The follower hears up to the window's end, as it does live.
        const int32_t end = std::min(n, centre + kFftSize / 2);
        while (pushed < end) {
            const int32_t m = std::min(kHop, end - pushed);
            follow.push(x.data() + pushed, m);
            follow.updateWide(1.0f);
            pushed += m;
        }
        for (int i = 0; i < kFftSize; ++i) {
            const int32_t at = centre - kFftSize / 2 + i;
            window[static_cast<size_t>(i)] = at >= 0 && at < n ? x[static_cast<size_t>(at)] : 0.0f;
        }
        analyser.frame(window.data(), follow.pitch(), follow.sureness(), 1.0f, last);
        s->frames[static_cast<size_t>(k)] = last;
    }
    // Levels: the loudest frames but a twentieth read 1.
    std::vector<float> levels;
    levels.reserve(s->frames.size());
    for (const Frame &f : s->frames) if (f.level > 0.0f) levels.push_back(f.level);
    if (!levels.empty()) {
        const size_t at = std::min(levels.size() - 1, levels.size() * 19 / 20);
        std::nth_element(levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(at), levels.end());
        const float ref = std::max(levels[at], 1e-6f);
        for (Frame &f : s->frames) f.level /= ref;
    }
    return s;
}

} // namespace acidulous::machine::abacus
