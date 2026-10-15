#include "Slicer.h"
#include <algorithm>
#include <cmath>
#include <engine/dsp/Math.h>

namespace acidulous::effect {

namespace {
/** Ticks a beat (see dsp::Lfo::phaseAt), and the slice lengths in beats: 1/4, 1/8, 1/16 and 1/32. */
constexpr int64_t kTicksPerBeat = 240;
constexpr float kRateBeats[4] = {1.0f, 0.5f, 0.25f, 0.125f};
/** Seconds the ring holds: a slice at the slowest tempo and longest rate, twice over. */
constexpr float kRingSeconds = 4.0f;
/** The fade at each end of a changed slice, so its edges don't click. */
constexpr float kFade = 0.002f;

uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
} // namespace

const ParamDef *Slicer::paramDefs(int32_t &count) const {
    static const ParamDef defs[Count] = {
        {"rate", 0.0f, 3.0f, 2.0f, Curve::Stepped, 4, ""}, // 1/4 1/8 1/16 1/32
        {"chance", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"repeat", 0.0f, 1.0f, 0.6f, Curve::Linear, 0, ""},
        {"reverse", 0.0f, 1.0f, 0.3f, Curve::Linear, 0, ""},
        {"drop", 0.0f, 1.0f, 0.2f, Curve::Linear, 0, ""},
        {"gate", 0.1f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"pitch", -12.0f, 12.0f, 0.0f, Curve::Linear, 0, "st"},
        {"seed", 0.0f, 15.0f, 0.0f, Curve::Stepped, 16, ""},
        {"mix", 0.0f, 1.0f, 1.0f, Curve::Linear, 0, ""},
        {"gain", -24.0f, 24.0f, 0.0f, Curve::Linear, 0, "dB"},
    };
    count = Count;
    return defs;
}

void Slicer::prepare(int32_t sampleRate) {
    sr = static_cast<float>(sampleRate);
    size = static_cast<int32_t>(sr * kRingSeconds);
    for (auto &r : ring) r.assign(static_cast<size_t>(size), 0.0f);
    reset();
}

void Slicer::reset() {
    for (auto &r : ring) std::fill(r.begin(), r.end(), 0.0f);
    write = 0;
    slice = -1;
    sliceStart = lastStart = 0;
    sliceLength = 1;
    age = 0;
    action = Play;
    head = 0.0;
}

Slicer::Action Slicer::choose(int64_t at) const {
    const auto &p = params_;
    // The same slice of the song makes the same choice every time it plays.
    const uint32_t seed = static_cast<uint32_t>(p.get(Seed) + 0.5f);
    const uint32_t h = hash(static_cast<uint32_t>(at) * 2654435761u ^ (seed * 40503u + 1u));
    const float roll = static_cast<float>(h & 0xffff) / 65536.0f;
    if (roll >= p.get(Chance)) return Play;
    const float weights[3] = {p.get(Repeat), p.get(Reverse), p.get(Drop)};
    const float total = weights[0] + weights[1] + weights[2];
    if (total <= 0.0f) return Play;
    float pick = static_cast<float>((h >> 16) & 0xffff) / 65536.0f * total;
    if ((pick -= weights[0]) < 0.0f) return Again;
    if ((pick -= weights[1]) < 0.0f) return Backwards;
    return Rest;
}

bool Slicer::process(float *L, float *R, int32_t frames, bool stereoIn) {
    const auto &p = params_;
    const float beats = kRateBeats[std::clamp(static_cast<int>(p.get(Rate) + 0.5f), 0, 3)];
    const auto sliceTicks = static_cast<int64_t>(beats * kTicksPerBeat);
    const float gate = p.get(Gate), mix = p.get(Mix);
    const double ratio = std::pow(2.0, p.get(Pitch) / 12.0);
    const double ticksPerSample = frames > 0 ? static_cast<double>(blockEnd - blockStart) / frames : 0.0;
    const float fade = std::max(1.0f, kFade * sr);

    for (int32_t i = 0; i < frames; ++i) {
        const float in[2] = {L[i], stereoIn ? R[i] : L[i]};
        ring[0][static_cast<size_t>(write)] = in[0];
        ring[1][static_cast<size_t>(write)] = in[1];

        // A new slice when the transport crosses a slice line.
        const auto tick = blockStart + static_cast<int64_t>(i * ticksPerSample);
        const int64_t now = sliceTicks > 0 ? tick / sliceTicks : 0;
        if (now != slice) {
            slice = now;
            lastStart = sliceStart;
            sliceStart = write;
            // Measured, not worked out from the tempo, so a tempo change can't
            // send a repeat past the audio it repeats.
            int32_t measured = sliceStart - lastStart;
            if (measured <= 0) measured += size;
            sliceLength = std::clamp(measured, 1, size / 2);
            age = 0;
            action = choose(slice);
            // A repeat replays the slice just heard; backwards starts at its end.
            head = action == Backwards ? static_cast<double>(sliceStart) - 1.0 : static_cast<double>(lastStart);
        }

        float out[2] = {in[0], in[1]};
        if (action != Play) {
            float s[2] = {0.0f, 0.0f};
            if (action != Rest) {
                double at = head;
                while (at < 0.0) at += size;
                while (at >= size) at -= size;
                const auto i0 = static_cast<int32_t>(at);
                const int32_t i1 = i0 + 1 >= size ? 0 : i0 + 1;
                const float f = static_cast<float>(at - i0);
                for (int c = 0; c < 2; ++c) s[c] = ring[c][static_cast<size_t>(i0)] + (ring[c][static_cast<size_t>(i1)] - ring[c][static_cast<size_t>(i0)]) * f;
                head += action == Backwards ? -ratio : ratio;
            }
            // The track fades into the replacement at the slice's start and
            // back at its end, and the gate cuts the replacement short.
            const float a = static_cast<float>(age), length = static_cast<float>(sliceLength);
            const float edge = std::clamp(std::min(a / fade, (length - a) / fade), 0.0f, 1.0f);
            const float gated = std::clamp((gate * length - a) / fade, 0.0f, 1.0f);
            for (int c = 0; c < 2; ++c) out[c] = in[c] * (1.0f - edge) + s[c] * gated * edge;
        } else if (gate < 1.0f) {
            // A played slice is gated too.
            const float end = gate * static_cast<float>(sliceLength);
            const float a = static_cast<float>(age);
            const float w = std::clamp((end - a) / fade, 0.0f, 1.0f);
            const float back = std::clamp((a - static_cast<float>(sliceLength) + fade) / fade, 0.0f, 1.0f);
            out[0] = in[0] * std::max(w, back);
            out[1] = in[1] * std::max(w, back);
        }
        ++age;
        if (++write >= size) write = 0;
        L[i] = in[0] + (out[0] - in[0]) * mix;
        R[i] = in[1] + (out[1] - in[1]) * mix;
    }
    return true;
}

} // namespace acidulous::effect
