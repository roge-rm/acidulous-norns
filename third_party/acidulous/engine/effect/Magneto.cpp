#include "Magneto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/** The bands' edges, Hz: narrow at the bottom, where the ear is sharp, wide at the top. */
constexpr float kEdges[] = {0,    100,  200,  300,  400,  510,  630,   770,   920,   1080,  1270,  1480,
                            1720, 2000, 2320, 2700, 3150, 3700,  4400,  5300,  6400,  7700,  9500,
                            11000, 12000, 13000, 14000, 15000, 16000, 17000, 18500, 20000, 22050};
constexpr int kBandsMax = static_cast<int>(sizeof(kEdges) / sizeof(kEdges[0])) - 1;
/** How fast one band's masking falls off into the next, dB a band. */
constexpr float kSpread = 9.0f;
/** The longest word a band can get, in bits. */
constexpr int kLongest = 15;

/**
 * The threshold of hearing, dB, lowest (about -5) near 3.3 kHz. Held at its
 * 16 kHz value above that: the formula climbs past 100 dB by 19 kHz, which
 * would starve a format that keeps its top, as SP does.
 */
float hearingDb(float hz) {
    const float k = std::clamp(hz, 20.0f, 16000.0f) / 1000.0f;
    return 3.64f * std::pow(k, -0.8f) - 6.5f * std::exp(-0.6f * (k - 3.3f) * (k - 3.3f)) + 0.001f * k * k * k * k;
}
/** How much of the side a curve keeps at [hz], as an amplitude. */
float sideKeep(const float (*curve)[2], float hz) {
    float db = 0.0f;
    if (curve[0][0] > 0.0f && hz > curve[0][0]) {
        db = curve[0][1];
        for (int i = 0; i + 1 < Magneto::kSidePoints && curve[i + 1][0] > 0.0f; ++i) {
            const float lo = curve[i][0], hi = curve[i + 1][0];
            db = curve[i + 1][1];
            if (hz <= hi) {
                const float t = std::log2(hz / lo) / std::log2(hi / lo);
                db = curve[i][1] + (curve[i + 1][1] - curve[i][1]) * t;
                break;
            }
        }
    }
    return std::pow(10.0f, db / 20.0f);
}
} // namespace

/*
 * Each format's numbers were set by measuring copies of the same test music
 * (tools/magneto_reference): where the top is cut, how much noise each band
 * carries, how much the bands flicker, the pre-echo and the stereo width.
 * SP, LP2 and LP4 against real discs: SP as a recorder encodes it itself,
 * LP2 and LP4 as the converter's remote encoder makes them. HQ against the
 * open encoder at its own high rate; XLP set by ear between LP2 and LP4.
 */
const Magneto::Format Magneto::kFormats[kModes] = {
    // frame  kbps  eff    top       tilt   mask   floor   joint  side  hold
    {1024,  292, 0.74f, 22050.0f, 1.81f,  8.3f, 140.0f, false, -1, 0.92f}, // SP
    {512,   132, 1.10f, 16000.0f, 3.08f, 13.4f, 138.4f, false, -1, 0.59f}, // LP2
    {1024,   66, 2.62f, 14100.0f, -1.69f, 14.9f, 91.5f, true,   0, 0.66f}, // LP4
    {1024,  352, 0.81f, 17000.0f, 0.80f, 12.6f, 128.0f, false, -1, 1.0f},  // HQ
    {1024,   64, 2.00f, 14000.0f, 2.93f, 12.5f,  95.0f, true,   1, 1.0f},  // XLP
};

const float Magneto::kSideCurves[2][kSidePoints][2] = {
    // LP4, as measured: the side gone in the upper mids, coming partly back above.
    {{2700.0f, 0.0f}, {3000.0f, -40.0f}, {4800.0f, -40.0f}, {5400.0f, -17.0f}, {8000.0f, -11.0f}, {10000.0f, -8.0f}, {12500.0f, -5.0f}, {0.0f, 0.0f}},
    // XLP: narrowing steadily from 5 kHz up.
    {{5000.0f, 0.0f}, {10000.0f, -10.0f}, {14000.0f, -15.0f}, {0.0f, 0.0f}},
};

