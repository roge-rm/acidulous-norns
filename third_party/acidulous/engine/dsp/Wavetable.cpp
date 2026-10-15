#include "Wavetable.h"
#include "Math.h"
#include <cmath>
#include <cstring>

namespace acidulous::dsp {

namespace {

constexpr int kH = WavetableBank::kMaxHarmonics;

// How many harmonics a mip gets: up to Nyquist for the highest fundamental
// that mip serves.
int harmonicsForMip(int mip) {
    float top = 40.0f;
    for (int i = 0; i < mip; ++i) top *= 2.0f;
    const int h = static_cast<int>(24000.0f / top);
    return h < 1 ? 1 : (h > kH ? kH : h);
}

// A fixed pseudo-random sequence, so the tables are identical every run.
struct Rng {
    uint32_t s = 0x1234567u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return static_cast<float>(s & 0xffffff) / 16777216.0f; }
};

struct Spectrum {
    float amp[kH + 1]{};
    float phase[kH + 1]{}; // in turns
};

// --- The eight recipes. All original designs.

// Saw at frame 0, then pulses narrowing from square to a thin spike.
void sweep(int frame, Spectrum &s) {
    if (frame == 0) {
        for (int n = 1; n <= kH; ++n) { s.amp[n] = 1.0f / static_cast<float>(n); s.phase[n] = 0.0f; }
        return;
    }
    const float duty = 0.5f - 0.46f * (static_cast<float>(frame - 1) / 6.0f);
    for (int n = 1; n <= kH; ++n) {
        s.amp[n] = std::fabs(2.0f / (static_cast<float>(n) * kPi) * std::sin(static_cast<float>(n) * kPi * duty));
        s.phase[n] = 0.25f;
    }
}

// A resonant band of harmonics that climbs the series: hollow to bright and thin.
void glass(int frame, Spectrum &s) {
    const float centre = 0.2f + static_cast<float>(frame) * 0.62f; // in octaves above the fundamental
    for (int n = 1; n <= kH; ++n) {
        const float d = std::log2(static_cast<float>(n)) - centre;
        s.amp[n] = std::exp(-d * d / 0.35f) / std::sqrt(static_cast<float>(n));
    }
}

// Three formants, sliding as if a mouth were opening.
void vowel(int frame, Spectrum &s) {
    const float t = static_cast<float>(frame) / 7.0f;
    const float c[3] = {2.0f + 6.0f * t, 9.0f + 18.0f * (1.0f - t), 24.0f + 22.0f * t};
    const float g[3] = {1.0f, 0.55f, 0.3f};
    const float w[3] = {1.6f, 3.0f, 6.0f};
    for (int n = 1; n <= kH; ++n) {
        float a = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = (static_cast<float>(n) - c[k]) / w[k];
            a += g[k] / (1.0f + d * d);
        }
        s.amp[n] = a / static_cast<float>(n < 4 ? 1 : 1 + (n >> 4));
    }
}

// A struck-metal partial set that fills in as the frame rises.
void bell(int frame, Spectrum &s) {
    static const int partials[] = {1, 2, 3, 4, 7, 9, 11, 13, 17, 19, 23, 29, 31, 37, 41, 47};
    const int count = 4 + frame + frame / 2;
    Rng rng;
    for (int i = 0; i < static_cast<int>(sizeof(partials) / sizeof(partials[0])) && i < count; ++i) {
        const int n = partials[i];
        if (n > kH) break;
        s.amp[n] = 1.0f / std::pow(static_cast<float>(n), 1.15f);
        s.phase[n] = rng.next();
    }
}

// A saw through a comb whose notches move: metallic, phasey.
void comb(int frame, Spectrum &s) {
    const float teeth = 1.0f + static_cast<float>(frame) * 1.4f;
    for (int n = 1; n <= kH; ++n) {
        s.amp[n] = std::fabs(std::sin(static_cast<float>(n) * kPi * teeth / 16.0f)) / static_cast<float>(n);
    }
}

