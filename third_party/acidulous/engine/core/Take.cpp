#include "Take.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace acidulous::audio {

void Take::detect(float sampleRate) {
    onsets.clear();
    if (frames <= 0) return;
    OnsetFinder finder;
    finder.reset(sampleRate);
    for (int32_t i = 0; i < frames; ++i) {
        const float l = left[static_cast<size_t>(i)];
        const float r = right.empty() ? l : right[static_cast<size_t>(i)];
        if (finder.push(l, r)) onsets.push_back(i);
        if (onsets.size() >= 512) break; // plenty for a cloud
    }
    guessShape(sampleRate);
}

namespace {
// The loop lengths a guess may give, in bars.
constexpr float kBarCandidates[] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};

/**
 * The share of [onsets] within a fifth of a sixteenth of the grid that starts
 * at [start] with [bars] bars in [length] frames. Onsets can be reported up
 * to 5 ms late, which is well inside that.
 */
double onGrid(const std::vector<int32_t> &onsets, int32_t start, double length, float bars) {
    if (onsets.empty()) return 1.0;
    const double step = length / (bars * 16.0);
    int32_t near = 0;
    for (const int32_t at : onsets) {
        if (at < start) continue;
        const double off = std::fmod(static_cast<double>(at - start), step);
        if (std::min(off, step - off) < step * 0.2) ++near;
    }
    return static_cast<double>(near) / static_cast<double>(onsets.size());
}

double tempoOf(double length, float bars, float sampleRate) {
    return bars * 4.0 * 60.0 * sampleRate / length;
}
} // namespace

/**
 * Where the music starts: the first frame that rises clear of the room.
 *
 * Rumble is filtered out first, and the room is the quietest tenth of the
 * 10 ms windows, as in Molt's trim, so room tone under a loop doesn't count
 * as music. A window has to
 * reach 20 dB under the take's level, or 13 dB over the room when the room
 * sits 12 to 33 dB under the take. Then the first frame in that window or the
 * one before that peaks at twice that, less half a millisecond for the
 * attack's rise.
 */
int32_t Take::musicStart(float sampleRate) const {
    const int32_t win = std::max(1, static_cast<int32_t>(sampleRate * 0.01f));
    if (frames < win * 10) return 0;
    // Mono, high-passed at 100 Hz (two poles) so rumble under a loop doesn't
    // read as music.
    std::vector<float> m(static_cast<size_t>(frames));
    {
        const double w = 2.0 * 3.141592653589793 * 100.0 / sampleRate;
        const double alpha = std::sin(w) / (2.0 * 0.7071), c = std::cos(w), a0 = 1.0 + alpha;
        const double b0 = (1.0 + c) / 2.0 / a0, b1 = -(1.0 + c) / a0, b2 = b0;
        const double a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (int32_t i = 0; i < frames; ++i) {
            const float l = left[static_cast<size_t>(i)];
            const double x = right.empty() ? l : 0.5 * (l + right[static_cast<size_t>(i)]);
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            m[static_cast<size_t>(i)] = static_cast<float>(y);
        }
    }
    auto mono = [&](int32_t i) { return m[static_cast<size_t>(i)]; };
    std::vector<double> power;
    power.reserve(static_cast<size_t>(frames / win));
    double sum = 0.0;
    for (int32_t at = 0; at + win <= frames; at += win) {
        double w = 0.0;
        for (int32_t i = at; i < at + win; ++i) w += static_cast<double>(mono(i)) * mono(i);
        power.push_back(w);
        sum += w;
    }
    const auto rms = static_cast<float>(std::sqrt(sum / (static_cast<double>(power.size()) * win)));
    if (rms <= 0.0f) return 0;
    float floorRms = rms * 0.1f;
    std::vector<double> sorted = power;
    const size_t tenth = sorted.size() / 10;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(tenth), sorted.end());
    const auto roomRms = static_cast<float>(std::sqrt(sorted[tenth] / win));
    if (roomRms * 4.5f > floorRms && roomRms < rms * 0.25f) floorRms = std::min(roomRms * 4.5f, rms * 0.56f);
    size_t w = 0;
    while (w < power.size() && std::sqrt(power[w] / win) < floorRms) ++w;
    if (w == 0 || w >= power.size()) return 0;
    const int32_t from = static_cast<int32_t>(w - 1) * win, to = static_cast<int32_t>(w + 1) * win;
    for (int32_t i = from; i < to; ++i) {
        if (std::fabs(mono(i)) >= 2.0f * floorRms) return std::max(0, i - static_cast<int32_t>(sampleRate * 0.0005f));
    }
    return from;
}

/**
 * Guesses how many bars a loop is, and where they start and end, from its
 * length and where its hits fall.
 *
 * A loop trimmed to whole bars only allows a few tempos (four seconds is two
 * bars at 120, one at 60 or four at 240). Of those between 70 and 180, the
 * one whose sixteenth grid the hits land on wins. Hits on a coarse grid are
 * also on a finer one, so ties go to the tempo nearest 120.
 *
 * A loop cut off a record often isn't trimmed: there's room before the first
 * hit, or the last one rings on past the bar. So the guess is also made from
 * the first hit, and with the end anywhere from the last hit to the end of
 * the file, and the grid fitted to the hits. Those only win when the hits sit
 * on their grid clearly better than on the whole file's, so a loop that is
 * trimmed keeps the tempo its length gives. The user can fix a wrong guess
 * with the bars knob.
 */
