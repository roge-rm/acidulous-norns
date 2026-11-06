#include "DeEsser.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/** How wide the band is. */
constexpr float kQ = 1.4f;
constexpr float kAttack = 0.0005f, kRelease = 0.06f;
/** How much of the level over the threshold comes off: 5 to 1. */
constexpr float kSlope = 0.8f;
/** In relative mode the threshold counts from here: the default -36 is a band 10 dB under the whole. */
constexpr float kRelativeOffset = 26.0f;
/** Throw's note values in quarter notes: 1/16, 1/8, 1/8., 1/4, 1/4., 1/2. */
constexpr int kThrowTimes = 6;
constexpr float kThrowBeats[kThrowTimes] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
/** The echo line's length: a half note at 40 bpm. Slower tempos are clamped to it. */
constexpr float kMaxThrowSeconds = 3.0f;
/**
 * Throw at full sends the cut 18 dB up. What a de-esser takes off is a few dB
 * of a band that is already well under the voice, so at 1:1 the echo is lost.
 */
constexpr float kSendGain = 8.0f;
/** The loop's high-pass, so repeats don't pile up low mud. */
constexpr float kLoopHighpassHz = 400.0f;
/** A change of read over this fraction crossfades, under it glides. */
constexpr float kFadeOver = 0.02f;
constexpr float kFadeSeconds = 0.03f;
/** Under this (-120 dB) the echo counts as rung out. */
constexpr float kQuiet = 1.0e-6f;
} // namespace

const ParamDef *DeEsser::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"freq", 2000.0f, 12000.0f, 6000.0f, Curve::Exponential, 0, "Hz"},
        {"threshold", -60.0f, 0.0f, -36.0f, Curve::Linear, 0, "dB"},
        {"range", 0.0f, 24.0f, 12.0f, Curve::Linear, 0, "dB"},
        {"mode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""}, // split, wide
        {"listen", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"relative", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"throw", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"time", 0.0f, 5.0f, 2.0f, Curve::Stepped, kThrowTimes, ""}, // 1/16 1/8 1/8. 1/4 1/4. 1/2
        {"feedback", 0.0f, 0.9f, 0.45f, Curve::Linear, 0, ""},
    };
    count = Count;
    return defs;
}

void DeEsser::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    freqWas = -1.0f;
    for (auto &e : echo) e.prepare(static_cast<int32_t>(sr * kMaxThrowSeconds) + 64);
    fadeLen = std::max(1, static_cast<int32_t>(kFadeSeconds * sr));
    reset();
}

void DeEsser::reset() {
    detect.reset();
    band[0].reset();
    band[1].reset();
    bandEnv = fullEnv = 0.0f;
    // The line is forgotten when the echo next wakes.
    echoing = false;
}

void DeEsser::wakeEcho() {
    for (auto &e : echo) e.forget();
    hpIn[0] = hpIn[1] = hpOut[0] = hpOut[1] = 0.0f;
    sendNow = fbNow = outNow = 0.0f;
    quietFrames = 0;
    fadeLeft = 0;
    snapRead = true;
    echoing = true;
}

