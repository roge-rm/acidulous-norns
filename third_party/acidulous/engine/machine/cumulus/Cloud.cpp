#include "Cloud.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <engine/dsp/Fft.h>

namespace acidulous::machine::cumulus {

namespace {

constexpr float kTwoPi = 6.28318530718f;

/** Table lengths per zone, since resolution has to follow the fundamental. */
int32_t sizeOfZone(int zone) {
    return zone == 0 ? (1 << 17) : (zone == 1 ? (1 << 16) : (1 << 15));
}

/** A small deterministic generator, so the same seed always gives the same cloud. */
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2654435761u + 1u) {}
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
    }
};

/** Five vowels as three formants each: centre Hz, width Hz, gain. */
struct Formant { float hz, bw, gain; };
const Formant kVowels[5][3] = {
    {{730, 90, 1.0f}, {1090, 110, 0.50f}, {2440, 140, 0.22f}},  // A
    {{530, 80, 1.0f}, {1840, 120, 0.42f}, {2480, 150, 0.30f}},  // E
    {{270, 70, 1.0f}, {2290, 130, 0.30f}, {3010, 160, 0.20f}},  // I
    {{570, 80, 1.0f}, {840, 100, 0.55f}, {2410, 140, 0.14f}},   // O
    {{300, 70, 1.0f}, {870, 100, 0.35f}, {2240, 140, 0.10f}},   // U
};

float formantGain(float hz, float vowel01, float amount) {
    if (amount <= 0.0001f) return 1.0f;
    const float x = std::clamp(vowel01, 0.0f, 1.0f) * 4.0f;
    const int a = static_cast<int>(x), b = std::min(a + 1, 4);
    const float f = x - static_cast<float>(a);
    float g = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const Formant &fa = kVowels[a][i];
        const Formant &fb = kVowels[b][i];
        const float centre = fa.hz + (fb.hz - fa.hz) * f;
        const float width = fa.bw + (fb.bw - fa.bw) * f;
        const float gain = fa.gain + (fb.gain - fa.gain) * f;
        const float d = (hz - centre) / width;
        g += gain / (1.0f + d * d); // a resonance, not a brick wall
    }
    // Blended in: at amount 1 it's a vowel, at 0 the other controls decide
    // the spectrum.
    return 1.0f + amount * (std::min(g, 4.0f) - 1.0f);
}

/** The amplitude of partial [n] (1-based) at this end of the morph. */
float partialGain(int n, float tilt, float odd, float comb, float combPeriod,
                  float formant, float formantAmount, float hz) {
    const float octaves = std::log2(static_cast<float>(n));
    float g = std::pow(10.0f, tilt * octaves / 20.0f);
    // Odd/even: at 0 the odd partials are removed, at 1 the even ones, which
    // sounds hollow like a clarinet or square wave.
    const bool isOdd = (n % 2) == 1;
    const float bias = isOdd ? odd : 1.0f - odd;
    g *= std::min(1.0f, bias * 2.0f);
    if (comb > 0.0001f) {
        const float phase = kTwoPi * static_cast<float>(n) / std::max(1.0f, combPeriod);
        g *= 1.0f - comb * 0.5f * (1.0f - std::cos(phase));
    }
    g *= formantGain(hz, formant, formantAmount);
    return g;
}

} // namespace

bool CloudSpec::operator==(const CloudSpec &o) const {
    return partials == o.partials && tilt == o.tilt && odd == o.odd && comb == o.comb &&
           combPeriod == o.combPeriod && formant == o.formant && formantAmount == o.formantAmount &&
           bandwidth == o.bandwidth && bwScale == o.bwScale && stretch == o.stretch &&
           bTilt == o.bTilt && bBandwidth == o.bBandwidth && bStretch == o.bStretch &&
           bComb == o.bComb && bFormant == o.bFormant && bOdd == o.bOdd && seed == o.seed;
}

