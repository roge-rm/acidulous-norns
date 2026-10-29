#include "Effects.h"
#include <algorithm>
#include <cmath>
#include <engine/core/Settings.h>
#include <engine/inputmod/Scales.h>

namespace acidulous::effect {

using dsp::clampf;
using dsp::fastTanh;
using dsp::Lfo;

namespace {

constexpr float kMaxDelaySeconds = 3.0f;

// Delay note values in quarter notes.
constexpr int kDelayTimes = 8;
constexpr float kDelayBeats[kDelayTimes] = {0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f};

inline float peakEnvelope(float &env, float in, float attack, float release) {
    const float a = std::fabs(in);
    env += (a - env) * (a > env ? attack : release);
    return env;
}

inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }

inline float xorshift01(uint32_t &s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<float>(s & 0xffffff) / 16777216.0f;
}

} // namespace

// --- Delay ------------------------------------------------------------------------

const ParamDef *Delay::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"time", 0.0f, 7.0f, 3.0f, Curve::Stepped, kDelayTimes, ""},   // 1/32 .. 1 bar
        {"feedback", 0.0f, 0.95f, 0.4f, Curve::Linear, 0, ""},
        {"tone", 300.0f, 18000.0f, 6000.0f, Curve::Exponential, 0, "Hz"},
        {"mode", 0.0f, 3.0f, 1.0f, Curve::Stepped, 4, ""}, // normal, ping-pong, multi-tap, reverse
        {"mix", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
        {"duck", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"wobble", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"taps", 2.0f, 4.0f, 3.0f, Curve::Stepped, 3, ""},        // multi-tap only
        {"spacing", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},     // multi-tap: early .. even .. late
    };
    count = Count;
    return defs;
}

namespace {
/** The crossfade between reverse stretches, at most a quarter of a stretch. */
constexpr float kReverseFadeSeconds = 0.01f;
/** Room left in the line for wobble's drift, which is at most 70 frames either way. */
constexpr float kReverseDriftRoom = 160.0f;
} // namespace

void Delay::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    // The line's length is part of how a read rounds (see wrappedReadIndex),
    // so it stays as it was for songs made before the modes.
    for (auto &l : line) l.prepare(static_cast<int32_t>(sr * kMaxDelaySeconds) + 64);
    // A stretch reads back up to twice its length and its tail.
    for (auto &l : revLine) l.prepare(static_cast<int32_t>(2.0f * (kMaxDelaySeconds + kReverseFadeSeconds) * sr + kReverseDriftRoom) + 64);
    reset();
}

void Delay::reset() {
    for (auto &l : line) l.clear();
    for (auto &l : revLine) l.forget();
    lp[0] = lp[1] = 0.0f;
    duckEnv = 0.0f;
    readSamples = clampf(kDelayBeats[3] * 60.0f / bpm * sr, 1.0f, sr * kMaxDelaySeconds);
    // Jump straight to the requested length on the first block after a reset
    // instead of gliding there, so a render always starts the same.
    snapRead = true;
    // Reset the flutter too, so renders repeat exactly.
    wobblePhase = 0.0f;
    revAt = 0.0f;
    revLen = revPrevLen = readSamples;
    revTail = 0.0f;
    revFadeAt = revFadeLen = revFadeD = 0.0f;
    revFadeNormal = false;
    revTicks = 0.0;
    revJumped = false;
    revStart = true;
    clock = 0.0;
    synced = false;
    revWindow = 0;
}

void Delay::onBlock(int64_t tickStart, int64_t, float b) {
    bpm = b;
    // Ticks are whole, so the clock keeps its own fraction. A jump (play, a
    // stop putting the clock back to 0) starts it from the block's tick.
    const double start = static_cast<double>(tickStart);
    if (!synced || clock < start - 1.0 || clock > start + 2.0) {
        revJumped = synced;
        clock = start;
    } else clock = std::clamp(clock, start, start + 0.999999);
    synced = true;
}

float Delay::wobbleDrift(float depth) {
    wobblePhase += 0.9f / sr;
    if (wobblePhase >= 1.0f) wobblePhase -= 1.0f;
    if (depth <= 0.0f) return 0.0f;
    return depth * (0.6f * std::sin(wobblePhase * dsp::kTwoPi) + 0.4f * std::sin(wobblePhase * dsp::kTwoPi * 2.71f));
}

float Delay::duckedMix(float inL, float inR) {
    const float env = peakEnvelope(duckEnv, (std::fabs(inL) + std::fabs(inR)) * 1.5f, duckAtk_, duckRel_);
    return mix_ * (1.0f - duck_ * clampf(env, 0.0f, 1.0f));
}

bool Delay::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const int timeIdx = static_cast<int>(p.get(Time) + 0.5f);
    const float target = clampf(kDelayBeats[timeIdx] * 60.0f / bpm * sr, 1.0f, sr * kMaxDelaySeconds);
    const float fb = p.get(Feedback), mix = p.get(Mix), duck = p.get(Duck), wobble = p.get(Wobble);
    const int mode = static_cast<int>(p.get(Mode) + 0.5f);
    const bool pingPong = mode == PingPong;
    const float toneCoeff = dsp::onePoleCoeff(1.0f / (dsp::kTwoPi * p.get(Tone)), sr);
    const float duckAtk = dsp::onePoleCoeff(0.002f, sr), duckRel = dsp::onePoleCoeff(0.25f, sr);
    // The read position glides to a new note value, giving a pitch swoop instead of a click.
    const float glide = dsp::onePoleCoeff(0.05f, sr);
    const float wobbleInc = 0.9f / sr, wobbleDepth = wobble * 70.0f;
    const bool wobbling = wobbleDepth > 0.0f;

    if (snapRead) { readSamples = target; snapRead = false; }
    // Each time the mode comes round to reverse, it starts part way into the
    // stretch the song is in, fading from the echo the other mode played.
    if (mode != Reverse) revStart = true;
    const double ticksPerFrame = static_cast<double>(bpm) * kPPQN / (60.0 * static_cast<double>(sr));
    if (mode == MultiTap || mode == Reverse) {
        fb_ = fb; mix_ = mix; duck_ = duck; wobbleDepth_ = wobbleDepth;
        duckAtk_ = duckAtk; duckRel_ = duckRel; glide_ = glide;
        if (mode == MultiTap) runMultiTap(L, R, frames, stereoIn, target, toneCoeff);
        else runReverse(L, R, frames, stereoIn, target, toneCoeff, timeIdx, ticksPerFrame);
        clock += frames * ticksPerFrame;
        revJumped = false;
        return true;
    }
    // Normal and ping-pong: this loop is the delay as it always was, so
    // songs made before the modes came out the same.
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        readSamples += (target - readSamples) * glide;
        wobblePhase += wobbleInc;
        if (wobblePhase >= 1.0f) wobblePhase -= 1.0f;
        // Two unrelated sines, so it sounds like tape flutter, not vibrato.
        // Skipped entirely when the control is at 0.
        const float drift = wobbling ? wobbleDepth * (0.6f * std::sin(wobblePhase * dsp::kTwoPi) +
                                                      0.4f * std::sin(wobblePhase * dsp::kTwoPi * 2.71f))
                                     : 0.0f;
        const float rd = readSamples + drift;
        const float tapL = line[0].read(rd), tapR = line[1].read(rd);
        lp[0] += (tapL - lp[0]) * toneCoeff;
        lp[1] += (tapR - lp[1]) * toneCoeff;
        const float w0 = pingPong ? inL * 0.5f + inR * 0.5f + lp[1] * fb : inL + lp[0] * fb;
        const float w1 = pingPong ? lp[0] * fb : inR + lp[1] * fb;
        line[0].write(w0);
        line[1].write(w1);
        revLine[0].write(w0);
        revLine[1].write(w1);
        // Duck: echoes stay under the dry signal and swell up in the gaps.
        const float env = peakEnvelope(duckEnv, (std::fabs(inL) + std::fabs(inR)) * 1.5f, duckAtk, duckRel);
        const float wet = mix * (1.0f - duck * clampf(env, 0.0f, 1.0f));
        L[i] = inL + tapL * wet;
        R[i] = inR + tapR * wet;
    }
    clock += frames * ticksPerFrame;
    revJumped = false;
    return true;
}

