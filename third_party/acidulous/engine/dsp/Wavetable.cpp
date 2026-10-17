#include "Wavetable.h"
#include "Fft.h"
#include "Math.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

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
    Rng() = default;
    explicit Rng(uint32_t seed) : s(seed) {}
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

// --- Drawn tables: a waveform drawn over one cycle and analysed into
// harmonics. Drawn at four times the table size so the analysis doesn't fold
// a sharp edge's upper harmonics back onto the ones kept.

constexpr int kDraw = WavetableBank::kSize * 4;

void analyse(const float *shape, Spectrum &s) {
    static const Fft fft(kDraw);
    std::vector<float> re(shape, shape + kDraw), im(kDraw, 0.0f);
    fft.transform(re.data(), im.data(), false);
    const float scale = 2.0f / static_cast<float>(kDraw);
    for (int n = 1; n <= kH; ++n) {
        // The forward transform's imaginary part is minus the sine sum.
        const float c = re[static_cast<size_t>(n)] * scale, sn = -im[static_cast<size_t>(n)] * scale;
        s.amp[n] = std::sqrt(c * c + sn * sn);
        s.phase[n] = std::atan2(c, sn) / kTwoPi;
    }
}

template <typename F> void drawn(Spectrum &s, F wave) {
    std::vector<float> shape(kDraw);
    for (int i = 0; i < kDraw; ++i) shape[static_cast<size_t>(i)] = wave(static_cast<float>(i) / static_cast<float>(kDraw));
    analyse(shape.data(), s);
}

float frac(float x) { return x - std::floor(x); }
float t7(int frame) { return static_cast<float>(frame) / 7.0f; }

// --- The second set of recipes, also our own.

// Odd harmonics added a few at a time: a sine filling out into a square.
void hollow(int frame, Spectrum &s) {
    static const int top[] = {1, 3, 5, 9, 17, 33, 65, 255};
    for (int n = 1; n <= top[frame]; n += 2) s.amp[n] = 1.0f / static_cast<float>(n);
}

// A sine with even harmonics rising under it: octaves and fifths, no hollowness.
void evens(int frame, Spectrum &s) {
    const float t = t7(frame);
    s.amp[1] = 1.0f;
    s.amp[2] = 0.35f + 0.4f * t;
    const float reach = 4.0f + 60.0f * t * t;
    for (int n = 4; n <= kH; n += 2) s.amp[n] = (0.2f + 1.2f * t) / static_cast<float>(n) * std::exp(-static_cast<float>(n) / reach);
}

// Nearly all fundamental, the low partials coming up: deep and round to warm.
void warm(int frame, Spectrum &s) {
    const float t = t7(frame);
    const float tilt = 3.4f - 1.9f * t;
    for (int n = 1; n <= kH; ++n) s.amp[n] = 1.0f / std::pow(static_cast<float>(n), tilt);
    s.amp[2] += 0.25f * t;
    s.amp[3] += 0.12f * t;
}

// A saw restarted every cycle while its own rate climbs: the hard-sync sweep.
void syncSweep(int frame, Spectrum &s) {
    const float ratio = std::exp2(t7(frame) * 2.5f); // 1 to about 5.7
    drawn(s, [ratio](float p) { return 2.0f * frac(p * ratio) - 1.0f; });
}

// A saw and a pulse together, the pulse narrowing: two oscillators in one.
void twin(int frame, Spectrum &s) {
    const float width = 0.5f - 0.42f * t7(frame);
    drawn(s, [width](float p) {
        const float saw = 2.0f * p - 1.0f;
        const float pulse = frac(p + 0.25f) < width ? 1.0f : -1.0f;
        return 0.6f * saw + 0.5f * pulse;
    });
}

