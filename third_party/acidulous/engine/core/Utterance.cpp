#include "Utterance.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace acidulous::audio {

namespace {

/** 48 kHz down to 8 kHz, where the pitch is searched for. */
constexpr int32_t kDecim = 6;

} // namespace

void removeRumble(std::vector<float> &x, float sampleRate, float hz, int poles) {
    if (x.empty() || sampleRate <= 0.0f || hz <= 0.0f) return;
    const float a = std::exp(-2.0f * 3.14159265f * hz / sampleRate);
    for (int pass = 0; pass < poles; ++pass) {
        float px = 0.0f, py = 0.0f;
        for (float &v : x) {
            py = a * (py + v - px);
            px = v;
            v = py;
        }
    }
}

void PitchTrack::find(const std::vector<float> &mono, int32_t frames, float sampleRate,
                      bool cleaned) {
    hz.clear();
    clarity.clear();
    hopFrames = sampleRate * kHopMs * 0.001f;
    if (frames <= 0) return;

    // --- remove rumble first ------------------------------------------------
    //
    // Rumble below kMinHz drifts across a 40 ms window and correlates best at
    // the shortest lag, so the tracker reads every frame at its maximum pitch.
    // Phone recordings nearly always have it. It takes four poles to fix; one
    // or two aren't enough. Clean material is unaffected.
    //
    // `Utterance::analyse` already does this (and passes [cleaned]), but the
    // harness calls `find` directly, so it's done here too.
    std::vector<float> clean(mono.begin(), mono.begin() + frames);
    if (!cleaned) removeRumble(clean, sampleRate, kMinHz, 4);

    // --- decimate -----------------------------------------------------------
    const float lowRate = sampleRate / static_cast<float>(kDecim);
    const int32_t lowFrames = frames / kDecim;
    std::vector<float> low(static_cast<size_t>(std::max(1, lowFrames)), 0.0f);
    for (int32_t i = 0; i < lowFrames; ++i) {
        float sum = 0.0f;
        for (int32_t k = 0; k < kDecim; ++k) sum += clean[static_cast<size_t>(i * kDecim + k)];
        low[static_cast<size_t>(i)] = sum / static_cast<float>(kDecim);
    }

    const int32_t minLag = static_cast<int32_t>(lowRate / kMaxHz);
    const int32_t maxLag = static_cast<int32_t>(lowRate / kMinHz) + 1;
    const int32_t window = static_cast<int32_t>(lowRate * kWindowMs * 0.001f);
    const int32_t hop = std::max(1, static_cast<int32_t>(lowRate * kHopMs * 0.001f));
    const int32_t hops = frames > 0 ? (frames + static_cast<int32_t>(hopFrames) - 1) /
                                          static_cast<int32_t>(hopFrames)
                                    : 0;
    hz.assign(static_cast<size_t>(std::max(0, hops)), 0.0f);
    clarity.assign(static_cast<size_t>(std::max(0, hops)), 0.0f);
    std::vector<float> scores;
    bool wasVoiced = false;

    for (int32_t h = 0; h < hops; ++h) {
        const int32_t at = h * hop;
        // A window that runs off the end is just shorter. The normalisation
        // uses what was actually summed, so it's still correct.
        const int32_t len = std::min(window, lowFrames - at);
        if (len <= maxLag + 2) break;

        double power = 0.0;
        for (int32_t i = 0; i < len; ++i) {
            const double s = low[static_cast<size_t>(at + i)];
            power += s * s;
        }
        if (power < 1e-7) { wasVoiced = false; continue; } // silence has no pitch

        scores.assign(static_cast<size_t>(maxLag - minLag + 1), 0.0f);
        float bestScore = 0.0f;
        for (int32_t lag = minLag; lag <= maxLag; ++lag) {
            double corr = 0.0, energy = 0.0;
            const int32_t n = len - lag;
            for (int32_t i = 0; i < n; ++i) {
                const double a = low[static_cast<size_t>(at + i)];
                const double b = low[static_cast<size_t>(at + i + lag)];
                corr += a * b;
                energy += b * b;
            }
            if (energy < 1e-9) continue;
            // Normalised against both halves so a lag doesn't win just by
            // pointing at louder audio.
            const float score = static_cast<float>(corr / std::sqrt(power * energy));
            scores[static_cast<size_t>(lag - minLag)] = score;
            bestScore = std::max(bestScore, score);
        }
        if (bestScore < (wasVoiced ? kVoicedHold : kVoiced)) { wasVoiced = false; continue; }

        int32_t bestLag = 0;
        for (int32_t lag = minLag; lag <= maxLag; ++lag) {
            if (scores[static_cast<size_t>(lag - minLag)] >= bestScore) {
                bestLag = lag;
                break;
            }
        }
        if (bestLag <= 0) { wasVoiced = false; continue; }

        // Halve the lag while the half scores nearly as well.
        //
        // A voice correlates about as well two periods away as one, so the
        // winner can be an octave low. Only exact halves are tried, at 95% of
        // the best score. A looser test lands on the first formant instead.
        for (int32_t k = 0; k < 3; ++k) {
            const int32_t half = bestLag / 2;
            if (half - 1 < minLag) break;
            // Check either side of the half as well, since halving an odd
            // lag rounds and the rounded lag can score too low to pass.
            float bestHalf = 0.0f;
            int32_t halfLag = half;
            for (int32_t l = half - 1; l <= half + 1 && l <= maxLag; ++l) {
                const float sc = scores[static_cast<size_t>(l - minLag)];
                if (sc > bestHalf) {
                    bestHalf = sc;
                    halfLag = l;
                }
            }
            if (bestHalf < bestScore * 0.95f) break;
            bestLag = halfLag;
        }

        // Refine by searching a narrow band at the original rate. One
        // decimated frame is six real ones, which at 200 Hz is a semitone.
        const float coarse = lowRate / static_cast<float>(bestLag);
        const int32_t fineCentre = static_cast<int32_t>(sampleRate / coarse);
        const int32_t fineFrom = std::max(2, fineCentre - kDecim);
        const int32_t fineTo = fineCentre + kDecim;
        const int32_t fineAt = at * kDecim;
        const int32_t fineLen = std::min(static_cast<int32_t>(sampleRate * kWindowMs * 0.001f),
                                         frames - fineAt);
        float fineBest = 0.0f;
        int32_t fineLag = fineCentre;
        if (fineLen > fineTo + 2) {
            for (int32_t lag = fineFrom; lag <= fineTo; ++lag) {
                double corr = 0.0, ea = 0.0, eb = 0.0;
                const int32_t n = fineLen - lag;
                for (int32_t i = 0; i < n; ++i) {
                    const double a = mono[static_cast<size_t>(fineAt + i)];
                    const double b = mono[static_cast<size_t>(fineAt + i + lag)];
                    corr += a * b;
                    ea += a * a;
                    eb += b * b;
                }
                if (ea < 1e-9 || eb < 1e-9) continue;
                const float score = static_cast<float>(corr / std::sqrt(ea * eb));
                if (score > fineBest) {
                    fineBest = score;
                    fineLag = lag;
                }
            }
        }
        hz[static_cast<size_t>(h)] = sampleRate / static_cast<float>(fineLag);
        clarity[static_cast<size_t>(h)] = std::max(bestScore, fineBest);
        wasVoiced = true;
    }

    // --- then fix octave jumps -----------------------------------------------
    //
    // Each hop picks its octave on its own, and neighbouring hops sometimes
    // disagree. A five-hop median removes the odd one out, since a real sung
    // interval lasts longer than that. Only voiced hops vote, so it never
    // invents a pitch or pulls one toward silence.
    {
        std::vector<float> smoothed = hz;
        std::vector<float> near;
        for (size_t i = 0; i < hz.size(); ++i) {
            if (hz[i] <= 0.0f) continue;
            near.clear();
            for (size_t k = i >= 2 ? i - 2 : 0; k < hz.size() && k <= i + 2; ++k) {
                if (hz[k] > 0.0f) near.push_back(hz[k]);
            }
            if (near.size() < 3) continue;
            std::nth_element(near.begin(), near.begin() + static_cast<long>(near.size() / 2), near.end());
            smoothed[i] = near[near.size() / 2];
        }
        hz.swap(smoothed);
    }
}

