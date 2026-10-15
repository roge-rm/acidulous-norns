#include "Throat.h"

#include <algorithm>
#include <cmath>

namespace acidulous::machine::diction {

namespace {

constexpr double kPi = 3.14159265358979323846;

/**
 * The folds' flow over one period: opening over 40% of it, closing faster over
 * the next 16%, then shut. It's the closing that excites the throat.
 */
double flowAt(double t) {
    constexpr double open = 0.40, close = 0.16;
    if (t < open) return 0.5 * (1.0 - std::cos(kPi * t / open));
    if (t < open + close) return std::cos(kPi * (t - open) / (2.0 * close));
    return 0.0;
}

constexpr int32_t kFlowPoints = 2048;
/** Frequencies the air's level is added up over. */
constexpr int32_t kGrid = 48;
/** The nose: a resonance and a notch. Level with each other, they cancel and the nose is shut. */
constexpr float kNasalHz = 270.0f, kNasalWidth = 100.0f;
/** The highest harmonic a level is worked out from. */
constexpr float kLevelsUpTo = 6000.0f;
/** Uniform noise from -1 to 1 has this RMS. */
const float kNoiseRms = 1.0f / std::sqrt(3.0f);

/** A resonator's |a / (1 - b e^-jw - c e^-2jw)| squared, from cos and sin of w. */
inline float resonancePower(float a, float b, float c, float cw, float sw) {
    const float c2 = 2.0f * cw * cw - 1.0f, s2 = 2.0f * sw * cw;
    const float re = 1.0f - b * cw - c * c2, im = b * sw + c * s2;
    return a * a / std::max(1e-20f, re * re + im * im);
}

/** A biquad's |H| squared at w, from cos and sin of w. */
inline float biquadPower(const Shelf &q, float cw, float sw) {
    const float c2 = 2.0f * cw * cw - 1.0f, s2 = 2.0f * sw * cw;
    const float nr = q.b0 + q.b1 * cw + q.b2 * c2, ni = -(q.b1 * sw + q.b2 * s2);
    const float dr = 1.0f + q.a1 * cw + q.a2 * c2, di = -(q.a1 * sw + q.a2 * s2);
    return (nr * nr + ni * ni) / std::max(1e-20f, dr * dr + di * di);
}

} // namespace

void Resonator::set(float hz, float bw, float sr) {
    const float nyquist = 0.49f * sr;
    hz = std::clamp(hz, 20.0f, nyquist);
    c = -std::exp(-2.0f * static_cast<float>(kPi) * bw / sr);
    b = 2.0f * std::exp(-static_cast<float>(kPi) * bw / sr) * std::cos(2.0f * static_cast<float>(kPi) * hz / sr);
    a = 1.0f - b - c;
}

void AntiResonator::set(float hz, float bw, float sr) {
    Resonator r;
    r.set(hz, bw, sr);
    a = 1.0f / r.a;
    b = -r.b / r.a;
    c = -r.c / r.a;
}

void Bandpass::set(float hz, float width, float sr) {
    hz = std::clamp(hz, 20.0f, 0.45f * sr);
    const float w = 2.0f * static_cast<float>(kPi) * hz / sr;
    const float q = std::max(0.3f, hz / std::max(1.0f, width));
    const float alpha = std::sin(w) / (2.0f * q), a0 = 1.0f + alpha;
    b0 = alpha / a0;
    b2 = -alpha / a0;
    a1 = -2.0f * std::cos(w) / a0;
    a2 = (1.0f - alpha) / a0;
}

void Highpass::set(float hz, float sr) {
    const float w = 2.0f * static_cast<float>(kPi) * hz / sr, c = std::cos(w);
    const float alpha = std::sin(w) / (2.0f * 0.7071f), a0 = 1.0f + alpha;
    b0 = (1.0f + c) / 2.0f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
}

void Shelf::set(float hz, float db, float sr) {
    const float A = std::pow(10.0f, db / 40.0f);
    const float w = 2.0f * static_cast<float>(kPi) * hz / sr, c = std::cos(w);
    const float alpha = std::sin(w) / 2.0f * std::sqrt(2.0f), root = 2.0f * std::sqrt(A) * alpha;
    const float a0 = (A + 1) - (A - 1) * c + root;
    b0 = A * ((A + 1) + (A - 1) * c + root) / a0;
    b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    b2 = A * ((A + 1) + (A - 1) * c - root) / a0;
    a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    a2 = ((A + 1) - (A - 1) * c - root) / a0;
}

float shiftFormant(float hz, float ratio) {
    // A big voice still has the ring near 3 kHz that carries it; without it a
    // lowered voice goes hollow rather than big. A small one doesn't go shrill.
    const float e = hz <= 1200.0f ? 1.0f : hz >= 1800.0f ? 0.35f : 1.0f - 0.65f * (hz - 1200.0f) / 600.0f;
    return hz * std::pow(ratio, e);
}

void Throat::prepare(float sr) {
    sampleRate = sr;
    flowTable.resize(kFlowPoints + 1);
    for (int32_t i = 0; i <= kFlowPoints; ++i) flowTable[static_cast<size_t>(i)] = static_cast<float>(flowAt(static_cast<double>(i) / kFlowPoints));

    // The pulse as the machine makes it (the change in flow from one sample
    // to the next), taken apart into its harmonics. Worked out at a low
    // pitch, so there are harmonics enough for the lowest note; the pulse's
    // harmonics are the same at any pitch, and only their size changes.
    const auto n = static_cast<int32_t>(std::lround(sr / kLowestHz));
    std::vector<double> x(static_cast<size_t>(n));
    double previous = 0.0;
    for (int32_t i = 0; i < n; ++i) {
        const double g = flowAt(static_cast<double>(i) / n);
        x[static_cast<size_t>(i)] = g - previous;
        previous = g;
    }
    // At the reference pitch the pulse is bigger by the ratio of the pitches,
    // since it changes as much in fewer samples.
    const double toReference = (kReferenceHz / kLowestHz) * (kReferenceHz / kLowestHz);
    harmonicPower.clear();
    for (int32_t h = 1; h < n / 2; ++h) {
        double re = 0.0, im = 0.0, c = 1.0, sn = 0.0;
        const double stepC = std::cos(2.0 * kPi * h / n), stepS = std::sin(2.0 * kPi * h / n);
        for (int32_t i = 0; i < n; ++i) {
            re += x[static_cast<size_t>(i)] * c;
            im -= x[static_cast<size_t>(i)] * sn;
            const double nc = c * stepC - sn * stepS;
            sn = c * stepS + sn * stepC;
            c = nc;
        }
        // Both halves of the spectrum, over the period's length squared (Parseval).
        harmonicPower.push_back(static_cast<float>(toReference * 2.0 * (re * re + im * im) / (static_cast<double>(n) * n)));
    }

    gridCos.resize(kGrid);
    gridSin.resize(kGrid);
    gridCut.resize(kGrid);
    Highpass cut;
    cut.set(1000.0f, sr);
    for (int32_t m = 0; m < kGrid; ++m) {
        const double w = kPi * (m + 0.5) / kGrid;
        gridCos[static_cast<size_t>(m)] = static_cast<float>(std::cos(w));
        gridSin[static_cast<size_t>(m)] = static_cast<float>(std::sin(w));
        // |H| of the high-pass, twice over.
        const double cw = std::cos(w), c2 = std::cos(2 * w), sw = std::sin(w), s2 = std::sin(2 * w);
        const double nr = cut.b0 + cut.b1 * cw + cut.b2 * c2, ni = -(cut.b1 * sw + cut.b2 * s2);
        const double dr = 1.0 + cut.a1 * cw + cut.a2 * c2, di = -(cut.a1 * sw + cut.a2 * s2);
        const double once = (nr * nr + ni * ni) / (dr * dr + di * di);
        gridCut[static_cast<size_t>(m)] = static_cast<float>(once * once);
    }
    reset();
}

void Throat::reset() {
    nasalPole.set(kNasalHz, kNasalWidth, sampleRate);
    nasalZero.set(kNasalHz, kNasalWidth, sampleRate);
    nasalPole.clear();
    nasalZero.clear();
    for (auto &r : voice) r.clear();
    for (auto &r : air) r.clear();
    airCut1.set(1000.0f, sampleRate);
    airCut2.set(1000.0f, sampleRate);
    airCut1.clear();
    airCut2.clear();
    hiss1.clear();
    hiss2.clear();
    ringDb = 0.0f;
    ring.set(1500.0f, 0.0f, sampleRate);
    ring.clear();
    airRinging = hissRinging = 0;
}

void Throat::setShape(const Shape &s) {
    nasalZero.set(kNasalHz + 200.0f * s.nasal, kNasalWidth, sampleRate);
    for (int32_t k = 0; k < kFormants; ++k) {
        voice[k].set(s.f[k], s.bw[k], sampleRate);
        // Air through an open glottis damps the throat: twice as wide.
        air[k].set(s.f[k], 2.0f * s.bw[k], sampleRate);
    }
}

void Throat::setRing(float db) {
    db = std::clamp(db, -18.0f, 18.0f);
    if (std::fabs(db - ringDb) < 0.05f) return;
    ringDb = db;
    ring.set(1500.0f, db, sampleRate);
}

void Throat::setHiss(const float band[2][3]) {
    hiss1.set(band[0][0], band[0][1], sampleRate);
    hiss2.set(band[1][0], band[1][1], sampleRate);
    // A band of noise [width] wide carries pi * width / rate of the noise's
    // power, near enough. Each is scaled to its share of kLevel.
    const float total = std::sqrt(band[0][2] * band[0][2] + band[1][2] * band[1][2]);
    for (int32_t k = 0; k < 2; ++k) {
        const float width = std::clamp(band[k][1], 1.0f, 0.4f * sampleRate);
        const float carried = kNoiseRms * std::sqrt(static_cast<float>(kPi) * width / sampleRate);
        hissShare[k] = total > 0.0f ? kLevel * band[k][2] / total / carried : 0.0f;
    }
}

void Throat::levelsFor(const Shape &s, float ringDb, float hz, float &voiceGain, float &airGain) const {
    Resonator v[kFormants], a[kFormants], pole;
    AntiResonator zero;
    for (int32_t k = 0; k < kFormants; ++k) {
        v[k].set(s.f[k], s.bw[k], sampleRate);
        a[k].set(s.f[k], 2.0f * s.bw[k], sampleRate);
    }
    pole.set(kNasalHz, kNasalWidth, sampleRate);
    zero.set(kNasalHz + 200.0f * s.nasal, kNasalWidth, sampleRate);
    Shelf lift;
    lift.set(1500.0f, std::clamp(ringDb, -18.0f, 18.0f), sampleRate);

    // Harmonic by harmonic at the pitch sung, stepping round the circle
    // rather than working out each one's angle. Only up to kLevelsUpTo: the
    // pulse's harmonics fall away fast enough that the rest are a fraction of
    // a dB, and summing them made a note's first block the costliest.
    double power = 0.0;
    const float w = 2.0f * static_cast<float>(kPi) * std::max(kLowestHz, hz) / sampleRate;
    const float top = 2.0f * static_cast<float>(kPi) * std::min(kLevelsUpTo, 0.5f * sampleRate) / sampleRate;
    const float stepC = std::cos(w), stepS = std::sin(w);
    float cw = stepC, sw = stepS;
    for (size_t h = 0; h < harmonicPower.size() && static_cast<float>(h + 1) * w < top; ++h) {
        float g = resonancePower(pole.a, pole.b, pole.c, cw, sw);
        // The notch is the resonance's inverse.
        g /= resonancePower(1.0f / zero.a, -zero.b / zero.a, -zero.c / zero.a, cw, sw);
        for (const auto &r : v) g *= resonancePower(r.a, r.b, r.c, cw, sw);
        g *= biquadPower(lift, cw, sw);
        power += harmonicPower[h] * g;
        const float nc = cw * stepC - sw * stepS;
        sw = cw * stepS + sw * stepC;
        cw = nc;
    }
    voiceGain = power > 1e-20 ? kLevel / static_cast<float>(std::sqrt(power)) : 0.0f;

    double airPower = 0.0;
    for (int32_t m = 0; m < kGrid; ++m) {
        float g = gridCut[static_cast<size_t>(m)];
        for (const auto &r : a) g *= resonancePower(r.a, r.b, r.c, gridCos[static_cast<size_t>(m)], gridSin[static_cast<size_t>(m)]);
        g *= biquadPower(lift, gridCos[static_cast<size_t>(m)], gridSin[static_cast<size_t>(m)]);
        airPower += g;
    }
    airPower = airPower / kGrid * kNoiseRms * kNoiseRms;
    airGain = airPower > 1e-20 ? kLevel / static_cast<float>(std::sqrt(airPower)) : 0.0f;
}

float Throat::flow(float t) const {
    const float p = std::clamp(t, 0.0f, 1.0f) * kFlowPoints;
    const auto i = std::min(static_cast<int32_t>(p), kFlowPoints - 1);
    const float f = p - static_cast<float>(i);
    return flowTable[static_cast<size_t>(i)] + (flowTable[static_cast<size_t>(i + 1)] - flowTable[static_cast<size_t>(i)]) * f;
}

} // namespace acidulous::machine::diction
