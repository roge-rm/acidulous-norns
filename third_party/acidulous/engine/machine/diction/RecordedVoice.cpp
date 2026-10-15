#include "RecordedVoice.h"

#include <engine/dsp/Fft.h>
#include <engine/format/WavReader.h>
#include <engine/machine/diction/Phones.h>
#include <engine/machine/diction/Throat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace acidulous::machine::diction {

namespace {

/** A margin either side of the steady part, so a grain at its edge reads real sound. */
constexpr float kMarginSeconds = 0.05f;
/** How much of the vowel either side of a consonant is kept with it. */
constexpr float kJoinSeconds = 0.15f;
/** The loudest a consonant is let be against the vowels either side of it: 3 dB under. */
constexpr float kConsonantCeiling = 0.708f;

/** Where the energy above 2 kHz is centred between [from] and [to], in hertz. */
float centreAbove2k(const std::vector<float> &x, int32_t from, int32_t to, float sampleRate) {
    constexpr int32_t kN = 1024;
    static const dsp::Fft fft(kN);
    std::vector<float> re(kN), im(kN);
    double weighted = 0.0, total = 0.0;
    for (int32_t at = from; at + kN <= to; at += kN / 2) {
        for (int32_t i = 0; i < kN; ++i) {
            const float w = 0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(i) / (kN - 1));
            re[static_cast<size_t>(i)] = x[static_cast<size_t>(at + i)] * w;
            im[static_cast<size_t>(i)] = 0.0f;
        }
        fft.transform(re.data(), im.data(), false);
        for (int32_t k = 1; k < kN / 2; ++k) {
            const double hz = static_cast<double>(k) * sampleRate / kN;
            if (hz < 2000.0) continue;
            const double p = static_cast<double>(re[static_cast<size_t>(k)]) * re[static_cast<size_t>(k)] +
                             static_cast<double>(im[static_cast<size_t>(k)]) * im[static_cast<size_t>(k)];
            weighted += p * hz;
            total += p;
        }
    }
    return total > 0.0 ? static_cast<float>(weighted / total) : 0.0f;
}

/** Taken off before the analysis, so the prediction finds the throat rather than the folds' tilt. */
constexpr float kEmphasis = 0.97f;

} // namespace

