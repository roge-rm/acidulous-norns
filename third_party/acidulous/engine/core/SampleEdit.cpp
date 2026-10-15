#include "SampleEdit.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include <engine/dsp/MultiFilter.h>

namespace acidulous::audio {

namespace {

/** Runs [fn] on each channel. A mono file has an empty `right`. */
template <typename Fn>
void eachChannel(SampleData &data, Fn fn) {
    fn(data.left);
    if (data.stereo && !data.right.empty()) fn(data.right);
}

int32_t msToFrames(float ms, int32_t rate) {
    return std::max(0, static_cast<int32_t>(ms * 0.001f * static_cast<float>(rate)));
}

float loudest(const SampleData &data) {
    float peak = 0.0f;
    for (float v : data.left) peak = std::max(peak, std::abs(v));
    for (float v : data.right) peak = std::max(peak, std::abs(v));
    return peak;
}

} // namespace

void cropTo(SampleData &data, int32_t from, int32_t to) {
    const int32_t end = to > from ? std::min(to, data.frames) : data.frames;
    const int32_t start = std::clamp(from, 0, end);
    if (start == 0 && end == data.frames) return;
    if (end - start < 1) return; // ignore a crop to nothing
    eachChannel(data, [&](std::vector<float> &ch) {
        ch.erase(ch.begin() + end, ch.end());
        ch.erase(ch.begin(), ch.begin() + start);
    });
    data.frames = end - start;
    data.loopStart = -1;
    data.loopEnd = -1;
}

void fadeEnds(SampleData &data, int32_t inFrames, int32_t outFrames) {
    if (data.frames <= 0) return;
    // Don't let the fades overlap, or they'd multiply and leave a dip in the
    // middle.
    const int32_t rise = std::clamp(inFrames, 0, data.frames);
    const int32_t fall = std::clamp(outFrames, 0, data.frames - rise);
    eachChannel(data, [&](std::vector<float> &ch) {
        for (int32_t i = 0; i < rise; ++i) {
            ch[static_cast<size_t>(i)] *= static_cast<float>(i) / static_cast<float>(rise);
        }
        for (int32_t i = 0; i < fall; ++i) {
            const int32_t at = data.frames - 1 - i;
            ch[static_cast<size_t>(at)] *= static_cast<float>(i) / static_cast<float>(fall);
        }
    });
}

void applyGain(SampleData &data, float linear) {
    if (linear == 1.0f) return;
    eachChannel(data, [&](std::vector<float> &ch) {
        for (float &v : ch) v *= linear;
    });
}

void normalisePeak(SampleData &data, float peak) {
    if (peak <= 0.0f) return;
    const float now = loudest(data);
    if (now < 1e-6f) return; // leave silence alone
    applyGain(data, peak / now);
    data.peak = peak;
}

void reverseInPlace(SampleData &data) {
    eachChannel(data, [](std::vector<float> &ch) { std::reverse(ch.begin(), ch.end()); });
}

void filterInPlace(SampleData &data, float cutoffHz, float resonance, int32_t type) {
    if (cutoffHz <= 0.0f || data.frames <= 0) return;
    // A separate filter per channel so they don't share state.
    eachChannel(data, [&](std::vector<float> &ch) {
        dsp::MultiFilter f;
        f.setSampleRate(static_cast<float>(data.rate));
        f.set(cutoffHz, resonance, type, dsp::MultiFilter::Clean, 0.0f);
        for (float &v : ch) v = f.process(v);
    });
}

void compressInPlace(SampleData &data, float amount, float attackMs, float releaseMs) {
    if (amount <= 0.0f || data.frames <= 0) return;
    const float squeeze = std::clamp(amount, 0.0f, 1.0f);
    // One knob: the threshold goes down to -24 dB and the ratio up to 8:1.
    const float threshold = std::pow(10.0f, -24.0f * squeeze / 20.0f);
    const float ratio = 1.0f + 7.0f * squeeze;

    // Since this runs offline it can look ahead. The gain reduction is worked
    // out for every frame first, then smoothed with the attack running
    // backwards, so the gain is already down when a transient arrives.
    //
    // The reduction is smoothed in dB with a slope limit, which matches what
    // attack and release times mean and can't overshoot.
    const auto n = static_cast<size_t>(data.frames);
    const bool twoUp = data.stereo && !data.right.empty();
    std::vector<float> cut(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const float l = std::abs(data.left[i]);
        const float peak = twoUp ? std::max(l, std::abs(data.right[i])) : l;
        if (peak > threshold) {
            const float overDb = 20.0f * std::log10(peak / threshold);
            cut[i] = overDb * (1.0f - 1.0f / ratio);
        }
    }

    const auto rate = static_cast<float>(data.rate);
    // 60 dB over the attack or release time.
    const float attackSlope = 60.0f / std::max(1.0f, attackMs * 0.001f * rate);
    const float releaseSlope = 60.0f / std::max(1.0f, releaseMs * 0.001f * rate);
    for (size_t i = n - 1; i-- > 0;) {
        cut[i] = std::max(cut[i], cut[i + 1] - attackSlope);
    }
    for (size_t i = 1; i < n; ++i) {
        cut[i] = std::max(cut[i], cut[i - 1] - releaseSlope);
    }

    const float before = loudest(data);
    for (size_t i = 0; i < n; ++i) {
        const float gain = std::pow(10.0f, -cut[i] / 20.0f);
        data.left[i] *= gain;
        if (twoUp) data.right[i] *= gain;
    }
    // Makeup gain back to the original peak, so the knob squeezes instead of
    // just making it quieter.
    const float after = loudest(data);
    if (after > 1e-6f && before > 1e-6f) applyGain(data, before / after);
}

bool applyEdit(SampleData &data, const SampleOps &ops, std::string &error) {
    if (data.frames <= 0) {
        error = "there is nothing to edit";
        return false;
    }
    cropTo(data, ops.from, ops.to);
    if (data.frames <= 0) {
        error = "that leaves nothing";
        return false;
    }
    if (ops.reverse) reverseInPlace(data);
    // High pass before the compressor so it doesn't react to low rumble.
    if (ops.lowCutHz > 0.0f) {
        filterInPlace(data, ops.lowCutHz, 0.0f, dsp::MultiFilter::HP12);
    }
    if (ops.cutoffHz > 0.0f) {
        filterInPlace(data, ops.cutoffHz, ops.resonance, ops.filterType);
    }
    compressInPlace(data, ops.squash, ops.squashAttackMs, ops.squashReleaseMs);
    if (ops.gainDb != 0.0f) applyGain(data, std::pow(10.0f, ops.gainDb / 20.0f));
    if (ops.normaliseTo > 0.0f) normalisePeak(data, ops.normaliseTo);
    fadeEnds(data, msToFrames(ops.fadeInMs, data.rate), msToFrames(ops.fadeOutMs, data.rate));
    data.measure();
    return true;
}

} // namespace acidulous::audio