// A saw through a resonant low-pass whose cutoff climbs the series.
void reso(int frame, Spectrum &s) {
    const float fc = std::exp2(1.0f + t7(frame) * 5.0f); // harmonic 2 to 64
    constexpr float q = 6.0f;
    for (int n = 1; n <= kH; ++n) {
        const float x = static_cast<float>(n) / fc;
        const float re = 1.0f - x * x, im = x / q;
        s.amp[n] = 1.0f / static_cast<float>(n) / std::sqrt(re * re + im * im);
        s.phase[n] = -std::atan2(im, re) / kTwoPi;
    }
}

// Two-operator FM at one to one, the index rising: sine to a bright buzz.
void fmOne(int frame, Spectrum &s) {
    const float depth = t7(frame) * 5.0f;
    drawn(s, [depth](float p) { return std::sin(kTwoPi * p + depth * std::sin(kTwoPi * p)); });
}

// FM with the modulator an octave up, which leaves only odd partials: woody.
void reed(int frame, Spectrum &s) {
    const float depth = 0.3f + t7(frame) * 3.7f;
    drawn(s, [depth](float p) { return std::sin(kTwoPi * p + depth * std::sin(2.0f * kTwoPi * p)); });
}

// A cosine read through a bent phase: the knee slides and it leans into a saw.
void bent(int frame, Spectrum &s) {
    const float knee = 0.5f - 0.47f * t7(frame);
    drawn(s, [knee](float p) {
        const float w = p < knee ? 0.5f * p / knee : 0.5f + 0.5f * (p - knee) / (1.0f - knee);
        return -std::cos(kTwoPi * w);
    });
}

// A fading burst of a higher cosine restarted each cycle: a resonance with no filter.
void ping(int frame, Spectrum &s) {
    const float ratio = 1.5f * std::pow(9.0f, t7(frame)); // 1.5 to 13.5
    drawn(s, [ratio](float p) { return (1.0f - p) * std::cos(kTwoPi * p * ratio); });
}

// A sine with fewer and fewer levels to stand on.
void crush(int frame, Spectrum &s) {
    static const float half[] = {32.0f, 12.0f, 6.0f, 4.0f, 3.0f, 2.0f, 1.5f, 1.0f};
    const float h = half[frame];
    drawn(s, [h](float p) { return std::round(std::sin(kTwoPi * p) * h) / h; });
}

// A saw held at fewer and fewer points a cycle, so its steps can be heard.
void stair(int frame, Spectrum &s) {
    static const float steps[] = {64.0f, 32.0f, 16.0f, 12.0f, 8.0f, 6.0f, 4.0f, 3.0f};
    const float n = steps[frame];
    drawn(s, [n](float p) { return 2.0f * (std::floor(p * n) / n) - 1.0f + 1.0f / n; });
}

// Formants placed for a voice around A3, wide and soft: oo, oh, ah, eh, ee.
void choir(int frame, Spectrum &s) {
    static const float vowels[5][3] = {
        {300, 870, 2240}, {450, 800, 2830}, {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}};
    const float x = t7(frame) * 4.0f;
    const int a = std::min(3, static_cast<int>(x));
    const float f = x - static_cast<float>(a);
    const float gain[3] = {1.0f, 0.5f, 0.25f};
    const float width[3] = {1.2f, 1.8f, 2.6f};
    for (int n = 1; n <= kH; ++n) {
        float amp = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float hz = vowels[a][k] + (vowels[a + 1][k] - vowels[a][k]) * f;
            const float d = (static_cast<float>(n) - hz / 220.0f) / width[k];
            amp += gain[k] / (1.0f + d * d);
        }
        s.amp[n] = amp / std::sqrt(static_cast<float>(n));
    }
}