void Delay::runMultiTap(float *L, float *R, int32_t frames, bool stereoIn, float target, float toneCoeff) {
    const auto &p = params_;
    const int taps = std::clamp(static_cast<int>(p.get(Taps) + 0.5f), 2, kMaxTaps);
    // Where each tap sits in the time, as a fraction: k/taps bent by spacing,
    // so at 0 they're even, below it they bunch early and above it late. The
    // last is always the full time.
    const float bend = std::exp2(-1.5f * p.get(Spacing));
    float frac[kMaxTaps], gl[kMaxTaps], gr[kMaxTaps];
    // Pans run left to right, taken outside in: hard left, hard right, then
    // the ones between, so each tap answers the last from the other side.
    static constexpr int kOrder[kMaxTaps][kMaxTaps] = {{0}, {0, 1}, {0, 2, 1}, {0, 3, 1, 2}};
    for (int k = 0; k < taps; ++k) {
        frac[k] = k == taps - 1 ? 1.0f : std::pow(static_cast<float>(k + 1) / static_cast<float>(taps), bend);
        const float pan = -1.0f + 2.0f * static_cast<float>(kOrder[taps - 1][k]) / static_cast<float>(taps - 1);
        const float th = (pan + 1.0f) * 0.25f * dsp::kPi;
        gl[k] = std::cos(th);
        gr[k] = std::sin(th);
    }
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        readSamples += (target - readSamples) * glide_;
        const float drift = wobbleDrift(wobbleDepth_);
        float outL = 0.0f, outR = 0.0f, last = 0.0f;
        for (int k = 0; k < taps; ++k) {
            const float t = line[0].read(readSamples * frac[k] + drift);
            outL += t * gl[k];
            outR += t * gr[k];
            last = t;
        }
        // Only the last tap feeds back, through the tone.
        lp[0] += (last - lp[0]) * toneCoeff;
        const float feed = (inL + inR) * 0.5f + lp[0] * fb_;
        line[0].write(feed);
        // The right line keeps time too, so switching back to a two-line
        // mode doesn't play something old.
        line[1].write(feed);
        revLine[0].write(feed);
        revLine[1].write(feed);
        const float wet = duckedMix(inL, inR);
        L[i] = inL + outL * wet;
        R[i] = inR + outR * wet;
    }
}

void Delay::runReverse(float *L, float *R, int32_t frames, bool stereoIn, float target, float toneCoeff, int timeIdx,
                       double ticksPerFrame) {
    // Stretches are counted from the song's start, so they fall on the same
    // beats whether the song plays through or starts part way.
    const double len = stretchTicks(timeIdx);
    const double song = clock + static_cast<double>(modTransport_.songOffset);
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        readSamples += (target - readSamples) * glide_;
        const float drift = wobbleDrift(wobbleDepth_);
        // Each stretch plays a short tail on past its end, which the next
        // fades in over.
        const double at = song + static_cast<double>(i) * ticksPerFrame;
        const auto window = static_cast<int64_t>(std::floor(at / len));
        // A stretch starts over on its line, and also part way in after a
        // jump (a loop, a move) or a new length.
        if (revStart || (revJumped && i == 0) || window != revWindow || len != revTicks) {
            const auto into = static_cast<float>((at - static_cast<double>(window) * len) / ticksPerFrame);
            startStretch(len, ticksPerFrame, into);
            if (revStart) {
                // Fade from the other mode's tap over this stretch's tail.
                revFadeNormal = true;
                revFadeD = readSamples;
                revFadeAt = 0.0f;
                revFadeLen = revTail;
            }
            revStart = false;
            revWindow = window;
        } else if (revAt >= stretchLimit()) {
            // The clock stood still: carry on a stretch at a time.
            startStretch(len, ticksPerFrame, 0.0f);
        }
        // At t frames into a stretch, reading 2t back plays the stretch
        // before it from its end to its start.
        float tapL, tapR;
        {
            const float d = 2.0f * revAt + 1.0f + drift;
            tapL = revLine[0].read(d);
            tapR = revLine[1].read(d);
        }
        // The fade is equal gain, not equal power. The samples it fades in
        // here are read again in the next stretch's tail and faded out there,
        // so their two gains must add up to 1 or the stretch ends build up in
        // the feedback. The stretch fading out is read on from where it was.
        if (revFadeAt < revFadeLen) {
            const float in = revFadeAt / revFadeLen, out = 1.0f - in;
            const float d = (revFadeNormal ? revFadeD : 2.0f * (revAt + revPrevLen) + 1.0f) + drift;
            tapL = tapL * in + revLine[0].read(d) * out;
            tapR = tapR * in + revLine[1].read(d) * out;
        }
        revFadeAt += 1.0f;
        revAt += 1.0f;
        lp[0] += (tapL - lp[0]) * toneCoeff;
        lp[1] += (tapR - lp[1]) * toneCoeff;
        const float w0 = inL + lp[0] * fb_, w1 = inR + lp[1] * fb_;
        line[0].write(w0);
        line[1].write(w1);
        revLine[0].write(w0);
        revLine[1].write(w1);
        const float wet = duckedMix(inL, inR);
        L[i] = inL + tapL * wet;
        R[i] = inR + tapR * wet;
    }
}

double Delay::stretchTicks(int timeIdx) const {
    // A note value longer than the line holds twice (a bar below 80 bpm) is
    // halved until it fits, so it plays back in halves or quarters, still on
    // the beat.
    const float room = static_cast<float>(revLine[0].capacity()) - kReverseDriftRoom;
    double ticks = static_cast<double>(kDelayBeats[timeIdx]) * kPPQN;
    float len = kDelayBeats[timeIdx] * 60.0f / bpm * sr, tail = std::min(kReverseFadeSeconds * sr, 0.25f * len);
    while (2.0f * (len + tail) > room && ticks > 1.0) {
        len *= 0.5f;
        ticks *= 0.5;
        tail = std::min(kReverseFadeSeconds * sr, 0.25f * len);
    }
    return ticks;
}

