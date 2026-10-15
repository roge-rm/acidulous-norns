#include "Cutter.h"

#include <engine/core/Utterance.h>
#include <engine/dsp/Fft.h>

#include <algorithm>
#include <cmath>

namespace acidulous::machine::diction {

namespace {

constexpr int32_t kFftSize = 1024;
constexpr int32_t kBands = 16;
constexpr float kLowHz = 100.0f, kHighHz = 8000.0f;

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    const auto k = static_cast<size_t>(std::clamp(p, 0.0f, 1.0f) * static_cast<float>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
}

/** Each hop's level in dB and its spectrum in [kBands] log-spaced bands, also in dB. */
struct Features {
    std::vector<float> level;
    std::vector<float> bands; // hop-major
    int32_t hops = 0;
};

Features featuresOf(const std::vector<float> &x, float sr, int32_t hop) {
    Features f;
    const auto frames = static_cast<int32_t>(x.size());
    f.hops = frames / hop;
    f.level.resize(static_cast<size_t>(f.hops));
    f.bands.resize(static_cast<size_t>(f.hops) * kBands);
    static const dsp::Fft fft(kFftSize);
    std::vector<float> re(kFftSize), im(kFftSize), window(kFftSize);
    for (int32_t i = 0; i < kFftSize; ++i) window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(i) / (kFftSize - 1));
    // The bands' edges, as FFT bins.
    int32_t edge[kBands + 1];
    for (int32_t b = 0; b <= kBands; ++b) {
        const float hz = kLowHz * std::pow(kHighHz / kLowHz, static_cast<float>(b) / kBands);
        edge[b] = std::clamp(static_cast<int32_t>(hz * kFftSize / sr), 1, kFftSize / 2 - 1);
    }
    for (int32_t h = 0; h < f.hops; ++h) {
        double power = 0.0;
        for (int32_t i = h * hop; i < (h + 1) * hop; ++i) power += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
        f.level[static_cast<size_t>(h)] = 10.0f * std::log10(static_cast<float>(power / hop) + 1e-12f);
        const int32_t centre = h * hop + hop / 2;
        for (int32_t i = 0; i < kFftSize; ++i) {
            const int32_t at = centre - kFftSize / 2 + i;
            re[static_cast<size_t>(i)] = (at >= 0 && at < frames ? x[static_cast<size_t>(at)] : 0.0f) * window[static_cast<size_t>(i)];
            im[static_cast<size_t>(i)] = 0.0f;
        }
        fft.transform(re.data(), im.data(), false);
        for (int32_t b = 0; b < kBands; ++b) {
            double e = 0.0;
            for (int32_t k = edge[b]; k < std::max(edge[b] + 1, edge[b + 1]); ++k) {
                e += static_cast<double>(re[static_cast<size_t>(k)]) * re[static_cast<size_t>(k)] +
                     static_cast<double>(im[static_cast<size_t>(k)]) * im[static_cast<size_t>(k)];
            }
            f.bands[static_cast<size_t>(h) * kBands + b] = 10.0f * std::log10(static_cast<float>(e) + 1e-12f);
        }
    }
    return f;
}

/** The average spectrum over hops [from, to). */
std::vector<float> averageSpectrum(const Features &f, int32_t from, int32_t to) {
    std::vector<float> avg(kBands, 0.0f);
    const int32_t n = std::max(1, to - from);
    for (int32_t h = from; h < to; ++h) {
        for (int32_t b = 0; b < kBands; ++b) avg[static_cast<size_t>(b)] += f.bands[static_cast<size_t>(h) * kBands + b] / n;
    }
    return avg;
}

/** How unlike [avg] a hop sounds: the RMS difference of their spectra, in dB. */
float distance(const Features &f, int32_t h, const std::vector<float> &avg) {
    float s = 0.0f;
    for (int32_t b = 0; b < kBands; ++b) {
        const float d = f.bands[static_cast<size_t>(h) * kBands + b] - avg[static_cast<size_t>(b)];
        s += d * d;
    }
    return std::sqrt(s / kBands);
}

/** A hop's spectrum with its level taken out, so a vowel fading away still compares by its shape. */
std::vector<float> shapeOf(const Features &f, int32_t h) {
    std::vector<float> s(kBands);
    float mean = 0.0f;
    for (int32_t b = 0; b < kBands; ++b) mean += f.bands[static_cast<size_t>(h) * kBands + b] / kBands;
    for (int32_t b = 0; b < kBands; ++b) s[static_cast<size_t>(b)] = f.bands[static_cast<size_t>(h) * kBands + b] - mean;
    return s;
}

/** The average shape over hops [from, to). */
std::vector<float> averageShape(const Features &f, int32_t from, int32_t to) {
    std::vector<float> avg(kBands, 0.0f);
    const int32_t n = std::max(1, to - from);
    for (int32_t h = from; h < to; ++h) {
        const auto s = shapeOf(f, h);
        for (int32_t b = 0; b < kBands; ++b) avg[static_cast<size_t>(b)] += s[static_cast<size_t>(b)] / n;
    }
    return avg;
}

float shapeDistance(const std::vector<float> &a, const std::vector<float> &b) {
    float s = 0.0f;
    for (int32_t k = 0; k < kBands; ++k) s += (a[static_cast<size_t>(k)] - b[static_cast<size_t>(k)]) * (a[static_cast<size_t>(k)] - b[static_cast<size_t>(k)]);
    return std::sqrt(s / kBands);
}

/** The window of [length] hops in [from, to) whose level varies least. */
int32_t steadiest(const Features &f, int32_t from, int32_t to, int32_t length) {
    int32_t best = from;
    float bestCost = 1e30f;
    for (int32_t h = from; h + length <= to; ++h) {
        float mean = 0.0f;
        for (int32_t k = h; k < h + length; ++k) mean += f.level[static_cast<size_t>(k)];
        mean /= static_cast<float>(length);
        float var = 0.0f;
        for (int32_t k = h; k < h + length; ++k) var += (f.level[static_cast<size_t>(k)] - mean) * (f.level[static_cast<size_t>(k)] - mean);
        if (var < bestCost) { bestCost = var; best = h; }
    }
    return best;
}

/**
 * How many samples sit in flat runs at the take's peak. A mic driven past
 * what it can take cuts the wave off flat, at whatever level that is on a
 * given phone, and the same value repeats; a wave that only peaks there
 * doesn't repeat it exactly.
 */
int32_t clippedSamples(const std::vector<float> &x) {
    float peak = 0.0f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    if (peak <= 0.0f) return 0;
    int32_t total = 0, run = 0;
    for (size_t i = 1; i <= x.size(); ++i) {
        if (i < x.size() && std::fabs(x[i]) >= 0.99f * peak && std::fabs(x[i]) == std::fabs(x[i - 1])) {
            ++run;
        } else {
            if (run >= 2) total += run + 1;
            run = 0;
        }
    }
    return total;
}

} // namespace