// Narrow formants high up and a notch that walks through the low partials.
void nasal(int frame, Spectrum &s) {
    const float t = t7(frame);
    const float zero = 2.5f + 8.0f * t;
    const float c[3] = {1.0f, 10.0f + 4.0f * t, 15.0f + 9.0f * t};
    const float g[3] = {0.8f, 0.9f, 0.5f};
    const float w[3] = {0.8f, 1.0f, 1.4f};
    for (int n = 1; n <= kH; ++n) {
        float amp = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float d = (static_cast<float>(n) - c[k]) / w[k];
            amp += g[k] / (1.0f + d * d);
        }
        const float notch = std::min(1.0f, std::fabs(static_cast<float>(n) - zero) / 2.0f);
        s.amp[n] = (amp + 0.15f / static_cast<float>(n)) * notch * notch;
    }
}

// A saw with a body's uneven resonances and spread phases, opening up as if bowed harder.
void bow(int frame, Spectrum &s) {
    const float t = t7(frame);
    Rng rng(0xb0b0b0u);
    float centre[6];
    for (float &c : centre) c = 2.0f + rng.next() * 30.0f;
    const float shift = 1.0f + 0.15f * t;
    const float reach = 8.0f + 70.0f * t;
    for (int n = 1; n <= kH; ++n) {
        float bump = 0.0f;
        for (float c : centre) {
            const float d = (static_cast<float>(n) - c * shift) / 1.5f;
            bump += 1.0f / (1.0f + d * d);
        }
        s.amp[n] = (0.5f + 0.7f * bump) / static_cast<float>(n) * std::exp(-static_cast<float>(n) / reach);
        s.phase[n] = rng.next();
    }
}

// Blown harder frame by frame: the series fills in from the bottom, like a horn.
void brass(int frame, Spectrum &s) {
    const float t = t7(frame);
    const float reach = 0.7f + t * t * 14.0f;
    for (int n = 1; n <= kH; ++n) {
        const float d = (static_cast<float>(n) - 6.0f) / 3.0f;
        s.amp[n] = std::exp(-static_cast<float>(n - 1) / reach) * (1.0f + 0.4f * t / (1.0f + d * d));
    }
}

// Random harmonics on a pitch, coloured from dark to bright.
void dust(int frame, Spectrum &s) {
    const float slope = 1.6f - 1.5f * t7(frame);
    Rng rng(0xd057u);
    for (int n = 1; n <= kH; ++n) {
        const float r = rng.next();
        s.phase[n] = rng.next();
        s.amp[n] = (0.2f + r) / std::pow(static_cast<float>(n), slope);
    }
    s.amp[1] = 1.0f;
}

// Pairs of neighbouring partials far up the series, climbing: a hard metal ring.
void metal(int frame, Spectrum &s) {
    const float t = t7(frame);
    static const float centres[] = {5.0f, 8.5f, 13.0f, 19.5f, 27.0f, 41.0f};
    s.amp[1] = 0.6f;
    for (int k = 0; k < 6; ++k) {
        const float c = centres[k] * (1.0f + 0.6f * t);
        for (int n = 2; n <= kH; ++n) {
            const float d = (static_cast<float>(n) - c) / 0.7f;
            s.amp[n] += (0.9f - 0.08f * static_cast<float>(k)) / (1.0f + d * d * d * d);
        }
    }
}

// A sound chip's waves: a stepped triangle, pulses at half, quarter and eighth,
// stepped saws, a double pulse and a fixed one-bit pattern.
void chip(int frame, Spectrum &s) {
    switch (frame) {
    case 0: drawn(s, [](float p) { return (std::floor((1.0f - 4.0f * std::fabs(frac(p + 0.25f) - 0.5f)) * 7.5f + 7.5f) - 7.5f) / 7.5f; }); break;
    case 1: drawn(s, [](float p) { return p < 0.5f ? 1.0f : -1.0f; }); break;
    case 2: drawn(s, [](float p) { return p < 0.25f ? 1.0f : -1.0f; }); break;
    case 3: drawn(s, [](float p) { return p < 0.125f ? 1.0f : -1.0f; }); break;
    // Falling saws, which start high like the pulses, so the crossfade between them doesn't cancel.
    case 4: drawn(s, [](float p) { return (7.5f - std::floor(p * 16.0f)) / 7.5f; }); break;
    case 5: drawn(s, [](float p) { return (3.5f - std::floor(p * 8.0f)) / 3.5f; }); break;
    case 6: drawn(s, [](float p) { return frac(p * 2.0f) < 0.25f ? 1.0f : -1.0f; }); break;
    default: {
        static const uint32_t pattern = 0xb4e2659du; // 32 fixed bits
        drawn(s, [](float p) { return (pattern >> static_cast<int>(p * 32.0f)) & 1u ? 1.0f : -1.0f; });
        break;
    }
    }
}