void Delay::startStretch(double ticks, double ticksPerFrame, float into) {
    // The stretch ending is read on from where it is now, so its length is
    // taken as where it was at the start of this one, [into] frames ago.
    revPrevLen = revAt - into;
    const float prevTail = revTail;
    revLen = static_cast<float>(ticks / ticksPerFrame);
    revTail = std::min(kReverseFadeSeconds * sr, 0.25f * revLen);
    revAt = into;
    revTicks = ticks;
    revFadeNormal = false;
    // On the line the old stretch's tail fades out from the new one's start.
    // Part way in, the fade starts now and takes the new stretch's tail.
    if (into < 1.0f) {
        revFadeAt = into;
        revFadeLen = prevTail;
    } else {
        revFadeAt = 0.0f;
        revFadeLen = revTail;
    }
}

float Delay::stretchLimit() const {
    // A read goes back twice this plus a fade's tail, and wobble's drift.
    const float room = static_cast<float>(revLine[0].capacity()) - kReverseDriftRoom;
    return std::min(2.0f * revLen, 0.5f * room - kReverseFadeSeconds * sr - 2.0f);
}

// --- Eq ---------------------------------------------------------------------------

const ParamDef *Eq::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"lowgain", -15.0f, 15.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"lowfreq", 40.0f, 500.0f, 120.0f, Curve::Exponential, 0, "Hz"},
        {"midgain", -15.0f, 15.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"midfreq", 200.0f, 8000.0f, 1000.0f, Curve::Exponential, 0, "Hz"},
        {"midq", 0.3f, 6.0f, 0.8f, Curve::Exponential, 0, ""},
        {"highgain", -15.0f, 15.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"highfreq", 1500.0f, 16000.0f, 6000.0f, Curve::Exponential, 0, "Hz"},
        {"tilt", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Eq::prepare(int32_t sampleRate) { sr = static_cast<float>(sampleRate); reset(); }
void Eq::reset() {
    for (int c = 0; c < 2; ++c) { low[c].reset(); mid[c].reset(); high[c].reset(); tiltLo[c].reset(); tiltHi[c].reset(); }
}

// Kept out of line, so process() compiles as it did before slicing and a
// song without mods renders to the bit.
__attribute__((noinline)) void Eq::remember() {
    const auto &p = params_;
    lowFor[0] = p.get(LowFreq); lowFor[1] = p.get(LowGain);
    midFor[0] = p.get(MidFreq); midFor[1] = p.get(MidGain); midFor[2] = p.get(MidQ);
    highFor[0] = p.get(HighFreq); highFor[1] = p.get(HighGain);
    tiltFor = p.get(Tilt);
}

__attribute__((noinline)) void Eq::designMoved() {
    const auto &p = params_;
    const float lf = p.get(LowFreq), lg = p.get(LowGain);
    if (lf != lowFor[0] || lg != lowFor[1]) {
        low[0].lowShelf(lf, lg, sr);
        low[1].copyCoefficients(low[0]);
    }
    const float mf = p.get(MidFreq), mg = p.get(MidGain), mq = p.get(MidQ);
    if (mf != midFor[0] || mg != midFor[1] || mq != midFor[2]) {
        mid[0].peak(mf, mg, mq, sr);
        mid[1].copyCoefficients(mid[0]);
    }
    const float hf = p.get(HighFreq), hg = p.get(HighGain);
    if (hf != highFor[0] || hg != highFor[1]) {
        high[0].highShelf(hf, hg, sr);
        high[1].copyCoefficients(high[0]);
    }
    const float t = p.get(Tilt);
    if (t != tiltFor) {
        const float tilt = t * 6.0f;
        tiltLo[0].lowShelf(650.0f, -tilt, sr);
        tiltHi[0].highShelf(650.0f, tilt, sr);
        tiltLo[1].copyCoefficients(tiltLo[0]);
        tiltHi[1].copyCoefficients(tiltHi[0]);
    }
    remember();
}

bool Eq::process(float *L, float *R, int32_t frames, bool stereoIn) {
    // A later slice of a block a mod glides across redesigns only the bands
    // it moved, once for both channels.
    if (subOffset_ > 0) {
        designMoved();
    } else {
        const auto &p = params_;
        const float tilt = p.get(Tilt) * 6.0f; // +-6 dB across the spectrum
        for (int c = 0; c < 2; ++c) {
            low[c].lowShelf(p.get(LowFreq), p.get(LowGain), sr);
            mid[c].peak(p.get(MidFreq), p.get(MidGain), p.get(MidQ), sr);
            high[c].highShelf(p.get(HighFreq), p.get(HighGain), sr);
            tiltLo[c].lowShelf(650.0f, -tilt, sr);
            tiltHi[c].highShelf(650.0f, tilt, sr);
        }
        remember();
    }
    const int chans = stereoIn ? 2 : 1;
    for (int c = 0; c < chans; ++c) {
        float *buf = c == 0 ? L : R;
        for (int32_t i = 0; i < frames; ++i) {
            float x = buf[i];
            x = low[c].process(x);
            x = mid[c].process(x);
            x = high[c].process(x);
            x = tiltLo[c].process(x);
            buf[i] = tiltHi[c].process(x);
        }
    }
    return stereoIn;
}

// --- Distortion -------------------------------------------------------------------

const ParamDef *Distortion::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"drive", 1.0f, 40.0f, 4.0f, Curve::Exponential, 0, ""},
        {"tone", 400.0f, 20000.0f, 8000.0f, Curve::Exponential, 0, "Hz"},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"mode", 0.0f, 3.0f, 0.0f, Curve::Stepped, 4, ""}, // soft, hard, fold, tube
        {"bias", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Distortion::prepare(int32_t sampleRate) { sr = static_cast<float>(sampleRate); reset(); }
void Distortion::reset() {
    for (int c = 0; c < 2; ++c) {
        tone[c].reset();
        halfband[c].reset();
        dcIn[c] = dcOut[c] = prevIn[c] = 0.0f;
    }
}

bool Distortion::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float drive = p.get(Drive), mix = p.get(Mix), bias = p.get(Bias) * 0.6f;
    const int mode = static_cast<int>(p.get(Mode) + 0.5f);
    for (int c = 0; c < 2; ++c) tone[c].lowpass(p.get(Tone), 0.707f, sr);
    // Compensate for the drive so the output level roughly follows the input.
    const float comp = 1.0f / std::sqrt(drive);
    const int chans = stereoIn ? 2 : 1;
    // At full quality the shaper runs at twice the rate and the top octave is
    // filtered off before decimating, to reduce aliasing. Lean quality runs
    // it once per sample.
    const bool oversample = fullQuality();
    for (int c = 0; c < 2; ++c) halfband[c].lowpass(std::min(19000.0f, sr * 0.45f), 0.707f, sr * 2.0f);
    auto shape = [&](float x) {
        switch (mode) {
        case 1: return clampf(x, -1.0f, 1.0f);
        case 2: { // wavefold: triangle-wrap x back into -1..1
            const float t = x * 0.25f + 0.25f;
            return 4.0f * std::fabs(t - std::floor(t + 0.5f)) - 1.0f;
        }
        case 3: return fastTanh(x) - 0.3f * fastTanh(x * 0.5f) * fastTanh(x * 0.5f); // tube: soft with a sag
        default: return fastTanh(x);
        }
    };
    for (int c = 0; c < chans; ++c) {
        float *buf = c == 0 ? L : R;
        for (int32_t i = 0; i < frames; ++i) {
            const float in = buf[i];
            float y;
            if (oversample) {
                // Two sub-samples: the midpoint of the last input and this
                // one, then this one. Both are filtered and the second is kept.
                const float mid = (prevIn[c] + in) * 0.5f;
                halfband[c].process(shape(mid * drive + bias));
                y = halfband[c].process(shape(in * drive + bias));
                prevIn[c] = in;
            } else {
                y = shape(in * drive + bias);
            }
            y *= comp * 2.0f;
            // Block the DC that bias introduces (one-pole highpass at ~10 Hz).
            const float hp = y - dcIn[c] + 0.9987f * dcOut[c];
            dcIn[c] = y;
            dcOut[c] = dsp::guardDenormal(hp);
            const float wet = tone[c].process(hp);
            buf[i] = in + (wet - in) * mix;
        }
    }
    return stereoIn;
}