namespace {

/**
 * The roots in (0, pi) of a symmetric polynomial of even degree [n] with
 * coefficients [c], evaluated on the unit circle as a sum of cosines: found
 * on a grid of [grid] and closed in on by false position. Up to [most] of
 * them into [out]; returns how many.
 */
int32_t circleRoots(const double *c, int32_t n, int32_t grid, float *out, int32_t most) {
    const int32_t half = n / 2;
    auto at = [&](double x) {
        // In x = cos w: c[half] + 2 sum c[half - m] T_m(x), by the recurrence.
        double tPrev = 1.0, tNow = x, sum = c[half];
        for (int32_t m = 1; m <= half; ++m) {
            sum += 2.0 * c[half - m] * tNow;
            const double next = 2.0 * x * tNow - tPrev;
            tPrev = tNow;
            tNow = next;
        }
        return sum;
    };
    int32_t found = 0;
    double x0 = 1.0, f0 = at(1.0);
    for (int32_t g = 1; g <= grid && found < most; ++g) {
        const double x1 = std::cos(3.14159265358979 * g / grid), f1 = at(x1);
        if ((f0 < 0.0) != (f1 < 0.0)) {
            // False position, the Illinois way: a curve this smooth is closed
            // in on in a few steps, where halving took twenty. Timed on a
            // whole voice, this and the coarser grid took the frequencies
            // from 800 ms to ...
            double lo = x0, hi = x1, flo = f0, fhi = f1;
            int32_t side = 0;
            double x = 0.5 * (lo + hi);
            for (int32_t i = 0; i < 12 && std::fabs(hi - lo) > 1e-9; ++i) {
                x = (lo * fhi - hi * flo) / (fhi - flo);
                const double fx = at(x);
                if ((fx < 0.0) == (flo < 0.0)) {
                    lo = x;
                    flo = fx;
                    if (side == -1) fhi *= 0.5;
                    side = -1;
                } else {
                    hi = x;
                    fhi = fx;
                    if (side == 1) flo *= 0.5;
                    side = 1;
                }
                if (fx == 0.0) break;
            }
            out[found++] = static_cast<float>(std::acos(std::clamp(x, -1.0, 1.0)));
        }
        x0 = x1;
        f0 = f1;
    }
    return found;
}

/** The line spectral frequencies of A(z) = [a] (order p, a[0] = 1) into [lsf]; false if not all were found. */
bool toLsf(const double *a, int32_t p, float *lsf) {
    // P(z) = A(z) + z^-(p+1) A(1/z) and Q(z) = A(z) - z^-(p+1) A(1/z), with
    // their roots at -1 and 1 divided out: two symmetric polynomials of
    // degree p whose roots on the circle interleave.
    std::vector<double> sum(static_cast<size_t>(p + 2)), diff(static_cast<size_t>(p + 2));
    for (int32_t i = 0; i <= p + 1; ++i) {
        const double x = i <= p ? a[i] : 0.0, y = p + 1 - i <= p ? a[p + 1 - i] : 0.0;
        sum[static_cast<size_t>(i)] = x + y;
        diff[static_cast<size_t>(i)] = x - y;
    }
    std::vector<double> ps(static_cast<size_t>(p + 1)), qs(static_cast<size_t>(p + 1));
    ps[0] = sum[0];
    qs[0] = diff[0];
    for (int32_t i = 1; i <= p; ++i) {
        ps[static_cast<size_t>(i)] = sum[static_cast<size_t>(i)] - ps[static_cast<size_t>(i - 1)];
        qs[static_cast<size_t>(i)] = diff[static_cast<size_t>(i)] + qs[static_cast<size_t>(i - 1)];
    }
    const int32_t half = p / 2;
    // A grid of 128, and where a close pair was missed, of 1024.
    for (int32_t grid : {128, 1024}) {
        if (circleRoots(ps.data(), p, grid, lsf, half) == half && circleRoots(qs.data(), p, grid, lsf + half, half) == half)
            return true;
    }
    return false;
}

} // namespace

void RecordedVoice::Tract::fromLsf(const float *lsf, float *k) {
    constexpr int p = kOrder, half = kOrder / 2;
    // Each set's roots multiplied out, (1 - 2 cos w z^-1 + z^-2) each, then
    // the roots at -1 and 1 put back, and A = (P + Q) / 2.
    double ps[p + 2] = {1.0}, qs[p + 2] = {1.0};
    int32_t degree = 0;
    for (int32_t r = 0; r < half; ++r) {
        const double cp = -2.0 * std::cos(static_cast<double>(lsf[r]));
        const double cq = -2.0 * std::cos(static_cast<double>(lsf[half + r]));
        for (int32_t i = degree + 2; i >= 0; --i) {
            const double p1 = i >= 1 ? ps[i - 1] : 0.0, p2 = i >= 2 ? ps[i - 2] : 0.0;
            const double q1 = i >= 1 ? qs[i - 1] : 0.0, q2 = i >= 2 ? qs[i - 2] : 0.0;
            ps[i] = (i <= degree ? ps[i] : 0.0) + cp * p1 + p2;
            qs[i] = (i <= degree ? qs[i] : 0.0) + cq * q1 + q2;
        }
        degree += 2;
    }
    double a[p + 1];
    for (int32_t i = 0; i <= p; ++i) {
        const double pFull = ps[i] + (i >= 1 ? ps[i - 1] : 0.0);
        const double qFull = qs[i] - (i >= 1 ? qs[i - 1] : 0.0);
        a[i] = 0.5 * (pFull + qFull);
    }
    // And back to reflection coefficients, stepping the order down.
    for (int32_t m = p; m >= 1; --m) {
        const double km = std::clamp(a[m], -0.9999, 0.9999);
        k[m - 1] = static_cast<float>(km);
        const double scale = 1.0 / (1.0 - km * km);
        double prev[p + 1];
        for (int32_t j = 1; j < m; ++j) prev[j] = (a[j] - km * a[m - j]) * scale;
        for (int32_t j = 1; j < m; ++j) a[j] = prev[j];
    }
}

