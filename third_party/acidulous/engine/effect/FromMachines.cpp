#include "FromMachines.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>
#include <engine/inputmod/Scales.h>

namespace acidulous::effect {

using dsp::clampf;

// --- Rotary -------------------------------------------------------------------------

namespace {
/** The tempo choices, as beats a turn: off, 1/1, 1/2, 1/4, 1/8 and 1/8 triplet, as Manual has them. */
constexpr float kTempoBeats[6] = {0.0f, 4.0f, 2.0f, 1.0f, 0.5f, 1.0f / 3.0f};
/** The drum turns a little slower than the horn, as in the cabinet. */
constexpr float kDrumRatio = 0.8f;
} // namespace

const ParamDef *Rotary::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"speed", 0.0f, 2.0f, 1.0f, Curve::Stepped, 3, ""}, // brake, slow, fast
        {"slow", 0.1f, 2.0f, 0.8f, Curve::Exponential, 0, "Hz"},
        {"fast", 2.0f, 10.0f, 6.6f, Curve::Exponential, 0, "Hz"},
        {"ramp", 0.05f, 4.0f, 0.9f, Curve::Exponential, 0, "s"},
        {"distance", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
        {"angle", 0.0f, 1.0f, 0.8f, Curve::Linear, 0, ""},
        {"width", 0.0f, 1.0f, 0.75f, Curve::Linear, 0, ""},
        {"tempo", 0.0f, 5.0f, 0.0f, Curve::Stepped, 6, ""},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Rotary::prepare(int32_t sampleRate) { cabinet.prepare(static_cast<float>(sampleRate)); reset(); }
void Rotary::reset() { cabinet.reset(); }

bool Rotary::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const int speed = std::clamp(static_cast<int>(p.get(Speed) + 0.5f), 0, 2);
    const int tempo = std::clamp(static_cast<int>(p.get(Tempo) + 0.5f), 0, 5);
    float horn = speed == 0 ? 0.0f : (speed == 1 ? p.get(Slow) : p.get(Fast));
    if (tempo != 0 && speed != 0) horn = (bpm / 60.0f) / kTempoBeats[tempo];
    // The motors take longer to slow than to speed up, as Manual's do.
    const float ramp = p.get(Ramp);
    cabinet.setTargets(horn, horn * kDrumRatio, ramp, ramp * 1.78f);
    cabinet.setMic(p.get(Distance), p.get(Angle), p.get(Width));
    const float mix = p.get(Mix);
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        float l = 0.0f, r = 0.0f;
        // The cabinet has one input, like the organ's: a stereo track is summed.
        cabinet.process(0.5f * (inL + inR), l, r);
        L[i] = inL + (l - inL) * mix;
        R[i] = inR + (r - inR) * mix;
    }
    return true;
}

// --- Grain --------------------------------------------------------------------------

namespace {
/** Seconds the cloud can reach back into. */
constexpr float kRingSeconds = 4.0f;
} // namespace

const ParamDef *Grain::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"size", 5.0f, 500.0f, 90.0f, Curve::Exponential, 0, "ms"},
        {"density", 1.0f, 100.0f, 20.0f, Curve::Exponential, 0, ""},
        {"spray", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"pitch", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "st"},
        {"scatter", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"reverse", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"freeze", 0.0f, 1.0f, 0.0f, Curve::Stepped, 2, ""},
        {"feedback", 0.0f, 0.9f, 0.0f, Curve::Linear, 0, ""},
        {"width", 0.0f, 1.0f, 0.7f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Grain::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    size = static_cast<int32_t>(sr * kRingSeconds);
    ringL.assign(static_cast<size_t>(size), 0.0f);
    ringR.assign(static_cast<size_t>(size), 0.0f);
    // A Hann window, as a table: a grain fades in and out so its edges don't click.
    for (int i = 0; i <= kWindow; ++i) window[i] = 0.5f - 0.5f * std::cos(6.2831853f * static_cast<float>(i) / kWindow);
    reset();
}