// --- Compressor -------------------------------------------------------------------

const ParamDef *Compressor::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"threshold", -60.0f, 0.0f, -18.0f, Curve::Linear, 0, "dB"},
        {"ratio", 1.0f, 20.0f, 4.0f, Curve::Exponential, 0, ""},
        {"attack", 0.1f, 100.0f, 5.0f, Curve::Exponential, 0, "ms"},
        {"release", 10.0f, 1000.0f, 120.0f, Curve::Exponential, 0, "ms"},
        {"makeup", 0.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        {"pump", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"pumprate", 0.0f, 3.0f, 2.0f, Curve::Stepped, 4, ""}, // 1/16 1/8 1/4 1/2
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        // Appended: 0 is the effect's own input, 1..16 a track it listens to.
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
    };
    count = Count;
    return defs;
}

void Compressor::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    ahead = std::clamp(static_cast<int32_t>(sr * 0.001f + 0.5f), 1, kAheadMax);
    reset();
}
void Compressor::reset() {
    env = 0.0f;
    gain = 1.0f;
    at = 0;
    std::fill(aheadL, aheadL + kAheadMax, 0.0f);
    std::fill(aheadR, aheadR + kAheadMax, 0.0f);
}

bool Compressor::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float thr = p.get(Threshold), ratio = p.get(Ratio), makeup = dbToGain(p.get(Makeup));
    const float atk = dsp::onePoleCoeff(p.get(Attack) * 0.001f, sr), rel = dsp::onePoleCoeff(p.get(Release) * 0.001f, sr);
    const float pump = p.get(Pump);
    // The pump's own four, which aren't the LFOs' list: 1/16, 1/8, 1/4, 1/2.
    static constexpr float kPumpBeats[4] = {0.25f, 0.5f, 1.0f, 2.0f};
    const float beats = kPumpBeats[std::clamp(static_cast<int>(p.get(PumpRate) + 0.5f), 0, 3)];
    const float inc = Lfo::phaseInc(beats, bpm, sr);
    float phase = slicePhase(Lfo::phaseAt(tick, beats), inc);
    const float slope = 1.0f - 1.0f / ratio;
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        // The detector listens to the sidechain if there is one, otherwise
        // this track.
        const float a = key_ != nullptr ? std::fabs(key_[i])
                                        : (std::fabs(inL) > std::fabs(inR) ? std::fabs(inL) : std::fabs(inR));
        env += (a - env) * (a > env ? atk : rel);
        float g = 1.0f;
        if (env > 1e-5f) {
            const float over = 20.0f * std::log10(env) - thr;
            if (over > 0.0f) g = dbToGain(-over * slope);
        }
        // Pump: a sidechain-style dip on every beat of the chosen note value,
        // with no routing needed. Recovers exponentially like a kick would.
        if (pump > 0.0f) {
            g *= 1.0f - pump * std::exp(-phase * 6.0f);
            phase += inc;
            if (phase >= 1.0f) phase -= 1.0f;
        }
        gain = g;
        // Keyed by another track, the audio isn't held back: the ducking
        // must land with the key's hit, and it's the key that's loud.
        float outL = inL, outR = inR;
        if (key_ == nullptr) {
            outL = aheadL[at];
            outR = aheadR[at];
            aheadL[at] = inL;
            aheadR[at] = inR;
            if (++at >= ahead) at = 0;
        }
        L[i] = outL * g * makeup;
        if (stereoIn) R[i] = outR * g * makeup;
    }
    return stereoIn;
}

// --- Swell ------------------------------------------------------------------------

const ParamDef *Swell::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"floor", -80.0f, 0.0f, -40.0f, Curve::Linear, 0, "dB"},
        {"ceiling", -30.0f, 0.0f, -3.0f, Curve::Linear, 0, "dB"},
        {"amount", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"split", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"release", 5.0f, 2000.0f, 150.0f, Curve::Exponential, 0, "ms"},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Swell::prepare(int32_t sampleRate) { swell.prepare(static_cast<float>(sampleRate)); }
void Swell::reset() { swell.reset(); }

bool Swell::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    dsp::Swell::Settings s;
    s.floorDb = p.get(Floor);
    s.ceilingDb = p.get(Ceiling);
    s.amount = p.get(Amount);
    s.split = p.get(Split);
    s.releaseSec = p.get(Release) * 0.001f;
    s.mix = p.get(Mix);
    swell.set(s);
    if (stereoIn) {
        for (int32_t i = 0; i < frames; ++i) swell.process(L[i], R[i]);
    } else {
        for (int32_t i = 0; i < frames; ++i) L[i] = swell.process(L[i]);
    }
    return stereoIn;
}

// --- Filter -----------------------------------------------------------------------

const ParamDef *Filter::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"cutoff", 20.0f, 20000.0f, 1500.0f, Curve::Exponential, 0, "Hz"},
        {"reso", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"mode", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""}, // LP BP HP
        {"lforate", 0.0f, Lfo::kRates - 1.0f, 13.0f, Curve::Stepped, Lfo::kRates, ""},
        {"lfodepth", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"envdepth", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        // Appended: 0 is the effect's own input, 1..16 a track it listens to.
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
    };
    count = Count;
    return defs;
}

void Filter::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &s : svf) s.setSampleRate(sr);
    reset();
}
void Filter::reset() { for (auto &s : svf) s.reset(); follower = 0.0f; fcSet = resoSet = -1.0f; }

