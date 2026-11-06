#include "Acid.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/inputmod/Scales.h>

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

/**
 * The note each step sings when `from` is a scale, in steps of the scale
 * above the key: the track's hits first, then a line for each pattern.
 * Mostly the root, as a bass line is, with the octave and a few others.
 */
constexpr int8_t kDegrees[Acid::kPatterns + 1][16] = {
    {0, 0, 2, 0, 4, 0, 7, 3, 0, 0, 5, 0, 2, 4, 0, -1}, // the track's hits
    {0, 0, 0, 0, 7, 0, 0, 0, 3, 0, 0, 0, 4, 0, 2, 0},
    {0, 0, 7, 0, 0, 2, 0, 3, 0, 0, 7, 0, 4, 3, 2, -1},
    {0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 4, 0, 0, 0, 3, 7},
    {0, 0, 0, 7, 0, 0, 3, 0, 0, 0, 0, 4, 0, 2, 0, 0},
    {0, 7, 0, 0, 3, 0, 0, 0, 4, 2, 0, 0, 0, 0, 7, 5},
    {0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 7, 3, 0, 0, 4, 0, 2, 0, 7, 5, 0, 0, 3, 0},
    {0, 0, 0, 7, 0, 3, 0, 4, 0, 0, 0, 2, 7, 0, 0, 0},
};
/** The steps that slide first in each line, a bit a step: as a bass line's slide steps. */
constexpr uint16_t kSlideSteps[Acid::kPatterns + 1] = {
    (1 << 7) | (1 << 13),
    (1 << 6) | (1 << 14),
    (1 << 3) | (1 << 7) | (1 << 13),
    (1 << 6) | (1 << 14),
    (1 << 3) | (1 << 9),
    (1 << 1) | (1 << 9) | (1 << 15),
    (1 << 8),
    (1 << 3) | (1 << 11) | (1 << 15),
    (1 << 3) | (1 << 7) | (1 << 12),
};

/**
 * Each step's place in the order steps start to slide as `slides` goes up:
 * a line's own slide steps, then the rest spread over the bar.
 */
struct SlideRanks {
    uint8_t rank[Acid::kPatterns + 1][16]{};
    constexpr SlideRanks() {
        constexpr uint8_t spread[16] = {15, 7, 11, 3, 13, 5, 9, 1, 14, 6, 10, 2, 12, 4, 8, 0};
        for (int r = 0; r <= Acid::kPatterns; ++r) {
            uint8_t next = 0;
            for (int i = 0; i < 16; ++i) if ((kSlideSteps[r] >> i) & 1) rank[r][i] = next++;
            for (int i : spread) if (!((kSlideSteps[r] >> i) & 1)) rank[r][i] = next++;
        }
    }
};
constexpr SlideRanks kRanks;
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
        {"follow", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"from", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""}, // a scale, the track's notes
        {"key", 0.0f, 11.0f, 9.0f, Curve::Stepped, 12, ""}, // C to B, A by default
        {"scale", 0.0f, 32.0f, 5.0f, Curve::Stepped, music::kScaleCount, ""}, // minor by default
        {"octave", -2.0f, 3.0f, 1.0f, Curve::Stepped, 6, ""},
        {"glide", 10.0f, 1000.0f, 80.0f, Curve::Exponential, 0, "ms"},
        {"slides", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
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
    for (int n = 0; n < 128; ++n) { held[n] = false; heldHz[n] = 0.0f; }
    heldCount = 0;
    keySet = false;
    keyAt = keyTo = 0.0f;
    keyCoef = 1.0f;
    hits = notes = cycle = 0;
}

bool Acid::slides(int32_t row, int32_t index) const {
    const auto count = static_cast<int32_t>(std::lround(std::clamp(params_.target(Slides), 0.0f, 1.0f) * 16.0f));
    return kRanks.rank[row][index & 15] < count;
}

void Acid::aimAt(float hz, bool slide) {
    if (!(hz > 0.0f)) return;
    keyTo = std::log2(hz);
    if (!keySet || !slide) {
        keyAt = keyTo;
        keyCoef = 1.0f;
        keySet = true;
        return;
    }
    // A slide gets most of the way there in the glide time. The key moves
    // with the filter, every fourth sample.
    keyCoef = dsp::onePoleCoeff(params_.target(Glide) * 0.001f / 3.0f, sr * 0.25f);
}

