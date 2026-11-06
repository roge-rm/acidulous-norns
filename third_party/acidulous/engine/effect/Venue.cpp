#include "Venue.h"
#include <engine/core/Handover.h>
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
constexpr float kHalfPi = 1.5707963f;
// At or past these the knob is off.
constexpr float kLowOff = 20.5f;
constexpr float kHighOff = 19900.0f;
constexpr float kDecayOff = 19.9f;
const float kSilence[kBlockFrames] = {};

/** Four-point cubic through the samples around [pos]. */
float cubicAt(const float *x, int32_t frames, double pos) {
    const auto i = static_cast<int32_t>(pos);
    const auto f = static_cast<float>(pos - i);
    auto at = [&](int32_t k) { return k >= 0 && k < frames ? x[k] : 0.0f; };
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}
} // namespace

std::unique_ptr<VenueImpulse> buildVenueImpulse(const float *left, const float *right, int32_t frames,
                                                int32_t sampleRate, const VenueShape &shape) {
    if (left == nullptr || frames <= 0) return nullptr;
    const float stretch = dsp::clampf(shape.stretch, 0.5f, 2.0f);
    const bool stretched = std::fabs(stretch - 1.0f) > 1e-4f;
    const auto full = static_cast<int32_t>(std::lround(static_cast<double>(frames) * (stretched ? stretch : 1.0f)));
    const auto cap = std::max<int32_t>(1, static_cast<int32_t>(std::lround(static_cast<double>(shape.length) * sampleRate)));
    const int32_t n = std::max(1, std::min(full, cap));

    std::vector<float> out[2];
    const int channels = right != nullptr ? 2 : 1;
    // Stretched by reading slower or faster, scaled so it keeps its energy.
    const float level = stretched ? 1.0f / std::sqrt(stretch) : 1.0f;
    for (int c = 0; c < channels; ++c) {
        const float *src = c == 0 ? left : right;
        out[c].resize(static_cast<size_t>(n));
        for (int32_t i = 0; i < n; ++i) {
            out[c][static_cast<size_t>(i)] =
                stretched ? cubicAt(src, frames, static_cast<double>(i) / stretch) * level : src[i];
        }
    }
    // Cut short: the last fifth fades out on a half cosine.
    if (full > cap) {
        const int32_t fadeLen = std::max(1, n / 5);
        for (int32_t i = 0; i < fadeLen; ++i) {
            const float g = 0.5f + 0.5f * std::cos(static_cast<float>(M_PI) * static_cast<float>(i + 1) / static_cast<float>(fadeLen));
            for (int c = 0; c < channels; ++c) out[c][static_cast<size_t>(n - fadeLen + i)] *= g;
        }
    }
    // An extra exponential: 60 dB down after [decay] seconds.
    if (shape.decay < kDecayOff) {
        const double perSample = -6.907755 / (std::max(0.01, static_cast<double>(shape.decay)) * sampleRate);
        for (int32_t i = 0; i < n; ++i) {
            const auto g = static_cast<float>(std::exp(perSample * i));
            for (int c = 0; c < channels; ++c) out[c][static_cast<size_t>(i)] *= g;
        }
    }
    return std::make_unique<VenueImpulse>(out[0].data(), channels == 2 ? out[1].data() : nullptr, n);
}

void normaliseImpulse(std::vector<float> &left, std::vector<float> &right) {
    double energy = 0.0;
    for (const float v : left) energy += static_cast<double>(v) * v;
    for (const float v : right) energy += static_cast<double>(v) * v;
    if (!right.empty()) energy *= 0.5;
    if (!(energy > 1e-20)) return;
    const auto g = static_cast<float>(1.0 / std::sqrt(energy));
    for (float &v : left) v *= g;
    for (float &v : right) v *= g;
}

Venue::~Venue() {
    delete cur;
    delete old;
    delete pending;
    delete spent;
}