const ParamDef *Magneto::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"mode", 0.0f, 4.0f, 0.0f, Curve::Stepped, kModes, ""}, // SP LP2 LP4 HQ XLP
        {"dubs", 0.0f, 3.0f, 0.0f, Curve::Stepped, kMaxDubs, ""}, // one to four generations
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Magneto::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    // Every frame size the formats use at this rate, made now so a mode
    // change never allocates.
    ffts.clear();
    int32_t largest = 0;
    for (const auto &f : kFormats) {
        const int32_t n = 1 << static_cast<int>(std::lround(std::log2(f.frameAt44k * sr / 44100.0f)));
        largest = std::max(largest, n);
        bool have = false;
        for (const auto &x : ffts) have = have || x->size() == n;
        if (!have) ffts.push_back(std::make_unique<dsp::Fft>(n));
    }
    window.assign(static_cast<size_t>(largest), 0.0f);
    level.assign(static_cast<size_t>(largest), 1.0f);
    for (int c = 0; c < 2; ++c) {
        re[c].assign(static_cast<size_t>(largest), 0.0f);
        im[c].assign(static_cast<size_t>(largest), 0.0f);
        for (auto &copy : copies) {
            copy.in[c].assign(static_cast<size_t>(largest), 0.0f);
            copy.out[c].assign(static_cast<size_t>(largest), 0.0f);
        }
        dry[c].assign(static_cast<size_t>(largest * kMaxDubs + 1), 0.0f);
    }
    edge.assign(kBandsMax + 1, 0);
    threshold.assign(kBandsMax, 0.0f);
    worth.assign(kBandsMax, 1.0f);
    narrow.assign(kBandsMax, 1.0f);
    fadeStep = 1.0f / std::max(1.0f, 0.01f * sr);
    mode = -1;
    reset();
}

void Magneto::reset() {
    for (auto &copy : copies) {
        for (int c = 0; c < 2; ++c) {
            std::fill(copy.in[c].begin(), copy.in[c].end(), 0.0f);
            std::fill(copy.out[c].begin(), copy.out[c].end(), 0.0f);
        }
        copy.fill = 0;
    }
    for (auto &d : dry) std::fill(d.begin(), d.end(), 0.0f);
    dryAt = 0;
    fadeIn = 0.0f;
}

void Magneto::configure(int newMode, int dubCount) {
    mode = newMode;
    dubs = dubCount;
    format = &kFormats[mode];
    frame = 1 << static_cast<int>(std::lround(std::log2(format->frameAt44k * sr / 44100.0f)));
    hop = frame / 2;
    for (const auto &x : ffts) {
        if (x->size() == frame) fft = x.get();
    }
    // A sine window, on the way in and again on the way out: squared, the
    // halves of neighbouring frames add up to one.
    for (int32_t i = 0; i < frame; ++i) {
        window[static_cast<size_t>(i)] = std::sin(static_cast<float>(M_PI) * (static_cast<float>(i) + 0.5f) / static_cast<float>(frame));
    }
    const float binHz = sr / static_cast<float>(frame);
    const int32_t half = frame / 2;
    bands = 0;
    for (int b = 0; b < kBandsMax; ++b) {
        const auto lo = std::clamp(static_cast<int32_t>(std::lround(kEdges[b] / binHz)), 0, half);
        const auto hi = std::clamp(static_cast<int32_t>(std::lround(kEdges[b + 1] / binHz)), 1, half);
        if (hi <= lo) continue; // too narrow at this frame size: it joins the next
        edge[static_cast<size_t>(bands)] = lo;
        edge[static_cast<size_t>(bands + 1)] = hi;
        const float centre = 0.5f * (static_cast<float>(lo) + static_cast<float>(hi)) * binHz;
        // The quietest a bin can be and be heard: the threshold of hearing,
        // its lowest point put `floorDb` below full scale.
        threshold[static_cast<size_t>(bands)] = std::pow(10.0f, (hearingDb(centre) + 5.0f - format->floorDb) / 10.0f);
        // A joint format keeps only part of the side, by its curve.
        narrow[static_cast<size_t>(bands)] = format->joint && format->sideCurve >= 0 ? sideKeep(kSideCurves[format->sideCurve], centre) : 1.0f;
        worth[static_cast<size_t>(bands)] = centre > 1000.0f ? std::pow(10.0f, -format->tilt * std::log2(centre / 1000.0f) / 10.0f) : 1.0f;
        ++bands;
    }
    reset();
}

