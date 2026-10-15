#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <engine/machine/draw/FreeReed.h>

// A free reed at the foot of a pipe, as a sheng's, a shō's or a khaen's.
//
// The pipe is a tube closed at the reed and open at the top: a delay line
// a round trip long, the open end throwing the wave back inverted through a
// low-pass (the air it radiates and the bamboo's loss). It rings at the
// note and its odd harmonics, so the reed's flow, which drives it, comes out
// nearly pure. The reed is tuned to its pipe, as a maker files it, and its
// pitch is its own: the pipe is driven by the reed, not the other way.
//
// SI units. The pipe's impedance is rho c / bore area.
namespace acidulous::machine::draw {

/** What a pipe instrument's pipes are like. */
struct PipeMake {
    /** The bore, metres across. */
    float bore;
    /** What the open end gives back each round trip, and the corner of its low-pass as a multiple of the note. */
    float reflection, corner;
    /** How much of the reed is heard directly, against the pipe (a khaen's reeds sound in its wind chest). */
    float reedHeard;
    /**
     * How far the pipe's upper modes stray from whole multiples of its note
     * (the open end's correction shrinks as the pitch rises): an all-pass's
     * coefficient, 0 for none. The reed's harmonics then miss the modes.
     */
    float stray;
};

class PipeReed {
  public:
    /** The longest round trip, samples: a pipe down to about 12 Hz at 48 kHz. */
    static constexpr int kLine = 2048;

    void prepare(float sampleRate) {
        sr = sampleRate;
        reed.prepare(sampleRate);
        clear();
    }

    void clear() {
        reed.clear();
        line.fill(0.0f);
        at = 0;
        returning = lowState = lowState2 = outWas = passIn = passOut = 0.0f;
    }

    void seed(uint32_t s) { reed.seed(s); }

    /** Makes the pipe for [hz], and its reed of [reedMake] for [reedHz]. */
    void make(float hz, float reedHz, const ReedMake &reedMake, const PipeMake &k) {
        hz = std::clamp(hz, 27.5f, 4500.0f);
        reed.make(std::clamp(reedHz, 27.5f, 4500.0f), reedMake);
        const float area = 3.14159265f * 0.25f * k.bore * k.bore;
        impedance = kRho * kC / area;
        reflection = k.reflection;
        heard = k.reedHeard;
        lowPole = std::exp(-6.2831853f * std::fmin(k.corner * hz, 0.45f * sr) / sr);
        // A quarter wave: the round trip is half the period, less what the
        // two low-passes and the all-pass delay the note.
        const float w = 6.2831853f * hz / sr;
        const float lowDelay = 2.0f * phaseDelay(lowPole, w);
        allpass = k.stray;
        const float passDelay = allpassDelay(allpass, w);
        delay = std::clamp(0.5f * sr / hz - lowDelay - passDelay, 2.0f, static_cast<float>(kLine - 2));
    }

    /** One sample blown at [pressure] Pa from a supply of [supply] m^3/s a Pa; returns what's heard. */
    float step(float pressure, float supply) {
        const float direct = reed.step(pressure, supply);
        const float u = reed.flow();
        // The wave going up the pipe, and the one coming back from the open end, a round trip later.
        line[at] = impedance * u + returning;
        float read = static_cast<float>(at) - delay;
        if (read < 0.0f) read += static_cast<float>(kLine);
        const int i0 = std::min(static_cast<int>(read), kLine - 1);
        const int i1 = i0 + 1 < kLine ? i0 + 1 : 0;
        const float frac = read - static_cast<float>(i0);
        const float top = line[i0] + (line[i1] - line[i0]) * frac;
        at = at + 1 < kLine ? at + 1 : 0;
        // The open end and the walls: two poles, so the note's harmonics lose far more than the note.
        lowState = top + lowPole * (lowState - top);
        lowState2 = lowState + lowPole * (lowState2 - lowState);
        // The all-pass: y = a x + x1 - a y1.
        const float passed = allpass * lowState2 + passIn - allpass * passOut;
        passIn = lowState2;
        passOut = passed;
        returning = -reflection * passed;
        // What leaves the open end: the wave that isn't thrown back.
        const float out = top + returning;
        const float change = out - outWas;
        outWas = out;
        return kPipeHeard * change * sr / impedance + heard * direct;
    }

    /** For tests: the reed's tip, metres. */
    float tip() const { return reed.tip(); }

  private:
    static constexpr float kRho = 1.2f, kC = 343.0f;
    /** The pipe's radiation against the reed's flow change, so a pipe sounds as loud as a reed in the open. */
    static constexpr float kPipeHeard = 1.0f;

    FreeReed reed;
    std::array<float, kLine> line{};
    int at = 0;
    float sr = 48000.0f, delay = 50.0f, impedance = 1e7f, reflection = 0.98f, lowPole = 0.5f, heard = 0.0f;
    float returning = 0.0f, lowState = 0.0f, lowState2 = 0.0f, outWas = 0.0f, passIn = 0.0f, passOut = 0.0f, allpass = 0.0f;

    /** A one-pole low-pass y = x + p (y1 - x)'s delay at [w] radians a sample, samples. */
    static float phaseDelay(float p, float w) {
        // H = (1 - p) / (1 - p e^-jw): its phase is -atan(p sin w / (1 - p cos w)).
        return std::atan2(p * std::sin(w), 1.0f - p * std::cos(w)) / w;
    }
    /** A first-order all-pass (a + z^-1) / (1 + a z^-1)'s delay at [w], samples. */
    static float allpassDelay(float a, float w) {
        const float phase = -w + 2.0f * std::atan2(a * std::sin(w), 1.0f + a * std::cos(w)) * -1.0f;
        return -phase / w;
    }
};

} // namespace acidulous::machine::draw
