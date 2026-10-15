#include "Tape.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

using dsp::clampf;

namespace {
/** Seconds of tape the head can fall behind while stopping, and the head's normal place behind the write. */
constexpr float kRingSeconds = 6.0f;
constexpr float kBase = 0.012f;
/** How far wow and flutter move the head at full, in seconds. */
constexpr float kWowDepth = 0.004f, kFlutterDepth = 0.00025f;
constexpr float kWowHz = 0.55f, kFlutterHz = 9.0f;
constexpr float kNominal = 0.3f;
} // namespace

const ParamDef *Tape::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"wow", 0.0f, 1.0f, 0.25f, Curve::Linear, 0, ""},
        {"flutter", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"drive", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"tone", 2000.0f, 20000.0f, 12000.0f, Curve::Exponential, 0, "Hz"},
        {"hiss", 0.0f, 1.0f, 0.15f, Curve::Linear, 0, ""},
        // How worn: the wow wanders more and less evenly, and the lows bump up.
        {"age", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"stop", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"stoptime", 0.1f, 4.0f, 1.0f, Curve::Exponential, 0, "s"},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Tape::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    size = static_cast<int32_t>(sr * kRingSeconds);
    for (auto &r : ring) r.assign(static_cast<size_t>(size), 0.0f);
    bumpDb = -100.0f;
    reset();
}

void Tape::reset() {
    for (auto &r : ring) std::fill(r.begin(), r.end(), 0.0f);
    write = 0;
    behind = kBase * sr;
    speed = 1.0f;
    wowPhase = flutterPhase = 0.0f;
    drift = driftTarget = 0.0f;
    lowpass[0] = lowpass[1] = hissLp = 0.0f;
    for (auto &b : bump) b.reset();
    rng = 0x2f6b9a1du;
}

bool Tape::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float wow = p.get(Wow), flutter = p.get(Flutter), drive = p.get(Drive), age = p.get(Age);
    const float hiss = p.get(Hiss), mix = p.get(Mix);
    const bool stopping = p.get(Stop) > 0.5f;
    const float stopStep = 1.0f / std::max(1.0f, p.get(StopTime) * sr);
    const float toneCoef = 1.0f - std::exp(-6.2831853f * p.get(Tone) / sr);
    // The head bump: a few dB of low end, more on a worn machine.
    const float db = 1.5f + 3.0f * age;
    if (std::fabs(db - bumpDb) > 0.05f) {
        bumpDb = db;
        for (auto &b : bump) b.lowShelf(90.0f, db, sr);
    }
    // Normalised where a track usually sits, as Reflux's drive is, so drive
    // changes the colour more than the level.
    const float driveGain = 1.0f + drive * 4.0f;
    const float driveComp = kNominal / std::tanh(kNominal * driveGain);

    for (int32_t i = 0; i < frames; ++i) {
        const float in[2] = {L[i], stereoIn ? R[i] : L[i]};
        ring[0][static_cast<size_t>(write)] = in[0];
        ring[1][static_cast<size_t>(write)] = in[1];
        if (++write >= size) write = 0;

        // Stopping slows the tape to nothing; starting again is instant, the
        // head back at its place.
        if (stopping) {
            speed = std::max(0.0f, speed - stopStep);
        } else if (speed < 1.0f) {
            speed = 1.0f;
            behind = kBase * sr;
        }
        // The head falls behind by however much slower than the write it runs.
        behind = std::min(static_cast<double>(size - 4), behind + (1.0 - speed));

        wowPhase += kWowHz * (1.0f + age * 0.5f * drift) / sr;
        if (wowPhase >= 1.0f) {
            wowPhase -= 1.0f;
            // An old machine's wow isn't even: each turn a little different.
            driftTarget = (random01() * 2.0f - 1.0f) * age;
        }
        drift += (driftTarget - drift) * 0.0001f;
        flutterPhase += kFlutterHz / sr;
        if (flutterPhase >= 1.0f) flutterPhase -= 1.0f;
        const float wobble = wow * kWowDepth * (std::sin(wowPhase * 6.2831853f) + drift) +
                             flutter * kFlutterDepth * std::sin(flutterPhase * 6.2831853f);
        const double at = behind + static_cast<double>(wobble * sr);

        double read = static_cast<double>(write) - at;
        while (read < 0.0) read += size;
        const auto i0 = static_cast<int32_t>(read);
        const int32_t i1 = i0 + 1 >= size ? 0 : i0 + 1;
        const float frac = static_cast<float>(read - i0);

        // The hiss: a little coloured noise, under everything.
        hissLp += ((random01() * 2.0f - 1.0f) - hissLp) * 0.5f;
        const float noise = hissLp * hiss * 0.006f;
        float out[2];
        for (int c = 0; c < 2; ++c) {
            float s = ring[c][static_cast<size_t>(i0)] + (ring[c][static_cast<size_t>(i1)] - ring[c][static_cast<size_t>(i0)]) * frac;
            // A stopping tape fades as it slows.
            s *= std::min(1.0f, speed * 4.0f);
            s = std::tanh(s * driveGain) * driveComp;
            s = bump[c].process(s);
            lowpass[c] += (s - lowpass[c]) * toneCoef;
            lowpass[c] = dsp::guardDenormal(lowpass[c]);
            out[c] = lowpass[c] + noise;
        }
        L[i] = in[0] + (out[0] - in[0]) * mix;
        R[i] = in[1] + (out[1] - in[1]) * mix;
    }
    return true;
}

} // namespace acidulous::effect