/**
 * Word lengths for each band of [channels] channels, spending no more than
 * [budget] bits: each band gets bits for how far it stands above what
 * hides it, less a level found by halving until the frame fits.
 */
void Magneto::allocate(int channels, const float *energy, int32_t *bits, float budget) const {
    float stand[2 * kBandsMax];
    for (int c = 0; c < channels; ++c) {
        const float *e = energy + c * kBandsMax;
        // The masking from the bands either side, falling off by kSpread a band.
        float mask[kBandsMax];
        const float fall = std::pow(10.0f, -kSpread / 10.0f);
        float run = 0.0f;
        for (int b = 0; b < bands; ++b) {
            const float perBin = e[b] / static_cast<float>(edge[static_cast<size_t>(b + 1)] - edge[static_cast<size_t>(b)]);
            run = std::max(run * fall, perBin);
            mask[b] = run;
        }
        run = 0.0f;
        for (int b = bands - 1; b >= 0; --b) {
            const float perBin = e[b] / static_cast<float>(edge[static_cast<size_t>(b + 1)] - edge[static_cast<size_t>(b)]);
            run = std::max(run * fall, perBin);
            mask[b] = std::max(mask[b], run);
        }
        const float under = std::pow(10.0f, -format->maskDb / 10.0f);
        const float binHz = sr / static_cast<float>(frame);
        for (int b = 0; b < bands; ++b) {
            const auto width = static_cast<float>(edge[static_cast<size_t>(b + 1)] - edge[static_cast<size_t>(b)]);
            const float perBin = e[b] / width;
            const float hides = std::max(mask[b] * under, threshold[static_cast<size_t>(b)]);
            const float top = static_cast<float>(edge[static_cast<size_t>(b + 1)]) * binHz;
            const bool kept = top <= format->topHz + binHz;
            stand[c * kBandsMax + b] = kept && perBin > hides ? 10.0f * std::log10(perBin / hides * worth[static_cast<size_t>(b)]) : -1000.0f;
        }
    }
    auto spend = [&](float level, int32_t *out) {
        float cost = 0.0f;
        for (int c = 0; c < channels; ++c) {
            for (int b = 0; b < bands; ++b) {
                const float s = stand[c * kBandsMax + b];
                int32_t w = s <= -999.0f ? 0 : static_cast<int32_t>(std::floor((s - level) / 6.02f)) + 1;
                w = w < 2 ? 0 : std::min(w, kLongest);
                if (out != nullptr) out[c * kBandsMax + b] = w;
                cost += static_cast<float>(w * (edge[static_cast<size_t>(b + 1)] - edge[static_cast<size_t>(b)]));
            }
        }
        return cost;
    };
    float lo = -60.0f, hi = 140.0f;
    for (int i = 0; i < 14; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (spend(mid, nullptr) > budget) lo = mid;
        else hi = mid;
    }
    spend(hi, bits);
}