void RecordedVoice::analyseTract(const audio::Utterance &u, float sampleRate, Tract &t, bool lsf) {
    constexpr int p = Tract::kOrder;
    const auto frames = static_cast<size_t>(u.frames);
    const size_t marks = u.epochs.size();
    t.k.assign(marks * p, 0.0f);
    t.gain.assign(marks, 0.0f);
    if (lsf) t.lsf.assign(marks * p, 0.0f);
    t.source.assign(frames, 0.0f);
    if (frames < 2 || marks == 0) return;
    std::vector<float> pre(frames);
    pre[0] = u.mono[0];
    for (size_t i = 1; i < frames; ++i) pre[i] = u.mono[i] - kEmphasis * u.mono[i - 1];

    std::vector<float> a(marks * (p + 1), 0.0f);
    std::vector<float> w;
    double r[p + 1];
    for (size_t m = 0; m < marks; ++m) {
        const audio::Epoch &e = u.epochs[m];
        // Two periods around the mark, and never less than 10 ms nor more than 50.
        const float period = std::max(e.period, 1.0f);
        const auto len = static_cast<int32_t>(std::clamp(2.0f * period, 0.01f * sampleRate, 0.05f * sampleRate));
        const int32_t from = std::clamp(e.at + static_cast<int32_t>(0.5f * period) - len / 2, 0, std::max(0, u.frames - len));
        const int32_t n = std::min(len, u.frames - from);
        w.resize(static_cast<size_t>(n));
        double energy = 0.0;
        for (int32_t i = 0; i < n; ++i) {
            const float hann = 0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(i) / static_cast<float>(n - 1));
            w[static_cast<size_t>(i)] = pre[static_cast<size_t>(from + i)] * hann;
            energy += static_cast<double>(hann) * hann;
        }
        // Four sums at once, in float, which the compiler can vectorise: in
        // double, one at a time, the whole voice's analysis more than doubled
        // its loading.
        for (int32_t lag = 0; lag <= p; ++lag) {
            const float *x = w.data() + lag, *y = w.data();
            const int32_t count = n - lag;
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
            int32_t i = 0;
            for (; i + 4 <= count; i += 4) {
                s0 += x[i] * y[i];
                s1 += x[i + 1] * y[i + 1];
                s2 += x[i + 2] * y[i + 2];
                s3 += x[i + 3] * y[i + 3];
            }
            for (; i < count; ++i) s0 += x[i] * y[i];
            r[lag] = static_cast<double>(s0) + s1 + s2 + s3;
        }
        // A floor 30 dB down, and each lag narrowed a little (a lag window,
        // about 60 Hz), so a take with no noise in it, whose spectrum falls
        // 60 dB between harmonics, doesn't give a throat that rings on them.
        // At 1.0001 alone the built-in voice's own ee lost its second formant.
        r[0] = r[0] * 1.001 + 1e-12;
        for (int32_t lag = 1; lag <= p; ++lag) {
            const double x = 2.0 * 3.14159265358979 * 60.0 * lag / sampleRate;
            r[lag] *= std::exp(-0.5 * x * x);
        }
        // Levinson-Durbin: A(z) = 1 + a1 z^-1 + ... + ap z^-p.
        double coef[p + 1] = {1.0};
        double err = r[0];
        for (int32_t order = 1; order <= p; ++order) {
            double sum = r[order];
            for (int32_t j = 1; j < order; ++j) sum += coef[j] * r[order - j];
            const double k = err > 0.0 ? -sum / err : 0.0;
            double next[p + 1];
            for (int32_t j = 0; j <= p; ++j) next[j] = coef[j];
            for (int32_t j = 1; j < order; ++j) next[j] = coef[j] + k * coef[order - j];
            next[order] = k;
            for (int32_t j = 0; j <= p; ++j) coef[j] = next[j];
            err *= (1.0 - k * k);
            t.k[m * p + static_cast<size_t>(order - 1)] = static_cast<float>(k);
        }
        for (int32_t j = 0; j <= p; ++j) a[m * (p + 1) + static_cast<size_t>(j)] = static_cast<float>(coef[j]);
        if (lsf && !toLsf(coef, p, &t.lsf[m * p])) t.lsf[m * p] = -1.0f;
        t.gain[m] = static_cast<float>(std::sqrt(std::max(0.0, err) / std::max(energy, 1e-9)));
    }
    // The source: each stretch from one mark to the next through its own
    // mark's inverse, then the tilt put back.
    for (size_t m = 0; m < marks; ++m) {
        const auto begin = static_cast<size_t>(std::max(0, m == 0 ? 0 : u.epochs[m].at));
        const size_t end = m + 1 < marks ? static_cast<size_t>(std::max(0, u.epochs[m + 1].at)) : frames;
        const float *coef = &a[m * (p + 1)];
        for (size_t i = begin; i < end && i < frames; ++i) {
            if (i < static_cast<size_t>(p)) {
                float sum = pre[i];
                for (int32_t j = 1; static_cast<size_t>(j) <= i; ++j) sum += coef[j] * pre[i - static_cast<size_t>(j)];
                t.source[i] = sum;
                continue;
            }
            const float *back = &pre[i - static_cast<size_t>(p)];
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
            for (int32_t j = 0; j < p; j += 4) {
                s0 += coef[p - j] * back[j];
                s1 += coef[p - j - 1] * back[j + 1];
                s2 += coef[p - j - 2] * back[j + 2];
                s3 += coef[p - j - 3] * back[j + 3];
            }
            t.source[i] = pre[i] + s0 + s1 + s2 + s3;
        }
    }
    for (size_t i = 1; i < frames; ++i) t.source[i] += kEmphasis * t.source[i - 1];
}