void Acid::pickNote(int32_t row, int32_t index) {
    const int octave = std::clamp(steppedTarget(Octave), -2, 3);
    if (fromNotes()) {
        // On a pattern each step takes the next held note up; with the
        // track's hits the notes themselves move it (see onNote).
        if (row == 0 || heldCount == 0) return;
        const int32_t pick = cycle % heldCount;
        cycle = pick + 1;
        int32_t seen = 0;
        for (int n = 0; n < 128; ++n) {
            if (!held[n]) continue;
            if (seen++ == pick) {
                aimAt(heldHz[n] * std::exp2(static_cast<float>(octave)), slides(row, index));
                return;
            }
        }
        return;
    }
    const music::ScaleDef &d = music::kScales[std::clamp(steppedTarget(Scale), 0, music::kScaleCount - 1)];
    const int degree = kDegrees[row][index & 15];
    const int up = degree >= 0 ? degree / d.count : -((-degree + d.count - 1) / d.count);
    const int step = degree - up * d.count;
    const int note = 48 + std::clamp(steppedTarget(Key), 0, 11) + 12 * (octave + up) + d.intervals[step];
    aimAt(noteHz(note), slides(row, index));
}

void Acid::onNote(int32_t note, float hz) {
    if (note < 0 || note > 127) return;
    const bool legato = heldCount > 0;
    if (!held[note]) ++heldCount;
    held[note] = true;
    heldHz[note] = hz;
    if (!fromNotes() || steppedTarget(Pattern) != 0) return;
    // Played straight: the note itself, gliding when it's tied to the last one.
    const int32_t index = notes;
    notes = (notes + 1) & 15;
    const int octave = std::clamp(steppedTarget(Octave), -2, 3);
    aimAt(hz * std::exp2(static_cast<float>(octave)), legato || slides(0, index));
}

void Acid::onNoteOff(int32_t note) {
    if (note < 0 || note > 127 || !held[note]) return;
    held[note] = false;
    --heldCount;
}

void Acid::onAllNotesOff() {
    for (bool &h : held) h = false;
    heldCount = 0;
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
    const float follow = std::clamp(p.get(Follow), 0.0f, 1.0f), logCutoff = std::log2(cutoff);
    const int pattern = std::clamp(static_cast<int>(p.get(Pattern) + 0.5f), 0, kPatterns);
    const bool bandpass = p.get(Mode) >= 0.5f;
    const float driveGain = 1.0f + p.get(Drive) * 7.0f;
    const float driveComp = kNominal / dsp::fastTanh(kNominal * driveGain);
    const float fastCoef = dsp::onePoleCoeff(0.002f, sr), slowCoef = dsp::onePoleCoeff(0.08f, sr);
    const double ticksPerSample = frames > 0 ? static_cast<double>(blockEnd - blockStart) / blockFramesOf(frames) : 0.0;

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
                pickNote(0, hits);
                hits = (hits + 1) & 15;
                armed = false;
            } else if (!armed && fast < slow * 1.2f) {
                armed = true;
            }
        } else {
            const auto tick = blockStart + static_cast<int64_t>((i + subOffset_) * ticksPerSample);
            const int64_t step = tick / kSixteenth;
            if (step != lastStep) {
                lastStep = step;
                const uint8_t s = kSteps[pattern - 1][step % 16];
                if (s != 0) {
                    hit(s == 2);
                    pickNote(pattern, static_cast<int32_t>(step & 15));
                }
            }
        }

        const float fenv = sweep.next();
        const float aenv = accentEnv.next();
        if (countdown-- <= 0) {
            // In octaves, as Reflux's: up to four, and half again on an accent.
            const float octaves = fenv * env * 4.0f * (1.0f + (accented ? accentAmt * 1.5f : 0.0f));
            float fc;
            if (follow > 0.0f && keySet) {
                // From the knob toward the note, in octaves, and the sweep on top.
                keyAt += (keyTo - keyAt) * keyCoef;
                fc = std::exp2(logCutoff + (keyAt - logCutoff) * follow + octaves);
            } else {
                keyAt = keyTo;
                fc = cutoff * std::exp2(octaves);
            }
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