void Magneto::codeFrame(Copy &copy, bool stereo) {
    const int channels = stereo ? 2 : 1;
    const bool joint = stereo && format->joint;
    const int32_t half = frame / 2;
    // The frame's loudness: the peak of each of kSteps steps, joined up
    // smoothly. Coding the frame with it divided out spreads the noise evenly
    // through the frame, and putting it back afterwards shapes the noise to
    // the sound, so a quiet start before a hit keeps quiet. `hold` says how
    // far the formats do this.
    const int32_t step = frame / kSteps;
    float peak[kSteps];
    for (int s = 0; s < kSteps; ++s) {
        float p = 0.0f;
        for (int32_t i = s * step; i < (s + 1) * step; ++i) {
            p = std::max(p, std::max(std::fabs(copy.in[0][static_cast<size_t>(i)]), std::fabs(copy.in[1][static_cast<size_t>(i)])));
        }
        peak[s] = std::log2(std::max(p, 1e-4f)) * format->hold;
    }
    float loudest = peak[0];
    for (float p : peak) loudest = std::max(loudest, p);
    for (int32_t i = 0; i < frame; ++i) {
        const float at = (static_cast<float>(i) + 0.5f) / static_cast<float>(step) - 0.5f;
        const int s0 = std::clamp(static_cast<int>(std::floor(at)), 0, kSteps - 1);
        const int s1 = std::min(s0 + 1, kSteps - 1);
        const float t = std::clamp(at - static_cast<float>(s0), 0.0f, 1.0f);
        level[static_cast<size_t>(i)] = std::exp2(peak[s0] + (peak[s1] - peak[s0]) * t - loudest);
    }
    // In: window, and for a joint format middle and side in place of left and right.
    for (int32_t i = 0; i < frame; ++i) {
        const float w = window[static_cast<size_t>(i)] / level[static_cast<size_t>(i)];
        float a = copy.in[0][static_cast<size_t>(i)], b = copy.in[1][static_cast<size_t>(i)];
        if (joint) {
            const float m = 0.5f * (a + b);
            b = 0.5f * (a - b);
            a = m;
        }
        re[0][static_cast<size_t>(i)] = a * w;
        re[1][static_cast<size_t>(i)] = b * w;
        im[0][static_cast<size_t>(i)] = im[1][static_cast<size_t>(i)] = 0.0f;
    }
    // Scaled so a full-scale sine's strongest bin has energy 1.
    const float scale = static_cast<float>(M_PI) / static_cast<float>(frame);
    float energy[2 * kBandsMax] = {};
    for (int c = 0; c < channels; ++c) fft->transform(re[c].data(), im[c].data(), false);
    for (int c = 0; c < channels; ++c) {
        for (int b = 0; b < bands; ++b) {
            float e = 0.0f;
            for (int32_t k = edge[static_cast<size_t>(b)]; k < edge[static_cast<size_t>(b + 1)]; ++k) {
                const float x = re[c][static_cast<size_t>(k)] * scale, y = im[c][static_cast<size_t>(k)] * scale;
                e += x * x + y * y;
            }
            energy[c * kBandsMax + b] = e;
        }
    }
    // The bits a frame has: the bitrate over a hop, each channel its share
    // unless the format pools them.
    const float budget = format->kbps * 1000.0f * static_cast<float>(hop) / sr * format->efficiency;
    int32_t bits[2 * kBandsMax] = {};
    if (joint) {
        allocate(2, energy, bits, budget);
    } else {
        for (int c = 0; c < channels; ++c) allocate(1, energy + c * kBandsMax, bits + c * kBandsMax, budget * 0.5f);
    }
    for (int c = 0; c < channels; ++c) {
        float *x = re[c].data(), *y = im[c].data();
        // Everything above the bands carries nothing.
        for (int32_t k = edge[static_cast<size_t>(bands)]; k <= half; ++k) x[k] = y[k] = 0.0f;
        for (int b = 0; b < bands; ++b) {
            const int32_t lo = edge[static_cast<size_t>(b)], hi = edge[static_cast<size_t>(b + 1)];
            const int32_t w = bits[c * kBandsMax + b];
            if (w == 0) {
                for (int32_t k = lo; k < hi; ++k) x[k] = y[k] = 0.0f;
                continue;
            }
            // One scale for the band, in steps of a third of an octave of level (2 dB).
            float peak = 0.0f;
            for (int32_t k = lo; k < hi; ++k) peak = std::max(peak, std::max(std::fabs(x[k]), std::fabs(y[k])));
            if (peak <= 0.0f) continue;
            const float sf = std::exp2(std::ceil(std::log2(peak) * 3.0f) / 3.0f);
            const auto levels = static_cast<float>((1 << (w - 1)) - 1);
            const float step = sf / levels, inv = levels / sf;
            for (int32_t k = lo; k < hi; ++k) {
                x[k] = std::round(x[k] * inv) * step;
                y[k] = std::round(y[k] * inv) * step;
            }
        }
    }
    // A joint format keeps only part of the side, band by band, by its curve:
    // coded as it is, then turned down, so what's kept still has its bits.
    if (joint) {
        for (int b = 0; b < bands; ++b) {
            const float keep = narrow[static_cast<size_t>(b)];
            for (int32_t k = edge[static_cast<size_t>(b)]; k < edge[static_cast<size_t>(b + 1)]; ++k) {
                re[1][static_cast<size_t>(k)] *= keep;
                im[1][static_cast<size_t>(k)] *= keep;
            }
        }
    }
    for (int c = 0; c < channels; ++c) {
        float *x = re[c].data(), *y = im[c].data();
        // The other half of the spectrum mirrors this one, as for any real signal.
        for (int32_t k = 1; k < half; ++k) {
            x[frame - k] = x[k];
            y[frame - k] = -y[k];
        }
        fft->transform(x, y, true);
    }
    if (!stereo) {
        std::copy(re[0].begin(), re[0].begin() + frame, re[1].begin());
    }
    // Out: the loudness back, window again and add onto the half the last frame left.
    for (int c = 0; c < 2; ++c) {
        float *out = copy.out[c].data();
        std::memmove(out, out + hop, sizeof(float) * static_cast<size_t>(frame - hop));
        std::fill(out + frame - hop, out + frame, 0.0f);
    }
    for (int32_t i = 0; i < frame; ++i) {
        const float w = window[static_cast<size_t>(i)] * level[static_cast<size_t>(i)];
        float a = re[0][static_cast<size_t>(i)] * w, b = re[1][static_cast<size_t>(i)] * w;
        if (joint) {
            const float l = a + b;
            b = a - b;
            a = l;
        }
        copy.out[0][static_cast<size_t>(i)] += a;
        copy.out[1][static_cast<size_t>(i)] += b;
    }
    for (int c = 0; c < 2; ++c) {
        float *in = copy.in[c].data();
        std::memmove(in, in + hop, sizeof(float) * static_cast<size_t>(frame - hop));
    }
}