// Harmonic levels rippled by a slowly turning pattern, with phases spread
// further each frame. Moving through it brings out its own cancellations.
void spiral(int frame, Spectrum &s) {
    const float t = t7(frame);
    for (int n = 1; n <= kH; ++n) {
        const float fn = static_cast<float>(n);
        s.amp[n] = (0.5f + 0.5f * std::cos(kTwoPi * fn * (0.02f + t * 0.35f))) / std::pow(fn, 0.8f);
        s.phase[n] = 0.01f * fn * fn * t;
    }
    s.amp[1] = 1.0f;
}

// A ramp up and a cosine down, the turn sliding from early to late.
void tilt(int frame, Spectrum &s) {
    const float turn = 0.1f + 0.8f * t7(frame);
    drawn(s, [turn](float p) {
        if (p < turn) return -1.0f + 2.0f * p / turn;
        return std::cos(kPi * (p - turn) / (1.0f - turn));
    });
}

// Only the prime-numbered harmonics, more of them each frame: bright and oddly hollow.
void prime(int frame, Spectrum &s) {
    static const int top[] = {3, 7, 13, 23, 47, 97, 181, 256};
    s.amp[1] = 1.0f;
    for (int n = 2; n <= top[frame]; ++n) {
        bool isPrime = true;
        for (int d = 2; d * d <= n; ++d) if (n % d == 0) { isPrime = false; break; }
        if (isPrime) s.amp[n] = 1.0f / std::pow(static_cast<float>(n), 0.9f);
    }
}

// A saw built a few harmonics at a time.
void build(int frame, Spectrum &s) {
    static const int top[] = {2, 3, 4, 6, 8, 12, 24, 256};
    for (int n = 1; n <= top[frame]; ++n) s.amp[n] = 1.0f / static_cast<float>(n);
}

// A string plucked at a point that moves from its middle towards the end.
void pluck(int frame, Spectrum &s) {
    const float at = 0.5f - 0.46f * t7(frame);
    for (int n = 1; n <= kH; ++n) {
        s.amp[n] = std::fabs(std::sin(static_cast<float>(n) * kPi * at)) / std::pow(static_cast<float>(n), 1.3f);
    }
}

using Recipe = void (*)(int, Spectrum &);
// Appended only: a song stores a table by its place in this list.
const Recipe kRecipes[WavetableBank::kTables] = {
    sweep, glass, vowel, bell, comb, fold, grit, organ,
    hollow, evens, warm, syncSweep, twin, reso, fmOne, reed,
    bent, ping, crush, stair, choir, nasal, bow, brass,
    dust, metal, chip, spiral, tilt, prime, build, pluck};
const char *const kNames[WavetableBank::kTables] = {
    "Sweep", "Glass", "Vowel", "Bell", "Comb", "Fold", "Grit", "Organ",
    "Hollow", "Even", "Round", "Sync", "Twin", "Reso", "Index", "Reed",
    "Bent", "Ping", "Crush", "Stair", "Choir", "Nasal", "Bow", "Brass",
    "Dust", "Metal", "Chip", "Spiral", "Tilt", "Prime", "Stack", "Pluck"};

} // namespace

const char *WavetableBank::tableName(int table) {
    return (table >= 0 && table < kTables) ? kNames[table] : "";
}