const ParamDef *Venue::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"predelay", 0.0f, kMaxPredelayMs, 0.0f, Curve::Linear, 0, "ms"},
        {"length", 0.1f, 10.0f, 10.0f, Curve::Exponential, 0, "s"},
        // At the top it's off: the impulse decays as recorded.
        {"decay", 0.1f, 20.0f, 20.0f, Curve::Exponential, 0, "s"},
        {"lowcut", 20.0f, 2000.0f, 20.0f, Curve::Exponential, 0, "Hz"},
        {"highcut", 1000.0f, 20000.0f, 20000.0f, Curve::Exponential, 0, "Hz"},
        {"stretch", 0.5f, 2.0f, 1.0f, Curve::Exponential, 0, "x"},
        {"width", 0.0f, 2.0f, 1.0f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

VenueShape Venue::shape() const {
    VenueShape s;
    s.length = params_.target(Length);
    s.decay = params_.target(Decay);
    s.stretch = params_.target(Stretch);
    return s;
}

void Venue::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    const auto cap = static_cast<int32_t>(kMaxPredelayMs * 0.001f * sr) + 4;
    for (auto &d : pre) d.prepare(cap);
    fadeStep = 1.0f / (kFadeSeconds * sr);
    presenceStep = 1.0f / (0.01f * sr);
    reset();
}

void Venue::retire(VenueImpulse *&x) {
    if (x == nullptr) return;
    if (retirer_ != nullptr) {
        // Straight to the engine's deleter thread, so ~17 MB isn't held until the next swap.
        x->next = nullptr;
        retirer_->retire(x, deleteAs<VenueImpulse>);
        x = nullptr;
        return;
    }
    x->next = spent;
    spent = x;
    x = nullptr;
}

void Venue::reset() {
    // Whatever was on its way is in place at once.
    retire(old);
    if (hasPending) {
        retire(cur);
        cur = pending;
        pending = nullptr;
        hasPending = false;
    }
    if (cur != nullptr) cur->conv.reset();
    fade = 0.0f;
    presence = cur != nullptr ? 1.0f : 0.0f;
    for (auto &d : pre) d.forget();
    for (int c = 0; c < 2; ++c) {
        low[c].reset();
        high[c].reset();
    }
    lowOn = highOn = false;
    lowHz = highHz = 0.0f;
}

void *Venue::swapObject(int32_t slot, void *object) {
    if (slot != 0) return object;
    auto *incoming = static_cast<VenueImpulse *>(object);
    if (incoming != nullptr) incoming->conv.reset();
    VenueImpulse *back = spent;
    spent = nullptr;
    if (old == nullptr) {
        old = cur;
        fade = old != nullptr ? 1.0f : 0.0f;
        cur = incoming;
    } else {
        // One still fading: this one waits for it, replacing any that waited.
        if (pending != nullptr) {
            pending->next = back;
            back = pending;
        }
        pending = incoming;
        hasPending = true;
    }
    return back;
}

void Venue::filters(float lo, float hi) {
    const bool loNow = lo > kLowOff, hiNow = hi < kHighOff;
    if (loNow && (!lowOn || lo != lowHz)) {
        if (!lowOn) for (auto &f : low) f.reset();
        for (auto &f : low) f.highpass(lo, 0.7071f, sr);
        lowHz = lo;
    }
    if (hiNow && (!highOn || hi != highHz)) {
        if (!highOn) for (auto &f : high) f.reset();
        for (auto &f : high) f.lowpass(hi, 0.7071f, sr);
        highHz = hi;
    }
    lowOn = loNow;
    highOn = hiNow;
}