std::unique_ptr<CloudSet> buildCloud(const CloudSpec &spec, int32_t sampleRate) {
    const auto started = std::chrono::steady_clock::now();
    auto set = std::make_unique<CloudSet>();
    set->spec = spec;
    const float sr = static_cast<float>(sampleRate);

    for (int zone = 0; zone < CloudSet::kZones; ++zone) {
        const int32_t n = sizeOfZone(zone);
        const float base = CloudSet::baseHzOf(zone);
        const float binHz = sr / static_cast<float>(n);
        dsp::Fft fft(n);

        // Partials above this would alias when played an octave up, so they're
        // left out. This is the band-limiting, done at build time for free.
        const float ceiling = sr * 0.5f * 0.95f / 2.0f;

        std::vector<float> re(static_cast<size_t>(n)), im(static_cast<size_t>(n));
        for (int frame = 0; frame < CloudSet::kFrames; ++frame) {
            const float t = static_cast<float>(frame) / (CloudSet::kFrames - 1); // 0 = A, 1 = B
            const float tilt = spec.tilt + spec.bTilt * t;
            const float odd = std::clamp(spec.odd + spec.bOdd * t, 0.0f, 1.0f);
            const float comb = std::clamp(spec.comb + spec.bComb * t, 0.0f, 1.0f);
            const float formant = std::clamp(spec.formant + spec.bFormant * t, 0.0f, 1.0f);
            const float bandwidth = std::max(1.0f, spec.bandwidth + spec.bBandwidth * t);
            const float stretch = spec.stretch + spec.bStretch * t;

            std::fill(re.begin(), re.end(), 0.0f);
            std::fill(im.begin(), im.end(), 0.0f);

            // One generator per frame from the same seed, so every frame gets
            // the same phases and the morph can crossfade tables without them
            // cancelling out.
            Rng rng(spec.seed + 1u);
            int32_t used = 0;
            for (int p = 1; p <= spec.partials; ++p) {
                // Stretch: a real string's partials run sharp, and more
                // stretch sounds like bells and gongs.
                const float ratio = std::pow(static_cast<float>(p), 1.0f + stretch);
                const float hz = base * ratio;
                if (hz > ceiling) break;
                const float gain = partialGain(p, tilt, odd, comb, spec.combPeriod, formant,
                                               spec.formantAmount, hz);
                if (gain < 1e-4f) continue;
                ++used;

                // The band: a Gaussian with a width in cents, so it's wider in
                // Hz the higher it is, scaled again by bwScale.
                const float cents = bandwidth * std::pow(static_cast<float>(p), spec.bwScale - 1.0f);
                const float sigmaHz = std::max(binHz * 0.6f, hz * (std::pow(2.0f, cents / 1200.0f) - 1.0f));
                const int32_t centre = static_cast<int32_t>(hz / binHz + 0.5f);
                const int32_t half = std::min<int32_t>(n / 2 - 1,
                                                       static_cast<int32_t>(3.0f * sigmaHz / binHz) + 1);
                for (int32_t k = centre - half; k <= centre + half; ++k) {
                    if (k < 1 || k >= n / 2) continue;
                    const float d = (static_cast<float>(k) * binHz - hz) / sigmaHz;
                    const float a = gain * std::exp(-0.5f * d * d);
                    if (a < 1e-6f) continue;
                    const float phase = rng.next() * kTwoPi;
                    // Overlapping bands add up.
                    re[static_cast<size_t>(k)] += a * std::cos(phase);
                    im[static_cast<size_t>(k)] += a * std::sin(phase);
                }
            }
            set->partialsUsed[zone] = used;

            // Real output: mirror the half spectrum.
            for (int32_t k = 1; k < n / 2; ++k) {
                re[static_cast<size_t>(n - k)] = re[static_cast<size_t>(k)];
                im[static_cast<size_t>(n - k)] = -im[static_cast<size_t>(k)];
            }
            re[0] = im[0] = 0.0f;
            re[static_cast<size_t>(n / 2)] = im[static_cast<size_t>(n / 2)] = 0.0f;
            fft.transform(re.data(), im.data(), true);

            CloudTable &table = set->tables[zone][frame];
            table.size = n;
            table.baseHz = base;
            table.data.resize(static_cast<size_t>(n) + 1);
            float peak = 1e-9f;
            for (int32_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(re[static_cast<size_t>(i)]));
            // Normalised per frame, so the morph doesn't change the level.
            const float norm = 0.9f / peak;
            for (int32_t i = 0; i < n; ++i) table.data[static_cast<size_t>(i)] = re[static_cast<size_t>(i)] * norm;
            table.data[static_cast<size_t>(n)] = table.data[0]; // wrap sample for the loop
        }
    }
    set->buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    return set;
}

} // namespace acidulous::machine::cumulus
