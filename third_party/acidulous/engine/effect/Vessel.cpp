#include "Vessel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/inputmod/Scales.h>

namespace acidulous::effect {

namespace {

constexpr int kModes = Vessel::kModes;

/**
 * A material: its first mode ratios (the rest carry on at the last step),
 * how fast the levels fall up the series, how long it rings against the
 * `decay` knob, and how much faster its high modes die.
 */
struct Body {
    int given;
    float ratio[24];
    float tilt;     // mode k's level is (k + 1)^-tilt
    float ring;     // times `decay`
    float loss;     // a mode's t60 falls as ratio^-loss, scaled by damp
    float levelDb;  // brings the wet to the others' level on a song
};

constexpr Body kSpecs[Vessel::KindCount] = {
    // Glass: a bowl, close to a bell, slow to die.
    {24, {1.000f, 2.000f, 3.010f, 4.170f, 5.430f, 6.790f, 8.210f, 9.700f, 11.24f, 12.83f, 14.47f, 16.15f,
          17.87f, 19.63f, 21.42f, 23.25f, 25.11f, 27.00f, 28.92f, 30.87f, 32.85f, 34.86f, 36.89f, 38.95f},
     0.5f, 1.4f, 0.35f, 2.6f},
    // Wood: the knocks of wooden bars, blocks and a slit log merged, high
    // modes gone fast.
    {12, {1.000f, 1.950f, 2.300f, 3.000f, 3.200f, 3.990f, 4.100f, 6.000f, 9.950f, 14.40f, 17.60f, 21.30f},
     0.9f, 0.35f, 1.4f, -2.0f},
    // Metal: square roots, inharmonic, long.
    {24, {1.000f, 1.414f, 1.732f, 2.236f, 2.646f, 3.162f, 3.606f, 4.123f, 4.583f, 5.099f, 5.568f, 6.083f,
          6.557f, 7.071f, 7.550f, 8.062f, 8.544f, 9.055f, 9.539f, 10.05f, 10.54f, 11.05f, 11.53f, 12.04f},
     0.4f, 1.6f, 0.25f, 3.6f},
    // Skin: a circular drumhead.
    {24, {1.000f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f, 3.600f, 3.652f, 4.060f, 4.154f,
          4.230f, 4.378f, 4.832f, 5.000f, 5.412f, 5.550f, 5.892f, 6.155f, 6.500f, 6.780f, 7.100f, 7.500f},
     0.8f, 0.5f, 0.8f, -1.1f},
    // Stone: a slab free at both ends, its overtones far apart, dull.
    {24, {1.000f, 2.756f, 5.404f, 8.933f, 13.34f, 18.64f, 24.82f, 31.87f, 39.80f, 48.60f, 58.28f, 68.83f,
          80.25f, 92.55f, 105.7f, 119.7f, 134.6f, 150.3f, 166.9f, 184.3f, 202.6f, 221.7f, 241.7f, 262.5f},
     0.7f, 0.45f, 1.2f, -1.7f},
    // Bell: a hanging tube, the note heard an octave under its fourth mode,
    // then a bronze disc's crowded modes on up.
    {12, {0.618f, 1.209f, 2.000f, 2.310f, 2.987f, 4.040f, 4.170f, 5.150f, 6.180f, 7.200f, 8.270f, 9.400f},
     0.35f, 2.0f, 0.3f, -0.3f},
    // Plate: dense, close-packed modes.
    {24, {1.000f, 2.040f, 3.010f, 3.660f, 4.580f, 5.540f, 6.120f, 7.020f, 7.940f, 8.480f, 9.320f, 10.15f,
          10.90f, 11.72f, 12.48f, 13.31f, 14.09f, 14.92f, 15.70f, 16.53f, 17.31f, 18.14f, 18.92f, 19.75f},
     0.45f, 1.0f, 0.5f, 1.9f},
    // Tube, open: the harmonic series.
    {2, {1.000f, 2.000f}, 0.6f, 0.8f, 0.6f, 1.2f},
};

/** The full 32 ratios of each material, the given ones carried on at their last step. */
struct Tables {
    float ratio[Vessel::KindCount][kModes];
    float amp[Vessel::KindCount][kModes];
    Tables() {
        for (int m = 0; m < Vessel::KindCount; ++m) {
            const Body &s = kSpecs[m];
            for (int k = 0; k < kModes; ++k) {
                if (k < s.given) ratio[m][k] = s.ratio[k];
                else ratio[m][k] = 2.0f * ratio[m][k - 1] - ratio[m][k - 2];
                amp[m][k] = std::pow(static_cast<float>(k + 1), -s.tilt);
            }
        }
    }
};
// Built when the library loads, never on the audio thread.
const Tables kTables;

/** The highest a mode may sit, as a share of the sample rate. */
constexpr float kTop = 0.45f;
/** The level drive is normalised on: a track at -12 dBFS goes in at the same level at any drive. */
constexpr float kNominal = 0.25f;
/** The wet is held to this many times the input's peak. */
constexpr float kGuard = 4.0f;
/** And its power to this many times the input's, about +3 dB. */
constexpr float kPowerGuard = 2.0f;

/** Where pink noise and white noise of the same level have the same density, roughly. */
constexpr double kPinkHz = 1000.0;

/** Where the pickup sits for the right, past the left's. */
constexpr float kPickupSpread = 0.07f;

/** [p] kept off the ends, where every mode has a node. */
float inside(float p) { return std::clamp(p, 0.02f, 0.98f); }

} // namespace

const ParamDef *Vessel::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        // glass, wood, metal, skin, stone, bell, plate, tube
        {"material", 0.0f, 7.0f, 1.0f, Curve::Stepped, KindCount, ""},
        // The fundamental as a MIDI note: C3 by default.
        {"size", 24.0f, 96.0f, 48.0f, Curve::Linear, 0, "st"},
        {"decay", 0.05f, 20.0f, 1.0f, Curve::Exponential, 0, "s"},
        {"damp", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"strike", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"pickup", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"follow", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        // off, then C to B
        {"key", 0.0f, 12.0f, 0.0f, Curve::Stepped, 13, ""},
        {"scale", 0.0f, 32.0f, 0.0f, Curve::Stepped, music::kScaleCount, ""},
        {"mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Vessel::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    reset();
}

void Vessel::reset() {
    bank.clear();
    heardNote = -1;
    heardHz = 0.0f;
    builtHz = builtDecay = builtDamp = builtStrike = builtPickup = -1.0f;
    builtKind = -1;
    longest = 1.0f;
    driveWas = mixWas = -1.0f;
    envIn = envWet = 0.0f;
    msIn = powIn = powWet = 0.0f;
    guard = 1.0f;
}

float Vessel::targetHz() const {
    const auto &p = params_;
    if (p.get(Follow) >= 0.5f && heardNote >= 0 && heardHz > 0.0f) return heardHz;
    float note = p.get(Size);
    const int key = std::clamp(static_cast<int>(p.get(Key) + 0.5f), 0, 12);
    if (key > 0) {
        // The nearest note of the scale, the lower on a tie.
        const music::ScaleDef &scale =
            music::kScales[std::clamp(static_cast<int>(p.get(Scale) + 0.5f), 0, music::kScaleCount - 1)];
        const int root = key - 1;
        const int base = static_cast<int>(std::floor(note));
        float best = note, bestGap = 1e9f;
        for (int n = base - 12; n <= base + 13; ++n) {
            const int degree = ((n - root) % 12 + 12) % 12;
            bool in = false;
            for (int d = 0; d < scale.count; ++d) in = in || scale.intervals[d] == degree;
            const float gap = std::fabs(static_cast<float>(n) - note);
            if (in && gap < bestGap) {
                bestGap = gap;
                best = static_cast<float>(n);
            }
        }
        note = best;
    }
    return 440.0f * std::exp2((note - 69.0f) / 12.0f);
}

void Vessel::build(float hz) {
    const auto &p = params_;
    const int kind = std::clamp(static_cast<int>(p.get(Material) + 0.5f), 0, KindCount - 1);
    const float decay = p.get(Decay), damp = p.get(Damp);
    const float strike = inside(p.get(Strike)), pickup = p.get(Pickup);
    if (kind == builtKind && hz == builtHz && decay == builtDecay && damp == builtDamp && strike == builtStrike &&
        pickup == builtPickup)
        return;
    const bool fresh = builtKind < 0;
    builtKind = kind;
    builtHz = hz;
    builtDecay = decay;
    builtDamp = damp;
    builtStrike = strike;
    builtPickup = pickup;

    const Body &spec = kSpecs[kind];
    const Tables &t = kTables;
    // The right pickup a little further along, folded back off the far end.
    const float pickL = inside(pickup);
    float pickR = pickup + kPickupSpread;
    if (pickR > 0.98f) pickR = pickup - kPickupSpread;
    pickR = inside(pickR);
    const float lossPow = spec.loss * (0.25f + 1.5f * damp);
    const float ring = decay * spec.ring;

    float wl[kModes], wr[kModes];
    double t60[kModes];
    int count = 0;
    double sumL = 0.0, sumR = 0.0;
    longest = 0.05f;
    for (int k = 0; k < kModes; ++k) {
        const float ratio = t.ratio[kind][k];
        if (hz * ratio > kTop * sr) break;
        const float pi = 3.14159265f;
        const float node = std::fabs(std::sin(pi * static_cast<float>(k + 1) * strike));
        const float a = t.amp[kind][k] * node;
        wl[k] = a * std::sin(pi * static_cast<float>(k + 1) * pickL);
        wr[k] = a * std::sin(pi * static_cast<float>(k + 1) * pickR);
        // Weighted as a track's level falls with frequency, about as pink
        // noise does, so a body is as loud on a song whatever its size.
        const double pink = kPinkHz / std::max(40.0, static_cast<double>(hz * ratio));
        sumL += pink * wl[k] * wl[k];
        sumR += pink * wr[k] * wr[k];
        t60[k] = std::clamp(static_cast<double>(ring) * std::pow(static_cast<double>(ratio), -lossPow), 0.005, 60.0);
        longest = std::max(longest, static_cast<float>(t60[k]));
        count = k + 1;
    }
    // Each mode passes white noise at unit level, so this brings pink noise
    // through the whole body out at about the level it went in.
    const double total = 0.5 * (sumL + sumR);
    const float norm = total > 1e-12 ? static_cast<float>(std::pow(10.0, spec.levelDb / 20.0) / std::sqrt(total)) : 0.0f;
    for (int k = 0; k < count; ++k) bank.setMode(k, hz * t.ratio[kind][k], t60[k], wl[k] * norm, wr[k] * norm, sr);
    for (int k = count; k < kModes; ++k) bank.off(k);
    // After a reset there is nothing to glide from.
    if (fresh) bank.settle();
}

bool Vessel::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    build(std::clamp(targetHz(), 20.0f, kTop * sr));

    const float driveDb = p.get(Drive), mix = p.get(Mix);
    if (driveWas < 0.0f) driveWas = driveDb;
    if (mixWas < 0.0f) mixWas = mix;

    bool silent = true;
    for (int32_t i = 0; i < frames && silent; ++i) silent = L[i] == 0.0f && (!stereoIn || R[i] == 0.0f);
    if (silent && bank.still()) {
        // Nothing in and nothing ringing: the output is silence, so skip the body.
        std::memset(L, 0, sizeof(float) * static_cast<size_t>(frames));
        std::memset(R, 0, sizeof(float) * static_cast<size_t>(frames));
        envIn = envWet = 0.0f;
        msIn = powIn = powWet = 0.0f;
        guard = 1.0f;
        driveWas = driveDb;
        mixWas = mix;
        return true;
    }

    // Drive normalised on the nominal level, so it adds bark, not level.
    const auto shape = [](float db) {
        const float k = std::pow(10.0f, db * 0.05f);
        return std::pair<float, float>(k, kNominal / std::tanh(kNominal * k));
    };
    const auto [k0, n0] = shape(driveWas);
    const auto [k1, n1] = shape(driveDb);
    const float fall = std::exp(-6.9078f / (longest * sr));
    const float wetFall = std::exp(-1.0f / (0.05f * sr));
    const float attack = 1.0f - std::exp(-1.0f / (0.002f * sr));
    const float release = 1.0f - std::exp(-1.0f / (0.1f * sr));
    // Mean square over about 50 ms; the input's is held, falling no faster than the body rings.
    const float powRise = 1.0f - std::exp(-1.0f / (0.05f * sr));
    const float powFall = std::exp(-13.8155f / (longest * sr));

    for (int32_t o = 0; o < frames; o += 256) {
        const int32_t n = std::min<int32_t>(256, frames - o);
        for (int32_t i = 0; i < n; ++i) {
            const float f = static_cast<float>(o + i + 1) / static_cast<float>(frames);
            const float x = stereoIn ? 0.5f * (L[o + i] + R[o + i]) : L[o + i];
            const float k = k0 + (k1 - k0) * f, nn = n0 + (n1 - n0) * f;
            drive[i] = std::tanh(x * k) * nn;
        }
        bank.process(drive, wetL, wetR, n);
        for (int32_t i = 0; i < n; ++i) {
            const float f = static_cast<float>(o + i + 1) / static_cast<float>(frames);
            envIn = std::max(std::fabs(drive[i]), envIn * fall);
            envWet = std::max(std::max(std::fabs(wetL[i]), std::fabs(wetR[i])), envWet * wetFall);
            const float in2 = drive[i] * drive[i];
            msIn += (in2 - msIn) * powRise;
            powIn = std::max(msIn, powIn * powFall);
            const float wet2 = 0.5f * (wetL[i] * wetL[i] + wetR[i] * wetR[i]);
            powWet += (wet2 - powWet) * powRise;
            const float cap = kGuard * envIn;
            float want = envWet > cap ? cap / envWet : 1.0f;
            if (powWet > kPowerGuard * powIn) want = std::min(want, std::sqrt(kPowerGuard * powIn / powWet));
            guard += (want - guard) * (want < guard ? attack : release);
            const float m = mixWas + (mix - mixWas) * f;
            const float inL = L[o + i], inR = stereoIn ? R[o + i] : L[o + i];
            L[o + i] = inL + (wetL[i] * guard - inL) * m;
            R[o + i] = inR + (wetR[i] * guard - inR) * m;
        }
    }
    if (envIn < 1e-20f) envIn = 0.0f;
    if (envWet < 1e-20f) envWet = 0.0f;
    if (msIn < 1e-30f) msIn = 0.0f;
    if (powIn < 1e-30f) powIn = 0.0f;
    if (powWet < 1e-30f) powWet = 0.0f;
    driveWas = driveDb;
    mixWas = mix;
    return true;
}

} // namespace acidulous::effect