// A sine folded harder and harder. Drawn as a waveform and then analysed into
// harmonics.
void fold(int frame, Spectrum &s) {
    const float gain = 1.0f + static_cast<float>(frame) * 0.9f;
    float shape[WavetableBank::kSize];
    for (int i = 0; i < WavetableBank::kSize; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(WavetableBank::kSize);
        float x = std::sin(p * kTwoPi) * gain;
        // triangle wrap back into -1..1
        const float t = x * 0.25f + 0.25f;
        shape[i] = 4.0f * std::fabs(t - std::floor(t + 0.5f)) - 1.0f;
    }
    for (int n = 1; n <= kH; ++n) {
        float re = 0.0f, im = 0.0f;
        for (int i = 0; i < WavetableBank::kSize; ++i) {
            const float a = kTwoPi * static_cast<float>(n) * static_cast<float>(i) / static_cast<float>(WavetableBank::kSize);
            re += shape[i] * std::cos(a);
            im += shape[i] * std::sin(a);
        }
        re /= static_cast<float>(WavetableBank::kSize) * 0.5f;
        im /= static_cast<float>(WavetableBank::kSize) * 0.5f;
        s.amp[n] = std::sqrt(re * re + im * im);
        s.phase[n] = std::atan2(re, im) / kTwoPi;
    }
}

// A fixed cloud of harmonics that thickens: dirt with a pitch.
void grit(int frame, Spectrum &s) {
    Rng rng;
    const float spread = 6.0f + static_cast<float>(frame) * 26.0f;
    for (int n = 1; n <= kH; ++n) {
        const float r = rng.next();
        s.phase[n] = rng.next();
        s.amp[n] = r * std::exp(-static_cast<float>(n) / spread) / std::sqrt(static_cast<float>(n));
    }
    s.amp[1] = 1.0f;
}

// Drawbars: the low harmonics traded against the upper ones.
void organ(int frame, Spectrum &s) {
    static const int bars[] = {1, 2, 3, 4, 6, 8, 12, 16};
    const float t = static_cast<float>(frame) / 7.0f;
    for (int i = 0; i < 8; ++i) {
        const float pos = static_cast<float>(i) / 7.0f;
        s.amp[bars[i]] = 0.15f + 0.85f * (1.0f - std::fabs(pos - t) * 1.6f > 0.0f ? 1.0f - std::fabs(pos - t) * 1.6f : 0.0f);
    }
}

using Recipe = void (*)(int, Spectrum &);
const Recipe kRecipes[WavetableBank::kTables] = {sweep, glass, vowel, bell, comb, fold, grit, organ};
const char *const kNames[WavetableBank::kTables] = {"Sweep", "Glass", "Vowel", "Bell", "Comb", "Fold", "Grit", "Organ"};

} // namespace

const char *WavetableBank::tableName(int table) {
    return (table >= 0 && table < kTables) ? kNames[table] : "";
}

WavetableBank::WavetableBank() {
    data.assign(static_cast<size_t>(kTables) * kFrames * kMips * (kSize + 1), 0.0f);
    for (int t = 0; t < kTables; ++t) {
        for (int f = 0; f < kFrames; ++f) {
            Spectrum spec;
            kRecipes[t](f, spec);
            // Normalise on the widest mip so the frames sit at one level.
            for (int m = 0; m < kMips; ++m) {
                const int top = harmonicsForMip(m);
                float *out = const_cast<float *>(row(t, f, m));
                for (int n = 1; n <= top; ++n) {
                    if (spec.amp[n] <= 1e-6f) continue;
                    // Rotate a phasor instead of calling sin() per sample. The
                    // bank has 53 million harmonic samples, so this cuts the
                    // build from about half a second to a tenth.
                    const double a = spec.amp[n];
                    const double step = 2.0 * 3.14159265358979 * static_cast<double>(n) / static_cast<double>(kSize);
                    const double cs = std::cos(step), sn = std::sin(step);
                    double re = std::cos(2.0 * 3.14159265358979 * static_cast<double>(spec.phase[n]));
                    double im = std::sin(2.0 * 3.14159265358979 * static_cast<double>(spec.phase[n]));
                    for (int i = 0; i < kSize; ++i) {
                        out[i] += static_cast<float>(a * im);
                        const double nr = re * cs - im * sn;
                        im = re * sn + im * cs;
                        re = nr;
                    }
                }
                float peak = 0.0f;
                for (int i = 0; i < kSize; ++i) peak = std::fabs(out[i]) > peak ? std::fabs(out[i]) : peak;
                if (peak > 1e-6f) {
                    const float g = 0.95f / peak;
                    for (int i = 0; i < kSize; ++i) out[i] *= g;
                }
                out[kSize] = out[0];
            }
        }
    }
}

const WavetableBank &WavetableBank::instance() {
    static const WavetableBank bank; // thread-safe init, built on the mount thread
    return bank;
}

} // namespace acidulous::dsp