namespace {

/** A take as mono, or null with [error] set. */
std::unique_ptr<SampleData> readTake(const std::string &path, float sampleRate, std::string &error) {
    std::unique_ptr<SampleData> data = WavReader::read(path, static_cast<int32_t>(sampleRate), error);
    if (data == nullptr || data->frames <= 0) {
        if (error.empty()) error = "couldn't be read";
        return nullptr;
    }
    return data;
}

void copyMono(const SampleData &data, int32_t from, int32_t to, std::vector<float> &out) {
    out.resize(static_cast<size_t>(to - from));
    for (int32_t i = from; i < to; ++i) {
        const auto k = static_cast<size_t>(i);
        out[static_cast<size_t>(i - from)] = data.stereo ? 0.5f * (data.left[k] + data.right[k]) : data.left[k];
    }
}

double powerOver(const std::vector<float> &x, int32_t from, int32_t to) {
    double power = 0.0;
    from = std::max(0, from);
    to = std::min(static_cast<int32_t>(x.size()), to);
    for (int32_t i = from; i < to; ++i) power += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
    return to > from ? power / (to - from) : 0.0;
}

} // namespace

std::unique_ptr<RecordedVoice> RecordedVoice::fromSpec(const std::string &spec, float sampleRate, int threads,
                                                       std::string &error) {
    std::vector<std::vector<std::string>> lines;
    bool lsf = false;
    size_t at = 0;
    while (at < spec.size()) {
        size_t end = spec.find('\n', at);
        if (end == std::string::npos) end = spec.size();
        const std::string line = spec.substr(at, end - at);
        at = end + 1;
        std::vector<std::string> f;
        size_t from = 0;
        for (size_t bar; (bar = line.find('|', from)) != std::string::npos; from = bar + 1) f.push_back(line.substr(from, bar - from));
        f.push_back(line.substr(from));
        if (f.size() >= 5) lines.push_back(std::move(f));
        else if (line == "LSF") lsf = true;
    }
    // Each thread builds a voice of its own from a run of the lines, and they
    // go together in order, so the result doesn't depend on the threads.
    const auto parts = static_cast<size_t>(std::clamp(threads, 1, 8));
    std::vector<RecordedVoice> built(parts);
    for (RecordedVoice &b : built) b.wantLsf = lsf;
    std::vector<std::string> errors(parts);
    auto work = [&](size_t part) {
        const size_t begin = lines.size() * part / parts, end = lines.size() * (part + 1) / parts;
        for (size_t i = begin; i < end; ++i) {
            const auto &f = lines[i];
            auto code = [](const std::string &name) { return phoneCode(name.c_str(), static_cast<int32_t>(name.size())); };
            std::string why;
            bool added = false;
            if (f.size() == 5 && f[0] == "V" && code(f[1]) > 0) {
                added = built[part].addVowel(f[2], static_cast<uint8_t>(code(f[1])), std::atoi(f[3].c_str()), std::atoi(f[4].c_str()), sampleRate, why);
            } else if (f.size() == 7 && f[0] == "D" && code(f[1]) > 0) {
                added = built[part].addDiphthong(f[2], static_cast<uint8_t>(code(f[1])), std::atoi(f[3].c_str()), std::atoi(f[4].c_str()),
                                                 std::atoi(f[5].c_str()), std::atoi(f[6].c_str()), sampleRate, why);
            } else if (f.size() == 6 && f[0] == "C" && code(f[1]) > 0 && code(f[2]) > 0) {
                added = built[part].addConsonant(f[3], static_cast<uint8_t>(code(f[1])), static_cast<uint8_t>(code(f[2])),
                                                 std::atoi(f[4].c_str()), std::atoi(f[5].c_str()), sampleRate, why);
            } else {
                continue;
            }
            if (!added && errors[part].empty()) errors[part] = f[1] + ": " + (why.empty() ? "unknown sound" : why);
        }
    };
    std::vector<std::thread> pool;
    for (size_t part = 1; part < parts; ++part) pool.emplace_back(work, part);
    work(0);
    for (auto &t : pool) t.join();
    auto voice = std::make_unique<RecordedVoice>();
    voice->wantLsf = lsf;
    for (size_t part = 0; part < parts; ++part) {
        for (auto &u : built[part].vowels) voice->vowels.push_back(std::move(u));
        for (auto &u : built[part].diphthongs) voice->diphthongs.push_back(std::move(u));
        for (auto &j : built[part].joins) voice->joins.push_back(std::move(j));
        if (error.empty()) error = errors[part];
    }
    return voice;
}