Cut cutTake(const std::vector<float> &input, float sr, TakeKind kind, float noteHz, int32_t consonantNear) {
    Cut cut;
    std::vector<float> x = input;
    // Handling noise and rumble under the voice would count as sound.
    audio::removeRumble(x, sr, 60.0f, 2);
    const auto hop = static_cast<int32_t>(sr * 0.01f);
    const Features f = featuresOf(x, sr, hop);
    if (f.hops < 20) { cut.problem = "too short"; return cut; }

    // --- where the singing is ---------------------------------------------
    // Against the take's own quiet and loud, so it works whatever the level.
    const float floor = percentile(f.level, 0.1f);
    const float peak = percentile(f.level, 0.95f);
    if (peak - floor < 12.0f) { cut.problem = "too quiet"; return cut; }
    if (clippedSamples(input) >= 16) { cut.problem = "too loud"; return cut; }
    const float threshold = std::max(floor + 8.0f, peak - 30.0f);
    auto loud = [&](int32_t h) { return f.level[static_cast<size_t>(h)] > threshold; };
    int32_t first = -1, last = -1;
    for (int32_t h = 0; h + 2 < f.hops && first < 0; ++h) if (loud(h) && loud(h + 1) && loud(h + 2)) first = h;
    for (int32_t h = f.hops - 1; h >= 2 && last < 0; --h) if (loud(h) && loud(h - 1) && loud(h - 2)) last = h;
    if (first < 0 || last - first < 30) { cut.problem = "too short"; return cut; }
    cut.start = std::max(0, first - 1) * hop;
    cut.end = std::min(f.hops, last + 2) * hop;
    const int32_t n = last - first + 1;

    // --- the pitch it was sung at ----------------------------------------------
    audio::PitchTrack track;
    track.find(x, static_cast<int32_t>(x.size()), sr, true);
    std::vector<float> sung;
    for (int32_t h = first; h <= last; ++h) {
        const auto t = static_cast<size_t>(static_cast<float>(h * hop) / std::max(1.0f, track.hopFrames));
        if (t < track.hz.size() && track.hz[t] > 0.0f) sung.push_back(track.hz[t]);
    }
    if (static_cast<int32_t>(sung.size()) < n / 3) { cut.problem = "no clear note"; return cut; }
    cut.rootHz = percentile(sung, 0.5f);
    if (noteHz > 0.0f && cut.rootHz > 0.0f) {
        const float cents = 1200.0f * std::log2(cut.rootHz / noteHz);
        cut.centsOff = cents - 1200.0f * std::round(cents / 1200.0f);
    }

    if (kind == TakeKind::Held) {
        // --- the vowel's steady part: the steadiest stretch in the middle ----
        const int32_t window = std::min(60, n * 6 / 10);
        const int32_t best = steadiest(f, first + n / 7, last - n / 10, window);
        cut.holdFrom = best * hop;
        cut.holdTo = (best + window) * hop;
        return cut;
    }

    if (kind == TakeKind::Glide) {
        // --- where it moves: from sounding like the first vowel to the second --
        // By shape, since the level falls as the singer stops. The first vowel
        // is taken from early on, clear of the onset; the second from the
        // last few hops, which is all a singer gives it.
        // The second vowel is taken while the voice is still near its held
        // level. As it fades, breath and the room change the shape as much as
        // a small glide does.
        const float held = percentile(std::vector<float>(f.level.begin() + first, f.level.begin() + last + 1), 0.5f);
        int32_t voiced = last;
        while (voiced > first && f.level[static_cast<size_t>(voiced)] < held - 6.0f) --voiced;
        // The first vowel from just after the voice starts. The second from
        // wherever the voice gets furthest from it in the second half, not
        // from its very end: a singer who glides all the way through, rather
        // than holding and moving at the end, has turned back by the time
        // they stop, and an eye sung that way came out with no move at all.
        const auto fromEarly = averageShape(f, first + n / 20, first + n / 8);
        // Read the old way, from a little later, clear of whatever the voice
        // does as it starts.
        const auto fromLater = averageShape(f, first + n / 8, first + n / 3);
        std::vector<float> apartAt(static_cast<size_t>(f.hops), 0.0f);
        for (int32_t h = first; h <= voiced; ++h) apartAt[static_cast<size_t>(h)] = shapeDistance(shapeOf(f, h), fromEarly);
        int32_t farthest = voiced;
        float mostApart = -1.0f;
        for (int32_t h = std::max(first + 2, first + n / 2); h <= voiced - 2; ++h) {
            float sum = 0.0f;
            for (int32_t k = h - 2; k <= h + 2; ++k) sum += apartAt[static_cast<size_t>(k)];
            if (sum > mostApart) { mostApart = sum; farthest = h; }
        }
        // Returns false to be read again from the end, true when done.
        const int32_t farthestFound = farthest;
        auto locate = [&](bool endOnly) {
            farthest = farthestFound;
            const auto &from = endOnly ? fromLater : fromEarly;
            // Only where the voice clearly turned back before it stopped; a small
            // move like an oh's is at its farthest wherever the wobble is, so
            // otherwise the end is the second vowel, as the prompt asks.
            const int32_t tail = std::max(4, n / 15);
            const auto atEnd = averageShape(f, voiced - tail, voiced + 1);
            const auto atFarthest = averageShape(f, std::max(first, farthest - tail / 2), std::min(voiced + 1, farthest + tail / 2 + 1));
            const bool turnedBack = shapeDistance(from, atEnd) < 0.75f * shapeDistance(from, atFarthest);
            const bool useFarthest = turnedBack && !endOnly;
            if (!useFarthest) farthest = voiced;
            const auto to = useFarthest ? atFarthest : atEnd;
            const float apart = shapeDistance(from, to);
            // Measured this way a vowel held throughout moves up to about 1.2 dB,
            // just from its own wobble, and a prairie "oh", the smallest glide,
            // from 1.4. Too close to tell apart, so a take is never turned down
            // for having no glide: where none shows, it's put where it's sung, as
            // the voice ends. Tuned on Diction's own voice.
            const auto atTheEnd = [&] {
                cut.glideFrom = std::max(first + 40, voiced - 12) * hop;
                cut.glideTo = (voiced + 1) * hop;
            };
            // How far along each hop is, 0 at the first vowel and 1 at the second:
            // how far its shape has moved in the direction from one to the other.
            // Wobble in any other direction doesn't count, which matters when the
            // move is as small as an "oh"'s. Smoothed over five hops, so one odd
            // hop doesn't decide it.
            if (apart < 1.3f) atTheEnd();
            std::vector<float> along(static_cast<size_t>(f.hops), 0.0f);
            float span = 0.0f;
            for (int32_t b = 0; b < kBands; ++b) span += (to[static_cast<size_t>(b)] - from[static_cast<size_t>(b)]) * (to[static_cast<size_t>(b)] - from[static_cast<size_t>(b)]);
            for (int32_t h = first; h <= voiced; ++h) {
                const auto sh = shapeOf(f, h);
                float dot = 0.0f;
                for (int32_t b = 0; b < kBands; ++b) dot += (sh[static_cast<size_t>(b)] - from[static_cast<size_t>(b)]) * (to[static_cast<size_t>(b)] - from[static_cast<size_t>(b)]);
                along[static_cast<size_t>(h)] = dot / span;
            }
            std::vector<float> smooth(along);
            for (int32_t h = first + 2; h <= voiced - 2; ++h) {
                float sum = 0.0f;
                for (int32_t k = h - 2; k <= h + 2; ++k) sum += along[static_cast<size_t>(k)];
                smooth[static_cast<size_t>(h)] = sum / 5.0f;
            }
            // The glide starts after the last hop before the farthest point that
            // still sounds like the first vowel, and ends at the first after it
            // that sounds like the second.
            if (apart >= 1.3f) {
                int32_t glideFrom = farthest;
                while (glideFrom > first && smooth[static_cast<size_t>(glideFrom - 1)] > 0.3f) --glideFrom;
                int32_t glideTo = glideFrom;
                while (glideTo < farthest && smooth[static_cast<size_t>(glideTo)] < 0.7f) ++glideTo;
                cut.glideFrom = glideFrom * hop;
                cut.glideTo = (glideTo + 1) * hop;
                // The first vowel has to be held long enough to hold a note on. Only
                // a glide as clear as an "eye" can say it wasn't: a small one early
                // on is as likely the vowel wobbling, and goes at the end instead.
                if (glideFrom - first < 40) {
                    // Read from its start, or its farthest point, a take can
                    // look as if it moved early when it didn't: read from later
                    // on and from its end first, before saying so.
                    if (!endOnly) return false;
                    if (apart >= 2.5f) { cut.problem = "glide too soon"; return true; }
                    atTheEnd();
                }
            }
            return true;
        };
        if (!locate(false)) locate(true);
        if (!cut.problem.empty()) return cut;
        const int32_t glideFrom = cut.glideFrom / hop;
        const int32_t before = glideFrom - first;
        const int32_t window = std::min(60, before * 6 / 10);
        const int32_t best = steadiest(f, first + before / 7, glideFrom - 2, window);
        cut.holdFrom = best * hop;
        cut.holdTo = (best + window) * hop;
        return cut;
    }

    // --- the consonant: a short change between two stretches of the vowel ---
    // Each hop is compared with the voice just before it and just after it,
    // 100 to 250 ms away, and counts by the nearer of the two. A vowel
    // drifting is like one side or the other; a consonant is like neither.
    // Only where both sides are sung near the held level, so the voice
    // starting or stopping isn't taken for one. A real voice drifts too much
    // to compare against the vowel's average instead: a plain "ah" stood out
    // from its own average as far as an "l" or an "ng" did.
    constexpr int32_t kNear = 10, kFar = 25;
    const float held = percentile(std::vector<float>(f.level.begin() + first, f.level.begin() + last + 1), 0.5f);
    int32_t from = first + kFar, to = last - kFar;
    if (consonantNear >= 0) {
        const auto spread = static_cast<int32_t>(0.6f * sr);
        from = std::max(from, (consonantNear - spread) / hop);
        to = std::min(to, (consonantNear + spread) / hop);
    }
    if (to - from < 3) { cut.problem = "no consonant found"; return cut; }
    auto levelOver = [&](int32_t a, int32_t b) {
        float sum = 0.0f;
        for (int32_t h = a; h < b; ++h) sum += f.level[static_cast<size_t>(h)];
        return sum / static_cast<float>(b - a);
    };
    std::vector<float> d(static_cast<size_t>(f.hops), 0.0f);
    for (int32_t h = from; h <= to; ++h) {
        if (levelOver(h - kFar, h - kNear) < held - 6.0f || levelOver(h + kNear, h + kFar) < held - 6.0f) continue;
        d[static_cast<size_t>(h)] = std::min(distance(f, h, averageSpectrum(f, h - kFar, h - kNear)),
                                             distance(f, h, averageSpectrum(f, h + kNear, h + kFar)));
    }
    // Smoothed a little, so one odd hop doesn't decide it.
    std::vector<float> smooth(d);
    for (int32_t h = from + 1; h < to; ++h) {
        smooth[static_cast<size_t>(h)] = (d[static_cast<size_t>(h - 1)] + d[static_cast<size_t>(h)] + d[static_cast<size_t>(h + 1)]) / 3.0f;
    }
    int32_t centre = from;
    for (int32_t h = from; h <= to; ++h) if (smooth[static_cast<size_t>(h)] > smooth[static_cast<size_t>(centre)]) centre = h;
    // On a real voice the softest consonant, an "ng" between two oo's, stood
    // out by 6.5 dB, and a plain vowel mostly by under 3.5.
    if (smooth[static_cast<size_t>(centre)] < 5.0f) { cut.problem = "no consonant found"; return cut; }

    // Its edges: where it stops sounding unlike the vowel on either side.
    const auto before = averageSpectrum(f, std::max(first, centre - 45), std::max(first + 1, centre - 20));
    const auto after = averageSpectrum(f, std::min(last, centre + 20), std::min(last + 1, centre + 45));
    const int32_t lo = std::max(first, centre - 40), hi = std::min(last, centre + 40);
    std::vector<float> e(static_cast<size_t>(f.hops), 0.0f);
    for (int32_t h = lo; h <= hi; ++h) e[static_cast<size_t>(h)] = std::min(distance(f, h, before), distance(f, h, after));
    float most = 0.0f;
    for (int32_t h = std::max(lo, centre - 3); h <= std::min(hi, centre + 3); ++h) most = std::max(most, e[static_cast<size_t>(h)]);
    int32_t left = centre, right = centre;
    const float edge = 0.4f * most;
    while (left > lo && e[static_cast<size_t>(left - 1)] > edge) --left;
    while (right < hi && e[static_cast<size_t>(right + 1)] > edge) ++right;
    const int32_t length = right - left + 1;
    if (length * 100 > n * 45) { cut.problem = "consonant unclear"; return cut; }
    cut.consonantFrom = left * hop;
    cut.consonantTo = (right + 1) * hop;
    return cut;
}

} // namespace acidulous::machine::diction