float PitchTrack::periodAt(float pos, float sampleRate) const {
    if (hz.empty() || hopFrames <= 0.0f) return 0.0f;
    const float h = pos / hopFrames;
    const int32_t i = static_cast<int32_t>(h);
    if (i < 0) return hz[0] > 0.0f ? sampleRate / hz[0] : 0.0f;
    if (i >= static_cast<int32_t>(hz.size()) - 1) {
        const float last = hz.back();
        return last > 0.0f ? sampleRate / last : 0.0f;
    }
    const float a = hz[static_cast<size_t>(i)];
    const float b = hz[static_cast<size_t>(i + 1)];
    // Don't average with an unvoiced neighbour. Use whichever end has a pitch.
    if (a <= 0.0f && b <= 0.0f) return 0.0f;
    if (a <= 0.0f) return sampleRate / b;
    if (b <= 0.0f) return sampleRate / a;
    const float t = h - static_cast<float>(i);
    return sampleRate / (a + (b - a) * t);
}

void Utterance::analyse(float sampleRate) {
    epochs.clear();
    rootHz = 0.0f;
    frames = static_cast<int32_t>(mono.size());
    if (frames <= 1) return;

    // Before anything reads it (see the header). Four poles at the tracker's
    // floor, since fewer aren't enough.
    removeRumble(mono, sampleRate, PitchTrack::kMinHz, 4);

    // Then trim the room tone before the voice starts, so notes don't start
    // late.
    //
    // Finds the first 10 ms window within 20 dB of the take's level and backs
    // off two windows so the first consonant keeps its start. Only the front
    // is trimmed, since `loop` needs the tail.
    {
        double sum = 0.0;
        for (float v : mono) sum += static_cast<double>(v) * v;
        const auto rms = static_cast<float>(std::sqrt(sum / static_cast<double>(frames)));
        const int32_t win = std::max(1, static_cast<int32_t>(sampleRate * 0.01f));
        const float floorRms = rms * 0.1f;
        int32_t at = 0;
        while (at + win <= frames) {
            double w = 0.0;
            for (int32_t i = at; i < at + win; ++i) w += static_cast<double>(mono[static_cast<size_t>(i)]) * mono[static_cast<size_t>(i)];
            if (std::sqrt(w / win) >= floorRms) break;
            at += win;
        }
        at = std::max(0, at - 2 * win);
        if (at > 0 && at < frames - win) {
            mono.erase(mono.begin(), mono.begin() + at);
            frames = static_cast<int32_t>(mono.size());
        }
    }

    PitchTrack track;
    track.find(mono, frames, sampleRate, true);

    // The median, not the mean, so odd pitches from breaths at either end
    // don't pull it off.
    {
        std::vector<float> voiced;
        voiced.reserve(track.hz.size());
        for (float f : track.hz) {
            if (f > 0.0f) voiced.push_back(f);
        }
        if (!voiced.empty()) {
            std::nth_element(voiced.begin(), voiced.begin() + static_cast<long>(voiced.size() / 2),
                             voiced.end());
            rootHz = voiced[voiced.size() / 2];
        }
    }

    // Unvoiced marks every 5 ms. Short enough that repeated noise isn't
    // audible, long enough to keep the grain count down.
    const float unvoicedPeriod = sampleRate * 0.005f;

    // A first pass of marks, before snapping to peaks, used only to decide
    // the polarity below.
    std::vector<Epoch> raw;
    for (float pos = 0.0f; pos < static_cast<float>(frames);) {
        const float period = track.periodAt(pos, sampleRate);
        Epoch e;
        e.voiced = period > 1.0f;
        e.period = e.voiced ? period : unvoicedPeriod;
        e.at = static_cast<int32_t>(pos);
        raw.push_back(e);
        pos += e.period;
    }

    // Decide once for the whole take which way up the glottal pulses are.
    //
    // Voiced marks snap to the biggest sample within a quarter period so
    // every grain is cut at the same point in the cycle. Snapping by
    // magnitude doesn't work, because the positive and negative peaks are
    // often close and marks would flip between them, making grains cancel.
    // So every mark snaps the same way up.
    double positive = 0.0, negative = 0.0;
    auto reachOf = [&](const Epoch &e) { return static_cast<int32_t>(e.period * 0.25f); };
    for (const Epoch &e : raw) {
        if (!e.voiced) continue;
        const int32_t reach = reachOf(e);
        const int32_t from = std::max(0, e.at - reach);
        const int32_t to = std::min(frames - 1, e.at + reach);
        float hi = 0.0f, lo = 0.0f;
        for (int32_t i = from; i <= to; ++i) {
            hi = std::max(hi, mono[static_cast<size_t>(i)]);
            lo = std::min(lo, mono[static_cast<size_t>(i)]);
        }
        positive += static_cast<double>(hi) * hi;
        negative += static_cast<double>(lo) * lo;
    }
    const float sign = negative > positive ? -1.0f : 1.0f;

    // Each step starts from where the last mark snapped to, not where it was
    // aimed. Otherwise a small error in the period builds up and the marks
    // drift off the pulses.
    for (float pos = 0.0f; pos < static_cast<float>(frames);) {
        const float period = track.periodAt(pos, sampleRate);
        Epoch e;
        e.voiced = period > 1.0f;
        e.period = e.voiced ? period : unvoicedPeriod;
        e.at = static_cast<int32_t>(pos);

        if (e.voiced) {
            const int32_t reach = reachOf(e);
            const int32_t from = std::max(0, e.at - reach);
            const int32_t to = std::min(frames - 1, e.at + reach);
            int32_t peak = e.at;
            float best = -std::numeric_limits<float>::max();
            for (int32_t i = from; i <= to; ++i) {
                const float v = sign * mono[static_cast<size_t>(i)];
                if (v > best) {
                    best = v;
                    peak = i;
                }
            }
            e.at = peak;
        }

        // Always move forward. A repeated position would make a zero-length
        // grain that the reader gets stuck on.
        if (!epochs.empty() && e.at <= epochs.back().at) e.at = epochs.back().at + 1;
        if (e.at >= frames) break;
        epochs.push_back(e);
        // Step from the mark, but always forward by at least half a period
        // in case the snap pulled it back.
        pos = std::max(static_cast<float>(e.at) + e.period, pos + e.period * 0.5f);
    }
}

int32_t Utterance::epochAt(float pos) const {
    if (epochs.empty()) return -1;
    const int32_t p = static_cast<int32_t>(pos);
    int32_t lo = 0, hi = static_cast<int32_t>(epochs.size()) - 1;
    if (p <= epochs[0].at) return 0;
    if (p >= epochs[static_cast<size_t>(hi)].at) return hi;
    while (lo < hi) {
        const int32_t mid = (lo + hi + 1) / 2;
        if (epochs[static_cast<size_t>(mid)].at <= p) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

} // namespace acidulous::audio