const RecordedVoice::Unit *RecordedVoice::nearest(const float f[3]) const {
    const Unit *best = nullptr;
    float bestDistance = 1e30f;
    for (const Unit &u : vowels) {
        const float d1 = std::log2(f[0] / u.f[0]), d2 = std::log2(f[1] / u.f[1]), d3 = std::log2(f[2] / u.f[2]);
        const float d = d1 * d1 + d2 * d2 + d3 * d3;
        if (d < bestDistance) { bestDistance = d; best = &u; }
    }
    return best;
}

const RecordedVoice::Unit *RecordedVoice::forPhone(uint8_t phone) const {
    int32_t count = 0;
    const Phone *table = phoneTable(count);
    if (phone == 0 || phone >= count) return nullptr;
    // The sounds a voice isn't recorded singing, and the one it sings them with.
    static const char *const kStandIns[][2] = {
        {"AX", "AH"}, {"IHC", "IH"}, {"EHC", "EH"}, {"AEC", "AE"},
        {"OC", "AA"}, {"OR", "AO"},
    };
    const char *name = table[phone].name;
    for (const auto &pair : kStandIns) {
        if (std::strcmp(name, pair[0]) == 0) { name = pair[1]; break; }
    }
    for (const Unit &u : vowels) {
        if (std::strcmp(table[u.phone].name, name) == 0) return &u;
    }
    return nullptr;
}