bool Filter::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float cutoff = p.get(Cutoff), reso = p.get(Reso), lfoDepth = p.get(LfoDepth), envDepth = p.get(EnvDepth);
    const int mode = static_cast<int>(p.get(Mode) + 0.5f);
    const float beats = Lfo::beatsOf(static_cast<int>(p.get(LfoRate) + 0.5f));
    const float inc = Lfo::phaseInc(beats, bpm, sr);
    float phase = slicePhase(Lfo::phaseAt(tick, beats), inc);
    const float atk = dsp::onePoleCoeff(0.004f, sr), rel = dsp::onePoleCoeff(0.12f, sr);
    const int chans = stereoIn ? 2 : 1;
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        // The follower listens to the sidechain if there is one, so a negative
        // envelope depth closes this filter on another track's hits.
        const float a = key_ != nullptr ? std::fabs(key_[i]) * 2.4f : (std::fabs(inL) + std::fabs(inR)) * 1.2f;
        follower += (a - follower) * (a > follower ? atk : rel);
        // Update the coefficients every sixteen samples, and only when the
        // cutoff has moved, since `Svf::set` calls `tan`. Sixteen samples
        // matches the machines' filters.
        if ((i & 15) == 0) {
            // Both modulators move the cutoff in octaves: the LFO up to +-3, the
            // follower up to 4 (an auto-wah when positive, a duck when negative).
            const float octaves = lfoDepth * 3.0f * Lfo::triangle(phase) + envDepth * 4.0f * clampf(follower, 0.0f, 1.0f);
            const float fc = cutoff * std::exp2(octaves);
            if (fc != fcSet || reso != resoSet) {
                fcSet = fc;
                resoSet = reso;
                for (auto &f : svf) f.set(fc, reso); // both, in case a mono input turns stereo
            }
        }
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        for (int c = 0; c < chans; ++c) {
            float *buf = c == 0 ? L : R;
            const float x = buf[i];
            buf[i] = mode == 1 ? svf[c].bandpass(x) : (mode == 2 ? svf[c].highpass(x) : svf[c].lowpass(x));
        }
    }
    return stereoIn;
}

// --- Bitcrusher -------------------------------------------------------------------

const ParamDef *Bitcrusher::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"bits", 1.0f, 16.0f, 8.0f, Curve::Linear, 0, ""},
        {"rate", 500.0f, 48000.0f, 12000.0f, Curve::Exponential, 0, "Hz"},
        {"jitter", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"tone", 300.0f, 20000.0f, 20000.0f, Curve::Exponential, 0, "Hz"},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Bitcrusher::prepare(int32_t sampleRate) { sr = static_cast<float>(sampleRate); reset(); }
// The jitter's generator goes back to its seed too, so renders repeat.
void Bitcrusher::reset() {
    hold[0] = hold[1] = 0.0f;
    phase = 0.0f;
    period = 1.0f;
    rng = 0x2545F491u;
    for (auto &t : tone) t.reset();
}

bool Bitcrusher::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float bits = p.get(Bits), jitter = p.get(Jitter), mix = p.get(Mix);
    const float inc = p.get(Rate) / sr;
    const float levels = std::exp2(bits - 1.0f);
    for (auto &t : tone) t.lowpass(p.get(Tone), 0.707f, sr);
    const int chans = stereoIn ? 2 : 1;
    for (int32_t i = 0; i < frames; ++i) {
        phase += inc;
        if (phase >= period) {
            phase -= period;
            // Jitter: each hold is a random length, so the aliasing smears
            // into noise instead of ringing at one pitch.
            period = 1.0f + jitter * (xorshift01(rng) * 1.8f - 0.9f);
            for (int c = 0; c < chans; ++c) {
                const float x = (c == 0 ? L : R)[i];
                hold[c] = std::round(x * levels) / levels;
            }
        }
        for (int c = 0; c < chans; ++c) {
            float *buf = c == 0 ? L : R;
            const float wet = tone[c].process(hold[c]);
            buf[i] = buf[i] + (wet - buf[i]) * mix;
        }
    }
    return stereoIn;
}

// --- Phaser -----------------------------------------------------------------------

const ParamDef *Phaser::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"rate", 0.0f, Lfo::kRates - 1.0f, 14.0f, Curve::Stepped, Lfo::kRates, ""},
        {"depth", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"feedback", 0.0f, 0.9f, 0.3f, Curve::Linear, 0, ""},
        {"stages", 0.0f, 3.0f, 1.0f, Curve::Stepped, 4, ""}, // 2 4 6 8
        {"spread", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Phaser::prepare(int32_t sampleRate) { sr = static_cast<float>(sampleRate); reset(); }
void Phaser::reset() { for (auto &ch : ap) for (auto &a : ch) a.reset(); fb[0] = fb[1] = 0.0f; }

bool Phaser::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float depth = p.get(Depth), feedback = p.get(Feedback), spread = p.get(Spread), mix = p.get(Mix);
    const int stages = 2 * (static_cast<int>(p.get(Stages) + 0.5f) + 1);
    const float phaserBeats = Lfo::beatsOf(static_cast<int>(p.get(Rate) + 0.5f));
    const float phase = slicePhase(Lfo::phaseAt(tick, phaserBeats), Lfo::phaseInc(phaserBeats, bpm, sr));
    // Sweep 120 Hz .. 5 kHz, the right channel offset by up to half a cycle.
    for (int c = 0; c < 2; ++c) {
        float ph = phase + (c == 1 ? spread * 0.5f : 0.0f);
        if (ph >= 1.0f) ph -= 1.0f;
        const float pos = 0.5f + 0.5f * depth * Lfo::sine(ph);
        const float fc = 120.0f * std::pow(5000.0f / 120.0f, pos);
        for (int s = 0; s < stages; ++s) ap[c][s].allpass(fc * (1.0f + 0.15f * static_cast<float>(s)), 0.6f, sr);
    }
    for (int32_t i = 0; i < frames; ++i) {
        const float in[2] = {L[i], stereoIn ? R[i] : L[i]};
        for (int c = 0; c < 2; ++c) {
            float x = in[c] + fb[c] * feedback;
            for (int s = 0; s < stages; ++s) x = ap[c][s].process(x);
            fb[c] = x;
            (c == 0 ? L : R)[i] = in[c] + (x - in[c]) * mix;
        }
    }
    return true;
}

// --- Flanger ----------------------------------------------------------------------

const ParamDef *Flanger::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"rate", 0.0f, Lfo::kRates - 1.0f, 14.0f, Curve::Stepped, Lfo::kRates, ""},
        {"depth", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"feedback", 0.0f, 0.95f, 0.5f, Curve::Linear, 0, ""},
        {"negative", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"spread", 0.0f, 1.0f, 0.4f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Flanger::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &l : line) l.prepare(static_cast<int32_t>(sr * 0.02f) + 64);
    reset();
}
void Flanger::reset() { for (auto &l : line) l.clear(); }