void Grain::reset() {
    std::fill(ringL.begin(), ringL.end(), 0.0f);
    std::fill(ringR.begin(), ringR.end(), 0.0f);
    write = 0;
    timer = 0.0f;
    for (auto &g : grains) g.active = false;
    rng = 0x5bd1e995u;
}

void Grain::spawn() {
    One *g = nullptr;
    for (auto &c : grains) if (!c.active) { g = &c; break; }
    if (g == nullptr) return; // the cloud is full; the next one waits
    const auto &p = params_;
    const float semis = p.get(Pitch) + (random01() < p.get(Scatter) ? (random01() < 0.5f ? -12.0f : 12.0f) : 0.0f);
    const double inc = std::pow(2.0, semis / 12.0);
    const int32_t length = std::max(16, static_cast<int32_t>(p.get(Size) * 0.001f * sr));
    // How far back it starts: far enough that a grain played faster than it
    // was heard doesn't catch up with the present, then some of the rest of
    // the ring, by spray. Frozen, the whole ring is fair game.
    const float reach = static_cast<float>(length) * static_cast<float>(std::max(1.0, inc)) + 64.0f;
    const float room = static_cast<float>(size) - reach - 64.0f;
    const float back = reach + random01() * p.get(Spray) * room;
    double pos = static_cast<double>(write) - back;
    while (pos < 0.0) pos += size;
    const bool reversed = random01() < p.get(Reverse);
    // Backwards, it starts at the far end of its slice and reads toward the start.
    if (reversed) {
        pos += static_cast<double>(length) * inc;
        while (pos >= size) pos -= size;
    }
    g->active = true;
    g->pos = pos;
    g->inc = reversed ? -inc : inc;
    g->age = 0;
    g->length = length;
    const float pan = (random01() * 2.0f - 1.0f) * p.get(Width);
    const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
    g->gainL = std::cos(angle) * 1.41421f;
    g->gainR = std::sin(angle) * 1.41421f;
}

bool Grain::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const bool frozen = p.get(Freeze) > 0.5f;
    const float feedback = p.get(Feedback), mix = p.get(Mix);
    // So the cloud is about as loud as the track at any density: grains
    // overlap size * density deep, and add like noise.
    const float overlap = std::max(1.0f, p.get(Size) * 0.001f * p.get(Density));
    const float level = 1.0f / std::sqrt(overlap);
    const float every = sr / p.get(Density);
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        timer -= 1.0f;
        if (timer <= 0.0f) {
            spawn();
            // A little irregular, so the cloud doesn't buzz at the density.
            timer += every * (0.75f + 0.5f * random01());
        }
        float wetL = 0.0f, wetR = 0.0f;
        for (auto &g : grains) {
            if (!g.active) continue;
            const float phase = static_cast<float>(g.age) / static_cast<float>(g.length);
            const float x = phase * kWindow;
            const int wi = std::min(static_cast<int>(x), kWindow - 1);
            const float w = window[wi] + (window[wi + 1] - window[wi]) * (x - static_cast<float>(wi));
            const int32_t i0 = static_cast<int32_t>(g.pos);
            const int32_t i1 = i0 + 1 >= size ? 0 : i0 + 1;
            const float f = static_cast<float>(g.pos - i0);
            const float sL = ringL[static_cast<size_t>(i0)] + (ringL[static_cast<size_t>(i1)] - ringL[static_cast<size_t>(i0)]) * f;
            const float sR = ringR[static_cast<size_t>(i0)] + (ringR[static_cast<size_t>(i1)] - ringR[static_cast<size_t>(i0)]) * f;
            // A grain keeps the track's own stereo and is then placed.
            wetL += sL * w * g.gainL;
            wetR += sR * w * g.gainR;
            g.pos += g.inc;
            if (g.pos >= size) g.pos -= size;
            if (g.pos < 0.0) g.pos += size;
            if (++g.age >= g.length) g.active = false;
        }
        wetL *= level;
        wetR *= level;
        if (!frozen) {
            // What it listens to, and the cloud fed back if asked, softly
            // clipped so feedback can't run away.
            ringL[static_cast<size_t>(write)] = dsp::guardDenormal(std::tanh(inL + wetL * feedback));
            ringR[static_cast<size_t>(write)] = dsp::guardDenormal(std::tanh(inR + wetR * feedback));
            if (++write >= size) write = 0;
        }
        L[i] = inL + (wetL - inL) * mix;
        R[i] = inR + (wetR - inR) * mix;
    }
    return true;
}