bool Magneto::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const int wantMode = std::clamp(static_cast<int>(p.get(Mode) + 0.5f), 0, kModes - 1);
    const int wantDubs = std::clamp(static_cast<int>(p.get(Dubs) + 0.5f), 0, kMaxDubs - 1) + 1;
    if (wantMode != mode || wantDubs != dubs) configure(wantMode, wantDubs);
    const float mix = p.get(Mix);
    const int32_t delay = latency();
    drySize = static_cast<int32_t>(dry[0].size());

    for (int32_t i = 0; i < frames; ++i) {
        float x[2] = {L[i], stereoIn ? R[i] : L[i]};
        // The dry signal, as far back as the wet runs.
        float d[2];
        int32_t at = dryAt - delay;
        if (at < 0) at += drySize;
        for (int c = 0; c < 2; ++c) {
            dry[c][static_cast<size_t>(dryAt)] = x[c];
            d[c] = dry[c][static_cast<size_t>(at)];
        }
        if (++dryAt >= drySize) dryAt = 0;

        for (int g = 0; g < dubs; ++g) {
            Copy &copy = copies[g];
            const int32_t pos = frame - hop + copy.fill;
            for (int c = 0; c < 2; ++c) {
                copy.in[c][static_cast<size_t>(pos)] = x[c];
                x[c] = copy.out[c][static_cast<size_t>(copy.fill)];
            }
            if (++copy.fill >= hop) {
                copy.fill = 0;
                codeFrame(copy, stereoIn);
            }
        }
        const float fade = fadeIn;
        if (fadeIn < 1.0f) fadeIn = std::min(1.0f, fadeIn + fadeStep);
        L[i] = (d[0] + (x[0] - d[0]) * mix) * fade;
        if (stereoIn) R[i] = (d[1] + (x[1] - d[1]) * mix) * fade;
    }
    return stereoIn;
}

} // namespace acidulous::effect