bool Flanger::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float depth = p.get(Depth), spread = p.get(Spread), mix = p.get(Mix);
    const float feedback = p.get(Feedback) * (p.get(Negative) >= 0.5f ? -1.0f : 1.0f);
    const float beats = Lfo::beatsOf(static_cast<int>(p.get(Rate) + 0.5f));
    const float inc = Lfo::phaseInc(beats, bpm, sr);
    float phase = slicePhase(Lfo::phaseAt(tick, beats), inc);
    const float base = 0.0006f * sr, range = depth * 0.008f * sr; // 0.6 ms .. 8.6 ms
    for (int32_t i = 0; i < frames; ++i) {
        const float in[2] = {L[i], stereoIn ? R[i] : L[i]};
        for (int c = 0; c < 2; ++c) {
            float ph = phase + (c == 1 ? spread * 0.5f : 0.0f);
            if (ph >= 1.0f) ph -= 1.0f;
            const float d = base + range * (0.5f + 0.5f * Lfo::triangle(ph));
            const float tap = line[c].read(d);
            line[c].write(in[c] + tap * feedback);
            // Negative feedback with the wet inverted, for a hollow, through-zero sound.
            const float wet = feedback < 0.0f ? -tap : tap;
            (c == 0 ? L : R)[i] = in[c] + wet * mix;
        }
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
    }
    return true;
}



// --- Chorus -----------------------------------------------------------------------

const ParamDef *Chorus::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"rate", 0.0f, Lfo::kRates - 1.0f, 13.0f, Curve::Stepped, Lfo::kRates, ""},
        {"depth", 0.0f, 1.0f, 0.45f, Curve::Linear, 0, ""},
        {"voices", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""}, // 2 3 4
        {"spread", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"drift", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Chorus::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    // 50 ms is more than a chorus needs, leaving room for the drift.
    for (auto &l : line) l.prepare(static_cast<int32_t>(sr * 0.05f));
    reset();
}

void Chorus::reset() {
    for (auto &l : line) l.clear();
    for (float &d : drift) d = 0.0f;
    rng = 0x9e3779b9u;
}

bool Chorus::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float depth = p.get(Depth), spread = p.get(Spread), mix = p.get(Mix);
    const float driftAmt = p.get(Drift);
    const int voices = static_cast<int>(p.get(Voices) + 0.5f) + 2;
    const float chorusBeats = Lfo::beatsOf(static_cast<int>(p.get(Rate) + 0.5f));
    const float phase = slicePhase(Lfo::phaseAt(tick, chorusBeats), Lfo::phaseInc(chorusBeats, bpm, sr));

    // 8 ms at the centre, swept by up to 5 ms either way. Much past 15 ms it
    // stops sounding like a chorus.
    const float centre = sr * 0.008f;
    const float swing = sr * 0.005f * depth;
    // One step of the random walk per block (about 750 Hz), slow enough to
    // sound like tuning drift instead of noise. Once a block, not once a slice.
    for (int v = 0; v < 4 && subOffset_ == 0; ++v) {
        rng = rng * 1664525u + 1013904223u;
        const float step = (static_cast<float>(rng >> 9) * (1.0f / 4194304.0f) - 1.0f) * 0.02f;
        drift[v] = clampf(drift[v] * 0.995f + step, -1.0f, 1.0f);
    }

    for (int32_t i = 0; i < frames; ++i) {
        const float in[2] = {L[i], stereoIn ? R[i] : L[i]};
        const float mono = (in[0] + in[1]) * 0.5f;
        line[0].write(mono);
        line[1].write(mono);
        float wet[2] = {0.0f, 0.0f};
        for (int v = 0; v < voices; ++v) {
            // Each voice sits at its own point in the cycle.
            const float vp = phase + static_cast<float>(v) / static_cast<float>(voices);
            const float ph = vp - std::floor(vp);
            const float d = centre + swing * Lfo::sine(ph) + drift[v] * driftAmt * sr * 0.002f;
            const float s = line[0].read(clampf(d, 2.0f, sr * 0.045f));
            // Voices alternate sides by `spread`. At 0 they're all in the
            // middle.
            const float side = (v % 2 == 0 ? -1.0f : 1.0f) * spread;
            wet[0] += s * (1.0f - 0.5f * (1.0f + side));
            wet[1] += s * (0.5f * (1.0f + side));
        }
        const float norm = 1.0f / std::sqrt(static_cast<float>(voices));
        L[i] = in[0] + (wet[0] * norm - in[0]) * mix;
        R[i] = in[1] + (wet[1] * norm - in[1]) * mix;
    }
    return true;
}

// --- Tremolo ----------------------------------------------------------------------

const ParamDef *Tremolo::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"rate", 0.0f, Lfo::kRates - 1.0f, 8.0f, Curve::Stepped, Lfo::kRates, ""},
        {"depth", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"shape", 0.0f, 2.0f, 0.0f, Curve::Stepped, 3, ""}, // sine, triangle, square
        {"pan", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"skew", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Tremolo::prepare(int32_t sampleRate) { sr = static_cast<float>(sampleRate); reset(); }
void Tremolo::reset() {}

bool Tremolo::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float depth = p.get(Depth), pan = p.get(Pan), skew = p.get(Skew), mix = p.get(Mix);
    const int shape = static_cast<int>(p.get(Shape) + 0.5f);
    const float beats = Lfo::beatsOf(static_cast<int>(p.get(Rate) + 0.5f));
    const float base0 = Lfo::phaseAt(tick, beats);
    // The phase advances per sample between blocks, using the same table as
    // the block phase so there's no step at block boundaries. `kBeats` is in
    // quarter notes, so a cycle is `beats * 60 / bpm` seconds.
    const float hz = bpm / (60.0f * beats);
    const float inc = hz / sr;
    const float base = slicePhase(base0, inc);

    for (int32_t i = 0; i < frames; ++i) {
        float ph = base + inc * static_cast<float>(i);
        ph -= std::floor(ph);
        // Skew bends the duty: symmetrical at 0.5, a stab or a swell either
        // side.
        const float k = clampf(skew, 0.05f, 0.95f);
        const float warped = ph < k ? 0.5f * ph / k : 0.5f + 0.5f * (ph - k) / (1.0f - k);
        float w;
        switch (shape) {
        case 1: w = Lfo::triangle(warped); break;
        case 2: w = warped < 0.5f ? 1.0f : -1.0f; break;
        default: w = Lfo::sine(warped); break;
        }
        // Pan offsets the channels' phases: 0 is a tremolo, fully opposite is
        // an auto-pan.
        float wR;
        {
            float ph2 = ph + 0.5f * pan;
            ph2 -= std::floor(ph2);
            const float warped2 = ph2 < k ? 0.5f * ph2 / k : 0.5f + 0.5f * (ph2 - k) / (1.0f - k);
            switch (shape) {
            case 1: wR = Lfo::triangle(warped2); break;
            case 2: wR = warped2 < 0.5f ? 1.0f : -1.0f; break;
            default: wR = Lfo::sine(warped2); break;
            }
        }
        const float gL = 1.0f - depth * 0.5f * (1.0f - w);
        const float gR = 1.0f - depth * 0.5f * (1.0f - wR);
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        L[i] = inL + (inL * gL - inL) * mix;
        R[i] = inR + (inR * gR - inR) * mix;
    }
    return true;
}

// --- Width ------------------------------------------------------------------------

