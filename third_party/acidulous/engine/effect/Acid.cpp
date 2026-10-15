#include "Acid.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/** Ticks in a sixteenth, at the engine's 240 a beat (see dsp::Lfo::phaseAt). */
constexpr int64_t kSixteenth = 60;
/** The level Reflux's drive is normalised at, so drive changes the tone and not the level. */
constexpr float kNominal = 0.45f;

/**
 * The patterns, a step each: 0 rests, 1 plays, 2 plays with an accent.
 * Written for a sixteenth-note bass line's feel, offbeats and pushes
 * included, so a held sound moves like one.
 */
constexpr uint8_t kSteps[Acid::kPatterns][16] = {
    {2, 0, 1, 0, 2, 0, 1, 0, 2, 0, 1, 0, 2, 0, 1, 0}, // eighths, accents on the beat
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, // every sixteenth
    {0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 1}, // offbeats
    {2, 0, 0, 1, 0, 0, 1, 0, 2, 0, 0, 1, 0, 1, 0, 0}, // a dotted push
    {2, 1, 0, 1, 1, 0, 2, 0, 1, 1, 0, 1, 2, 0, 1, 1}, // busy
    {2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, // half notes
    {1, 0, 2, 1, 0, 1, 2, 0, 1, 0, 2, 1, 0, 1, 2, 2}, // syncopated
    {2, 0, 1, 1, 0, 1, 0, 1, 2, 1, 0, 1, 1, 0, 2, 0}, // rolling
};
} // namespace

const ParamDef *Acid::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"cutoff", 40.0f, 12000.0f, 500.0f, Curve::Exponential, 0, "Hz"},
        {"resonance", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"env", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"decay", 30.0f, 3000.0f, 250.0f, Curve::Exponential, 0, "ms"},
        {"accent", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"pattern", 0.0f, 8.0f, 0.0f, Curve::Stepped, 9, ""}, // the track, then eight patterns
        {"drive", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"mode", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""}, // low pass, band pass
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Acid::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (int c = 0; c < 2; ++c) {
        first[c].setSampleRate(sr);
        second[c].setSampleRate(sr);
    }
    sweep.setSampleRate(sr);
    accentEnv.setSampleRate(sr);
    accentEnv.setTimes(0.001f, 0.12f);
    reset();
}

void Acid::reset() {
    for (int c = 0; c < 2; ++c) { first[c].reset(); second[c].reset(); }
    sweep.kill();
    accentEnv.kill();
    accented = false;
    fast = slow = loud = 0.0f;
    armed = true;
    lastStep = -1;
    countdown = 0;
}

void Acid::hit(bool accent) {
    // As Reflux's note-on: the sweep, shorter on an accent, and the accent's own.
    sweep.setTimes(0.003f, params_.get(Decay) * 0.001f * (accent ? 0.6f : 1.0f));
    sweep.trigger();
    accented = accent;
    if (accent) accentEnv.trigger();
}

bool Acid::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float cutoff = p.get(Cutoff), resonance = p.get(Resonance), env = p.get(Env);
    const float accentAmt = p.get(Accent), mix = p.get(Mix);
    const int pattern = std::clamp(static_cast<int>(p.get(Pattern) + 0.5f), 0, kPatterns);
    const bool bandpass = p.get(Mode) >= 0.5f;
    const float driveGain = 1.0f + p.get(Drive) * 7.0f;
    const float driveComp = kNominal / dsp::fastTanh(kNominal * driveGain);
    const float fastCoef = dsp::onePoleCoeff(0.002f, sr), slowCoef = dsp::onePoleCoeff(0.08f, sr);
    const double ticksPerSample = frames > 0 ? static_cast<double>(blockEnd - blockStart) / frames : 0.0;

    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        if (pattern == 0) {
            // A hit is the fast follower jumping well over the slow one; it
            // re-arms once they've come back together.
            const float a = std::max(std::fabs(inL), std::fabs(inR));
            fast += (a - fast) * fastCoef;
            slow += (a - slow) * slowCoef;
            loud += (a - loud) * slowCoef * 0.05f;
            if (armed && fast > 0.01f && fast > slow * 2.0f) {
                hit(fast > loud * 3.0f);
                armed = false;
            } else if (!armed && fast < slow * 1.2f) {
                armed = true;
            }
        } else {
            const auto tick = blockStart + static_cast<int64_t>(i * ticksPerSample);
            const int64_t step = tick / kSixteenth;
            if (step != lastStep) {
                lastStep = step;
                const uint8_t s = kSteps[pattern - 1][step % 16];
                if (s != 0) hit(s == 2);
            }
        }

        const float fenv = sweep.next();
        const float aenv = accentEnv.next();
        if (countdown-- <= 0) {
            // In octaves, as Reflux's: up to four, and half again on an accent.
            const float octaves = fenv * env * 4.0f * (1.0f + (accented ? accentAmt * 1.5f : 0.0f));
            const float fc = cutoff * std::exp2(octaves);
            for (int c = 0; c < 2; ++c) {
                first[c].set(fc, resonance);
                second[c].set(fc, resonance * 0.3f);
            }
            countdown = 3;
        }
        const float level = 1.0f + (accented ? accentAmt * aenv * 0.6f : 0.0f);
        float out[2];
        const float in[2] = {inL, inR};
        for (int c = 0; c < (stereoIn ? 2 : 1); ++c) {
            float s = bandpass ? first[c].bandpass(in[c]) * first[c].bandNorm() : first[c].lowpass(in[c]);
            s = dsp::fastTanh(s * 1.3f) * 0.77f;
            s = second[c].lowpass(s);
            out[c] = dsp::fastTanh(s * driveGain) * driveComp * level;
        }
        L[i] = inL + (out[0] - inL) * mix;
        if (stereoIn) R[i] = inR + (out[1] - inR) * mix;
    }
    return stereoIn;
}

} // namespace acidulous::effect