bool Venue::process(float *L, float *R, int32_t frames, bool stereoIn) {
    if (cur == nullptr && old == nullptr && presence <= 0.0f) return stereoIn;
    const auto &p = params_;
    float preSamples = p.get(Predelay) * 0.001f * sr;
    // A setting a hair off a whole sample is that sample, so a delay in
    // whole milliseconds is exact.
    if (std::fabs(preSamples - std::round(preSamples)) < 1e-3f) preSamples = std::round(preSamples);
    const float width = p.get(Width);
    const float mix = p.get(Mix);
    filters(p.get(Lowcut), p.get(Highcut));
    const float *inR = stereoIn ? R : L;
    const float presenceTarget = (cur != nullptr || old != nullptr) ? 1.0f : 0.0f;

    for (int32_t at = 0; at < frames; at += kBlockFrames) {
        const int32_t n = std::min(kBlockFrames, frames - at);
        float dl[kBlockFrames], dr[kBlockFrames], wl[kBlockFrames], wr[kBlockFrames];
        for (int32_t i = 0; i < n; ++i) {
            pre[0].write(L[at + i]);
            pre[1].write(inR[at + i]);
            dl[i] = pre[0].read(preSamples + 1.0f);
            dr[i] = pre[1].read(preSamples + 1.0f);
        }
        if (cur != nullptr) {
            cur->conv.process(dl, dr, wl, wr, n);
        } else {
            std::fill(wl, wl + n, 0.0f);
            std::fill(wr, wr + n, 0.0f);
        }
        if (old != nullptr) {
            float ol[kBlockFrames], orr[kBlockFrames];
            old->conv.process(kSilence, kSilence, ol, orr, n);
            for (int32_t i = 0; i < n; ++i) {
                fade = std::max(0.0f, fade - fadeStep);
                wl[i] += ol[i] * fade;
                wr[i] += orr[i] * fade;
            }
            if (fade <= 0.0f) {
                retire(old);
                if (hasPending) {
                    // The one that waited goes in now, and the current one fades.
                    old = cur;
                    fade = old != nullptr ? 1.0f : 0.0f;
                    cur = pending;
                    pending = nullptr;
                    hasPending = false;
                }
            }
        }
        if (lowOn || highOn) {
            for (int32_t i = 0; i < n; ++i) {
                if (lowOn) {
                    wl[i] = low[0].process(wl[i]);
                    wr[i] = low[1].process(wr[i]);
                }
                if (highOn) {
                    wl[i] = high[0].process(wl[i]);
                    wr[i] = high[1].process(wr[i]);
                }
            }
        }
        if (width != 1.0f) {
            for (int32_t i = 0; i < n; ++i) {
                const float m = 0.5f * (wl[i] + wr[i]), s = 0.5f * (wl[i] - wr[i]) * width;
                wl[i] = m + s;
                wr[i] = m - s;
            }
        }

        // Equal power, dry against wet, eased in and out with the impulse.
        const float startPresence = presence;
        if (presence < presenceTarget) presence = std::min(presenceTarget, presence + presenceStep * n);
        if (presence > presenceTarget) presence = std::max(presenceTarget, presence - presenceStep * n);
        if (mix <= 0.0f) {
            // Still convolving, so turning it up later finds the tail in place.
            continue;
        }
        auto gains = [&](float pres, float &dry, float &wet) {
            const float m = mix * pres;
            dry = m >= 1.0f ? 0.0f : std::cos(m * kHalfPi);
            wet = m >= 1.0f ? 1.0f : std::sin(m * kHalfPi);
        };
        float dry0, wet0, dry1, wet1;
        gains(startPresence, dry0, wet0);
        gains(presence, dry1, wet1);
        const float step = 1.0f / static_cast<float>(n);
        for (int32_t i = 0; i < n; ++i) {
            const float f = startPresence == presence ? 1.0f : static_cast<float>(i + 1) * step;
            const float dry = dry0 + (dry1 - dry0) * f, wet = wet0 + (wet1 - wet0) * f;
            const float xl = L[at + i], xr = inR[at + i];
            L[at + i] = xl * dry + wl[i] * wet;
            R[at + i] = xr * dry + wr[i] * wet;
        }
    }
    return mix > 0.0f || stereoIn;
}

} // namespace acidulous::effect