const ParamDef *Width::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"width", 0.0f, 2.0f, 1.0f, Curve::Linear, 0, ""},
        {"below", 20.0f, 500.0f, 20.0f, Curve::Exponential, 0, "Hz"},
        {"haas", 0.0f, 20.0f, 0.0f, Curve::Linear, 0, "ms"},
        {"rotate", -1.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Width::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    haasLine.prepare(static_cast<int32_t>(sr * 0.025f));
    reset();
}

void Width::reset() {
    haasLine.clear();
    lowL.reset();
    lowR.reset();
}

bool Width::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float amount = p.get(Amount), below = p.get(Below), rotate = p.get(Rotate);
    const float haas = p.get(Haas) * 0.001f * sr;
    // The crossover below which the bass is made mono. At the bottom of its
    // range it's off.
    const bool folding = below > 21.0f;
    if (folding) {
        lowL.lowpass(below, 0.707f, sr);
        lowR.lowpass(below, 0.707f, sr);
    }
    const float ang = (rotate + 1.0f) * 0.25f * 3.14159265f;
    const float rl = std::cos(ang) * 1.4142f, rr = std::sin(ang) * 1.4142f;

    for (int32_t i = 0; i < frames; ++i) {
        float l = L[i], r = stereoIn ? R[i] : L[i];
        if (haas > 1.0f) {
            haasLine.write(r);
            r = haasLine.read(haas);
        }
        float lowSum = 0.0f;
        if (folding) {
            // Take the lows out of both, sum them to mono and put them back.
            // Only what's above the crossover gets widened.
            const float ll = lowL.process(l), lr = lowR.process(r);
            l -= ll;
            r -= lr;
            lowSum = (ll + lr) * 0.5f;
        }
        const float mid = (l + r) * 0.5f;
        const float side = (l - r) * 0.5f * amount;
        l = mid + side + lowSum;
        r = mid - side + lowSum;
        L[i] = l * rl;
        R[i] = r * rr;
    }
    return true;
}


// --- Shifter ----------------------------------------------------------------------

namespace {
// A Hilbert pair: two four-section allpass chains whose phases stay about 90
// degrees apart from tens of Hz to most of Nyquist. Standard coefficients.
// The Q chain is read one sample late, which is needed for the quadrature.
constexpr float kHilbertI[4] = {0.6923877778065f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
constexpr float kHilbertQ[4] = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};
} // namespace

const ParamDef *Shifter::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"shift", -500.0f, 500.0f, 0.0f, Curve::Linear, 0, "Hz"},
        {"fine", -20.0f, 20.0f, 0.0f, Curve::Linear, 0, "Hz"},
        {"spread", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"feedback", 0.0f, 0.9f, 0.0f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Shifter::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < 4; ++i) {
            apI[c][i].a = kHilbertI[i];
            apQ[c][i].a = kHilbertQ[i];
        }
    }
    reset();
}

void Shifter::reset() {
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < 4; ++i) { apI[c][i].clear(); apQ[c][i].clear(); }
        delayed[c] = 0.0f;
        fb[c] = 0.0f;
    }
    phase = 0.0f;
}

bool Shifter::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float hz = p.get(Shift) + p.get(Fine);
    const float spread = p.get(Spread), feedback = p.get(Feedback), mix = p.get(Mix);
    const float inc = hz / sr;

    for (int32_t i = 0; i < frames; ++i) {
        const float in[2] = {L[i], stereoIn ? R[i] : L[i]};
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        if (phase < 0.0f) phase += 1.0f;
        const float c = std::cos(phase * dsp::kTwoPi), s = std::sin(phase * dsp::kTwoPi);
        for (int ch = 0; ch < 2; ++ch) {
            float x = in[ch] + fb[ch] * feedback;
            float qi = x, qq = x;
            for (int k = 0; k < 4; ++k) qi = apI[ch][k].process(qi);
            for (int k = 0; k < 4; ++k) qq = apQ[ch][k].process(qq);
            // The Q chain is a sample behind, as the network needs.
            const float q = delayed[ch];
            delayed[ch] = qq;
            // With `spread` up, the right channel shifts the other way.
            const float sign = (ch == 1) ? (1.0f - 2.0f * spread) : 1.0f;
            const float wet = qi * c - q * s * sign;
            // The Hilbert pair gains a little near the band's edges, enough
            // to run away at high feedback (see feedbackCeiling).
            fb[ch] = dsp::guardDenormal(dsp::feedbackCeiling(wet));
            (ch == 0 ? L : R)[i] = in[ch] + (wet - in[ch]) * mix;
        }
    }
    return true;
}

// --- Harmonizer -------------------------------------------------------------------

const ParamDef *Harmonizer::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        // Scale degrees, not semitones: +2 is a third, major or minor
        // depending on the note.
        {"interval", -7.0f, 7.0f, 2.0f, Curve::Stepped, 15, ""},
        {"interval2", -7.0f, 7.0f, 0.0f, Curve::Stepped, 15, ""},
        {"scale", 0.0f, 32.0f, 0.0f, Curve::Stepped, music::kScaleCount, ""},
        {"key", 0.0f, 11.0f, 0.0f, Curve::Stepped, 12, ""},
        {"window", 10.0f, 120.0f, 45.0f, Curve::Exponential, 0, "ms"},
        {"feedback", 0.0f, 0.85f, 0.0f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Harmonizer::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &l : line) l.prepare(static_cast<int32_t>(sr * 0.3f));
    reset();
}

void Harmonizer::reset() {
    for (auto &l : line) l.clear();
    for (auto &ch : voice) for (auto &v : ch) { v.phase = 0.0f; v.ratio = 1.0f; }
    zeroPrev = 0.0f;
    trackedHz = 0.0f;
    zeroCount = 0;
    zeroWindow = 0;
    confidence = 0.0f;
    fb[0] = fb[1] = 0.0f;
}