const RecordedVoice::Join *RecordedVoice::consonant(uint8_t phone, const float vowel[3], From from) const {
    int32_t count = 0;
    const Phone *table = phoneTable(count);
    if (phone == 0 || phone >= count) return nullptr;
    const char *name = std::strcmp(table[phone].name, "DX") == 0 ? "D" : table[phone].name;
    const char *carrier = from == From::Ah ? "AA" : from == From::Ee ? "IY" : from == From::Oo ? "UW" : nullptr;
    if (carrier != nullptr) {
        for (const Join &j : joins) {
            if (std::strcmp(table[j.phone].name, name) == 0 && std::strcmp(table[j.carrier].name, carrier) == 0) return &j;
        }
    }
    const Join *best = nullptr;
    float bestDistance = 1e30f;
    for (const Join &j : joins) {
        if (std::strcmp(table[j.phone].name, name) != 0) continue;
        // By how far forward the vowel is (its second formant), which is
        // what the way into and out of a consonant carries: by all three,
        // your's vowel went with the ah take, and your came out ya.
        const float d = std::fabs(std::log2(vowel[1] / j.f[1]));
        if (d < bestDistance) { bestDistance = d; best = &j; }
    }
    return best;
}

bool RecordedVoice::addConsonant(const std::string &path, uint8_t phone, uint8_t carrier, int32_t from, int32_t to,
                                 float sampleRate, std::string &error) {
    int32_t count = 0;
    const Phone *table = phoneTable(count);
    if (phone == 0 || phone >= count || carrier == 0 || carrier >= count) { error = "no such sound"; return false; }
    const std::unique_ptr<SampleData> data = readTake(path, sampleRate, error);
    if (data == nullptr) return false;
    const auto keep = static_cast<int32_t>(kJoinSeconds * sampleRate);
    const int32_t start = std::clamp(from - keep, 0, data->frames);
    const int32_t end = std::clamp(to + keep, start, data->frames);
    if (to <= from || end - start < 2) { error = "too short"; return false; }

    Join join;
    join.phone = phone;
    join.carrier = carrier;
    std::copy(table[carrier].f, table[carrier].f + 3, join.f);
    copyMono(*data, start, end, join.sound.mono);
    join.sound.frames = end - start;
    join.sound.name = table[phone].name;
    join.sound.analyse(sampleRate);
    if (!join.sound.usable()) { error = "no pitch found"; return false; }
    analyseTract(join.sound, sampleRate, join.tract, wantLsf);
    join.from = std::clamp(from, start, end) - start;
    join.to = std::clamp(to, start, end) - start;
    // Its level from the vowels either side, clear of the consonant, so a
    // quiet consonant stays as quiet against them as it was sung.
    const auto clear = static_cast<int32_t>(0.03f * sampleRate);
    const double before = powerOver(join.sound.mono, 0, join.from - clear);
    const double after = powerOver(join.sound.mono, join.to + clear, join.sound.frames);
    const auto rms = static_cast<float>(std::sqrt(std::max(before, after)));
    join.gain = rms > 1e-5f ? Throat::kLevel / rms : 0.0f;
    const auto loudest = static_cast<float>(std::sqrt(powerOver(join.sound.mono, join.from, join.to)));
    const float ceiling = rms * kConsonantCeiling;
    join.consonantGain = loudest > ceiling && loudest > 0.0f ? ceiling / loudest : 1.0f;
    // How bright a sibilant is: where its energy above 2 kHz is centred. Only
    // an S, Z, SH or ZH, whose hiss has a peak; an F or TH is flat to the
    // top and measures bright whoever sings it.
    const char *name = table[phone].name;
    const bool sibilant = std::strcmp(name, "S") == 0 || std::strcmp(name, "Z") == 0 || std::strcmp(name, "SH") == 0 ||
                          std::strcmp(name, "ZH") == 0;
    if (sibilant) join.bright = centreAbove2k(join.sound.mono, join.from, join.to, sampleRate) > 8500.0f;
    // A stop's burst: the sharpest rise in level inside it, 1 ms against the
    // 5 ms before. After an S a stop is only its closure and burst; the
    // breath after the burst belongs to a stop that starts a word.
    const Kind kind = table[phone].kind;
    if (kind == Kind::Stop || kind == Kind::Affricate) {
        const auto ms = static_cast<int32_t>(0.001f * sampleRate);
        float bestRise = -1e30f;
        for (int32_t i = join.from + 5 * ms; i + ms <= join.to; i += ms) {
            const double now = powerOver(join.sound.mono, i, i + ms);
            const double before = powerOver(join.sound.mono, i - 5 * ms, i);
            const auto rise = static_cast<float>(10.0 * std::log10((now + 1e-12) / (before + 1e-12)));
            if (rise > bestRise) { bestRise = rise; join.burst = i; }
        }
    }
    joins.push_back(std::move(join));
    return true;
}