bool DeEsser::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float freq = p.get(Freq);
    if (freq != freqWas) {
        freqWas = freq;
        detect.bandpass(freq, kQ, sr);
        band[0].copyCoefficients(detect);
        band[1].copyCoefficients(detect);
    }
    const float threshold = p.get(Threshold), range = p.get(Range);
    const bool wide = p.get(Mode) >= 0.5f, listen = p.get(Listen) >= 0.5f, relative = p.get(Relative) >= 0.5f;
    const float atk = dsp::onePoleCoeff(kAttack, sr), rel = dsp::onePoleCoeff(kRelease, sr);
    const int chans = stereoIn ? 2 : 1;

    // Throw. The echo sleeps until it's sent something and again once it has rung out.
    const int timeIdx = std::clamp(static_cast<int>(p.get(Time) + 0.5f), 0, kThrowTimes - 1);
    const float maxRead = sr * kMaxThrowSeconds;
    const float target = std::clamp(kThrowBeats[timeIdx] * 60.0f / std::max(bpm, 1.0f) * sr, 1.0f, maxRead);
    const float sendTarget = listen ? 0.0f : p.get(Throw) * kSendGain;
    const float fbTarget = p.get(Feedback), outTarget = listen ? 0.0f : 1.0f;
    if (!echoing && sendTarget > 0.0f) wakeEcho();
    if (echoing && snapRead) {
        readSamples = target;
        fbNow = fbTarget;
        outNow = outTarget;
        snapRead = false;
    }
    // A new note value or a tempo jump crossfades to the new read; a tempo
    // drifting by a little glides it, which bends the repeats by as little.
    if (echoing && fadeLeft == 0 && std::fabs(target - readSamples) > kFadeOver * readSamples) {
        fadeFrom = readSamples;
        readSamples = target;
        fadeLeft = fadeLen;
    }
    const float smooth = dsp::onePoleCoeff(0.01f, sr);
    const float glide = dsp::onePoleCoeff(0.05f, sr);
    const float hpA = 1.0f / (1.0f + dsp::kTwoPi * kLoopHighpassHz / sr);

    for (int32_t i = 0; i < frames; ++i) {
        const float l = L[i], r = stereoIn ? R[i] : l;
        const float mono = (l + r) * 0.5f;
        const float b = std::fabs(detect.process(mono)), a = std::fabs(mono);
        bandEnv = dsp::guardDenormal(bandEnv + (b - bandEnv) * (b > bandEnv ? atk : rel));
        fullEnv = dsp::guardDenormal(fullEnv + (a - fullEnv) * (a > fullEnv ? atk : rel));
        float over = 20.0f * std::log10(bandEnv + 1.0e-9f);
        // Relative: the band against the whole sound, with the threshold
        // moved up by kRelativeOffset so -36 dB means the band within 10 dB.
        over -= relative ? 20.0f * std::log10(fullEnv + 1.0e-9f) + threshold + kRelativeOffset : threshold;
        const float cut = std::clamp(over * kSlope, 0.0f, range);
        const float g = cut > 0.0f ? std::pow(10.0f, -cut * 0.05f) : 1.0f;
        float taken = 0.0f; // what came off the band, for the throw
        for (int c = 0; c < chans; ++c) {
            float *buf = c == 0 ? L : R;
            const float x = buf[i];
            const float bx = band[c].process(x);
            if (listen) buf[i] = bx;
            else if (wide) buf[i] = x * g;
            else buf[i] = x + (g - 1.0f) * bx;
            taken += bx * (1.0f - g);
        }
        if (!echoing) continue;
        if (!stereoIn) R[i] = L[i];
        else taken *= 0.5f;
        sendNow += (sendTarget - sendNow) * smooth;
        fbNow += (fbTarget - fbNow) * smooth;
        outNow += (outTarget - outNow) * smooth;
        float tap[2];
        if (fadeLeft > 0) {
            const float t = 1.0f - static_cast<float>(fadeLeft--) / static_cast<float>(fadeLen);
            for (int c = 0; c < 2; ++c) tap[c] = echo[c].read(fadeFrom) * (1.0f - t) + echo[c].read(readSamples) * t;
        } else {
            readSamples += (target - readSamples) * glide;
            tap[0] = echo[0].read(readSamples);
            tap[1] = echo[1].read(readSamples);
        }
        for (int c = 0; c < 2; ++c) {
            hpOut[c] = dsp::guardDenormal(hpA * (hpOut[c] + tap[c] - hpIn[c]));
            hpIn[c] = tap[c];
        }
        // Ping-pong: left takes the cut and the right's repeat, right the
        // left's. The soft limit keeps a loud s or a long feedback under 1.
        const float w0 = dsp::fastTanh(taken * sendNow + fbNow * hpOut[1]);
        const float w1 = dsp::fastTanh(fbNow * hpOut[0]);
        echo[0].write(w0);
        echo[1].write(w1);
        L[i] += hpOut[0] * outNow;
        R[i] += hpOut[1] * outNow;
        const float loud = std::max(std::max(std::fabs(w0), std::fabs(w1)), std::max(std::fabs(hpOut[0]), std::fabs(hpOut[1])));
        // Held at the line's length, which is all the sleep check needs, so it can't overflow.
        quietFrames = loud > kQuiet ? 0 : std::min(quietFrames + 1, echo[0].capacity());
    }
    if (!echoing) return stereoIn;
    // Nothing more going in and nothing left that a read can reach: asleep.
    if (sendTarget <= 0.0f && sendNow < kQuiet && static_cast<float>(quietFrames) > std::max({readSamples, target, fadeLeft > 0 ? fadeFrom : 0.0f}) + 2.0f) {
        echoing = false;
    }
    return true;
}

} // namespace acidulous::effect
