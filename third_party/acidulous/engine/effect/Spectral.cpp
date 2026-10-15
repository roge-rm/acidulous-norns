#include "Spectral.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
constexpr float kTwoPi = 6.2831853f;
/** A frame, in seconds; the size is the power of two nearest it at the rate. */
constexpr float kFrameSeconds = 0.043f;
/** Hann windows in and out, four frames over each sample: together they add up to 1.5. */
constexpr float kOverlap = 1.0f / 1.5f;

float wrap(float a) {
    a = std::fmod(a + static_cast<float>(M_PI), kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a - static_cast<float>(M_PI);
}
} // namespace

const ParamDef *Spectral::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"freeze", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"blur", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"smear", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"robot", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"peaks", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tilt", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Spectral::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    size = 1 << static_cast<int>(std::lround(std::log2(kFrameSeconds * sr)));
    hop = size / 4;
    fft = std::make_unique<dsp::Fft>(size);
    const auto n = static_cast<size_t>(size), bins = n / 2 + 1;
    window.resize(n);
    for (size_t i = 0; i < n; ++i) window[i] = 0.5f - 0.5f * std::cos(kTwoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(size));
    re.assign(n, 0.0f);
    im.assign(n, 0.0f);
    lean.assign(bins, 1.0f);
    leanDb = 1000.0f;
    for (int c = 0; c < 2; ++c) {
        in[c].assign(n, 0.0f);
        out[c].assign(n, 0.0f);
        held[c].assign(bins, 0.0f);
        last[c].assign(bins, 0.0f);
        turn[c].assign(bins, 0.0f);
        played[c].assign(bins, 0.0f);
        dry[c].assign(n + 1, 0.0f);
    }
    reset();
}

void Spectral::reset() {
    for (int c = 0; c < 2; ++c) {
        for (auto *v : {&in[c], &out[c], &held[c], &last[c], &turn[c], &played[c], &dry[c]}) std::fill(v->begin(), v->end(), 0.0f);
    }
    fill = 0;
    dryAt = 0;
    frozen = false;
    rng = 0x9e3779b9u;
}

void Spectral::frame(int c) {
    const auto &p = params_;
    const int32_t half = size / 2;
    for (int32_t i = 0; i < size; ++i) {
        re[static_cast<size_t>(i)] = in[c][static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
        im[static_cast<size_t>(i)] = 0.0f;
    }
    fft->transform(re.data(), im.data(), false);

    const bool freeze = frozen;
    // Blur: how much of each bin's level carries over from one frame to the
    // next, up to a few seconds' fade at the top.
    const float blur = p.get(Blur);
    const float keep = blur <= 0.0f ? 0.0f : std::pow(blur, 0.15f) * 0.995f;
    const float smear = p.get(Smear), robot = p.get(Robot), peaks = p.get(Peaks);
    float *H = held[c].data(), *Last = last[c].data(), *Turn = turn[c].data(), *Played = played[c].data();

    float loudest = 0.0f;
    for (int32_t k = 0; k <= half; ++k) {
        const auto i = static_cast<size_t>(k);
        const float mag = std::sqrt(re[i] * re[i] + im[i] * im[i]);
        const float phase = std::atan2(im[i], re[i]);
        if (!freeze && mag >= H[k] * keep) {
            // How far the bin turned since the last frame: its frequency, held
            // for a freeze or while it fades.
            Turn[k] = wrap(phase - Last[k]);
            H[k] = mag;
            Played[k] = phase;
        } else {
            // Frozen, or fading under blur: the bin goes on turning as it did.
            if (!freeze) H[k] *= keep;
            Played[k] = wrap(Played[k] + Turn[k]);
        }
        Last[k] = phase;
        loudest = std::max(loudest, H[k]);
    }
    // Scrambled or thrown-away timing cancels between overlapping frames
    // and the level drops; held levels pile up a little. Made up here, so
    // turning these keeps about the same loudness.
    const float makeup = std::pow(10.0f, (robot * 15.0f * (1.0f - 0.4f * smear) + smear * 9.0f - blur * 4.0f) / 20.0f);
    // Peaks: everything further under the loudest bin than this goes.
    const float floor = peaks > 0.0f ? loudest * std::pow(10.0f, -(1.0f - peaks) * 72.0f / 20.0f) : 0.0f;
    for (int32_t k = 0; k <= half; ++k) {
        const auto i = static_cast<size_t>(k);
        float mag = H[k];
        if (mag < floor) mag = 0.0f;
        float phase = Played[k];
        if (smear > 0.0f) phase += smear * static_cast<float>(M_PI) * random();
        phase *= 1.0f - robot;
        mag *= lean[i] * makeup;
        re[i] = mag * std::cos(phase);
        im[i] = mag * std::sin(phase);
    }
    im[0] = 0.0f;
    im[static_cast<size_t>(half)] = 0.0f;
    for (int32_t k = 1; k < half; ++k) {
        re[static_cast<size_t>(size - k)] = re[static_cast<size_t>(k)];
        im[static_cast<size_t>(size - k)] = -im[static_cast<size_t>(k)];
    }
    fft->transform(re.data(), im.data(), true);

    float *o = out[c].data();
    std::memmove(o, o + hop, sizeof(float) * static_cast<size_t>(size - hop));
    std::fill(o + size - hop, o + size, 0.0f);
    for (int32_t i = 0; i < size; ++i) o[i] += re[static_cast<size_t>(i)] * window[static_cast<size_t>(i)] * kOverlap;
    float *x = in[c].data();
    std::memmove(x, x + hop, sizeof(float) * static_cast<size_t>(size - hop));
}

bool Spectral::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float mix = p.get(Mix);
    const float tilt = p.get(Tilt);
    if (std::fabs(tilt - leanDb) > 0.01f) {
        // Tilt turns about 1 kHz, a few octaves either way. It lifts by no
        // more than 6 dB, so leaning dark doesn't boom and bright doesn't
        // hiss.
        leanDb = tilt;
        const float binHz = sr / static_cast<float>(size);
        for (size_t k = 0; k < lean.size(); ++k) {
            const float hz = std::max(50.0f, static_cast<float>(k) * binHz);
            lean[k] = std::pow(10.0f, std::min(6.0f, tilt * std::log2(hz / 1000.0f)) / 20.0f);
        }
    }
    stereo = stereoIn;
    const bool wantFreeze = p.get(Freeze) > 0.5f;
    const auto dryLength = static_cast<int32_t>(dry[0].size());

    for (int32_t i = 0; i < frames; ++i) {
        const float x[2] = {L[i], stereoIn ? R[i] : L[i]};
        float d[2], y[2];
        int32_t at = dryAt - size;
        if (at < 0) at += dryLength;
        for (int c = 0; c < 2; ++c) {
            dry[c][static_cast<size_t>(dryAt)] = x[c];
            d[c] = dry[c][static_cast<size_t>(at)];
            in[c][static_cast<size_t>(size - hop + fill)] = x[c];
            y[c] = out[c][static_cast<size_t>(fill)];
        }
        if (++dryAt >= dryLength) dryAt = 0;
        if (++fill >= hop) {
            fill = 0;
            // A freeze starts or ends on a frame.
            frozen = wantFreeze;
            frame(0);
            if (stereoIn) frame(1);
            else std::copy(out[0].begin(), out[0].end(), out[1].begin());
        }
        L[i] = d[0] + (y[0] - d[0]) * mix;
        if (stereoIn) R[i] = d[1] + (y[1] - d[1]) * mix;
    }
    return stereoIn;
}

} // namespace acidulous::effect