bool Harmonizer::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const int degrees[2] = {static_cast<int>(std::lround(p.get(Interval))),
                            static_cast<int>(std::lround(p.get(Interval2)))};
    const music::ScaleDef &scale =
        music::kScales[std::clamp(static_cast<int>(p.get(Scale) + 0.5f), 0, music::kScaleCount - 1)];
    const int key = std::clamp(static_cast<int>(p.get(Key) + 0.5f), 0, 11);
    const float win = clampf(p.get(Window) * 0.001f * sr, 64.0f, sr * 0.25f);
    const float feedback = p.get(Feedback), mix = p.get(Mix);
    // A new interval glides over about 30 ms instead of jumping.
    const float glide = dsp::onePoleCoeff(0.03f, sr);

    // The semitone shift for each degree, given the current input note.
    // Recomputed once per block.
    float semis[2];
    for (int v = 0; v < 2; ++v) {
        if (confidence > 0.25f && trackedHz > 40.0f) {
            const float note = 69.0f + 12.0f * std::log2(trackedHz / 440.0f);
            const int midi = static_cast<int>(std::lround(note));
            const int pc = music::floorMod(midi - key, 12);
            const int deg = music::degreeAtOrBelow(scale, pc);
            // The difference between this note's degree and the one N above
            // it is the interval to shift by.
            const int here = music::degreeInterval(scale, deg);
            const int there = music::degreeInterval(scale, deg + degrees[v]);
            semis[v] = static_cast<float>(there - here);
        } else {
            // No confident pitch, so use a plain chromatic reading of the
            // degree.
            semis[v] = static_cast<float>(music::degreeInterval(scale, degrees[v]));
        }
    }
    const float target[2] = {std::pow(2.0f, semis[0] / 12.0f), std::pow(2.0f, semis[1] / 12.0f)};
    const bool second = degrees[1] != 0;

    for (int32_t i = 0; i < frames; ++i) {
        const float in[2] = {L[i], stereoIn ? R[i] : L[i]};
        const float mono = (in[0] + in[1]) * 0.5f;

        // Track pitch on the dry input, never the output, or it would chase
        // its own harmony.
        if ((zeroPrev <= 0.0f) != (mono <= 0.0f)) ++zeroCount;
        zeroPrev = mono;
        if (++zeroWindow >= 1024) {
            const float hz = static_cast<float>(zeroCount) * sr / (2.0f * 1024.0f);
            // A steady pitch crosses zero twice a cycle. Noise crosses far more
            // often, so an unlikely count means there's no clear pitch.
            if (hz > 40.0f && hz < 2000.0f) {
                trackedHz = trackedHz > 0.0f ? trackedHz + (hz - trackedHz) * 0.4f : hz;
                confidence += (1.0f - confidence) * 0.3f;
            } else {
                confidence *= 0.5f;
            }
            zeroCount = 0;
            zeroWindow = 0;
        }

        for (int c = 0; c < 2; ++c) {
            line[c].write(in[c] + fb[c] * feedback);
        }
        float wet[2] = {0.0f, 0.0f};
        // What goes back into the line: each voice's taps at the level they
        // read (divided by their gains' sum), shared between the voices, so
        // the loop never gives back more than it was given.
        float back[2] = {0.0f, 0.0f};
        for (int c = 0; c < 2; ++c) {
            for (int v = 0; v < (second ? 2 : 1); ++v) {
                Voice &vo = voice[c][v];
                vo.ratio += (target[v] - vo.ratio) * glide;
                // The read point slides against the write head at the shift's
                // rate and wraps inside one window.
                vo.phase += (1.0f - vo.ratio) / win;
                while (vo.phase >= 1.0f) vo.phase -= 1.0f;
                while (vo.phase < 0.0f) vo.phase += 1.0f;
                float ph2 = vo.phase + 0.5f;
                if (ph2 >= 1.0f) ph2 -= 1.0f;
                // Two taps half a window apart, crossfaded so each wraps while
                // it's silent. The sine crossfade keeps the power constant.
                const float g1 = std::sin(vo.phase * 3.14159265f);
                const float g2 = std::sin(ph2 * 3.14159265f);
                const float t1 = line[c].read(vo.phase * win + 2.0f);
                const float t2 = line[c].read(ph2 * win + 2.0f);
                wet[c] += t1 * g1 + t2 * g2;
                back[c] += (t1 * g1 + t2 * g2) / std::fmax(g1 + g2, 1.0f);
            }
        }
        const float norm = second ? 0.7071f : 1.0f;
        for (int c = 0; c < 2; ++c) {
            fb[c] = dsp::feedbackCeiling(back[c] * (second ? 0.5f : 1.0f));
            (c == 0 ? L : R)[i] = in[c] + (wet[c] * norm - in[c]) * mix;
        }
    }
    return true;
}

// --- Gate -------------------------------------------------------------------------

const ParamDef *Gate::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"threshold", -80.0f, 0.0f, -45.0f, Curve::Linear, 0, "dB"},
        {"hyst", 0.0f, 24.0f, 4.0f, Curve::Linear, 0, "dB"},
        {"attack", 0.05f, 50.0f, 1.0f, Curve::Exponential, 0, "ms"},
        {"hold", 0.0f, 500.0f, 40.0f, Curve::Linear, 0, "ms"},
        {"release", 5.0f, 2000.0f, 150.0f, Curve::Exponential, 0, "ms"},
        {"duck", -90.0f, 0.0f, -90.0f, Curve::Linear, 0, "dB"},
        {"key", 20.0f, 2000.0f, 20.0f, Curve::Exponential, 0, "Hz"},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
        // Appended: 0 is the effect's own input, 1..16 a track it listens to.
        {"sidechain", 0.0f, 16.0f, 0.0f, Curve::Stepped, 17, ""},
    };
    count = Count;
    return defs;
}

void Gate::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &k : key) k.setSampleRate(sr);
    keyHz = -1.0f;
    reset();
}

void Gate::reset() {
    env = 0.0f;
    // Reset closed, so a panic is silent.
    gain = 0.0f;
    holdLeft = 0.0f;
    open = false;
    for (auto &k : key) k.reset();
}

bool Gate::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float openAt = dbToGain(p.get(Threshold));
    const float shutAt = dbToGain(p.get(Threshold) - p.get(Hyst));
    const float atk = dsp::onePoleCoeff(p.get(Attack) * 0.001f, sr);
    const float rel = dsp::onePoleCoeff(p.get(Release) * 0.001f, sr);
    const float shut = dbToGain(p.get(Duck));
    const float holdSamples = p.get(Hold) * 0.001f * sr;

    const float hz = p.get(Key);
    if (hz != keyHz) {
        keyHz = hz;
        for (auto &k : key) k.set(hz, 0.0f);
    }

    /**
     * How fast the detector falls, which isn't the gate's release.
     *
     * Kept short (2 ms) so `hold` works across its whole range. The dips
     * between cycles of a low note are covered by `hold`: every peak over
     * the threshold restarts the timer, so an 82 Hz note restarts it every
     * 12 ms and the default 40 ms never runs out.
     */
    static constexpr float kDetectRelease = 0.002f;
    const float detRel = dsp::onePoleCoeff(kDetectRelease, sr);

    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        // The key filter runs on the audio, per channel, before the rectifier.
        // With a sidechain it filters that instead, so the gate opens on
        // another track's hits.
        const float srcL = key_ != nullptr ? key_[i] : inL;
        const float srcR = key_ != nullptr ? key_[i] : inR;
        const float kL = std::fabs(key[0].step(srcL).hp);
        const float kR = std::fabs(key[1].step(srcR).hp);
        const float det = kL > kR ? kL : kR;
        env = det > env ? det : dsp::undenormal(env + (det - env) * detRel);

        if (env > openAt) {
            open = true;
            holdLeft = holdSamples;
        } else if (env < shutAt) {
            if (holdLeft > 0.0f) holdLeft -= 1.0f;
            else open = false;
        }
        // Between the two thresholds nothing changes. The hysteresis stops a
        // signal near the threshold from chattering.

        const float want = open ? 1.0f : shut;
        const float c = want > gain ? atk : rel;
        gain = dsp::undenormal(gain + (want - gain) * c);

        L[i] = inL * gain;
        if (stereoIn) R[i] = inR * gain;
    }
    return stereoIn;
}

} // namespace acidulous::effect