namespace {
// Scales a mip so its peak sits at 0.95, so the frames sit at one level, and
// closes the loop with the extra sample.
void normalise(float *out) {
    float peak = 0.0f;
    for (int i = 0; i < WavetableBank::kSize; ++i) peak = std::fabs(out[i]) > peak ? std::fabs(out[i]) : peak;
    if (peak > 1e-6f) {
        const float g = 0.95f / peak;
        for (int i = 0; i < WavetableBank::kSize; ++i) out[i] *= g;
    }
    out[WavetableBank::kSize] = out[0];
}

// The first eight tables are summed sample by sample, the way they were first
// built, so songs made with them keep the same samples. Later tables use an
// inverse FFT per mip, which is faster but rounds differently.
constexpr int kSummedTables = 8;
} // namespace

WavetableBank::WavetableBank() {
    data.assign(static_cast<size_t>(kTables) * kFrames * kMips * (kSize + 1), 0.0f);
    int lastOf[kMips];
    for (int m = 0; m < kMips; ++m) lastOf[m] = harmonicsForMip(m);
    std::vector<float> sum(kSize), re(kSize), im(kSize);
    const Fft fft(kSize);
    for (int t = 0; t < kTables; ++t) {
        for (int f = 0; f < kFrames; ++f) {
            Spectrum spec;
            kRecipes[t](f, spec);
            if (t >= kSummedTables) {
                for (int m = 0; m < kMips; ++m) {
                    std::fill(re.begin(), re.end(), 0.0f);
                    std::fill(im.begin(), im.end(), 0.0f);
                    for (int n = 1; n <= lastOf[m]; ++n) {
                        if (spec.amp[n] <= 1e-6f) continue;
                        const float a = kTwoPi * spec.phase[n];
                        re[static_cast<size_t>(n)] = spec.amp[n] * std::cos(a);
                        im[static_cast<size_t>(n)] = spec.amp[n] * std::sin(a);
                    }
                    // The imaginary part of the inverse is the sum of sines, over
                    // kSize; scaled back up so normalise() sees the level the
                    // summed tables have.
                    fft.transform(re.data(), im.data(), true);
                    float *out = const_cast<float *>(row(t, f, m));
                    for (int i = 0; i < kSize; ++i) out[i] = im[static_cast<size_t>(i)] * static_cast<float>(kSize);
                    normalise(out);
                }
                continue;
            }
            // Each mip is the first lastOf[m] harmonics of the frame, so the
            // frame is summed once, harmonic by harmonic, and each mip is
            // copied out when the sum reaches its last harmonic. The same
            // additions in the same order as summing each mip on its own.
            std::fill(sum.begin(), sum.end(), 0.0f);
            for (int n = 1; n <= kH; ++n) {
                if (spec.amp[n] > 1e-6f) {
                    // Rotate a phasor instead of calling sin() per sample.
                    const double a = spec.amp[n];
                    const double step = 2.0 * 3.14159265358979 * static_cast<double>(n) / static_cast<double>(kSize);
                    const double cs = std::cos(step), sn = std::sin(step);
                    double pr = std::cos(2.0 * 3.14159265358979 * static_cast<double>(spec.phase[n]));
                    double pi = std::sin(2.0 * 3.14159265358979 * static_cast<double>(spec.phase[n]));
                    for (int i = 0; i < kSize; ++i) {
                        sum[static_cast<size_t>(i)] += static_cast<float>(a * pi);
                        const double nr = pr * cs - pi * sn;
                        pi = pr * sn + pi * cs;
                        pr = nr;
                    }
                }
                for (int m = 0; m < kMips; ++m) {
                    if (lastOf[m] != n) continue;
                    float *out = const_cast<float *>(row(t, f, m));
                    std::copy(sum.begin(), sum.end(), out);
                    normalise(out);
                }
            }
        }
    }
}

const WavetableBank &WavetableBank::instance() {
    static const WavetableBank bank; // thread-safe init, built on the mount thread
    return bank;
}

} // namespace acidulous::dsp