const RecordedVoice::Unit *RecordedVoice::diphthong(uint8_t phone) const {
    int32_t count = 0;
    const Phone *table = phoneTable(count);
    if (phone == 0 || phone >= count) return nullptr;
    static const char *const kVersions[][2] = {
        {"OWP", "OW"}, {"EYP", "EY"}, {"AYC", "AY"}, {"AWC", "AW"}, {"AWP", "AW"}, {"EG", "EY"},
    };
    const char *name = table[phone].name;
    for (const auto &pair : kVersions) {
        if (std::strcmp(name, pair[0]) == 0) { name = pair[1]; break; }
    }
    for (const Unit &u : diphthongs) {
        if (std::strcmp(table[u.phone].name, name) == 0) return &u;
    }
    return nullptr;
}

bool RecordedVoice::addDiphthong(const std::string &path, uint8_t phone, int32_t holdFrom, int32_t holdTo,
                                 int32_t glideFrom, int32_t glideTo, float sampleRate, std::string &error) {
    // As a vowel held from holdFrom to holdTo, but kept on to past the move.
    const int32_t end = std::max(holdTo, glideTo + static_cast<int32_t>(0.06f * sampleRate));
    if (!addVowel(path, phone, holdFrom, end, sampleRate, error)) return false;
    Unit unit = std::move(vowels.back());
    vowels.pop_back();
    const int32_t start = unit.from; // holdFrom, in the unit's frames
    const int32_t shift = holdFrom - start;
    unit.to = std::clamp(holdTo - shift, unit.from + 1, unit.sound.frames);
    unit.glideFrom = std::clamp(glideFrom - shift, unit.to, unit.sound.frames - 1);
    unit.glideTo = std::clamp(glideTo - shift, unit.glideFrom + 1, unit.sound.frames);
    // Its level from the held vowel alone.
    const auto rms = static_cast<float>(std::sqrt(powerOver(unit.sound.mono, unit.from, unit.to)));
    unit.gain = rms > 1e-5f ? Throat::kLevel / rms : 0.0f;
    diphthongs.push_back(std::move(unit));
    return true;
}

bool RecordedVoice::addVowel(const std::string &path, uint8_t phone, int32_t holdFrom, int32_t holdTo, float sampleRate,
                             std::string &error) {
    int32_t count = 0;
    const Phone *table = phoneTable(count);
    if (phone == 0 || phone >= count) { error = "no such sound"; return false; }
    const std::unique_ptr<SampleData> data = readTake(path, sampleRate, error);
    if (data == nullptr) return false;
    const auto margin = static_cast<int32_t>(kMarginSeconds * sampleRate);
    const int32_t from = std::clamp(holdFrom - margin, 0, data->frames);
    const int32_t to = std::clamp(holdTo + margin, from, data->frames);
    if (holdTo - holdFrom < static_cast<int32_t>(0.1f * sampleRate) || to - from < 2) { error = "too short"; return false; }

    Unit unit;
    unit.phone = phone;
    std::copy(table[phone].f, table[phone].f + 3, unit.f);
    auto &mono = unit.sound.mono;
    copyMono(*data, from, to, mono);
    unit.sound.frames = to - from;
    unit.sound.name = table[phone].name;
    unit.sound.analyse(sampleRate);
    if (!unit.sound.usable()) { error = "no pitch found"; return false; }
    analyseTract(unit.sound, sampleRate, unit.tract, wantLsf);
    unit.from = std::clamp(holdFrom, from, to) - from;
    unit.to = std::clamp(holdTo, from, to) - from;

    // Its level over the steady part, brought to the built-in voice's.
    const auto rms = static_cast<float>(std::sqrt(powerOver(mono, unit.from, unit.to)));
    unit.gain = rms > 1e-5f ? Throat::kLevel / rms : 0.0f;
    vowels.push_back(std::move(unit));
    return true;
}

} // namespace acidulous::machine::diction