int32_t Take::attackNear(int32_t at) const {
    constexpr int32_t kBack = 512, kAhead = 256;
    const int32_t from = std::max(0, at - kBack), to = std::min(frames, at + kAhead);
    auto level = [&](int32_t i) {
        const float l = std::fabs(left[static_cast<size_t>(i)]);
        return right.empty() ? l : std::max(l, std::fabs(right[static_cast<size_t>(i)]));
    };
    float peak = 0.0f;
    int32_t peakAt = at;
    for (int32_t i = std::max(from, at - 256); i < to; ++i) {
        if (level(i) > peak) { peak = level(i); peakAt = i; }
    }
    // The first frame in reach that gets to half the peak. A hit is louder
    // than what's left of the one before it, near enough always.
    for (int32_t i = from; i < peakAt; ++i) {
        if (level(i) >= peak * 0.5f) return i;
    }
    return peakAt;
}

void Take::guessShape(float sampleRate) {
    bars = 0.0f;
    barsStart = 0;
    barsFrames = frames;
    if (frames <= 0 || sampleRate <= 0.0f) return;
    const auto score = [&](int32_t start, double length, float b) {
        return onGrid(onsets, start, length, b) - 0.1 * std::fabs(std::log2(tempoOf(length, b, sampleRate) / 120.0));
    };
    double best = -1e9;
    for (const float b : kBarCandidates) {
        const double bpm = tempoOf(frames, b, sampleRate);
        if (bpm < 70.0 || bpm >= 180.0) continue;
        const double s = score(0, frames, b);
        if (s > best) { best = s; bars = b; }
    }
    if (onsets.size() < 4) return;

    // The trimmed guesses: from the first sound, with the end at the end of
    // the file or anywhere after the last hit, in 2 ms steps. The first sound
    // only, since with the grid free to slide, hits 150 ms in would fit a
    // grid from the file's start almost as well.
    const int32_t lead = musicStart(sampleRate);
    const int32_t lastHit = onsets.back();
    const double stride = sampleRate * 0.002;
    double trimmedBest = -1e9, trimmedLength = 0.0;
    int32_t trimmedStart = 0;
    float trimmedBars = 0.0f;
    {
        const int32_t start = lead;
        for (const float b : kBarCandidates) {
            // Slower than 70 bpm doesn't count, and a tail is at most a
            // quarter of the loop.
            const double longest = std::min(static_cast<double>(frames - start), b * 4.0 * 60.0 * sampleRate / 70.0);
            // The bar has to end after the last hit, by at least a fifth of a sixteenth.
            const double shortest = std::max({static_cast<double>(lastHit - start) * (1.0 + 0.2 / (b * 16.0)),
                                              (frames - start) * 0.75, 1.0});
            for (double length = longest; length >= shortest; length -= stride) {
                if (tempoOf(length, b, sampleRate) >= 180.0) break;
                const double s = score(start, length, b);
                if (s > trimmedBest) {
                    trimmedBest = s;
                    trimmedLength = length;
                    trimmedStart = start;
                    trimmedBars = b;
                }
            }
        }
    }
    if (trimmedBars <= 0.0f || trimmedBest < best + 0.1) return;

    // Fits the grid to the hits on it, so the tempo isn't the edge of the
    // range of lengths that score the same. The finder reports a hit at the
    // end of a 256-frame hop, so each is moved back to its attack first. A
    // second pass fits again to the hits near the first line, one per
    // sixteenth, so a hit just off the grid (swing, or a hit found twice)
    // doesn't pull it.
    std::vector<double> attacks;
    for (const int32_t at : onsets) {
        if (at >= trimmedStart) attacks.push_back(attackNear(at) - trimmedStart);
    }
    double step = trimmedLength / (trimmedBars * 16.0), origin = 0.0, fitted = trimmedLength;
    for (int pass = 0; pass < 2; ++pass) {
        const double reach = step * (pass == 0 ? 0.2 : 0.1);
        std::vector<std::pair<double, double>> nearest; // (k, t), the closest hit to each line
        for (const double t : attacks) {
            const double k = std::round((t - origin) / step);
            const double off = std::fabs(t - origin - k * step);
            if (off >= reach) continue;
            if (!nearest.empty() && nearest.back().first == k) {
                if (off < std::fabs(nearest.back().second - origin - k * step)) nearest.back().second = t;
            } else {
                nearest.emplace_back(k, t);
            }
        }
        double n = 0.0, sk = 0.0, st = 0.0, skk = 0.0, skt = 0.0;
        for (const auto &[k, t] : nearest) {
            n += 1.0;
            sk += k;
            st += t;
            skk += k * k;
            skt += k * t;
        }
        const double spread = n * skk - sk * sk;
        if (spread <= 0.0) break;
        step = (n * skt - sk * st) / spread;
        origin = (st - step * sk) / n;
        fitted = step * trimmedBars * 16.0;
    }
    // The fit only refines the length the search found, so a skewed second
    // pass can't move it more than a few percent.
    fitted = std::clamp(fitted, trimmedLength * 0.97, trimmedLength * 1.03);
    bars = trimmedBars;
    barsStart = trimmedStart;
    barsFrames = static_cast<int32_t>(std::min(static_cast<double>(frames - trimmedStart), std::round(fitted)));
}

} // namespace acidulous::audio