// --- Resonator ----------------------------------------------------------------------

const ParamDef *Resonator::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"key", 0.0f, 11.0f, 0.0f, Curve::Stepped, 12, ""},
        {"scale", 0.0f, 32.0f, 0.0f, Curve::Stepped, music::kScaleCount, ""},
        // The lowest string, as a MIDI note: C2 by default.
        {"low", 24.0f, 72.0f, 36.0f, Curve::Linear, 0, "st"},
        {"strings", 4.0f, 16.0f, 12.0f, Curve::Stepped, 13, ""},
        {"decay", 0.2f, 12.0f, 3.0f, Curve::Exponential, 0, "s"},
        {"tone", 0.0f, 1.0f, 0.5f, Curve::Linear, 0, ""},
        {"metal", 0.0f, 1.0f, 0.0f, Curve::Linear, 0, ""},
        {"width", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"mix", 0.0f, 1.0f, 0.35f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Resonator::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    for (auto &s : strings) s.prepare(sr);
    reset();
}

void Resonator::reset() {
    for (auto &s : strings) s.clear();
}

bool Resonator::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const int key = std::clamp(static_cast<int>(p.get(Key) + 0.5f), 0, 11);
    const music::ScaleDef &scale =
        music::kScales[std::clamp(static_cast<int>(p.get(Scale) + 0.5f), 0, music::kScaleCount - 1)];
    count = std::clamp(static_cast<int>(p.get(Strings) + 0.5f), 4, kMaxStrings);
    // The strings climb the scale from the first note of the key at or
    // above `low`.
    const int low = static_cast<int>(std::lround(p.get(Low)));
    int root = low - ((low - key) % 12 + 12) % 12;
    if (root < low) root += 12;
    const float decay = p.get(Decay), tone = 0.05f + 0.95f * p.get(Tone), metal = p.get(Metal);
    for (int s = 0; s < count; ++s) {
        const int degree = s % scale.count, octave = s / scale.count;
        const float note = static_cast<float>(root + 12 * octave + scale.intervals[degree]);
        const float hz = 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
        hzOf[s] = hz;
        strings[s].setFrequency(hz);
        // One turn round the string is a period, so this many turns make the
        // decay a T60 in seconds.
        strings[s].setDamping(std::pow(10.0f, -3.0f / (decay * hz)), tone);
        strings[s].setDispersion(metal, 4);
    }
    const float width = p.get(Width), mix = p.get(Mix);
    // Each string hears a share of the track, so more strings aren't louder.
    const float feed = 0.15f / std::sqrt(static_cast<float>(count));
    for (int32_t i = 0; i < frames; ++i) {
        const float inL = L[i], inR = stereoIn ? R[i] : L[i];
        const float drive = (inL + inR) * 0.5f * feed;
        float wetL = 0.0f, wetR = 0.0f;
        for (int s = 0; s < count; ++s) {
            const float y = strings[s].step(drive);
            // Low strings to the left, high to the right, by width.
            const float pan = (count > 1 ? (static_cast<float>(s) / static_cast<float>(count - 1)) * 2.0f - 1.0f : 0.0f) * width;
            wetL += y * (1.0f - pan) * 0.5f;
            wetR += y * (1.0f + pan) * 0.5f;
        }
        L[i] = inL + (wetL - inL) * mix;
        R[i] = inR + (wetR - inR) * mix;
    }
    return true;
}

} // namespace acidulous::effect
